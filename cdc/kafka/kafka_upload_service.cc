/*
 * Copyright (C) 2020 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <vector>
#include <set>

#include <iomanip>

#include "database.hh"
#include "kafka_upload_service.hh"

#include "avro/lang/c++/api/Compiler.hh"
#include "avro/lang/c++/api/Encoder.hh"
#include "avro/lang/c++/api/Decoder.hh"
#include "avro/lang/c++/api/Specific.hh"
#include "avro/lang/c++/api/Generic.hh"

#include "cql3/column_specification.hh"
#include "cql3/column_identifier.hh"
#include "cql3/selection/selection.hh"
#include "cql3/result_set.hh"

namespace cdc::kafka {

using seastar::sstring;
using namespace std::chrono_literals;

kafka_upload_service::kafka_upload_service(service::storage_proxy& proxy, auth::service& auth_service)
: _proxy(proxy)
, _timer([this] { on_timer(); })
, _auth_service(auth_service)
, _client_state(service::client_state::external_tag{}, _auth_service)
, _pending_queue(seastar::make_ready_future<>())
, _producer_initialized(seastar::make_ready_future<>())
{
    kafka4seastar::producer_properties properties;
    properties._client_id = "cdc_replication_service";
    properties._servers = {
            {"localhost", 9092}
    };

    _proxy.set_kafka_upload_service(this);

    // TODO: What if doesn't connect to any broker? Handle exceptions
    _producer = std::make_unique<kafka4seastar::kafka_producer>(std::move(properties));
    _producer_initialized = _producer->init().then_wrapped([this] (auto&& f) {
        try {
            f.get();
            arm_timer();
        } catch (...) {
            std::cout << "\nConnection exception: Kafka Upload Service not started\n";
        }
    });
}

std::vector<std::pair<schema_ptr, schema_ptr>> kafka_upload_service::get_cdc_tables() {
    auto tables = _proxy.get_db().local().get_column_families();

    std::vector<std::pair<schema_ptr, schema_ptr>> cdc_tables;
    for (auto& [id, table] : tables) {
        auto schema = table->schema();
        if (schema->cdc_options().enabled()) {
            auto table_schema = _proxy.get_db().local().find_schema(schema->ks_name(), schema->cf_name());
            auto cdc_schema = _proxy.get_db().local().find_schema(schema->ks_name(), schema->cf_name() + "_scylla_cdc_log");
            cdc_tables.push_back(std::make_pair(table_schema, cdc_schema));
        }
    }

    return cdc_tables;
}

timeuuid kafka_upload_service::do_kafka_replicate(schema_ptr table_schema, timeuuid last_seen) {
    return last_seen;
}

seastar::sstring get_schema_id(uint32_t x) {
    std::vector<unsigned char> v { static_cast<const unsigned char*>(static_cast<const void*>(&x)),
                        static_cast<const unsigned char*>(static_cast<const void*>(&x)) + sizeof x };
    std::reverse(std::begin(v), std::end(v));
    seastar::sstring str {v.begin(), v.end()};
    return str;
}

void kafka_upload_service::on_timer() {
    std::map<std::pair<sstring, sstring>, std::set<sstring>> cdc_primary_keys;
    auto cdc_tables = get_cdc_tables();
    std::set<std::pair<sstring, sstring>> cdc_keyspace_table;

    // Remove all entries not seen in set of CDC enabled tables
    for (auto& entry : cdc_tables) {
        cdc_keyspace_table.emplace(entry.first->ks_name(), entry.first->cf_name());
    }

    for (auto it = _last_seen_row_key.cbegin(); it != _last_seen_row_key.cend(); ) {
        auto should_delete = cdc_keyspace_table.count(it->first) == 0;
        if (should_delete) {
            _last_seen_row_key.erase(it++);
        } else {
            ++it;
        }
    }

    for (auto& tables : cdc_tables) {
        std::pair<sstring, sstring> entry = {tables.first->ks_name(), tables.first->cf_name()};
        auto has_entry = _last_seen_row_key.count(entry) != 0;
        if (!has_entry) {
            _last_seen_row_key[entry] = utils::UUID();
        }
        // Create Kafka topic and schema
        auto last_seen = _last_seen_row_key[entry];
        auto result = select(tables.second, last_seen).then([this, entry, table = tables.first] (lw_shared_ptr<cql3::untyped_result_set> results) {
            if (!results) {
                std::cout << "DEBUG	 Kafka_Upload_Service: no new mutations for " << table->cf_name() << "\n";
                return;
            }
            for (auto &row : *results) {
                auto op = row.get_opt<int8_t>("cdc$operation");
                if (op) {
                    if (op.value() == 2 || op.value() == 1 || op.value() == 3) { // INSERT and UPDATE and DELETE
                        auto key_and_value = convert(table, row, op.value());

                        seastar::sstring value { key_and_value.second->begin(), key_and_value.second->end() };
                        seastar::sstring key { key_and_value.first->begin(), key_and_value.first->end() };
                        seastar::sstring topic { table->cf_name().begin(), table->cf_name().end() };

                        uint32_t key_schema_id = 4;
                        seastar::sstring magic_key = "\0" + get_schema_id(key_schema_id);

                        uint32_t val_schema_id = 3;
                        seastar::sstring magic_val = "\0" + get_schema_id(val_schema_id);

                        std::cout << "\n\ntopic: " << topic << "\n";
                        std::cout << "len: " << key.length() << "\nkey: ";
                        std::cout << key << "\n";
                        std::cout << "len: " << value.length() << "\nvalue: ";
                        std::cout << value << "\n\n";

						key = magic_key + key;
						value = op.value() == 3 ? value : magic_val + value;

                        auto f = _producer->produce(topic, key, value).handle_exception([] (auto ex) {
                            std::cout << "\n\nproblem producing: " << ex << "\n\n";
                        });
                        _pending_queue = _pending_queue.then([this, f = std::move(f)] () mutable {
                            return std::move(f);
                        });
                    }
                }
				
				auto timestamp = row.get_opt<timeuuid>("cdc$time");
				// std::cout << "\nApproaching timestamp: ";
				if (timestamp) {
					if (timestamp.value() > _last_seen_row_key[entry]) {
						// std::cout << "INSIDE! \n" << timestamp.value();
						_last_seen_row_key[entry] = timestamp.value();
					}
				}
				std::cout << "\n";
            }
        });
		// TODO: Change last_seen_row to reflect where we left the replication
        //_last_seen_row_key[entry] = do_kafka_replicate(table, last_seen);
    }
    _pending_queue = _pending_queue.then([this] () {
        arm_timer();
    });
}

sstring kafka_upload_service::kind_to_avro_type(abstract_type::kind kind) {
    switch (kind) {
        //TODO: Complex types + Check if all kinds are translated into appropriate avro types
        case abstract_type::kind::boolean:
            return sstring("boolean");

        case abstract_type::kind::counter:
        case abstract_type::kind::long_kind:
            return sstring("long");

        case abstract_type::kind::decimal:
        case abstract_type::kind::float_kind:
            return sstring("float");

        case abstract_type::kind::double_kind:
            return sstring("double");

        case abstract_type::kind::int32:
        case abstract_type::kind::short_kind:
            return sstring("int");

        case abstract_type::kind::ascii:
        case abstract_type::kind::byte:
        case abstract_type::kind::bytes:
        case abstract_type::kind::date:
        case abstract_type::kind::duration:
        case abstract_type::kind::empty:
        case abstract_type::kind::inet:
        case abstract_type::kind::list:
        case abstract_type::kind::map:
        case abstract_type::kind::reversed:
        case abstract_type::kind::set:
        case abstract_type::kind::simple_date:
        case abstract_type::kind::time:
        case abstract_type::kind::timestamp:
        case abstract_type::kind::timeuuid:
        case abstract_type::kind::tuple:
        case abstract_type::kind::user:
        case abstract_type::kind::utf8:
        case abstract_type::kind::uuid:
        case abstract_type::kind::varint:
        default:
            return sstring("string");
    }
}

seastar::sstring kafka_upload_service::compose_key_schema_for(schema_ptr schema){

    sstring key_schema, key_schema_fields;
    schema::columns_type primary_key_columns;
    for(const column_definition& cdef : schema->all_columns()){
        if(cdef.is_primary_key()){
            primary_key_columns.push_back(cdef);
        }
    }
    key_schema_fields = compose_avro_record_fields(primary_key_columns);
    key_schema = compose_avro_schema("key_schema", schema->ks_name() + "." + schema->cf_name(),
                                     key_schema_fields);
    return key_schema;
}

sstring kafka_upload_service::compose_value_schema_for(schema_ptr schema){

    sstring value_schema, value_schema_fields;
    value_schema_fields = compose_avro_record_fields(schema->all_columns());
    value_schema = compose_avro_schema("value_schema", schema->ks_name() + "." + schema->cf_name(),
                                       value_schema_fields);
    return value_schema;
}

sstring kafka_upload_service::compose_avro_record_fields(const schema::columns_type& columns){
    sstring result = "";
    int n = 0;
    for(const column_definition& cdef : columns){
        if (n++ != 0) {
            result += ",";
        }
        result += "{";
        result += "\"name\":\"" + cdef.name_as_text() + "\",";
        result += "\"type\":[\"null\",\""  + kind_to_avro_type(cdef.type->get_kind()) + "\"]";
        result += "}";
    }
    return result;
}

sstring kafka_upload_service::compose_avro_schema(sstring avro_name, sstring avro_namespace, sstring avro_fields) {
        sstring result = sstring("{"
                                 "\"type\":\"record\","
                                 "\"name\":\"" + avro_name + "\","
                                 "\"namespace\":\"" + avro_namespace + "\","
                                 "\"fields\":[" + avro_fields + "]"
                                 "}");

		if (avro_name != "key_schema") {
			result = "[\"null\"," + result + "]";
		}

        std::cout << "\nschema: " << result << "\n";
        return result;
}

future<lw_shared_ptr<cql3::untyped_result_set>> kafka_upload_service::select(schema_ptr table, timeuuid last_seen_key) {
    std::vector<query::clustering_range> bounds;

	// std::cout << "\nLast Seen: " << last_seen_key << "\n";

    auto lckp = clustering_key_prefix::from_single_value(*table, timeuuid_type->decompose(last_seen_key));
    auto lb = range_bound(lckp, false);
    auto rb_timestamp = std::chrono::system_clock::now() - std::chrono::seconds(10);
    auto rckp = clustering_key_prefix::from_single_value(*table, timeuuid_type->decompose(utils::UUID_gen::get_time_UUID(rb_timestamp)));
    auto rb = range_bound(rckp, true);
    bounds.push_back(query::clustering_range::make(lb, rb));
    auto selection = cql3::selection::selection::wildcard(table);

    query::column_id_vector static_columns, regular_columns;
    for (const column_definition& c : table->static_columns()) {
        static_columns.emplace_back(c.id);
    }
    for (const column_definition& c : table->regular_columns()) {
        regular_columns.emplace_back(c.id);
    }

    auto opts = selection->get_query_options();
    auto partition_slice = query::partition_slice(std::move(bounds), std::move(static_columns), std::move(regular_columns), opts);
    auto timeout = seastar::lowres_clock::now() + std::chrono::seconds(10);
    auto command = make_lw_shared<query::read_command> (
        table->id(),
        table->version(),
        partition_slice);
    dht::partition_range_vector partition_ranges;
    partition_ranges.push_back(query::full_partition_range);

    return _proxy.query(
        table, 
        command, 
        std::move(partition_ranges), 
        db::consistency_level::QUORUM,
        service::storage_proxy::coordinator_query_options(
            timeout,
            empty_service_permit(),
            _client_state
        )
    ).then([table = table, partition_slice = std::move(partition_slice), selection = std::move(selection)] 
        (service::storage_proxy::coordinator_query_result qr) -> lw_shared_ptr<cql3::untyped_result_set> {
        cql3::selection::result_set_builder builder(*selection, gc_clock::now(), cql_serialization_format::latest());
        query::result_view::consume(*qr.query_result, std::move(partition_slice), cql3::selection::result_set_builder::visitor(builder, *table, *selection));
        auto result_set = builder.build();
        if (!result_set || result_set->empty()) {
            return {};
        }
        return make_lw_shared<cql3::untyped_result_set>(*result_set);
    });
    /*.handle_exception([] (std::exception_ptr ep) {
        try {
            std::rethrow_exception(ep);
        } catch (exceptions::unavailable_exception &e) {
            std::cout << "unavailable_exception" << std::endl;
        }
    })*/
}

