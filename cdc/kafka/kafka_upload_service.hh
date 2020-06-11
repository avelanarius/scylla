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

#include "utils/UUID.hh"
#include "service/storage_proxy.hh"
#include "service/client_state.hh"

#include "avro/lang/c++/api/Encoder.hh"

#include "cql3/untyped_result_set.hh"

class schema;
class schema_extension;

using schema_ptr = seastar::lw_shared_ptr<const schema>;
using timeuuid = utils::UUID;

namespace cdc::kafka {

using seastar::sstring;

class kafka_upload_service final {
    service::storage_proxy& _proxy;
    timer<seastar::lowres_clock> _timer;
    auth::service& _auth_service;
    service::client_state _client_state;

    std::map<std::pair<sstring, sstring>, timeuuid> _last_seen_row_key;

    std::unique_ptr<kafka4seastar::kafka_producer> _producer;

    seastar::future<> _pending_queue;
    seastar::future<> _producer_initialized;

    void on_timer();

    sstring compose_value_schema_for(schema_ptr schema);

    sstring compose_key_schema_for(schema_ptr schema);

    sstring compose_avro_record_fields(const schema::columns_type& columns);

    sstring kind_to_avro_type(abstract_type::kind kind);

    sstring compose_avro_schema(sstring avro_name, sstring avro_namespace, sstring avro_fields);

    future<lw_shared_ptr<cql3::untyped_result_set>> select(schema_ptr table, timeuuid last_seen_key);

    std::pair<std::shared_ptr<std::vector<uint8_t>>,std::shared_ptr<std::vector<uint8_t>>> convert(schema_ptr schema, const cql3::untyped_result_set_row &row, int8_t operation);

    std::vector<std::pair<schema_ptr, schema_ptr>> get_cdc_tables();

    timeuuid do_kafka_replicate(schema_ptr table_schema, timeuuid last_seen);

    void encode_union(avro::GenericDatum &un, const cql3::untyped_result_set_row &row, sstring &name, abstract_type::kind kind);

    void arm_timer() {
        _timer.arm(seastar::lowres_clock::now() + std::chrono::seconds(10));
    }

public:
    kafka_upload_service(service::storage_proxy& proxy, auth::service& auth_service);

    future<> stop() {
        _timer.cancel();
        return _pending_queue.discard_result();
    }

};

} // namespace cdc::kafka
