#include "graph-properties-convert.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <libxml/xmlreader.h>
#include <arrow/api.h>
#include <arrow/buffer.h>
#include <arrow/io/api.h>
#include <arrow/array/builder_binary.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include "galois/ErrorCode.h"
#include "galois/Galois.h"
#include "galois/Logging.h"
#include "galois/graphs/PropertyFileGraph.h"
#include "galois/ParallelSTL.h"
#include "galois/SharedMemSys.h"
#include "galois/Threads.h"

namespace {

using galois::ImportDataType;

using ArrayBuilders   = std::vector<std::shared_ptr<arrow::ArrayBuilder>>;
using StringBuilders  = std::vector<std::shared_ptr<arrow::StringBuilder>>;
using BooleanBuilders = std::vector<std::shared_ptr<arrow::BooleanBuilder>>;
using ChunkedArrays   = std::vector<std::shared_ptr<arrow::ChunkedArray>>;
using ArrowArrays     = std::vector<std::shared_ptr<arrow::Array>>;
using StringArrays    = std::vector<std::shared_ptr<arrow::StringArray>>;
using BooleanArrays   = std::vector<std::shared_ptr<arrow::BooleanArray>>;
using ArrowFields     = std::vector<std::shared_ptr<arrow::Field>>;
using NullMaps =
    std::pair<std::unordered_map<int, std::shared_ptr<arrow::Array>>,
              std::unordered_map<int, std::shared_ptr<arrow::Array>>>;

struct KeyGraphML {
  std::string id;
  bool for_node;
  bool for_edge;
  std::string name;
  ImportDataType type;
  bool is_list;

