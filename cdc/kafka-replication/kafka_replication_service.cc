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
#include "kafka_replication_service.hh"
#include "avro_row_serializer.hh"

#include "avro/lang/c++/api/Compiler.hh"
#include "avro/lang/c++/api/Encoder.hh"
#include "avro/lang/c++/api/Decoder.hh"
#include "avro/lang/c++/api/Specific.hh"
#include "avro/lang/c++/api/Generic.hh"

#include "cql3/column_specification.hh"
#include "cql3/column_identifier.hh"
#include "cql3/selection/selection.hh"
#include "cql3/result_set.hh"

namespace cdc::kafka_replication {

static logging::logger rlogger("kafka_replication_service");

kafka_replication_service::kafka_replication_service(service::storage_proxy& proxy, auth::service& auth_service)
: _proxy(proxy)
, _timer([this] { on_timer(); })
, _auth_service(auth_service)
, _client_state(service::client_state::external_tag{}, _auth_service)
, _pending_queue(seastar::make_ready_future<>()) {
    kafka4seastar::producer_properties properties;
    properties.client_id = "cdc_replication_service";
    properties.servers = {
            {"127.0.0.1", 9092}
    };

    _proxy.set_kafka_replication_service(this);

    // TODO: What if doesn't connect to any broker? Handle exceptions
    _producer = std::make_unique<kafka4seastar::kafka_producer>(std::move(properties));
    _pending_queue = _producer->init().then_wrapped([this] (auto&& f) {
        try {
            f.get();
            arm_timer();
        } catch (...) {
            rlogger.error("Connection to Kafka exception");
        }
    });
}

void kafka_replication_service::on_timer() {
    for (auto& table : list_replicated_tables()) {
        replicate_table(table);
    }
   
    _pending_queue = _pending_queue.then([this] () {
        arm_timer();
    });
}

void kafka_replication_service::arm_timer() {
    _timer.arm(seastar::lowres_clock::now() + std::chrono::seconds(10));
}

std::vector<kafka_replication_service::replicated_table> kafka_replication_service::list_replicated_tables() {
    auto tables = _proxy.get_db().local().get_column_families();

    std::vector<kafka_replication_service::replicated_table> cdc_tables;
    for (auto& [id, table] : tables) {
        auto schema = table->schema();
        if (schema->cdc_options().enabled()) {
            auto base_table_schema = _proxy.get_db().local().find_schema(schema->ks_name(), schema->cf_name());
            auto cdc_table_schema = _proxy.get_db().local().find_schema(schema->ks_name(), schema->cf_name() + "_scylla_cdc_log");
            cdc_tables.push_back({base_table_schema, cdc_table_schema});
        }
    }

    // Cleanup stray _last_seen_row_key entries.
    std::set<std::pair<sstring, sstring>> seen_tables;
    for (auto& entry : cdc_tables) {
        seen_tables.emplace(entry.base_table_schema->ks_name(), entry.base_table_schema->cf_name());
    }

    for (auto it = _last_seen_row_key.cbegin(); it != _last_seen_row_key.cend(); ) {
        if (seen_tables.count(it->first) == 0) {
            it = _last_seen_row_key.erase(it);
        } else {
            it++;
        }
    }

    return cdc_tables;
}

void kafka_replication_service::replicate_table(kafka_replication_service::replicated_table& table) {
    auto last_seen = _last_seen_row_key[{table.base_table_schema->ks_name(), table.base_table_schema->cf_name()}];

    auto query_future = query_changes(table.cdc_table_schema, last_seen);

    auto replicate_future = query_future.then([this, table] (lw_shared_ptr<cql3::untyped_result_set> results) {
        if (!results) {
            return;
        }
        for (auto& row : *results) {
            replicate_row(table, row);
        }
    });
}


void kafka_replication_service::replicate_row(const kafka_replication_service::replicated_table& table, 
                                              const cql3::untyped_result_set::row& row) {
    auto operation_optional = row.get_opt<int8_t>("cdc$operation");
    auto timestamp_optional = row.get_opt<timeuuid>("cdc$time");
    if (!operation_optional || !timestamp_optional) {
        return;
    }

    auto operation = static_cast<cdc::operation>(*operation_optional);
    if (operation != cdc::operation::insert && operation != cdc::operation::update && operation != cdc::operation::row_delete) {
        // Operation type not supported.
        return;
    }

    avro_row_serializer row_serializer(table.base_table_schema);
    auto serialized_row = row_serializer.serialize_wire_format(row, table.key_schema_id, table.value_schema_id);

    std::optional<sstring> key(std::in_place, serialized_row.key->begin(), serialized_row.key->end());
    auto value = operation == cdc::operation::row_delete ? std::optional<sstring>() 
        : std::optional<sstring>(std::in_place, serialized_row.value->begin(), serialized_row.value->end());

    auto produce_future = _producer->produce(table.base_table_schema->cf_name(), key, value).handle_exception([](auto ex) {
        rlogger.error("Error sending replicated row to Kafka");
    });
    _pending_queue = _pending_queue.then([this, produce_future = std::move(produce_future)]() mutable {
        return std::move(produce_future);
    });

    std::pair<sstring, sstring> base_table_name{table.base_table_schema->ks_name(), table.base_table_schema->cf_name()};
    if (timeuuid_type->less(timeuuid_type->decompose(_last_seen_row_key[base_table_name]),
                            timeuuid_type->decompose(*timestamp_optional))) {
        _last_seen_row_key[base_table_name] = *timestamp_optional;
    }
}

future<lw_shared_ptr<cql3::untyped_result_set>> kafka_replication_service::query_changes(schema_ptr table, timeuuid last_seen_key) {
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
    auto timeout = seastar::lowres_clock::now() + std::chrono::seconds(10);
    auto command = make_lw_shared<query::read_command> (table->id(), table->version(), partition_slice);
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
    }).handle_exception([] (auto ex) {
        rlogger.error("Could not query CDC table.");
        return lw_shared_ptr<cql3::untyped_result_set>();
    });
}

future<> kafka_replication_service::stop() {
    _timer.cancel();
    return _pending_queue.discard_result();
}

} // namespace cdc:kafka_replication

