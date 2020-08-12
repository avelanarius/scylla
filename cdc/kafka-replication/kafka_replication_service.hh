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

#pragma once

#include <map>
#include <chrono>

#include <seastar/core/seastar.hh>
#include <seastar/core/timer.hh>

#include <kafka4seastar/producer/kafka_producer.hh>

#include "schema.hh"
#include "cdc/log.hh"
#include "utils/UUID.hh"
#include "service/storage_proxy.hh"
#include "service/client_state.hh"
#include "cql3/untyped_result_set.hh"

#include "avro/lang/c++/api/Encoder.hh"

using timeuuid = utils::UUID;

namespace cdc::kafka_replication {

class kafka_replication_service final {
    service::storage_proxy& _proxy;
    timer<seastar::lowres_clock> _timer;
    auth::service& _auth_service;
    service::client_state _client_state;

    std::map<std::pair<sstring, sstring>, timeuuid> _last_seen_row_key;

    std::unique_ptr<kafka4seastar::kafka_producer> _producer;

    seastar::future<> _pending_queue;

    void on_timer();
    void arm_timer();

    struct replicated_table {
        schema_ptr base_table_schema;
        schema_ptr cdc_table_schema;

        uint32_t key_schema_id;
        uint32_t value_schema_id;
    };

    std::vector<replicated_table> list_replicated_tables();

    void replicate_table(replicated_table& table);

    void replicate_row(const replicated_table& table, const cql3::untyped_result_set::row& row);

    future<lw_shared_ptr<cql3::untyped_result_set>> query_changes(schema_ptr table, timeuuid last_seen_key);

public:
    kafka_replication_service(service::storage_proxy& proxy, auth::service& auth_service);

    future<> stop();
};

} // namespace cdc:kafka_replication