  KeyGraphML(const std::string& id_, bool for_node_, bool for_edge_,
             const std::string& name_, ImportDataType type_, bool is_list_)
      : id(id_), for_node(for_node_), for_edge(for_edge_), name(name_),
        type(type_), is_list(is_list_) {}
  KeyGraphML(const std::string& id, bool for_node)
      : KeyGraphML(id, for_node, !for_node, id, ImportDataType::kString,
                   false) {}
};

struct PropertiesState {
  std::unordered_map<std::string, size_t> keys;
  ArrowFields schema;
  ArrayBuilders builders;
  std::vector<ArrowArrays> chunks;
};

struct LabelsState {
  std::unordered_map<std::string, size_t> keys;
  ArrowFields schema;
  BooleanBuilders builders;
  std::vector<ArrowArrays> chunks;
};

struct TopologyState {
  // maps node IDs to node indexes
  std::unordered_map<std::string, size_t> node_indexes;
  // node's start of edge lists
  std::vector<uint64_t> out_indices;
  // edge list of destinations
  std::vector<uint32_t> out_dests;
  // list of sources of edges
  std::vector<uint32_t> sources;
  // list of destinations of edges
  std::vector<uint32_t> destinations;
};

struct GraphState {
  PropertiesState node_properties;
  PropertiesState edge_properties;
  LabelsState node_labels;
  LabelsState edge_types;
  TopologyState topology_builder;
  size_t nodes;
  size_t edges;
};

struct WriterProperties {
  NullMaps null_arrays;
  std::shared_ptr<arrow::Array> false_array;
  const size_t chunk_size;
};

/************************************/
/* Basic Building Utility Functions */
/************************************/

template <typename T>
std::shared_ptr<arrow::Array> BuildArray(std::shared_ptr<T> builder) {
  std::shared_ptr<arrow::Array> array;
  auto st = builder->Finish(&array);
  if (!st.ok()) {
    GALOIS_LOG_FATAL("Error building arrow array: {}", st.ToString());
  }
  return array;
}

ChunkedArrays BuildChunks(std::vector<ArrowArrays>* chunks) {
  ChunkedArrays chunked_arrays;
  chunked_arrays.resize(chunks->size());
  for (size_t n = 0; n < chunks->size(); n++) {
    chunked_arrays[n] = std::make_shared<arrow::ChunkedArray>(chunks->at(n));
  }
  return chunked_arrays;
}

std::shared_ptr<arrow::Table> BuildTable(std::vector<ArrowArrays>* chunks,
                                         ArrowFields* schema_vector) {
  ChunkedArrays columns = BuildChunks(chunks);

  auto schema = std::make_shared<arrow::Schema>(*schema_vector);
  return arrow::Table::Make(schema, columns);
}

/*************************************************************/
/* Utility functions for retrieving null arrays from the map */
/*************************************************************/

template <typename T>
std::shared_ptr<arrow::Array> FindNullArray(std::shared_ptr<T> builder,
                                            WriterProperties* properties) {
  auto type = builder->type()->id();
  std::shared_ptr<arrow::Array> null_array;
  if (type != arrow::Type::LIST) {
    null_array = properties->null_arrays.first.find(type)->second;
  } else {
    auto list_builder = std::static_pointer_cast<arrow::ListBuilder>(builder);
    null_array        = properties->null_arrays.second
                     .find(list_builder->value_builder()->type()->id())
                     ->second;
  }
  return null_array;
}

std::shared_ptr<arrow::Array> FindNullArray(std::shared_ptr<arrow::Array> array,
                                            WriterProperties* properties) {
  auto type = array->type()->id();
  std::shared_ptr<arrow::Array> null_array;
  if (type != arrow::Type::LIST) {
    null_array = properties->null_arrays.first.find(type)->second;
  } else {
    auto list_array = std::static_pointer_cast<arrow::ListArray>(array);
    null_array =
        properties->null_arrays.second.find(list_array->values()->type()->id())
            ->second;
  }
  return null_array;
}

/******************************************************/
/* Functions for finding basic statistics on datasets */
/******************************************************/

void WriteNullStats(const std::vector<ArrowArrays>& table,
                    WriterProperties* properties, size_t total) {
  if (table.size() == 0) {
    std::cout << "This table has no entries" << std::endl;
    return;
  }
  size_t null_constants  = 0;
  size_t non_null_values = 0;

  for (auto col : table) {
    auto null_array = FindNullArray(col[0], properties);
    for (auto chunk : col) {
      if (chunk == null_array) {
        null_constants++;
      } else {
        for (int64_t i = 0; i < chunk->length(); i++) {
          if (!chunk->IsNull(i)) {
            non_null_values++;
          }
        }
      }
    }
  }
  std::cout << "Total non-null Values in Table: " << non_null_values
            << std::endl;
  std::cout << "Total Values in Table: " << total * table.size() << std::endl;
  std::cout << "Value Ratio: "
            << ((double)non_null_values) / (total * table.size()) << std::endl;
  std::cout << "Total Null Chunks in table " << null_constants << std::endl;
  std::cout << "Total Chunks in Table: " << table[0].size() * table.size()
            << std::endl;
  std::cout << "Constant Ratio: "
            << ((double)null_constants) / (table[0].size() * table.size())
            << std::endl;
  std::cout << std::endl;
}

void WriteFalseStats(const std::vector<ArrowArrays>& table,
                     WriterProperties* properties, size_t total) {
  if (table.size() == 0) {
    std::cout << "This table has no entries" << std::endl;
    return;
  }
  size_t false_constants = 0;
  size_t true_values     = 0;

  for (auto col : table) {
    for (auto chunk : col) {
      if (chunk == properties->false_array) {
        false_constants++;
      } else {
        auto array = std::static_pointer_cast<arrow::BooleanArray>(chunk);
        for (int64_t i = 0; i < chunk->length(); i++) {
          if (array->Value(i)) {
            true_values++;
          }
        }
      }
    }
  }
  std::cout << "Total true Values in Table: " << true_values << std::endl;
  std::cout << "Total Values in Table: " << total * table.size() << std::endl;
  std::cout << "True Ratio: " << ((double)true_values) / (total * table.size())
            << std::endl;
  std::cout << "Total False Chunks in table " << false_constants << std::endl;
  std::cout << "Total Chunks in Table: " << table[0].size() * table.size()
            << std::endl;
  std::cout << "Constant Ratio: "
            << ((double)false_constants) / (table[0].size() * table.size())
            << std::endl;
  std::cout << std::endl;
}

/**************************************/
/* Functions for adding arrow columns */
/**************************************/

// Special case for building boolean builders where the empty value is false,
// not null
size_t AddFalseBuilder(const std::string& column, LabelsState* labels) {
  // add entry to map
  auto index = labels->keys.size();
  labels->keys.insert(std::pair<std::string, size_t>(column, index));

  // add column to schema, builders, and chunks
  labels->schema.push_back(arrow::field(column, arrow::boolean()));
  labels->builders.push_back(std::make_shared<arrow::BooleanBuilder>());
  labels->chunks.push_back(ArrowArrays{});

  return index;
}

// Special case for adding properties not forward declared as strings since we
// do not know their type
size_t AddStringBuilder(const std::string& column,
                        PropertiesState* properties) {
  // add entry to map
  auto index = properties->keys.size();
  properties->keys.insert(std::pair<std::string, size_t>(column, index));

  // add column to schema, builders, and chunks
  properties->schema.push_back(arrow::field(column, arrow::utf8()));
  properties->builders.push_back(std::make_shared<arrow::StringBuilder>());
  properties->chunks.push_back(ArrowArrays{});

  return index;
}

// Case for adding properties for which we know their type
void AddBuilder(PropertiesState* properties, KeyGraphML key) {
  if (!key.is_list) {
    switch (key.type) {
    case ImportDataType::kString: {
      properties->schema.push_back(arrow::field(key.name, arrow::utf8()));
      properties->builders.push_back(std::make_shared<arrow::StringBuilder>());
      break;
    }
    case ImportDataType::kInt64: {
      properties->schema.push_back(arrow::field(key.name, arrow::int64()));
      properties->builders.push_back(std::make_shared<arrow::Int64Builder>());
      break;
    }
    case ImportDataType::kInt32: {
      properties->schema.push_back(arrow::field(key.name, arrow::int32()));
      properties->builders.push_back(std::make_shared<arrow::Int32Builder>());
      break;
    }
    case ImportDataType::kDouble: {
      properties->schema.push_back(arrow::field(key.name, arrow::float64()));
      properties->builders.push_back(std::make_shared<arrow::DoubleBuilder>());
      break;
    }
    case ImportDataType::kFloat: {
      properties->schema.push_back(arrow::field(key.name, arrow::float32()));
      properties->builders.push_back(std::make_shared<arrow::FloatBuilder>());
      break;
    }
    case ImportDataType::kBoolean: {
      properties->schema.push_back(arrow::field(key.name, arrow::boolean()));
      properties->builders.push_back(std::make_shared<arrow::BooleanBuilder>());
      break;
    }
    default:
      // for now handle uncaught types as strings
      GALOIS_LOG_WARN("treating unknown type {} as string", key.type);
      properties->schema.push_back(arrow::field(key.name, arrow::utf8()));
      properties->builders.push_back(std::make_shared<arrow::StringBuilder>());
      break;
    }
  } else {
    auto* pool = arrow::default_memory_pool();
    switch (key.type) {
    case ImportDataType::kString: {
      properties->schema.push_back(
          arrow::field(key.name, arrow::list(arrow::utf8())));
      properties->builders.push_back(std::make_shared<arrow::ListBuilder>(
          pool, std::make_shared<arrow::StringBuilder>()));
      break;
    }
    case ImportDataType::kInt64: {
      properties->schema.push_back(
          arrow::field(key.name, arrow::list(arrow::int64())));
      properties->builders.push_back(std::make_shared<arrow::ListBuilder>(
          pool, std::make_shared<arrow::Int64Builder>()));
      break;
    }
    case ImportDataType::kInt32: {
      properties->schema.push_back(
          arrow::field(key.name, arrow::list(arrow::int32())));
      properties->builders.push_back(std::make_shared<arrow::ListBuilder>(
          pool, std::make_shared<arrow::Int32Builder>()));
      break;
    }
    case ImportDataType::kDouble: {
      properties->schema.push_back(
          arrow::field(key.name, arrow::list(arrow::float64())));
      properties->builders.push_back(std::make_shared<arrow::ListBuilder>(
          pool, std::make_shared<arrow::DoubleBuilder>()));
      break;
    }
    case ImportDataType::kFloat: {
      properties->schema.push_back(
          arrow::field(key.name, arrow::list(arrow::float32())));
      properties->builders.push_back(std::make_shared<arrow::ListBuilder>(
          pool, std::make_shared<arrow::FloatBuilder>()));
      break;
    }
    case ImportDataType::kBoolean: {
      properties->schema.push_back(
          arrow::field(key.name, arrow::list(arrow::boolean())));
      properties->builders.push_back(std::make_shared<arrow::ListBuilder>(
          pool, std::make_shared<arrow::BooleanBuilder>()));
      break;
    }
    default:
      // for now handle uncaught types as strings
      GALOIS_LOG_WARN("treating unknown array type {} as a string array",
                      key.type);
      properties->schema.push_back(
          arrow::field(key.name, arrow::list(arrow::utf8())));
      properties->builders.push_back(std::make_shared<arrow::ListBuilder>(
          pool, std::make_shared<arrow::StringBuilder>()));
      break;
    }
  }
  properties->chunks.push_back(ArrowArrays{});
  properties->keys.insert(
      std::pair<std::string, size_t>(key.name, properties->keys.size()));
}

/*************************************/
/* Functions for parsing string data */
/*************************************/

std::vector<std::string> ParseStringList(std::string raw_list) {
  std::vector<std::string> list;

  if (raw_list.size() >= 2 && raw_list.front() == '[' &&
      raw_list.back() == ']') {
    raw_list.erase(0, 1);
    raw_list.erase(raw_list.length() - 1, 1);
  } else {
    GALOIS_LOG_ERROR(
        "The provided list was not formatted like neo4j, returning string");
    list.push_back(raw_list);
    return list;
  }

  const char* char_list = raw_list.c_str();
  // parse the list
  for (size_t i = 0; i < raw_list.size();) {
    bool first_quote_found  = false;
    bool found_end_of_elem  = false;
    size_t start_of_elem    = i;
    int consecutive_slashes = 0;

    // parse the field
    for (; !found_end_of_elem && i < raw_list.size(); i++) {
      // if second quote not escaped then end of element reached
      if (char_list[i] == '\"') {
        if (consecutive_slashes % 2 == 0) {
          if (!first_quote_found) {
            first_quote_found = true;
            start_of_elem     = i + 1;
          } else if (first_quote_found) {
            found_end_of_elem = true;
          }
        }
        consecutive_slashes = 0;
      } else if (char_list[i] == '\\') {
        consecutive_slashes++;
      } else {
        consecutive_slashes = 0;
      }
    }
    size_t end_of_elem = i - 1;
    size_t elem_length = end_of_elem - start_of_elem;

    if (end_of_elem <= start_of_elem) {
      list.push_back("");
    } else {

      std::string elem_rough(&char_list[start_of_elem], elem_length);
      std::string elem("");
      elem.reserve(elem_rough.size());
      size_t curr_index = 0;
      size_t next_slash = elem_rough.find_first_of('\\');

      while (next_slash != std::string::npos) {
        elem.append(elem_rough.begin() + curr_index,
                    elem_rough.begin() + next_slash);

        switch (elem_rough[next_slash + 1]) {
        case 'n':
          elem.append("\n");
          break;
        case '\\':
          elem.append("\\");
          break;
        case 'r':
          elem.append("\r");
          break;
        case '0':
          elem.append("\0");
          break;
        case 'b':
          elem.append("\b");
          break;
        case '\'':
          elem.append("\'");
          break;
        case '\"':
          elem.append("\"");
          break;
        case 't':
          elem.append("\t");
          break;
        case 'f':
          elem.append("\f");
          break;
        case 'v':
          elem.append("\v");
          break;
        case '\xFF':
          elem.append("\xFF");
          break;
        default:
          GALOIS_LOG_WARN("Unhandled escape character: {}",
                          elem_rough[next_slash + 1]);
        }

        curr_index = next_slash + 2;
        next_slash = elem_rough.find_first_of('\\', curr_index);
      }
      elem.append(elem_rough.begin() + curr_index, elem_rough.end());

      list.push_back(elem);
    }
  }

  return list;
}

template <typename T>
std::vector<T> ParseNumberList(std::string raw_list) {
  std::vector<T> list;

  if (raw_list.front() == '[' && raw_list.back() == ']') {
    raw_list.erase(0, 1);
    raw_list.erase(raw_list.length() - 1, 1);
  } else {
    GALOIS_LOG_ERROR("The provided list was not formatted like neo4j, "
                     "returning empty vector");
    return list;
  }
  std::vector<std::string> elems;
  boost::split(elems, raw_list, boost::is_any_of(","));

  for (std::string s : elems) {
    list.push_back(boost::lexical_cast<T>(s));
  }
  return list;
}

std::vector<bool> ParseBooleanList(std::string raw_list) {
  std::vector<bool> list;

  if (raw_list.front() == '[' && raw_list.back() == ']') {
    raw_list.erase(0, 1);
    raw_list.erase(raw_list.length() - 1, 1);
  } else {
    GALOIS_LOG_ERROR("The provided list was not formatted like neo4j, "
                     "returning empty vector");
    return list;
  }
  std::vector<std::string> elems;
  boost::split(elems, raw_list, boost::is_any_of(","));

  for (std::string s : elems) {
    bool bool_val = s[0] == 't' || s[0] == 'T';
    list.push_back(bool_val);
  }
  return list;
}

/************************************************/
/* Functions for adding values to arrow builder */
/************************************************/

// Adds nulls to an array being built until its length == total
template <typename T>
void AddNulls(std::shared_ptr<T> builder, ArrowArrays* chunks,
              std::shared_ptr<arrow::Array> null_array,
              WriterProperties* properties, size_t total) {
  auto chunk_size   = properties->chunk_size;
  auto nulls_needed = total - (chunks->size() * chunk_size) - builder->length();
  arrow::Status st;

  // simplest case, no falses needed
  if (nulls_needed == 0) {
    return;
  }

  // case where nulls needed but mid-array
  if (builder->length() != 0) {
    auto nulls_to_add = std::min(chunk_size - builder->length(), nulls_needed);
    st                = builder->AppendNulls(nulls_to_add);
    nulls_needed -= nulls_to_add;

    // if we filled up a chunk, flush it
    if (((size_t)builder->length()) == chunk_size) {
      chunks->push_back(BuildArray(builder));
    } else {
      // if we did not fill up the array we know it must need no more nulls
      return;
    }
  }

  // case where we are at the start of a new array and have null_arrays to add
  for (auto i = chunk_size; i <= nulls_needed; i += chunk_size) {
    chunks->push_back(null_array);
  }
  nulls_needed %= chunk_size;

  // case where we are at the start of a new array and have less than a
  // null_array to add
  st = builder->AppendNulls(nulls_needed);

  if (!st.ok()) {
    GALOIS_LOG_FATAL("Error appending to an arrow array builder: {}",
                     st.ToString());
  }
}

// Adds nulls to an array being built until its length == total
template <typename T>
void AddNulls(std::shared_ptr<T> builder, ArrowArrays* chunks,
              WriterProperties* properties, size_t total) {
  auto nulls_needed =
      total - (chunks->size() * properties->chunk_size) - builder->length();
  if (nulls_needed == 0) {
    return;
  }
  auto null_array = FindNullArray(builder, properties);
  AddNulls(builder, chunks, null_array, properties, total);
}

// Adds falses to an array being built until its length == total
void AddFalses(std::shared_ptr<arrow::BooleanBuilder> builder,
               ArrowArrays* chunks, WriterProperties* properties,
               size_t total) {
  auto chunk_size = properties->chunk_size;
  auto falses_needed =
      total - (chunks->size() * chunk_size) - builder->length();
  arrow::Status st;

  // simplest case, no falses needed
  if (falses_needed == 0) {
    return;
  }

  // case where falses needed but mid-array
  if (builder->length() != 0) {
    auto falses_to_add =
        std::min(chunk_size - builder->length(), falses_needed);
    for (size_t i = 0; i < falses_to_add; i++) {
      st = builder->Append(false);
    }
    falses_needed -= falses_to_add;

    // if we filled up a chunk, flush it
    if (((size_t)builder->length()) == chunk_size) {
      chunks->push_back(BuildArray(builder));
    } else {
      // if we did not fill up the array we know it must need no more falses
      return;
    }
  }

  // case where we are at the start of a new array and have false_arrays to add
  for (auto i = chunk_size; i <= falses_needed; i += chunk_size) {
    chunks->push_back(properties->false_array);
  }
  falses_needed %= chunk_size;

  // case where we are at the start of a new array and have less than a
  // false_array to add
  for (size_t i = 0; i < falses_needed; i++) {
    st = builder->Append(false);
  }

  if (!st.ok()) {
    GALOIS_LOG_FATAL("Error appending to an arrow array builder: {}",
                     st.ToString());
  }
}

// Append an array to a builder
void AppendArray(std::shared_ptr<arrow::ListBuilder> list_builder,
                 const std::string& val) {
  arrow::Status st = arrow::Status::OK();

  switch (list_builder->value_builder()->type()->id()) {
  case arrow::Type::STRING: {
    auto sb = static_cast<arrow::StringBuilder*>(list_builder->value_builder());
    st      = list_builder->Append();
    auto sarray = ParseStringList(val);
    st          = sb->AppendValues(sarray);
    break;
  }
  case arrow::Type::INT64: {
    auto lb = static_cast<arrow::Int64Builder*>(list_builder->value_builder());
    st      = list_builder->Append();
    auto larray = ParseNumberList<int64_t>(val);
    st          = lb->AppendValues(larray);
    break;
  }
  case arrow::Type::INT32: {
    auto ib = static_cast<arrow::Int32Builder*>(list_builder->value_builder());
    st      = list_builder->Append();
    auto iarray = ParseNumberList<int32_t>(val);
    st          = ib->AppendValues(iarray);
    break;
  }
  case arrow::Type::DOUBLE: {
    auto db = static_cast<arrow::DoubleBuilder*>(list_builder->value_builder());
    st      = list_builder->Append();
    auto darray = ParseNumberList<double>(val);
    st          = db->AppendValues(darray);
    break;
  }
  case arrow::Type::FLOAT: {
    auto fb = static_cast<arrow::FloatBuilder*>(list_builder->value_builder());
    st      = list_builder->Append();
    auto farray = ParseNumberList<float>(val);
    st          = fb->AppendValues(farray);
    break;
  }
  case arrow::Type::BOOL: {
    auto bb =
        static_cast<arrow::BooleanBuilder*>(list_builder->value_builder());
    st          = list_builder->Append();
    auto barray = ParseBooleanList(val);
    st          = bb->AppendValues(barray);
    break;
  }
  default: {
    break;
  }
  }
  if (!st.ok()) {
    GALOIS_LOG_FATAL("Error adding value to arrow list array builder: {}",
                     st.ToString());
  }
}

// Append a non-null value to an array
void AppendValue(std::shared_ptr<arrow::ArrayBuilder> array,
                 const std::string& val) {
  arrow::Status st = arrow::Status::OK();

  switch (array->type()->id()) {
  case arrow::Type::STRING: {
    auto sb = std::static_pointer_cast<arrow::StringBuilder>(array);
    st      = sb->Append(val.c_str(), val.length());
    break;
  }
  case arrow::Type::INT64: {
    auto lb = std::static_pointer_cast<arrow::Int64Builder>(array);
    st      = lb->Append(boost::lexical_cast<int64_t>(val));
    break;
  }
  case arrow::Type::INT32: {
    auto ib = std::static_pointer_cast<arrow::Int32Builder>(array);
    st      = ib->Append(boost::lexical_cast<int32_t>(val));
    break;
  }
  case arrow::Type::DOUBLE: {
    auto db = std::static_pointer_cast<arrow::DoubleBuilder>(array);
    st      = db->Append(boost::lexical_cast<double>(val));
    break;
  }
  case arrow::Type::FLOAT: {
    auto fb = std::static_pointer_cast<arrow::FloatBuilder>(array);
    st      = fb->Append(boost::lexical_cast<float>(val));
    break;
  }
  case arrow::Type::BOOL: {
    auto bb       = std::static_pointer_cast<arrow::BooleanBuilder>(array);
    bool bool_val = val[0] == 't' || val[0] == 'T';
    st            = bb->Append(bool_val);
    break;
  }
  case arrow::Type::LIST: {
    auto lb = std::static_pointer_cast<arrow::ListBuilder>(array);
    AppendArray(lb, val);
    break;
  }
  default: {
    break;
  }
  }
  if (!st.ok()) {
    GALOIS_LOG_FATAL(
        "Error adding value to arrow array builder: {}, parquet error: {}", val,
        st.ToString());
  }
}

// Add nulls until the array is even and then append val so that length = total
// + 1 at the end
void AddValue(const std::string& val,
              std::shared_ptr<arrow::ArrayBuilder> builder, ArrowArrays* chunks,
              WriterProperties* properties, size_t total) {
  AddNulls(builder, chunks, properties, total);
  AppendValue(builder, val);

  // if we filled up a chunk, flush it
  if (((size_t)builder->length()) == properties->chunk_size) {
    chunks->push_back(BuildArray(builder));
  }
}

// Add nulls until the array is even and then append val so that length = total
// + 1 at the end
template <typename T, typename W>
void AddTypedValue(const W& val, std::shared_ptr<T> builder,
                   ArrowArrays* chunks,
                   std::shared_ptr<arrow::Array> null_array,
                   WriterProperties* properties, size_t total) {
  AddNulls(builder, chunks, null_array, properties, total);
  auto st = builder->Append(val);
  if (!st.ok()) {
    GALOIS_LOG_FATAL("Error appending to an arrow array builder: {}",
                     st.ToString());
  }

  // if we filled up a chunk, flush it
  if (((size_t)builder->length()) == properties->chunk_size) {
    chunks->push_back(BuildArray(builder));
  }
}

// Add nulls until the array is even and then append a list so that length =
// total + 1 at the end
template <typename T, typename W>
void AddArray(const std::shared_ptr<arrow::ListArray>& list_vals,
              const std::shared_ptr<W>& vals, size_t index,
              std::shared_ptr<arrow::ListBuilder> list_builder, T* type_builder,
              ArrowArrays* chunks, std::shared_ptr<arrow::Array> null_array,
              WriterProperties* properties, size_t total) {
  AddNulls(list_builder, chunks, null_array, properties, total);

  int32_t start = list_vals->value_offset(index);
  int32_t end   = list_vals->value_offset(index + 1);

  auto st = list_builder->Append();
  for (int32_t s = start; s < end; s++) {
    st = type_builder->Append(vals->Value(s));
    if (!st.ok()) {
      GALOIS_LOG_FATAL("Error appending value to an arrow array builder: {}",
                       st.ToString());
    }
  }
  // if we filled up a chunk, flush it
  if (((size_t)list_builder->length()) == properties->chunk_size) {
    chunks->push_back(BuildArray(list_builder));
  }
}

// Add nulls until the array is even and then append a list so that length =
// total + 1 at the end
void AddArray(const std::shared_ptr<arrow::ListArray>& list_vals,
              const std::shared_ptr<arrow::StringArray>& vals, size_t index,
              std::shared_ptr<arrow::ListBuilder> list_builder,
              arrow::StringBuilder* type_builder, ArrowArrays* chunks,
              std::shared_ptr<arrow::Array> null_array,
              WriterProperties* properties, size_t total) {
  AddNulls(list_builder, chunks, null_array, properties, total);

  int32_t start = list_vals->value_offset(index);
  int32_t end   = list_vals->value_offset(index + 1);

  auto st = list_builder->Append();
  for (int32_t s = start; s < end; s++) {
    st = type_builder->Append(vals->GetView(s));
    if (!st.ok()) {
      GALOIS_LOG_FATAL("Error appending value to an arrow array builder: {}",
                       st.ToString());
    }
  }
  // if we filled up a chunk, flush it
  if (((size_t)list_builder->length()) == properties->chunk_size) {
    chunks->push_back(BuildArray(list_builder));
  }
}

// Add falses until the array is even and then append true so that length =
// total + 1 at the end
void AddLabel(std::shared_ptr<arrow::BooleanBuilder> builder,
              ArrowArrays* chunks, WriterProperties* properties, size_t total) {
  AddFalses(builder, chunks, properties, total);
  auto st = builder->Append(true);
  if (!st.ok()) {
    GALOIS_LOG_FATAL("Error appending to an arrow array builder: {}",
                     st.ToString());
  }

  // if we filled up a chunk, flush it
  if (((size_t)builder->length()) == properties->chunk_size) {
    chunks->push_back(BuildArray(builder));
  }
}

/******************************************************************************/
/* Functions for ensuring all arrow arrays are of the right length in the end */
/******************************************************************************/

// Used to build the out_dests component of the CSR representation
uint64_t SetEdgeId(TopologyState* topology_builder,
                   std::vector<uint64_t>* offsets, size_t index) {
  uint32_t src  = topology_builder->sources[index];
  uint64_t base = src ? topology_builder->out_indices[src - 1] : 0;
  uint64_t i    = base + offsets->at(src)++;

  topology_builder->out_dests[i] = topology_builder->destinations[index];
  return i;
}

// Adds nulls to the array until its length == total
template <typename T>
void EvenOutArray(ArrowArrays* chunks, std::shared_ptr<T> builder,
                  std::shared_ptr<arrow::Array> null_array,
                  WriterProperties* properties, size_t total) {
  AddNulls(builder, chunks, null_array, properties, total);

  if (total % properties->chunk_size != 0) {
    chunks->push_back(BuildArray(builder));
  }
}

// Adds falses to the array until its length == total
void EvenOutArray(ArrowArrays* chunks,
                  std::shared_ptr<arrow::BooleanBuilder> builder,
                  WriterProperties* properties, size_t total) {
  AddFalses(builder, chunks, properties, total);

  if (total % properties->chunk_size != 0) {
    chunks->push_back(BuildArray(builder));
  }
}

// Adds nulls to the arrays until each length == total
void EvenOutChunkBuilders(ArrayBuilders* builders,
                          std::vector<ArrowArrays>* chunks,
                          WriterProperties* properties, size_t total) {
  galois::do_all(galois::iterate((size_t)0, builders->size()),
                 [&](const size_t& i) {
                   AddNulls(builders->at(i), &chunks->at(i), properties, total);

                   if (total % properties->chunk_size != 0) {
                     chunks->at(i).push_back(BuildArray(builders->at(i)));
                   }
                 });
}

// Adds falses to the arrays until each length == total
void EvenOutChunkBuilders(BooleanBuilders* builders,
                          std::vector<ArrowArrays>* chunks,
                          WriterProperties* properties, size_t total) {
  galois::do_all(
      galois::iterate((size_t)0, builders->size()), [&](const size_t& i) {
        AddFalses(builders->at(i), &chunks->at(i), properties, total);

        if (total % properties->chunk_size != 0) {
          chunks->at(i).push_back(BuildArray(builders->at(i)));
        }
      });
}

/**************************************************/
/* Functions for reordering edges into CSR format */
/**************************************************/

// Rearrange an array's entries to match up with those of mapping
template <typename T, typename W>
ArrowArrays
RearrangeArray(std::shared_ptr<T> builder,
               const std::shared_ptr<arrow::ChunkedArray>& chunked_array,
               const std::vector<size_t>& mapping,
               WriterProperties* properties) {
  auto chunk_size = properties->chunk_size;
  ArrowArrays chunks;
  auto st = builder->Reserve(chunk_size);
  if (!st.ok()) {
    GALOIS_LOG_FATAL("Error reserving space for arrow array: {}",
                     st.ToString());
  }
  // cast and store array chunks for use in loop
  std::vector<std::shared_ptr<W>> arrays;
  for (auto chunk : chunked_array->chunks()) {
    arrays.push_back(std::static_pointer_cast<W>(chunk));
  }
  std::shared_ptr<arrow::Array> null_array =
      properties->null_arrays.first.find(builder->type()->id())->second;

  // add non-null values
  for (size_t i = 0; i < mapping.size(); i++) {
    auto array = arrays[mapping[i] / chunk_size];

    if (!array->IsNull(mapping[i] % chunk_size)) {
      auto val = array->Value(mapping[i] % chunk_size);
      AddTypedValue(val, builder, &chunks, null_array, properties, i);
    }
  }
  EvenOutArray(&chunks, builder, null_array, properties, mapping.size());
  return chunks;
}

// Rearrange an array's entries to match up with those of mapping
ArrowArrays
RearrangeArray(std::shared_ptr<arrow::StringBuilder> builder,
               const std::shared_ptr<arrow::ChunkedArray>& chunked_array,
               const std::vector<size_t>& mapping,
               WriterProperties* properties) {
  auto chunk_size = properties->chunk_size;
  ArrowArrays chunks;
  auto st = builder->Reserve(chunk_size);
  if (!st.ok()) {
    GALOIS_LOG_FATAL("Error reserving space for arrow array: {}",
                     st.ToString());
  }
  // cast and store array chunks for use in loop
  std::vector<std::shared_ptr<arrow::StringArray>> arrays;
  for (auto chunk : chunked_array->chunks()) {
    arrays.push_back(std::static_pointer_cast<arrow::StringArray>(chunk));
  }
  std::shared_ptr<arrow::Array> null_array =
      properties->null_arrays.first.find(builder->type()->id())->second;

  // add non-null values
  for (size_t i = 0; i < mapping.size(); i++) {
    auto array = arrays[mapping[i] / chunk_size];

    if (!array->IsNull(mapping[i] % chunk_size)) {
      auto val = array->GetView(mapping[i] % chunk_size);
      AddTypedValue(val, builder, &chunks, null_array, properties, i);
    }
  }
  EvenOutArray(&chunks, builder, null_array, properties, mapping.size());
  return chunks;
}

// Rearrange an array's entries to match up with those of mapping, for labels
ArrowArrays
RearrangeArray(std::shared_ptr<arrow::BooleanBuilder> builder,
               const std::shared_ptr<arrow::ChunkedArray>& chunked_array,
               const std::vector<size_t>& mapping,
               WriterProperties* properties) {
  auto chunk_size = properties->chunk_size;
  ArrowArrays chunks;
  auto st = builder->Reserve(chunk_size);
  if (!st.ok()) {
    GALOIS_LOG_FATAL("Error reserving space for arrow array: {}",
                     st.ToString());
  }
  // cast and store array chunks for use in loop
  std::vector<std::shared_ptr<arrow::BooleanArray>> arrays;
  for (auto chunk : chunked_array->chunks()) {
    arrays.push_back(std::static_pointer_cast<arrow::BooleanArray>(chunk));
  }

  // add non-null values
  for (size_t i = 0; i < mapping.size(); i++) {
    auto val = arrays[mapping[i] / chunk_size]->Value(mapping[i] % chunk_size);
    if (val) {
      AddLabel(builder, &chunks, properties, i);
    }
  }
  EvenOutArray(&chunks, builder, properties, mapping.size());
  return chunks;
}

// Rearrange a list array's entries to match up with those of mapping
template <typename T, typename W>
ArrowArrays RearrangeArray(
    const std::shared_ptr<arrow::ListBuilder>& builder, T* type_builder,
    const std::shared_ptr<arrow::ChunkedArray>& chunked_array,
    const std::vector<size_t>& mapping, WriterProperties* properties) {
  auto chunk_size = properties->chunk_size;
  ArrowArrays chunks;
  auto st = builder->Reserve(chunk_size);
  if (!st.ok()) {
    GALOIS_LOG_FATAL("Error reserving space for arrow array: {}",
                     st.ToString());
  }
  // cast and store array chunks for use in loop
  std::vector<std::shared_ptr<arrow::ListArray>> list_arrays;
  std::vector<std::shared_ptr<W>> sub_arrays;
  for (auto chunk : chunked_array->chunks()) {
    auto array_temp = std::static_pointer_cast<arrow::ListArray>(chunk);
    list_arrays.push_back(array_temp);
    sub_arrays.push_back(std::static_pointer_cast<W>(array_temp->values()));
  }
  std::shared_ptr<arrow::Array> null_array =
      properties->null_arrays.second.find(type_builder->type()->id())->second;

  // add values
  for (size_t i = 0; i < mapping.size(); i++) {
    auto list_array = list_arrays[mapping[i] / chunk_size];
    auto sub_array  = sub_arrays[mapping[i] / chunk_size];
    auto index      = mapping[i] % chunk_size;
    if (!list_array->IsNull(index)) {
      AddArray(list_array, sub_array, index, builder, type_builder, &chunks,
               null_array, properties, i);
    }
  }
  EvenOutArray(&chunks, builder, null_array, properties, mapping.size());
  return chunks;
}

// Rearrange a list array's entries to match up with those of mapping
ArrowArrays RearrangeListArray(
    const std::shared_ptr<arrow::ChunkedArray>& list_chunked_array,
    const std::vector<size_t>& mapping, WriterProperties* properties) {
  auto* pool = arrow::default_memory_pool();
  ArrowArrays chunks;
  auto list_type =
      std::static_pointer_cast<arrow::BaseListType>(list_chunked_array->type())
          ->value_type()
          ->id();

  switch (list_type) {
  case arrow::Type::STRING: {
    auto builder = std::make_shared<arrow::ListBuilder>(
        pool, std::make_shared<arrow::StringBuilder>());
    auto sb = static_cast<arrow::StringBuilder*>(builder->value_builder());
    chunks  = RearrangeArray<arrow::StringBuilder, arrow::StringArray>(
        builder, sb, list_chunked_array, mapping, properties);
    break;
  }
  case arrow::Type::INT64: {
    auto builder = std::make_shared<arrow::ListBuilder>(
        pool, std::make_shared<arrow::Int64Builder>());
    auto lb = static_cast<arrow::Int64Builder*>(builder->value_builder());
    chunks  = RearrangeArray<arrow::Int64Builder, arrow::Int64Array>(
        builder, lb, list_chunked_array, mapping, properties);
    break;
  }
  case arrow::Type::INT32: {
    auto builder = std::make_shared<arrow::ListBuilder>(
        pool, std::make_shared<arrow::Int32Builder>());
    auto ib = static_cast<arrow::Int32Builder*>(builder->value_builder());
    chunks  = RearrangeArray<arrow::Int32Builder, arrow::Int32Array>(
        builder, ib, list_chunked_array, mapping, properties);
    break;
  }
  case arrow::Type::DOUBLE: {
    auto builder = std::make_shared<arrow::ListBuilder>(
        pool, std::make_shared<arrow::DoubleBuilder>());
    auto db = static_cast<arrow::DoubleBuilder*>(builder->value_builder());
    chunks  = RearrangeArray<arrow::DoubleBuilder, arrow::DoubleArray>(
        builder, db, list_chunked_array, mapping, properties);
    break;
  }
  case arrow::Type::FLOAT: {
    auto builder = std::make_shared<arrow::ListBuilder>(
        pool, std::make_shared<arrow::FloatBuilder>());
    auto fb = static_cast<arrow::FloatBuilder*>(builder->value_builder());
    chunks  = RearrangeArray<arrow::FloatBuilder, arrow::FloatArray>(
        builder, fb, list_chunked_array, mapping, properties);
    break;
  }
  case arrow::Type::BOOL: {
    auto builder = std::make_shared<arrow::ListBuilder>(
        pool, std::make_shared<arrow::BooleanBuilder>());
    auto bb = static_cast<arrow::BooleanBuilder*>(builder->value_builder());
    chunks  = RearrangeArray<arrow::BooleanBuilder, arrow::BooleanArray>(
        builder, bb, list_chunked_array, mapping, properties);
    break;
  }
  default: {
    GALOIS_LOG_FATAL(
        "Unsupported arrow array type passed to RearrangeListArray: {}",
        list_type);
  }
  }
  return chunks;
}

// Rearrange each column in a table so that their entries match up with those of
// mapping
std::vector<ArrowArrays> RearrangeTable(const ChunkedArrays& initial,
                                        const std::vector<size_t>& mapping,
                                        WriterProperties* properties) {
  std::vector<ArrowArrays> rearranged;
  rearranged.resize(initial.size());

  galois::do_all(
      galois::iterate((size_t)0, initial.size()), [&](const size_t& n) {
        auto array     = initial[n];
        auto arrayType = array->type()->id();
        ArrowArrays ca;

        switch (arrayType) {
        case arrow::Type::STRING: {
          auto sb = std::make_shared<arrow::StringBuilder>();
          ca      = RearrangeArray(sb, array, mapping, properties);
          break;
        }
        case arrow::Type::INT64: {
          auto lb = std::make_shared<arrow::Int64Builder>();
          ca      = RearrangeArray<arrow::Int64Builder, arrow::Int64Array>(
              lb, array, mapping, properties);
          break;
        }
        case arrow::Type::INT32: {
          auto ib = std::make_shared<arrow::Int32Builder>();
          ca      = RearrangeArray<arrow::Int32Builder, arrow::Int32Array>(
              ib, array, mapping, properties);
          break;
        }
        case arrow::Type::DOUBLE: {
          auto db = std::make_shared<arrow::DoubleBuilder>();
          ca      = RearrangeArray<arrow::DoubleBuilder, arrow::DoubleArray>(
              db, array, mapping, properties);
          break;
        }
        case arrow::Type::FLOAT: {
          auto fb = std::make_shared<arrow::FloatBuilder>();
          ca      = RearrangeArray<arrow::FloatBuilder, arrow::FloatArray>(
              fb, array, mapping, properties);
          break;
        }
        case arrow::Type::BOOL: {
          auto bb = std::make_shared<arrow::BooleanBuilder>();
          ca      = RearrangeArray<arrow::BooleanBuilder, arrow::BooleanArray>(
              bb, array, mapping, properties);
          break;
        }
        case arrow::Type::LIST: {
          ca = RearrangeListArray(array, mapping, properties);
          break;
        }
        default: {
          GALOIS_LOG_FATAL(
              "Unsupported arrow array type passed to RearrangeTable: {}",
              arrayType);
        }
        }
        rearranged[n] = ca;
        array.reset();
      });
  return rearranged;
}

// Rearrange each column in a table so that their entries match up with those of
// mapping, for labels
std::vector<ArrowArrays> RearrangeTypeTable(const ChunkedArrays& initial,
                                            const std::vector<size_t>& mapping,
                                            WriterProperties* properties) {
  std::vector<ArrowArrays> rearranged;
  rearranged.resize(initial.size());

  galois::do_all(galois::iterate((size_t)0, initial.size()),
                 [&](const size_t& n) {
                   auto array = initial[n];

                   auto bb = std::make_shared<arrow::BooleanBuilder>();
                   auto ca = RearrangeArray(bb, array, mapping, properties);
                   rearranged[n] = ca;

                   array.reset();
                 });
  return rearranged;
}

// Build CSR format and rearrange edge tables to correspond to the CSR
std::pair<std::shared_ptr<arrow::Table>, std::shared_ptr<arrow::Table>>
BuildFinalEdges(GraphState* builder, WriterProperties* properties) {
  galois::ParallelSTL::partial_sum(
      builder->topology_builder.out_indices.begin(),
      builder->topology_builder.out_indices.end(),
      builder->topology_builder.out_indices.begin());

  std::vector<size_t> edge_mapping;
  edge_mapping.resize(builder->edges, std::numeric_limits<uint64_t>::max());

  std::vector<uint64_t> offsets;
  offsets.resize(builder->nodes, 0);

  // get edge indices
  for (size_t i = 0; i < builder->topology_builder.sources.size(); i++) {
    uint64_t edgeId = SetEdgeId(&builder->topology_builder, &offsets, i);
    // edge_mapping[i] = edgeId;
    edge_mapping[edgeId] = i;
  }

  auto initial_edges = BuildChunks(&builder->edge_properties.chunks);
  auto initial_types = BuildChunks(&builder->edge_types.chunks);

  auto finalEdgeBuilders =
      RearrangeTable(initial_edges, edge_mapping, properties);
  auto finalTypeBuilders =
      RearrangeTypeTable(initial_types, edge_mapping, properties);

  std::cout << "Edge Properties Post:" << std::endl;
  WriteNullStats(finalEdgeBuilders, properties, builder->edges);
  std::cout << "Edge Types Post:" << std::endl;
  WriteFalseStats(finalTypeBuilders, properties, builder->edges);

  return std::pair<std::shared_ptr<arrow::Table>,
                   std::shared_ptr<arrow::Table>>(
      BuildTable(&finalEdgeBuilders, &builder->edge_properties.schema),
      BuildTable(&finalTypeBuilders, &builder->edge_types.schema));
}

/********************************************************************/
/* Helper functions for building initial null arrow array constants */
/********************************************************************/

template <typename T>
void AddNullArrays(
    std::unordered_map<int, std::shared_ptr<arrow::Array>>* null_map,
    std::unordered_map<int, std::shared_ptr<arrow::Array>>* lists_null_map,
    size_t elts) {
  auto* pool = arrow::default_memory_pool();

  // the builder types are still added for the list types since the list type is
  // extraneous info
  auto builder = std::make_shared<T>();
  auto st      = builder->AppendNulls(elts);
  null_map->insert(std::pair<int, std::shared_ptr<arrow::Array>>(
      builder->type()->id(), BuildArray(builder)));

  auto list_builder =
      std::make_shared<arrow::ListBuilder>(pool, std::make_shared<T>());
  st = list_builder->AppendNulls(elts);
  lists_null_map->insert(std::pair<int, std::shared_ptr<arrow::Array>>(
      builder->type()->id(), BuildArray(list_builder)));

  if (!st.ok()) {
    GALOIS_LOG_FATAL("Error creating null builders: {}", st.ToString());
  }
}

NullMaps GetNullArrays(size_t elts) {
  std::unordered_map<int, std::shared_ptr<arrow::Array>> null_map;
  std::unordered_map<int, std::shared_ptr<arrow::Array>> lists_null_map;

  AddNullArrays<arrow::StringBuilder>(&null_map, &lists_null_map, elts);
  AddNullArrays<arrow::Int32Builder>(&null_map, &lists_null_map, elts);
  AddNullArrays<arrow::Int64Builder>(&null_map, &lists_null_map, elts);
  AddNullArrays<arrow::FloatBuilder>(&null_map, &lists_null_map, elts);
  AddNullArrays<arrow::DoubleBuilder>(&null_map, &lists_null_map, elts);
  AddNullArrays<arrow::BooleanBuilder>(&null_map, &lists_null_map, elts);

  return NullMaps(std::move(null_map), std::move(lists_null_map));
}

std::shared_ptr<arrow::Array> GetFalseArray(size_t elts) {
  auto builder = std::make_shared<arrow::BooleanBuilder>();
  arrow::Status st;
  for (size_t i = 0; i < elts; i++) {
    st = builder->Append(false);
  }
  if (!st.ok()) {
    GALOIS_LOG_FATAL("Error appending to an arrow array builder: {}",
                     st.ToString());
  }
  return BuildArray(builder);
}

/***************************************/
/* Functions for parsing GraphML files */
/***************************************/

// extract the type from an attr.type or attr.list attribute from a key element
ImportDataType ExtractTypeGraphML(xmlChar* value) {
  ImportDataType type = ImportDataType::kString;
  if (xmlStrEqual(value, BAD_CAST "string")) {
    type = ImportDataType::kString;
  } else if (xmlStrEqual(value, BAD_CAST "long")) {
    type = ImportDataType::kInt64;
  } else if (xmlStrEqual(value, BAD_CAST "int")) {
    type = ImportDataType::kInt32;
  } else if (xmlStrEqual(value, BAD_CAST "double")) {
    type = ImportDataType::kDouble;
  } else if (xmlStrEqual(value, BAD_CAST "float")) {
    type = ImportDataType::kFloat;
  } else if (xmlStrEqual(value, BAD_CAST "boolean")) {
    type = ImportDataType::kBoolean;
  } else {
    GALOIS_LOG_ERROR("Came across attr.type: {}, that is not supported",
                     std::string((const char*)value));
    type = ImportDataType::kString;
  }
  return type;
}

/*
 * reader should be pointing to key node elt before calling
 *
 * extracts key attribute information for use later
 */
KeyGraphML ProcessKey(xmlTextReaderPtr reader) {
  int ret = xmlTextReaderMoveToNextAttribute(reader);
  xmlChar *name, *value;

  std::string id;
  bool for_node = false;
  bool for_edge = false;
  std::string attr_name;
  ImportDataType type = ImportDataType::kString;
  bool is_list        = false;

  while (ret == 1) {
    name  = xmlTextReaderName(reader);
    value = xmlTextReaderValue(reader);
    if (name != NULL) {
      if (xmlStrEqual(name, BAD_CAST "id")) {
        id = std::string((const char*)value);
      } else if (xmlStrEqual(name, BAD_CAST "for")) {
        for_node = xmlStrEqual(value, BAD_CAST "node") == 1;
        for_edge = xmlStrEqual(value, BAD_CAST "edge") == 1;
      } else if (xmlStrEqual(name, BAD_CAST "attr.name")) {
        attr_name = std::string((const char*)value);
      } else if (xmlStrEqual(name, BAD_CAST "attr.type")) {
        // do this check for neo4j
        if (!is_list) {
          type = ExtractTypeGraphML(value);
        }
      } else if (xmlStrEqual(name, BAD_CAST "attr.list")) {
        is_list = true;
        type    = ExtractTypeGraphML(value);
      } else {
        GALOIS_LOG_ERROR("Attribute on key: {}, was not recognized",
                         std::string((const char*)name));
      }
    }

    xmlFree(name);
    xmlFree(value);
    ret = xmlTextReaderMoveToNextAttribute(reader);
  }
  return KeyGraphML{
      id, for_node, for_edge, attr_name, type, is_list,
  };
}

/*
 * reader should be pointing at the data element before calling
 *
 * parses data from a GraphML file into property: pair<string, string>
 */
std::pair<std::string, std::string> ProcessData(xmlTextReaderPtr reader) {
  auto minimum_depth = xmlTextReaderDepth(reader);

  int ret = xmlTextReaderMoveToNextAttribute(reader);
  xmlChar *name, *value;

  std::string key;
  std::string propertyData;

  // parse node attributes for key (required)
  while (ret == 1) {
    name  = xmlTextReaderName(reader);
    value = xmlTextReaderValue(reader);
    if (name != NULL) {
      if (xmlStrEqual(name, BAD_CAST "key")) {
        key = std::string((const char*)value);
      } else {
        GALOIS_LOG_ERROR("Attribute on node: {}, was not recognized",
                         std::string((const char*)name));
      }
    }

    xmlFree(name);
    xmlFree(value);
    ret = xmlTextReaderMoveToNextAttribute(reader);
  }

  // parse xml text nodes for property data
  ret = xmlTextReaderRead(reader);
  // will terminate when </data> reached or an improper read
  while (ret == 1 && minimum_depth < xmlTextReaderDepth(reader)) {
    name = xmlTextReaderName(reader);
    // if elt is an xml text node
    if (xmlTextReaderNodeType(reader) == 3) {
      value        = xmlTextReaderValue(reader);
      propertyData = std::string((const char*)value);
      xmlFree(value);
    }

    xmlFree(name);
    ret = xmlTextReaderRead(reader);
  }
  return make_pair(key, propertyData);
}

/*
 * reader should be pointing at the node element before calling
 *
 * parses the node from a GraphML file into readable form
 */
bool ProcessNode(xmlTextReaderPtr reader, GraphState* builder,
                 WriterProperties* properties) {
  auto minimum_depth = xmlTextReaderDepth(reader);

  int ret = xmlTextReaderMoveToNextAttribute(reader);
  xmlChar *name, *value;

  std::string id;
  std::vector<std::string> labels;

  bool extractedLabels = false; // neo4j includes these twice so only parse 1

  // parse node attributes for id (required) and label(s) (optional)
  while (ret == 1) {
    name  = xmlTextReaderName(reader);
    value = xmlTextReaderValue(reader);

    if (name != NULL) {
      if (xmlStrEqual(name, BAD_CAST "id")) {
        id = std::string((const char*)value);
      } else if (xmlStrEqual(name, BAD_CAST "labels") ||
                 xmlStrEqual(name, BAD_CAST "label")) {
        std::string data((const char*)value);
        // erase prepended ':' if it exists
        if (data.front() == ':') {
          data.erase(0, 1);
        }
        boost::split(labels, data, boost::is_any_of(":"));
        extractedLabels = true;
      } else {
        GALOIS_LOG_ERROR(
            "Attribute on node: {}, with value {} was not recognized",
            std::string((const char*)name), std::string((const char*)value));
      }
    }

    xmlFree(name);
    xmlFree(value);
    ret = xmlTextReaderMoveToNextAttribute(reader);
  }

  bool validNode = !id.empty();
  if (validNode) {
    builder->topology_builder.node_indexes.insert(
        std::pair<std::string, size_t>(
            id, builder->topology_builder.node_indexes.size()));
  }

  // parse "data" xml nodes for properties
  ret = xmlTextReaderRead(reader);
  // will terminate when </node> reached or an improper read
  while (ret == 1 && minimum_depth < xmlTextReaderDepth(reader)) {
    name = xmlTextReaderName(reader);
    if (name == NULL) {
      name = xmlStrdup(BAD_CAST "--");
    }
    // if elt is an xml node (we do not parse text for nodes)
    if (xmlTextReaderNodeType(reader) == 1) {
      // if elt is a "data" xml node read it in
      if (xmlStrEqual(name, BAD_CAST "data")) {
        auto property = ProcessData(reader);
        if (property.first.size() > 0) {
          // we reserve the data fields label and labels for node/edge labels
          if (property.first == std::string("label") ||
              property.first == std::string("labels")) {
            if (!extractedLabels) {
              // erase prepended ':' if it exists
              std::string data = property.second;
              if (data.front() == ':') {
                data.erase(0, 1);
              }
              boost::split(labels, data, boost::is_any_of(":"));
              extractedLabels = true;
            }
          } else if (property.first != std::string("IGNORE")) {
            if (validNode) {
              auto keyIter = builder->node_properties.keys.find(property.first);
              size_t index;
              // if an entry for the key does not already exist, make a default
              // entry for it
              if (keyIter == builder->node_properties.keys.end()) {
                index =
                    AddStringBuilder(property.first, &builder->node_properties);
              } else {
                index = keyIter->second;
              }
              AddValue(property.second,
                       builder->node_properties.builders[index],
                       &builder->node_properties.chunks[index], properties,
                       builder->nodes);
            }
          }
        }
      } else {
        GALOIS_LOG_ERROR("In node found element: {}, which was ignored",
                         std::string((const char*)name));
      }
    }

    xmlFree(name);
    ret = xmlTextReaderRead(reader);
  }

  // add labels if they exists
  if (validNode && labels.size() > 0) {
    for (std::string label : labels) {
      auto entry = builder->node_labels.keys.find(label);
      size_t index;

      // if label does not already exist, add a column
      if (entry == builder->node_labels.keys.end()) {
        index = AddFalseBuilder(label, &builder->node_labels);
      } else {
        index = entry->second;
      }
      AddLabel(builder->node_labels.builders[index],
               &builder->node_labels.chunks[index], properties, builder->nodes);
    }
  }
  return validNode;
}

/*
 * reader should be pointing at the edge element before calling
 *
 * parses the edge from a GraphML file into readable form
 */
bool ProcessEdge(xmlTextReaderPtr reader, GraphState* builder,
                 WriterProperties* properties) {
  auto minimum_depth = xmlTextReaderDepth(reader);

  int ret = xmlTextReaderMoveToNextAttribute(reader);
  xmlChar *name, *value;

  std::string source;
  std::string target;
  std::string type;
  bool extracted_type = false; // neo4j includes these twice so only parse 1

  // parse node attributes for id (required) and label(s) (optional)
  while (ret == 1) {
    name  = xmlTextReaderName(reader);
    value = xmlTextReaderValue(reader);

    if (name != NULL) {
      if (xmlStrEqual(name, BAD_CAST "id")) {
      } else if (xmlStrEqual(name, BAD_CAST "source")) {
        source = std::string((const char*)value);
      } else if (xmlStrEqual(name, BAD_CAST "target")) {
        target = std::string((const char*)value);
      } else if (xmlStrEqual(name, BAD_CAST "labels") ||
                 xmlStrEqual(name, BAD_CAST "label")) {
        type           = std::string((const char*)value);
        extracted_type = true;
      } else {
        GALOIS_LOG_ERROR(
            "Attribute on edge: {}, with value {} was not recognized",
            std::string((const char*)name), std::string((const char*)value));
      }
    }

    xmlFree(name);
    xmlFree(value);
    ret = xmlTextReaderMoveToNextAttribute(reader);
  }

  bool valid_edge = !source.empty() && !target.empty();
  if (valid_edge) {
    auto src_entry  = builder->topology_builder.node_indexes.find(source);
    auto dest_entry = builder->topology_builder.node_indexes.find(target);

    valid_edge = src_entry != builder->topology_builder.node_indexes.end() &&
                 dest_entry != builder->topology_builder.node_indexes.end();
    if (valid_edge) {
      builder->topology_builder.sources.push_back(src_entry->second);
      builder->topology_builder.destinations.push_back(
          (uint32_t)dest_entry->second);
      builder->topology_builder.out_indices[src_entry->second]++;
    }
  }

  // parse "data" xml edges for properties
  ret = xmlTextReaderRead(reader);
  // will terminate when </edge> reached or an improper read
  while (ret == 1 && minimum_depth < xmlTextReaderDepth(reader)) {
    name = xmlTextReaderName(reader);
    if (name == NULL) {
      name = xmlStrdup(BAD_CAST "--");
    }
    // if elt is an xml node (we do not parse text for nodes)
    if (xmlTextReaderNodeType(reader) == 1) {
      // if elt is a "data" xml node read it in
      if (xmlStrEqual(name, BAD_CAST "data")) {
        auto property = ProcessData(reader);
        if (property.first.size() > 0) {
          // we reserve the data fields label and labels for node/edge labels
          if (property.first == std::string("label") ||
              property.first == std::string("labels")) {
            if (!extracted_type) {
              type           = property.second;
              extracted_type = true;
            }
          } else if (property.first != std::string("IGNORE")) {
            if (valid_edge) {
              auto keyIter = builder->edge_properties.keys.find(property.first);
              size_t index;
              // if an entry for the key does not already exist, make a default
              // entry for it
              if (keyIter == builder->edge_properties.keys.end()) {
                index =
                    AddStringBuilder(property.first, &builder->edge_properties);
              } else {
                index = keyIter->second;
              }
              AddValue(property.second,
                       builder->edge_properties.builders[index],
                       &builder->edge_properties.chunks[index], properties,
                       builder->edges);
            }
          }
        }
      } else {
        GALOIS_LOG_ERROR("In edge found element: {}, which was ignored",
                         std::string((const char*)name));
      }
    }

    xmlFree(name);
    ret = xmlTextReaderRead(reader);
  }

  // add type if it exists
  if (valid_edge && type.length() > 0) {
    auto entry = builder->edge_types.keys.find(type);
    size_t index;

    // if type does not already exist, add a column
    if (entry == builder->edge_types.keys.end()) {
      index = AddFalseBuilder(type, &builder->edge_types);
    } else {
      index = entry->second;
    }
    AddLabel(builder->edge_types.builders[index],
             &builder->edge_types.chunks[index], properties, builder->edges);
  }
  return valid_edge;
}

/*
 * reader should be pointing at the graph element before calling
 *
 * parses the graph structure from a GraphML file into Galois format
 */
void ProcessGraph(xmlTextReaderPtr reader, GraphState* builder,
                  WriterProperties* properties) {
  auto minimum_depth = xmlTextReaderDepth(reader);
  int ret            = xmlTextReaderRead(reader);

  bool finished_nodes = false;

  // will terminate when </graph> reached or an improper read
  while (ret == 1 && minimum_depth < xmlTextReaderDepth(reader)) {
    xmlChar* name;
    name = xmlTextReaderName(reader);
    if (name == NULL) {
      name = xmlStrdup(BAD_CAST "--");
    }
    // if elt is an xml node
    if (xmlTextReaderNodeType(reader) == 1) {
      // if elt is a "node" xml node read it in
      if (xmlStrEqual(name, BAD_CAST "node")) {
        if (ProcessNode(reader, builder, properties)) {
          builder->topology_builder.out_indices.push_back(0);
          builder->nodes++;

          if (builder->nodes % (properties->chunk_size * 100) == 0) {
            GALOIS_LOG_VERBOSE("Nodes Processed: {}", builder->nodes);
          }
        }
      } else if (xmlStrEqual(name, BAD_CAST "edge")) {
        if (!finished_nodes) {
          finished_nodes = true;
          std::cout << "Finished processing nodes" << std::endl;
        }
        // if elt is an "egde" xml node read it in
        if (ProcessEdge(reader, builder, properties)) {
          builder->edges++;

          if (builder->edges % (properties->chunk_size * 100) == 0) {
            GALOIS_LOG_VERBOSE("Edges Processed: {}", builder->edges);
          }
        }
      } else {
        GALOIS_LOG_ERROR("Found element: {}, which was ignored",
                         std::string((const char*)name));
      }
    }

    xmlFree(name);
    ret = xmlTextReaderRead(reader);
  }
  builder->node_properties.keys.clear();
  builder->node_labels.keys.clear();
  builder->edge_properties.keys.clear();
  builder->edge_types.keys.clear();
  std::cout << "Finished processing edges" << std::endl;

  // add buffered rows before exiting and even out columns
  EvenOutChunkBuilders(&builder->node_properties.builders,
                       &builder->node_properties.chunks, properties,
                       builder->nodes);
  EvenOutChunkBuilders(&builder->node_labels.builders,
                       &builder->node_labels.chunks, properties,
                       builder->nodes);
  EvenOutChunkBuilders(&builder->edge_properties.builders,
                       &builder->edge_properties.chunks, properties,
                       builder->edges);
  EvenOutChunkBuilders(&builder->edge_types.builders,
                       &builder->edge_types.chunks, properties, builder->edges);

  builder->topology_builder.out_dests.resize(
      builder->edges, std::numeric_limits<uint32_t>::max());
}

} // end of unnamed namespace