void kafka_upload_service::encode_union(avro::GenericDatum &un, const cql3::untyped_result_set_row &row, sstring &name, abstract_type::kind kind) {
    switch (kind) {
        //TODO: Complex types + Check if all kinds are translated into appropriate avro types
        case abstract_type::kind::boolean:
        {
            auto value = row.get_opt<bool>(name);
            if (value) {
                un.selectBranch(1);
                un.value<bool>() = value.value();
            }
            break;
        }
        case abstract_type::kind::counter:
        case abstract_type::kind::long_kind:
        {
            auto value = row.get_opt<int64_t>(name);
            if (value) {
                un.selectBranch(1);
                un.value<int64_t>() = value.value();
            }
            break;
        }
        case abstract_type::kind::decimal:
        case abstract_type::kind::float_kind:
        {
            auto value = row.get_opt<float>(name);
            if (value) {
                un.selectBranch(1);
                un.value<float>() = value.value();
            }
            break;
        }
        case abstract_type::kind::double_kind:
        {
            auto value = row.get_opt<double>(name);
            if (value) {
                un.selectBranch(1);
                un.value<double>() = value.value();
            }
            break;
        }
        case abstract_type::kind::int32:
        case abstract_type::kind::short_kind:
        {
            auto value = row.get_opt<int32_t>(name);
            if (value) {
                un.selectBranch(1);
                un.value<int32_t>() = value.value();
            }
            break;
        }
        case abstract_type::kind::ascii:
        case abstract_type::kind::byte:
        case abstract_type::kind::bytes:
        case abstract_type::kind::date:
        case abstract_type::kind::duration:
        case abstract_type::kind::empty:
        case abstract_type::kind::inet:
        case abstract_type::kind::list:
        case abstract_type::kind::map:
        case abstract_type::kind::reversed:
        case abstract_type::kind::set:
        case abstract_type::kind::simple_date:
        case abstract_type::kind::time:
        case abstract_type::kind::timestamp:
        case abstract_type::kind::timeuuid:
        case abstract_type::kind::tuple:
        case abstract_type::kind::user:
        case abstract_type::kind::utf8:
        case abstract_type::kind::uuid:
        case abstract_type::kind::varint:
        default:
        {
            auto value = row.get_opt<sstring>(name);
            if (value) {
                un.selectBranch(1);
                un.value<std::string>() = std::string(value.value());
            }
            break;
        }
    }
}

