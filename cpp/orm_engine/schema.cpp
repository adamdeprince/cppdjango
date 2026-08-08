#include "orm_engine/schema.hpp"

#include <utility>

namespace django::orm {
namespace {

void index_fields(ModelSchema& schema) {
  schema.field_by_name.clear();
  schema.pk_field = 0;
  FieldId fid = 0;
  for (auto& f : schema.fields) {
    f.id = fid++;
    schema.field_by_name[f.name] = f.id;
    if (f.attname != f.name && !f.attname.empty()) {
      schema.field_by_name[f.attname] = f.id;
    }
    if (f.primary_key) {
      schema.field_by_name["pk"] = f.id;
      schema.pk_field = f.id;
    }
  }
}

}  // namespace

SchemaRegistry& SchemaRegistry::instance() {
  static SchemaRegistry reg;
  return reg;
}

ModelId SchemaRegistry::register_model(ModelSchema schema) {
  index_fields(schema);
  ++generation_;
  if (auto it = by_label_.find(schema.label); it != by_label_.end()) {
    ModelId id = it->second;
    schema.id = id;
    models_[id] = std::move(schema);
    return id;
  }
  ModelId id = static_cast<ModelId>(models_.size());
  schema.id = id;
  by_label_[schema.label] = id;
  models_.push_back(std::move(schema));
  return id;
}

const ModelSchema* SchemaRegistry::get(ModelId id) const {
  if (id >= models_.size()) {
    return nullptr;
  }
  return &models_[id];
}

const ModelSchema* SchemaRegistry::get_by_label(std::string_view label) const {
  auto it = by_label_.find(std::string(label));
  if (it == by_label_.end()) {
    return nullptr;
  }
  return get(it->second);
}

std::optional<ModelId> SchemaRegistry::find_id(std::string_view label) const {
  auto it = by_label_.find(std::string(label));
  if (it == by_label_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void SchemaRegistry::clear() {
  models_.clear();
  by_label_.clear();
  ++generation_;
}

FieldType field_type_from_class_name(std::string_view class_name) {
  if (class_name == "AutoField" || class_name == "SmallAutoField") {
    return FieldType::Auto;
  }
  if (class_name == "BigAutoField") {
    return FieldType::BigAuto;
  }
  if (class_name == "IntegerField" || class_name == "PositiveIntegerField") {
    return FieldType::Integer;
  }
  if (class_name == "SmallIntegerField" ||
      class_name == "PositiveSmallIntegerField") {
    return FieldType::SmallInteger;
  }
  if (class_name == "BigIntegerField" || class_name == "PositiveBigIntegerField") {
    return FieldType::BigInteger;
  }
  if (class_name == "FloatField") {
    return FieldType::Float;
  }
  if (class_name == "DecimalField") {
    return FieldType::Decimal;
  }
  if (class_name == "BooleanField" || class_name == "NullBooleanField") {
    return FieldType::Boolean;
  }
  if (class_name == "TextField") {
    return FieldType::Text;
  }
  if (class_name == "CharField" || class_name == "SlugField" ||
      class_name == "EmailField" || class_name == "URLField") {
    return FieldType::Char;
  }
  if (class_name == "DateField") {
    return FieldType::Date;
  }
  if (class_name == "DateTimeField") {
    return FieldType::DateTime;
  }
  if (class_name == "TimeField") {
    return FieldType::Time;
  }
  if (class_name == "UUIDField") {
    return FieldType::UUID;
  }
  if (class_name == "BinaryField") {
    return FieldType::Binary;
  }
  if (class_name == "ForeignKey" || class_name == "OneToOneField") {
    return FieldType::ForeignKey;
  }
  return FieldType::Other;
}

RelKind rel_kind_from_string(std::string_view s) {
  if (s == "fk" || s == "forward_fk") {
    return RelKind::ForwardFK;
  }
  if (s == "rev_fk" || s == "reverse_fk") {
    return RelKind::ReverseFK;
  }
  if (s == "m2m" || s == "forward_m2m") {
    return RelKind::ForwardM2M;
  }
  if (s == "rev_m2m" || s == "reverse_m2m") {
    return RelKind::ReverseM2M;
  }
  return RelKind::None;
}

bool field_type_has_direct_primitive_prep(FieldType type) {
  switch (type) {
    case FieldType::Integer:
    case FieldType::BigInteger:
    case FieldType::SmallInteger:
    case FieldType::Auto:
    case FieldType::BigAuto:
    case FieldType::Float:
    case FieldType::Boolean:
    case FieldType::Text:
    case FieldType::Char:
      return true;
    default:
      return false;
  }
}

}  // namespace django::orm