/// ConvertGraphML converts a GraphML file into katana form
///
/// \param infilename path to source graphml file
/// \returns arrow tables of node properties/labels, edge properties/types, and
/// csr topology
galois::GraphComponents galois::ConvertGraphML(const std::string& infilename,
                                               size_t chunk_size) {
  xmlTextReaderPtr reader;
  int ret = 0;

  GraphState builder{};
  WriterProperties properties{GetNullArrays(chunk_size),
                              GetFalseArray(chunk_size), chunk_size};

  galois::setActiveThreads(1000);
  bool finishedGraph = false;
  std::cout << "Start converting GraphML file: " << infilename << std::endl;

  reader = xmlNewTextReaderFilename(infilename.c_str());
  if (reader != NULL) {
    ret = xmlTextReaderRead(reader);

    // procedure:
    // read in "key" xml nodes and add them to nodeKeys and edgeKeys
    // once we reach the first "graph" xml node we parse it using the above keys
    // once we have parsed the first "graph" xml node we exit
    while (ret == 1 && !finishedGraph) {
      xmlChar* name;
      name = xmlTextReaderName(reader);
      if (name == NULL) {
        name = xmlStrdup(BAD_CAST "--");
      }
      // if elt is an xml node
      if (xmlTextReaderNodeType(reader) == 1) {
        // if elt is a "key" xml node read it in
        if (xmlStrEqual(name, BAD_CAST "key")) {
          KeyGraphML key = ProcessKey(reader);
          if (key.id.size() > 0 && key.id != std::string("label") &&
              key.id != std::string("IGNORE")) {
            if (key.for_node) {
              AddBuilder(&builder.node_properties, std::move(key));
            } else if (key.for_edge) {
              AddBuilder(&builder.edge_properties, std::move(key));
            }
          }
        } else if (xmlStrEqual(name, BAD_CAST "graph")) {
          std::cout << "Finished processing property headers" << std::endl;
          std::cout << "Node Properties declared: "
                    << builder.node_properties.keys.size() << std::endl;
          std::cout << "Edge Properties declared: "
                    << builder.edge_properties.keys.size() << std::endl;
          ProcessGraph(reader, &builder, &properties);
          finishedGraph = true;
        }
      }

      xmlFree(name);
      ret = xmlTextReaderRead(reader);
    }
    xmlFreeTextReader(reader);
    if (ret < 0) {
      GALOIS_LOG_FATAL("Failed to parse {}", infilename);
    }
  } else {
    GALOIS_LOG_FATAL("Unable to open {}", infilename);
  }

  std::cout << "Node Properties:" << std::endl;
  WriteNullStats(builder.node_properties.chunks, &properties, builder.nodes);
  std::cout << "Node Labels:" << std::endl;
  WriteFalseStats(builder.node_labels.chunks, &properties, builder.nodes);
  std::cout << "Edge Properties Pre:" << std::endl;
  WriteNullStats(builder.edge_properties.chunks, &properties, builder.edges);
  std::cout << "Edge Types Pre:" << std::endl;
  WriteFalseStats(builder.edge_types.chunks, &properties, builder.edges);

  // build final nodes
  auto final_node_table = BuildTable(&builder.node_properties.chunks,
                                     &builder.node_properties.schema);
  auto final_label_table =
      BuildTable(&builder.node_labels.chunks, &builder.node_labels.schema);

  std::cout << "Finished building nodes" << std::endl;

  // rearrange edges to match implicit edge IDs
  auto edgesTables = BuildFinalEdges(&builder, &properties);
  std::shared_ptr<arrow::Table> final_edge_table = edgesTables.first;
  std::shared_ptr<arrow::Table> final_type_table = edgesTables.second;

  std::cout << "Finished topology and ordering edges" << std::endl;

  // build topology
  auto topology = std::make_shared<galois::graphs::GraphTopology>();
  arrow::Status st;
  std::shared_ptr<arrow::UInt64Builder> topology_indices_builder =
      std::make_shared<arrow::UInt64Builder>();
  st = topology_indices_builder->AppendValues(
      builder.topology_builder.out_indices);
  if (!st.ok()) {
    GALOIS_LOG_FATAL("Error building topology: {}", st.ToString());
  }
  std::shared_ptr<arrow::UInt32Builder> topology_dests_builder =
      std::make_shared<arrow::UInt32Builder>();
  st = topology_dests_builder->AppendValues(builder.topology_builder.out_dests);
  if (!st.ok()) {
    GALOIS_LOG_FATAL("Error building topology: {}", st.ToString());
  }

  st = topology_indices_builder->Finish(&topology->out_indices);
  if (!st.ok()) {
    GALOIS_LOG_FATAL("Error building arrow array for topology: {}",
                     st.ToString());
  }
  st = topology_dests_builder->Finish(&topology->out_dests);
  if (!st.ok()) {
    GALOIS_LOG_FATAL("Error building arrow array for topology: {}",
                     st.ToString());
  }

  std::cout << "Finished graphml conversion to arrow" << std::endl;
  std::cout << "Nodes: " << final_node_table->num_rows() << std::endl;
  std::cout << "Node Properties: " << final_node_table->num_columns()
            << std::endl;
  std::cout << "Node Labels: " << final_label_table->num_columns() << std::endl;
  std::cout << "Edges: " << final_edge_table->num_rows() << std::endl;
  std::cout << "Edge Properties: " << final_edge_table->num_columns()
            << std::endl;
  std::cout << "Edge Types: " << final_type_table->num_columns() << std::endl;

  return GraphComponents{final_node_table, final_label_table, final_edge_table,
                         final_type_table, topology};
}

