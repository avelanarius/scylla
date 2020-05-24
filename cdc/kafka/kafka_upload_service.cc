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
#include "kafka_upload_service.hh"

namespace cdc::kafka {

std::vector<schema_ptr> get_tables_with_cdc_enabled() {
    return std::vector<schema_ptr>();
}

timeuuid do_kafka_replicate(schema_ptr table_schema, timeuuid last_seen) {
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
        _last_seen_row_key[entry] = do_kafka_replicate(table, last_seen);
    }
}

sstring kind_to_avro_type(abstract_type::kind kind) {
    switch (kind) {
        //TODO: Complex types + Check if all kinds are translated into appropriate avro types
        case abstract_type::kind::ascii:
            return sstring("string");
        case abstract_type::kind::boolean:
            return sstring("boolean");
        case abstract_type::kind::byte:
            return sstring("string");
        case abstract_type::kind::bytes:
            return sstring("string");
        case abstract_type::kind::counter:
            return sstring("long");
        case abstract_type::kind::date:
            return sstring("string");
        case abstract_type::kind::decimal:
            return sstring("float");
        case abstract_type::kind::double_kind:
            return sstring("double");
        case abstract_type::kind::duration:
            return sstring("string");
        case abstract_type::kind::empty:
            return sstring("string");
        case abstract_type::kind::float_kind:
            return sstring("float");
        case abstract_type::kind::inet:
            return sstring("string");
        case abstract_type::kind::int32:
            return sstring("int");
        case abstract_type::kind::list:
            return sstring("string");
        case abstract_type::kind::long_kind:
            return sstring("long");
        case abstract_type::kind::map:
            return sstring("string");
        case abstract_type::kind::reversed:
            return sstring("string");
        case abstract_type::kind::set:
            return sstring("string");
        case abstract_type::kind::short_kind:
            return sstring("int");
        case abstract_type::kind::simple_date:
            return sstring("string");
        case abstract_type::kind::time:
            return sstring("string");
        case abstract_type::kind::timestamp:
            return sstring("string");
        case abstract_type::kind::timeuuid:
            return sstring("string");
        case abstract_type::kind::tuple:
            return sstring("string");
        case abstract_type::kind::user:
            return sstring("string");
        case abstract_type::kind::utf8:
            return sstring("string");
        case abstract_type::kind::uuid:
            return sstring("string");
        case abstract_type::kind::varint:
            return sstring("string");
        default:
            return sstring("string");
    }
}

sstring compose_key_schema_for(schema_ptr schema){

    sstring key_schema, key_schema_fields;
    key_schema_fields = compose_avro_record_fields(schema->partition_key_columns());
    key_schema = compose_avro_schema("key_schema", schema->ks_name() + "." + schema->cf_name(),
                                     key_schema_fields);
    return key_schema;
}

sstring compose_value_schema_for(schema_ptr schema){

    sstring value_schema, value_schema_fields;
    value_schema_fields = compose_avro_record_fields(
            boost::make_iterator_range(schema->all_columns().begin(), schema->all_columns().end()));
    value_schema = compose_avro_schema("value_schema", schema->ks_name() + "." + schema->cf_name(),
                                       value_schema_fields);
    return value_schema;
}

sstring compose_avro_record_fields(schema::const_iterator_range_type column_range){
    sstring result = "";
    int n = 0;
    for(const column_definition& cdef : column_range){
        if (n++ != 0) {
            result += ",";
        }
        result += "{";
        result += "\"name\":\"" + cdef.name_as_text() + "\"";
        result += ",\"type\":[\"null\",\""  + kind_to_avro_type(cdef.type->get_kind()) + "\"]";
        result += "}";
    }
    return result;
}

sstring compose_avro_schema(sstring avro_name, sstring avro_namespace, sstring avro_fields) {
        sstring result = sstring("{"
                                 "\"type\":\"record\","
                                 "\"name\":\"" + avro_name + "\","
                                 "\"namespace\":\"" + avro_namespace + "\","
                                 "\"fields\":[" + avro_fields + "]"
                                 "}");
        return result;
 }

} // namespace cdc::kafka
