// Schema snapshot registry: Python Meta → C++ (setup path, not per-filter).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace django::orm {

using ModelId = std::uint32_t;
using FieldId = std::uint32_t;

enum class FieldType : std::uint8_t {
  Integer = 0,
  BigInteger,
  SmallInteger,
  Auto,
  BigAuto,
  Float,
  Decimal,
  Boolean,
  Text,
  Char,
  Date,
  DateTime,
  Time,
  UUID,
  Binary,
  ForeignKey,
  Other,
};

struct FieldSchema {
  FieldId id = 0;
  std::string name;
  std::string attname;
  std::string column;
  FieldType type = FieldType::Other;
  bool primary_key = false;
  bool nullable = false;
  // FK: remote table/pk for simple joins (denormalized).
  bool is_relation = false;
  std::string remote_table;
  std::string remote_pk_column;
  std::string remote_model_label;
};

struct ModelSchema {
  ModelId id = 0;
  std::string label;
  std::string db_table;
  std::vector<FieldSchema> fields;
  FieldId pk_field = 0;
  std::unordered_map<std::string, FieldId> field_by_name;
};

class SchemaRegistry {
 public:
  static SchemaRegistry& instance();

  ModelId register_model(ModelSchema schema);

  [[nodiscard]] const ModelSchema* get(ModelId id) const;
  [[nodiscard]] const ModelSchema* get_by_label(std::string_view label) const;
  [[nodiscard]] std::optional<ModelId> find_id(std::string_view label) const;

  void clear();

 private:
  SchemaRegistry() = default;
  std::vector<ModelSchema> models_;
  std::unordered_map<std::string, ModelId> by_label_;
};

[[nodiscard]] FieldType field_type_from_class_name(std::string_view class_name);

}  // namespace django::orm