/// writePropertyGraph formally builds katana form via PropertyFileGraph from
/// imported components and writes the result to target directory
///
/// \param graph_comps imported components to convert into a PropertyFileGraph
/// \param dir local FS directory or s3 directory to write PropertyFileGraph to
void galois::WritePropertyGraph(const galois::GraphComponents& graph_comps,
                                const std::string& dir) {
  galois::graphs::PropertyFileGraph graph;

  auto result = graph.SetTopology(*graph_comps.topology);
  if (!result) {
    GALOIS_LOG_FATAL("Error adding topology: {}", result.error());
  }

  if (graph_comps.node_properties->num_columns() > 0) {
    result = graph.AddNodeProperties(graph_comps.node_properties);
    if (!result) {
      GALOIS_LOG_FATAL("Error adding node properties: {}", result.error());
    }
  }
  if (graph_comps.node_labels->num_columns() > 0) {
    result = graph.AddNodeProperties(graph_comps.node_labels);
    if (!result) {
      GALOIS_LOG_FATAL("Error adding node labels: {}", result.error());
    }
  }
  if (graph_comps.edge_properties->num_columns() > 0) {
    result = graph.AddEdgeProperties(graph_comps.edge_properties);
    if (!result) {
      GALOIS_LOG_FATAL("Error adding edge properties: {}", result.error());
    }
  }
  if (graph_comps.edge_types->num_columns() > 0) {
    result = graph.AddEdgeProperties(graph_comps.edge_types);
    if (!result) {
      GALOIS_LOG_FATAL("Error adding edge types: {}", result.error());
    }
  }

  WritePropertyGraph(std::move(graph), dir);
}

void galois::WritePropertyGraph(galois::graphs::PropertyFileGraph prop_graph,
                                const std::string& dir) {
  std::string meta_file = dir;
  if (meta_file[meta_file.length() - 1] == '/') {
    meta_file += "meta";
  } else {
    meta_file += "/meta";
  }
  auto result = prop_graph.Write(meta_file);
  if (!result) {
    GALOIS_LOG_FATAL("Error writing to fs: {}", result.error());
  }
}
