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

#include "avro_row_serializer.hh"

#include <seastar/net/byteorder.hh>

namespace cdc::kafka_replication {

avro_row_serializer::avro_row_serializer(schema_ptr scylla_schema) 
    : _scylla_schema(scylla_schema)
    , _key_schema_json(compose_key_schema_json())
    , _value_schema_json(compose_value_schema_json())
    , _key_compiled_schema(avro::compileJsonSchemaFromString(_key_schema_json))
    , _value_compiled_schema(avro::compileJsonSchemaFromString(_value_schema_json))
    , _key_encoder(avro::validatingEncoder(_key_compiled_schema, avro::binaryEncoder()))
    , _value_encoder(avro::validatingEncoder(_value_compiled_schema, avro::binaryEncoder())) {}

sstring avro_row_serializer::compose_key_schema_json() {
    sstring key_schema, key_schema_fields;
    schema::columns_type primary_key_columns;
    for (const auto& cdef : _scylla_schema->all_columns()) {
        if (cdef.is_primary_key()) {
            primary_key_columns.push_back(cdef);
        }
    }
    key_schema_fields = compose_avro_record_fields(primary_key_columns);
    key_schema = compose_avro_schema("key_schema", _scylla_schema->ks_name() + "." + _scylla_schema->cf_name(),
                                     key_schema_fields, false);
    return key_schema;
}

sstring avro_row_serializer::compose_value_schema_json() {
    sstring value_schema, value_schema_fields;
    value_schema_fields = compose_avro_record_fields(_scylla_schema->all_columns());
    value_schema = compose_avro_schema("value_schema", _scylla_schema->ks_name() + "." + _scylla_schema->cf_name(),
                                       value_schema_fields, true);
    return value_schema;
}

sstring avro_row_serializer::compose_avro_record_fields(const schema::columns_type& columns) {
    sstring result;
    bool is_first = true;
    for (const auto& cdef : columns) {
        if (!is_first) {
            result += ",";
        }
        is_first = false;
        result += "{";
        result += "\"name\":\"" + cdef.name_as_text() + "\",";
        result += "\"type\":[\"null\",\""  + kind_to_avro_type(cdef.type->get_kind()) + "\"]";
        result += "}";
    }
    return result;
}

sstring avro_row_serializer::compose_avro_schema(sstring avro_name, sstring avro_namespace, 
                                                 sstring avro_fields, bool is_nullable) {
        sstring result;
        result += "{";
        result += "\"type\":\"record\",";
        result += "\"name\":\"" + avro_name + "\",";
        result += "\"namespace\":\"" + avro_namespace + "\",";
        result += "\"fields\":[" + avro_fields + "]";
        result += "}";

        if (is_nullable) {
            result = "[\"null\"," + result + "]";
        }
        return result;
}

sstring avro_row_serializer::kind_to_avro_type(abstract_type::kind kind) {
    // TODO: Complex types + Check if all kinds are translated into appropriate avro types
    switch (kind) {
        case abstract_type::kind::boolean:
            return "boolean";

        case abstract_type::kind::counter:
        case abstract_type::kind::long_kind:
            return "long";

        case abstract_type::kind::decimal:
        case abstract_type::kind::float_kind:
            return "float";

        case abstract_type::kind::double_kind:
            return "double";

        case abstract_type::kind::int32:
        case abstract_type::kind::short_kind:
            return "int";

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
            return "string";
    }
}

void avro_row_serializer::encode_union(avro::GenericDatum& datum, const cql3::untyped_result_set_row& row, sstring& name, abstract_type::kind kind) {
    // TODO: Complex types + Check if all kinds are translated into appropriate avro types
    switch (kind) {
        case abstract_type::kind::boolean: {
            auto value = row.get_opt<bool>(name);
            if (value) {
                datum.selectBranch(1);
                datum.value<bool>() = value.value();
            }
            break;
        }
        case abstract_type::kind::counter:
        case abstract_type::kind::long_kind: {
            auto value = row.get_opt<int64_t>(name);
            if (value) {
                datum.selectBranch(1);
                datum.value<int64_t>() = value.value();
            }
            break;
        }
        case abstract_type::kind::decimal:
        case abstract_type::kind::float_kind: {
            auto value = row.get_opt<float>(name); 
            if (value) {
                datum.selectBranch(1);
                datum.value<float>() = value.value();
            }
            break;
        }
        case abstract_type::kind::double_kind: {
            auto value = row.get_opt<double>(name);
            if (value) {
                datum.selectBranch(1);
                datum.value<double>() = value.value();
            }
            break;
        }
        case abstract_type::kind::int32:
        case abstract_type::kind::short_kind: {
            auto value = row.get_opt<int32_t>(name);
            if (value) {
                datum.selectBranch(1);
                datum.value<int32_t>() = value.value();
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
        default: {
            auto value = row.get_opt<sstring>(name);
            if (value) {
                datum.selectBranch(1);
                datum.value<std::string>() = std::string(value.value());
            }
            break;
        }
    }
}

avro_row_serializer::encoded_row avro_row_serializer::serialize(const cql3::untyped_result_set_row& row) {
    avro::OutputStreamPtr stream_key = avro::memoryOutputStream();
    avro::OutputStreamPtr stream_value = avro::memoryOutputStream();

    _key_encoder->init(*stream_key);
    _value_encoder->init(*stream_value);

    avro::GenericDatum key_datum(_key_compiled_schema);
    avro::GenericDatum value_datum(_value_compiled_schema);

    std::set<sstring> primary_key_columns;
    for (const column_definition& cdef : _scylla_schema->all_columns()) {
        if (cdef.is_primary_key()) {
            primary_key_columns.insert(cdef.name_as_text());
        }
    }

    if (key_datum.type() == avro::AVRO_RECORD) {
        avro::GenericRecord& keyRecord = key_datum.value<avro::GenericRecord>();

        for (auto& column : _scylla_schema->all_columns()) {
            auto name = column.name_as_text();

            if (primary_key_columns.count(name) > 0) {
                abstract_type::kind kind = column.type->get_kind();
                avro::GenericDatum& key_un = keyRecord.field(name);
                encode_union(key_un, row, name, kind);
            }
        }
    }

    value_datum.selectBranch(1);

    if (value_datum.type() == avro::AVRO_RECORD) {
        avro::GenericRecord& valueRecord = value_datum.value<avro::GenericRecord>();

        for (auto& column : _scylla_schema->all_columns()) {
            auto name = column.name_as_text();

            abstract_type::kind kind = column.type->get_kind();
            avro::GenericDatum& value_un = valueRecord.field(name);
            encode_union(value_un, row, name, kind);
        }
    }

    avro::encode(*_key_encoder, key_datum);
    _key_encoder->flush();

    avro::encode(*_value_encoder, value_datum);
    _value_encoder->flush();

    return {avro::snapshot(*stream_key), avro::snapshot(*stream_value)};
}

avro_row_serializer::encoded_row avro_row_serializer::serialize_wire_format(const cql3::untyped_result_set_row& row,
                                                                            uint32_t key_schema_id, uint32_t value_schema_id) {
    auto serialized_avro = serialize(row);
    
    key_schema_id = seastar::net::hton(key_schema_id);
    value_schema_id = seastar::net::hton(value_schema_id);

    auto key_schema_id_pointer = reinterpret_cast<const uint8_t*>(&key_schema_id);
    auto value_schema_id_pointer = reinterpret_cast<const uint8_t*>(&value_schema_id);

    // Prepend network-order (Confluent Platform Schema Registry) schema id.
    serialized_avro.key->insert(serialized_avro.key->begin(), key_schema_id_pointer, key_schema_id_pointer + sizeof(uint32_t));
    serialized_avro.value->insert(serialized_avro.value->begin(), value_schema_id_pointer, value_schema_id_pointer + sizeof(uint32_t));

    // Prepend magic byte (0).
    serialized_avro.key->insert(serialized_avro.key->begin(), 0);
    serialized_avro.value->insert(serialized_avro.value->begin(), 0);

    return serialized_avro;
}

} // namespace cdc:kafka_replication

