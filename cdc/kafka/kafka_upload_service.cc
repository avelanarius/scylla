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
//    properties._request_timeout = 10000;
    properties._servers = {
            {"172.20.0.3", 9092}
    };

    // TODO: What if doesn't connect to any broker? Handle exceptions
    _producer = std::make_unique<kafka4seastar::kafka_producer>(std::move(properties));
    _producer_initialized = _producer->init().handle_exception([] (auto exp) {
        std::cout << "\n\nconnection exception\n\n";
    });


    _proxy.set_kafka_upload_service(this);
    arm_timer();
}

std::vector<schema_ptr> kafka_upload_service::get_tables_with_cdc_enabled() {
    auto tables = _proxy.get_db().local().get_column_families();

    std::vector<schema_ptr> tables_with_cdc;
    for (auto& [id, table] : tables) {
        auto schema = table->schema();
        if (schema->cdc_options().enabled()) {
            auto schema_cdc = _proxy.get_db().local().find_schema(schema->ks_name(), schema->cf_name() + "_scylla_cdc_log");
            tables_with_cdc.push_back(schema_cdc);
        }
    }

    return tables_with_cdc;
}

timeuuid kafka_upload_service::do_kafka_replicate(schema_ptr table_schema, timeuuid last_seen) {
//    sstring topic = "topic";
//    sstring key = "";
//
//    auto f = select(table_schema, last_seen).then([table_schema] (auto result) {
//        size_t len;
//        uint8_t* buffer;
//        auto avro = convert(table_schema, result->);
//        avro->next(&buffer, &len);
//
//        std::stringstream ss;
//        for (size_t i = 0; i < len; i++) {
//            ss << buffer[i];
//        }
//        std::string value = ss.str();
//        std::cout << "value: " << value << "\n";
//    });
//    _pending_queue = _pending_queue.then([f = std::move(f)] () mutable {
//       return std::move(f);
//    });

    return last_seen;
}

void kafka_upload_service::on_timer() {
    arm_timer();

    auto tables_with_cdc_enabled = get_tables_with_cdc_enabled();
    std::set<std::pair<sstring, sstring>> cdc_keyspace_table;

    // Remove all entries not seen in set of CDC enabled tables
    for (auto& table : tables_with_cdc_enabled) {
        cdc_keyspace_table.emplace(table->ks_name(), table->cf_name());
    }

    for (auto it = _last_seen_row_key.cbegin(); it != _last_seen_row_key.cend(); ) {
        auto should_delete = cdc_keyspace_table.count(it->first) == 0;
        if (should_delete) {
            _last_seen_row_key.erase(it++);
        } else {
            ++it;
        }
    }

    for (auto& table : tables_with_cdc_enabled) {
        std::pair<sstring, sstring> entry = {table->ks_name(), table->cf_name()};
        auto has_entry = _last_seen_row_key.count(entry) != 0;
        if (!has_entry) {
            _last_seen_row_key[entry] = utils::UUID();
        }
        // Create Kafka topic and schema
        auto last_seen = _last_seen_row_key[entry];
        auto result = select(table, last_seen).then([this, table](lw_shared_ptr<cql3::untyped_result_set> results){
            if (! results) {
                std::cout << "empty_query_results" << std::endl;
                return;
            }
            for (auto &row : *results) {
                auto op = row.get_opt<int8_t>("cdc$operation");
                if (op) {
                    if (op.value() == 2) {
                        auto data = convert(table, row); // send this data

                        seastar::sstring value { data->begin(), data->end() };
                        std::string sufix = "_scylla_cdc_log";
                        seastar::sstring topic { table->cf_name().begin(), table->cf_name().end() - sufix.length() };
//                        seastar::sstring topic = "";

                        std::cout << "\n\n\ntopic: " << topic << "\n";
                        std::cout << "len: " << data->size() << " -> "  << value.length() << "\nvalue: ";
                        std::cout << value << "\n\n";

                        auto f = _producer->produce(topic, "1", value).handle_exception([] (auto ex) {
//                        auto f = _producer->produce("topic", "", "KUPSKO").handle_exception([] (auto ex) {
                            std::cout << "\n\nproblem producing: " << ex << "\n\n";
                        });
                        _pending_queue = _pending_queue.then([f = std::move(f)] () mutable {
                            return std::move(f);
                        });
                    }
                }
            }
        });
        //_last_seen_row_key[entry] = do_kafka_replicate(table, last_seen);
    }
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
    key_schema = compose_avro_schema("key_schema", schema->ks_name(),
                                     key_schema_fields);
    return key_schema;
}