std::pair<std::shared_ptr<std::vector<uint8_t>>,std::shared_ptr<std::vector<uint8_t>>> kafka_upload_service::convert(schema_ptr schema, const cql3::untyped_result_set_row &row, int8_t operation) {
    auto key_schema = compose_key_schema_for(schema);
    auto value_schema = compose_value_schema_for(schema);

    avro::ValidSchema compiled_value_schema = avro::compileJsonSchemaFromString(value_schema);
    avro::ValidSchema compiled_key_schema = avro::compileJsonSchemaFromString(key_schema);
    avro::OutputStreamPtr out_value = avro::memoryOutputStream();
    avro::OutputStreamPtr out_key = avro::memoryOutputStream();
    avro::EncoderPtr e_value = avro::validatingEncoder(compiled_value_schema, avro::binaryEncoder());
    avro::EncoderPtr e_key = avro::validatingEncoder(compiled_key_schema, avro::binaryEncoder());

    e_value->init(*out_value);
    e_key->init(*out_key);

    avro::GenericDatum value_datum(compiled_value_schema);
    avro::GenericDatum key_datum(compiled_key_schema);

    std::set <sstring> primary_key_columns;
    for (const column_definition& cdef : schema->all_columns()) {
        if(cdef.is_primary_key()){
            primary_key_columns.insert(cdef.name_as_text());
        }
    }

	if (key_datum.type() == avro::AVRO_RECORD) {
        avro::GenericRecord &keyRecord = key_datum.value<avro::GenericRecord>();
            
        auto columns = schema->all_columns();
        for (auto &column : columns) {
            auto name = column.name_as_text();

            if (primary_key_columns.count(name) > 0) {
				abstract_type::kind kind = column.type->get_kind();
                avro::GenericDatum &key_un = keyRecord.field(name);
                encode_union(key_un, row, name, kind);
            }
        }
    }

	if (operation == 3) {
		// value should be NULL
		value_datum.selectBranch(0);
	} else {
		value_datum.selectBranch(1);

		if (value_datum.type() == avro::AVRO_RECORD) {
			avro::GenericRecord &valueRecord = value_datum.value<avro::GenericRecord>();

			auto columns = schema->all_columns();
			for (auto &column : columns) {
				auto name = column.name_as_text();

				abstract_type::kind kind = column.type->get_kind();
				avro::GenericDatum &value_un = valueRecord.field(name);
				encode_union(value_un, row, name, kind);
			}
		}
	}

    avro::encode(*e_value, value_datum);
    avro::encode(*e_key, key_datum);
    e_value->flush();
    e_key->flush();
    auto v = avro::snapshot(*out_value);
    auto k = avro::snapshot(*out_key);

    return std::make_pair(k, v);
}

} // namespace cdc::kafka
