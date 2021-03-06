/*
 * Copyright (C) 2014 ScyllaDB
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

#include "mutation.hh"
#include "query-result-writer.hh"
#include "flat_mutation_reader.hh"

mutation::data::data(dht::decorated_key&& key, schema_ptr&& schema)
    : _schema(std::move(schema))
    , _dk(std::move(key))
    , _p(_schema)
{ }

mutation::data::data(partition_key&& key_, schema_ptr&& schema)
    : _schema(std::move(schema))
    , _dk(dht::global_partitioner().decorate_key(*_schema, std::move(key_)))
    , _p(_schema)
{ }

mutation::data::data(schema_ptr&& schema, dht::decorated_key&& key, const mutation_partition& mp)
    : _schema(std::move(schema))
    , _dk(std::move(key))
    , _p(mp)
{ }

mutation::data::data(schema_ptr&& schema, dht::decorated_key&& key, mutation_partition&& mp)
    : _schema(std::move(schema))
    , _dk(std::move(key))
    , _p(std::move(mp))
{ }

void mutation::set_static_cell(const column_definition& def, atomic_cell_or_collection&& value) {
    partition().static_row().apply(def, std::move(value));
}

void mutation::set_static_cell(const bytes& name, const data_value& value, api::timestamp_type timestamp, ttl_opt ttl) {
    auto column_def = schema()->get_column_definition(name);
    if (!column_def) {
        throw std::runtime_error(sprint("no column definition found for '%s'", name));
    }
    if (!column_def->is_static()) {
        throw std::runtime_error(sprint("column '%s' is not static", name));
    }
    partition().static_row().apply(*column_def, atomic_cell::make_live(timestamp, column_def->type->decompose(value), ttl));
}

void mutation::set_clustered_cell(const clustering_key& key, const bytes& name, const data_value& value,
        api::timestamp_type timestamp, ttl_opt ttl) {
    auto column_def = schema()->get_column_definition(name);
    if (!column_def) {
        throw std::runtime_error(sprint("no column definition found for '%s'", name));
    }
    return set_clustered_cell(key, *column_def, atomic_cell::make_live(timestamp, column_def->type->decompose(value), ttl));
}

void mutation::set_clustered_cell(const clustering_key& key, const column_definition& def, atomic_cell_or_collection&& value) {
    auto& row = partition().clustered_row(*schema(), key).cells();
    row.apply(def, std::move(value));
}

void mutation::set_cell(const clustering_key_prefix& prefix, const bytes& name, const data_value& value,
        api::timestamp_type timestamp, ttl_opt ttl) {
    auto column_def = schema()->get_column_definition(name);
    if (!column_def) {
        throw std::runtime_error(sprint("no column definition found for '%s'", name));
    }
    return set_cell(prefix, *column_def, atomic_cell::make_live(timestamp, column_def->type->decompose(value), ttl));
}

void mutation::set_cell(const clustering_key_prefix& prefix, const column_definition& def, atomic_cell_or_collection&& value) {
    if (def.is_static()) {
        set_static_cell(def, std::move(value));
    } else if (def.is_regular()) {
        set_clustered_cell(prefix, def, std::move(value));
    } else {
        throw std::runtime_error("attemting to store into a key cell");
    }
}

bool mutation::operator==(const mutation& m) const {
    return decorated_key().equal(*schema(), m.decorated_key())
           && partition().equal(*schema(), m.partition(), *m.schema());
}

bool mutation::operator!=(const mutation& m) const {
    return !(*this == m);
}

void
mutation::query(query::result::builder& builder,
    const query::partition_slice& slice,
    gc_clock::time_point now,
    uint32_t row_limit) &&
{
    auto pb = builder.add_partition(*schema(), key());
    auto is_reversed = slice.options.contains<query::partition_slice::option::reversed>();
    mutation_partition& p = partition();
    auto limit = std::min(row_limit, slice.partition_row_limit());
    p.compact_for_query(*schema(), now, slice.row_ranges(*schema(), key()), is_reversed, limit);
    p.query_compacted(pb, *schema(), limit);
}

query::result
mutation::query(const query::partition_slice& slice,
    query::result_request request,
    gc_clock::time_point now, uint32_t row_limit) &&
{
    query::result::builder builder(slice, request, { });
    std::move(*this).query(builder, slice, now, row_limit);
    return builder.build();
}

query::result
mutation::query(const query::partition_slice& slice,
    query::result_request request,
    gc_clock::time_point now, uint32_t row_limit) const&
{
    return mutation(*this).query(slice, request, now, row_limit);
}

size_t
mutation::live_row_count(gc_clock::time_point query_time) const {
    return partition().live_row_count(*schema(), query_time);
}

bool
mutation_decorated_key_less_comparator::operator()(const mutation& m1, const mutation& m2) const {
    return m1.decorated_key().less_compare(*m1.schema(), m2.decorated_key());
}

boost::iterator_range<std::vector<mutation>::const_iterator>
slice(const std::vector<mutation>& partitions, const dht::partition_range& r) {
    struct cmp {
        bool operator()(const dht::ring_position& pos, const mutation& m) const {
            return m.decorated_key().tri_compare(*m.schema(), pos) > 0;
        };
        bool operator()(const mutation& m, const dht::ring_position& pos) const {
            return m.decorated_key().tri_compare(*m.schema(), pos) < 0;
        };
    };

    return boost::make_iterator_range(
        r.start()
            ? (r.start()->is_inclusive()
                ? std::lower_bound(partitions.begin(), partitions.end(), r.start()->value(), cmp())
                : std::upper_bound(partitions.begin(), partitions.end(), r.start()->value(), cmp()))
            : partitions.cbegin(),
        r.end()
            ? (r.end()->is_inclusive()
              ? std::upper_bound(partitions.begin(), partitions.end(), r.end()->value(), cmp())
              : std::lower_bound(partitions.begin(), partitions.end(), r.end()->value(), cmp()))
            : partitions.cend());
}

void
mutation::upgrade(const schema_ptr& new_schema) {
    if (_ptr->_schema != new_schema) {
        schema_ptr s = new_schema;
        partition().upgrade(*schema(), *new_schema);
        _ptr->_schema = std::move(s);
    }
}

void mutation::apply(mutation&& m) {
    partition().apply(*schema(), std::move(m.partition()), *m.schema());
}

void mutation::apply(const mutation& m) {
    partition().apply(*schema(), m.partition(), *m.schema());
}

void mutation::apply(const mutation_fragment& mf) {
    partition().apply(*schema(), mf);
}

mutation& mutation::operator=(const mutation& m) {
    return *this = mutation(m);
}

mutation mutation::operator+(const mutation& other) const {
    auto m = *this;
    m.apply(other);
    return m;
}

mutation& mutation::operator+=(const mutation& other) {
    apply(other);
    return *this;
}

mutation& mutation::operator+=(mutation&& other) {
    apply(std::move(other));
    return *this;
}

mutation mutation::sliced(const query::clustering_row_ranges& ranges) const {
    return mutation(schema(), decorated_key(), partition().sliced(*schema(), ranges));
}

class mutation_rebuilder {
    mutation _m;
    size_t _remaining_limit;

public:
    mutation_rebuilder(dht::decorated_key dk, schema_ptr s)
        : _m(std::move(dk), std::move(s)), _remaining_limit(0) {
    }

    stop_iteration consume(tombstone t) {
        _m.partition().apply(t);
        return stop_iteration::no;
    }

    stop_iteration consume(range_tombstone&& rt) {
        _m.partition().apply_row_tombstone(*_m.schema(), std::move(rt));
        return stop_iteration::no;
    }

    stop_iteration consume(static_row&& sr) {
        _m.partition().static_row().apply(*_m.schema(), column_kind::static_column, std::move(sr.cells()));
        return stop_iteration::no;
    }

    stop_iteration consume(clustering_row&& cr) {
        auto& dr = _m.partition().clustered_row(*_m.schema(), std::move(cr.key()));
        dr.apply(cr.tomb());
        dr.apply(cr.marker());
        dr.cells().apply(*_m.schema(), column_kind::regular_column, std::move(cr.cells()));
        return stop_iteration::no;
    }

    mutation_opt consume_end_of_stream() {
        return mutation_opt(std::move(_m));
    }
};

future<mutation_opt> mutation_from_streamed_mutation(streamed_mutation_opt sm) {
    if (!sm) {
        return make_ready_future<mutation_opt>();
    }
    return do_with(std::move(*sm), [] (auto& sm) {
        return consume(sm, mutation_rebuilder(sm.decorated_key(), sm.schema()));
    });
}

future<mutation> mutation_from_streamed_mutation(streamed_mutation& sm) {
    return consume(sm, mutation_rebuilder(sm.decorated_key(), sm.schema())).then([] (mutation_opt&& mo) {
        return std::move(*mo);
    });
}

future<mutation_opt> read_mutation_from_flat_mutation_reader(schema_ptr s, flat_mutation_reader& r) {
    if (r.is_buffer_empty()) {
        if (r.is_end_of_stream()) {
            return make_ready_future<mutation_opt>();
        }
        return r.fill_buffer().then([&r, s = std::move(s)] {
            return read_mutation_from_flat_mutation_reader(std::move(s), r);
        });
    }
    // r.is_buffer_empty() is always false at this point
    struct adapter {
        schema_ptr _s;
        stdx::optional<mutation_rebuilder> _builder;
        adapter(schema_ptr s) : _s(std::move(s)) { }

        void consume_new_partition(const dht::decorated_key& dk) {
            _builder = mutation_rebuilder(dk, std::move(_s));
        }

        stop_iteration consume(tombstone t) {
            assert(_builder);
            return _builder->consume(t);
        }

        stop_iteration consume(range_tombstone&& rt) {
            assert(_builder);
            return _builder->consume(std::move(rt));
        }

        stop_iteration consume(static_row&& sr) {
            assert(_builder);
            return _builder->consume(std::move(sr));
        }

        stop_iteration consume(clustering_row&& cr) {
            assert(_builder);
            return _builder->consume(std::move(cr));
        }

        stop_iteration consume_end_of_partition() {
            assert(_builder);
            return stop_iteration::yes;
        }

        mutation_opt consume_end_of_stream() {
            if (!_builder) {
                return mutation_opt();
            }
            return _builder->consume_end_of_stream();
        }
    };
    return r.consume(adapter(std::move(s)));
}

std::ostream& operator<<(std::ostream& os, const mutation& m) {
    const ::schema& s = *m.schema();
    fprint(os, "{%s.%s %s ", s.ks_name(), s.cf_name(), m.decorated_key());
    os << m.partition() << "}";
    return os;
}