sstring kafka_upload_service::compose_value_schema_for(schema_ptr schema){

    sstring value_schema, value_schema_fields;
    value_schema_fields = compose_avro_record_fields(schema->all_columns());
    std::string sufix = "_scylla_cdc_log";
    seastar::sstring topic { schema->cf_name().begin(), schema->cf_name().end() - sufix.length() };
    value_schema = compose_avro_schema("value_schema_" + topic, schema->ks_name(),
                                       value_schema_fields);
    return value_schema;
}

sstring kafka_upload_service::compose_avro_record_fields(const schema::columns_type& columns){
    sstring result = "";
    int n = 0;
    for(const column_definition& cdef : columns){

        if (cdef.name_as_text().compare(0, 4, "cdc$") == 0) {
            continue;
        }

        if (n++ != 0) {
            result += ",";
        }
        result += "{";
        result += "\"name\":\"" + cdef.name_as_text() + "\",";
        result += "\"type\":[\"null\",\""  + kind_to_avro_type(cdef.type->get_kind()) + "\"]";
//        result += "\"type\":\""  + kind_to_avro_type(cdef.type->get_kind()) + "\"";
        result += "}";
    }
    return result;
}

sstring kafka_upload_service::compose_avro_schema(sstring avro_name, sstring avro_namespace, sstring avro_fields) {
        sstring result = sstring("{"
                                 "\"type\":\"record\","
                                 "\"name\":\"" + avro_name + "\","
//                                 "\"namespace\":\"" + avro_namespace + "\","
                                 "\"fields\":[" + avro_fields + "]"
                                 "}");
        std::cout << "\n\nschema: " << result << "\n\n";
        return result;
 }

future<lw_shared_ptr<cql3::untyped_result_set>> kafka_upload_service::select(schema_ptr table, timeuuid last_seen_key) {
    std::vector<query::clustering_range> bounds;
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
    // db::timeout_clock::time_point timeout = db::timeout_clock::now + 10s;
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

std::shared_ptr<std::vector<uint8_t>> kafka_upload_service::convert(schema_ptr schema, const cql3::untyped_result_set_row &row) {
    auto avro_schema = compose_value_schema_for(schema);
    //auto avro_schema = "{\"type\":\"record\",\"name\":\"value_schema\",\"namespace\":\"ks.t_scylla_cdc_log\",\"fields\":[{\"name\":\"cdc$stream_id\",\"type\":[\"null\",\"string\"]},{\"name\":\"cdc$time\",\"type\":[\"null\",\"string\"]},{\"name\":\"cdc$batch_seq_no\",\"type\":[\"null\",\"int\"]},{\"name\":\"cdc$operation\",\"type\":[\"null\",\"string\"]},{\"name\":\"cdc$ttl\",\"type\":[\"null\",\"long\"]},{\"name\":\"ck\",\"type\":[\"null\",\"int\"]},{\"name\":\"pk\",\"type\":[\"null\",\"int\"]},{\"name\":\"v\",\"type\":[\"null\",\"int\"]}]}";
    avro::ValidSchema compiledSchema;
    compiledSchema = avro::compileJsonSchemaFromString(avro_schema);
    avro::OutputStreamPtr out = avro::memoryOutputStream();
    avro::EncoderPtr e = avro::validatingEncoder(compiledSchema, avro::binaryEncoder());
    e->init(*out);
    avro::GenericDatum datum(compiledSchema);
    if (datum.type() == avro::AVRO_RECORD) {
        avro::GenericRecord &record = datum.value<avro::GenericRecord>();
        auto columns = row.get_columns();
        for (auto &column : columns) {
            auto name = column->name->to_string();

            if (name.compare(0, 4, "cdc$") == 0) {
                continue;
            }

            abstract_type::kind kind = column->type->get_kind();
            avro::GenericDatum &un = record.field(name);
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
    }
    avro::encode(*e,datum);
    e->flush();
    auto v = avro::snapshot(*out);

    /* Write framing */
    //schema->framing_write(res);
    return v;
}

} // namespace cdc::kafka
