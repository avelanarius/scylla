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

#include "schema.hh"

#include "avro/lang/c++/api/Compiler.hh"
#include "avro/lang/c++/api/Encoder.hh"
#include "avro/lang/c++/api/Decoder.hh"
#include "avro/lang/c++/api/Specific.hh"
#include "avro/lang/c++/api/Generic.hh"
#include "cql3/untyped_result_set.hh"

#include <seastar/core/seastar.hh>

namespace cdc::kafka_replication {

class avro_row_serializer {
    schema_ptr _scylla_schema;

    sstring _key_schema_json;
    sstring _value_schema_json;

    avro::ValidSchema _key_compiled_schema;
    avro::ValidSchema _value_compiled_schema;

    avro::EncoderPtr _key_encoder;
    avro::EncoderPtr _value_encoder;

    sstring compose_key_schema_json();
    sstring compose_value_schema_json();

    sstring compose_avro_record_fields(const schema::columns_type& columns);
    sstring kind_to_avro_type(abstract_type::kind kind);
    sstring compose_avro_schema(sstring avro_name, sstring avro_namespace, sstring avro_fields, bool is_nullable);
    void encode_union(avro::GenericDatum& un, const cql3::untyped_result_set_row& row, sstring& name, abstract_type::kind kind);

public:
    struct encoded_row {
        std::shared_ptr<std::vector<uint8_t>> key;
        std::shared_ptr<std::vector<uint8_t>> value;
    };

    avro_row_serializer(schema_ptr scylla_schema);

    encoded_row serialize(const cql3::untyped_result_set_row& row);

    // Serializes using Confluent Platform wire format:
    // https://docs.confluent.io/current/schema-registry/serdes-develop/index.html#wire-format
    encoded_row serialize_wire_format(const cql3::untyped_result_set_row& row, uint32_t key_schema_id, uint32_t value_schema_id);
};

} // namespace cdc:kafka_replication

