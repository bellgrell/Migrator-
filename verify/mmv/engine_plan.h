#pragma once

#include <string>
#include <vector>

#include "engine_common.h"

namespace mmv {
namespace vdetail {

KeyValue parse_key(const std::string& t, const NormSpec& s);
bool key_equal(const KeyValue& a, const KeyValue& b);
bool key_less(const KeyValue& a, const KeyValue& b);
bool tuple_equal(const std::vector<KeyValue>& a, const std::vector<KeyValue>& b);
bool tuple_less(const std::vector<KeyValue>& a, const std::vector<KeyValue>& b);

std::string key_select_list(const std::vector<ColPair>& keys, bool source, Dialect d);
std::string key_order_list(const std::vector<ColPair>& keys, bool source);

Status sample_boundaries(SqlBackend& db, const std::string& table_q,
                         const std::vector<ColPair>& keys, int64_t chunk_rows,
                         int64_t max_chunks, std::vector<std::vector<KeyValue>>& out,
                         const std::string& extra_where = "");

std::vector<TupleRange> ranges_from_bounds(const std::vector<std::vector<KeyValue>>& b);
std::vector<TupleRange> numeric_ranges(const std::vector<KeyRange>& ks);
bool split_numeric(const TupleRange& r, TupleRange& a, TupleRange& b);
Status split_sampled(SqlBackend& src, const std::string& table_q, const TupleRange& r,
                     const std::vector<ColPair>& keys, int64_t max_rows,
                     TupleRange& a, TupleRange& b, bool& ok);

Status bounds_of(SqlBackend& db, const std::string& table_q, const std::string& key_q,
                 Bounds& out);
Status table_stats(SqlBackend& db, const std::string& table, TableStats& out);
Status explain_uses_index(SqlBackend& db, const std::string& table_q,
                          const std::string& key_q, bool& uses,
                          const std::string& extra_where = "");
Status key_indexed(SqlBackend& db, const std::string& table,
                   const std::vector<std::string>& keys, bool& covered);

Status list_partitions(SqlBackend& db, const std::string& table, bool leaf,
                       std::vector<std::string>& out);

Status partition_rows(SqlBackend& db, const std::string& table_q,
                      const std::string& where, int64_t& rows);

Status list_partitions_level(SqlBackend& db, const std::string& table, int level,
                             std::vector<std::string>& out);
Status partition_depth(SqlBackend& db, const std::string& table, int& depth);

Status explain_uses_where(SqlBackend& db, const std::string& table_q,
                          const std::string& where, bool& uses);
Status first_key_where(SqlBackend& db, const std::string& table_q,
                       const std::vector<ColPair>& keys,
                       const std::string& partition_where, std::string& out);

Status sample_key_where(SqlBackend& db, const std::string& table_q,
                        const std::vector<ColPair>& keys,
                        const std::string& partition_where, int n,
                        std::vector<std::string>& out);
Status partition_method(SqlBackend& db, const std::string& table, std::string& out);

Status partition_bounds(SqlBackend& db, const std::string& table_q,
                        const std::string& key_q, const std::string& where, Bounds& out);

}  // namespace vdetail
}  // namespace mmv
