// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/fidl/fidlc/include/fidl/tables_generator.h"

#include <zircon/assert.h>

#include "tools/fidl/fidlc/include/fidl/coded_ast.h"
#include "tools/fidl/fidlc/include/fidl/names.h"

namespace fidl {

namespace {

std::string PrimitiveSubtypeToString(fidl::types::PrimitiveSubtype subtype) {
  using fidl::types::PrimitiveSubtype;
  switch (subtype) {
    case PrimitiveSubtype::kBool:
      return "Bool";
    case PrimitiveSubtype::kInt8:
      return "Int8";
    case PrimitiveSubtype::kInt16:
      return "Int16";
    case PrimitiveSubtype::kInt32:
      return "Int32";
    case PrimitiveSubtype::kInt64:
      return "Int64";
    case PrimitiveSubtype::kUint8:
      return "Uint8";
    case PrimitiveSubtype::kZxUchar:
      return "ZxUchar";
    case PrimitiveSubtype::kUint16:
      return "Uint16";
    case PrimitiveSubtype::kUint32:
      return "Uint32";
    case PrimitiveSubtype::kUint64:
      return "Uint64";
    case PrimitiveSubtype::kZxUsize:
      return "ZxUsize";
    case PrimitiveSubtype::kZxUintptr:
      return "ZxUintptr";
    case PrimitiveSubtype::kFloat32:
      return "Float32";
    case PrimitiveSubtype::kFloat64:
      return "Float64";
  }
}

std::string InternalSubtypeToString(fidl::types::InternalSubtype subtype) {
  using fidl::types::InternalSubtype;
  switch (subtype) {
    case InternalSubtype::kTransportErr:
      return "TransportErr";
  }
}

// When generating coding tables for containers employing envelopes (unions & tables),
// we need to reference coding tables for primitives, in addition to types that need coding.
// This function handles naming coding tables for both cases.
std::string CodedNameForEnvelope(const fidl::coded::Type* type) {
  switch (type->kind) {
    case coded::Type::Kind::kPrimitive: {
      using fidl::types::PrimitiveSubtype;
      // To save space, all primitive types of the same underlying subtype
      // share the same table.
      std::string suffix =
          PrimitiveSubtypeToString(static_cast<const coded::PrimitiveType*>(type)->subtype);
      return "fidl_internal_k" + suffix;
    }
    case coded::Type::Kind::kInternal: {
      using fidl::types::InternalSubtype;
      std::string suffix =
          InternalSubtypeToString(static_cast<const coded::InternalType*>(type)->subtype);
      return "fidl_internal_k" + suffix;
    }
    default:
      return type->coded_name;
  }
}

template <typename Type>
std::string TableTypeName([[maybe_unused]] const Type& type) {
  using T = std::decay_t<Type>;
  if constexpr (std::is_same_v<T, fidl::coded::PrimitiveType>)
    return "FidlCodedPrimitive";
  if constexpr (std::is_same_v<T, fidl::coded::InternalType>) {
    switch (static_cast<const fidl::coded::InternalType*>(&type)->subtype) {
      case types::InternalSubtype::kTransportErr:
        return "FidlCodedEnum";
    }
  }
  if constexpr (std::is_same_v<T, fidl::coded::BitsType>)
    return "FidlCodedBits";
  if constexpr (std::is_same_v<T, fidl::coded::EnumType>)
    return "FidlCodedEnum";
  if constexpr (std::is_same_v<T, fidl::coded::HandleType>)
    return "FidlCodedHandle";
  if constexpr (std::is_same_v<T, fidl::coded::RequestHandleType>)
    return "FidlCodedHandle";
  if constexpr (std::is_same_v<T, fidl::coded::ProtocolHandleType>)
    return "FidlCodedHandle";
  if constexpr (std::is_same_v<T, fidl::coded::ArrayType>)
    return "FidlCodedArray";
  if constexpr (std::is_same_v<T, fidl::coded::VectorType>)
    return "FidlCodedVector";
  if constexpr (std::is_same_v<T, fidl::coded::StructType>)
    return "FidlCodedStruct";
  if constexpr (std::is_same_v<T, fidl::coded::StructPointerType>)
    return "FidlCodedStructPointer";
  if constexpr (std::is_same_v<T, fidl::coded::TableType>)
    return "FidlCodedTable";
  if constexpr (std::is_same_v<T, fidl::coded::UnionType>)
    return "FidlCodedUnion";
  if constexpr (std::is_same_v<T, fidl::coded::ZxExperimentalPointerType>)
    return "FidlCodedZxExperimentalPointer";
}

constexpr auto kIndent = "    ";

void Emit(std::ostream* file, std::string_view data) { *file << data; }

void EmitNewlineAndIndent(std::ostream* file, size_t indent_level) {
  *file << "\n";
  while (indent_level--)
    *file << kIndent;
}

void EmitArrayBegin(std::ostream* file) { *file << "{"; }

void EmitArraySeparator(std::ostream* file, size_t indent_level) {
  *file << ",";
  EmitNewlineAndIndent(file, indent_level);
}

void EmitArrayEnd(std::ostream* file) { *file << "}"; }

void Emit(std::ostream* file, uint16_t value) { *file << value << "u"; }

void Emit(std::ostream* file, uint32_t value) { *file << value << "u"; }

void Emit(std::ostream* file, uint64_t value) { *file << value << "ul"; }

void Emit(std::ostream* file, types::Nullability nullability) {
  switch (nullability) {
    case types::Nullability::kNullable:
      Emit(file, "kFidlNullability_Nullable");
      break;
    case types::Nullability::kNonnullable:
      Emit(file, "kFidlNullability_Nonnullable");
      break;
  }
}

void Emit(std::ostream* file, types::Strictness strictness) {
  switch (strictness) {
    case types::Strictness::kFlexible:
      Emit(file, "kFidlStrictness_Flexible");
      break;
    case types::Strictness::kStrict:
      Emit(file, "kFidlStrictness_Strict");
      break;
  }
}

void Emit(std::ostream* file, types::Resourceness resourceness) {
  switch (resourceness) {
    case types::Resourceness::kResource:
      Emit(file, "kFidlIsResource_Resource");
      break;
    case types::Resourceness::kValue:
      Emit(file, "kFidlIsResource_NotResource");
      break;
  }
}

void Emit(std::ostream* file, coded::MemcpyCompatibility element_memcpy_compatibility) {
  switch (element_memcpy_compatibility) {
    case coded::MemcpyCompatibility::kCannotMemcpy:
      Emit(file, "kFidlMemcpyCompatibility_CannotMemcpy");
      break;
    case coded::MemcpyCompatibility::kCanMemcpy:
      Emit(file, "kFidlMemcpyCompatibility_CanMemcpy");
      break;
  }
}
}  // namespace

template <typename Collection>
void TablesGenerator::GenerateArray(const Collection& collection) {
  EmitArrayBegin(&tables_file_);

  if (!collection.empty())
    EmitNewlineAndIndent(&tables_file_, ++indent_level_);

  for (size_t i = 0; i < collection.size(); ++i) {
    if (i)
      EmitArraySeparator(&tables_file_, indent_level_);
    Generate(collection[i]);
  }

  if (!collection.empty())
    EmitNewlineAndIndent(&tables_file_, --indent_level_);

  EmitArrayEnd(&tables_file_);
}

void TablesGenerator::Generate(const coded::EnumType& enum_type) {
  std::string validator_func_ref;
  if (enum_type.strictness == types::Strictness::kStrict) {
    std::string validator_func =
        std::string("EnumValidatorFor_") + std::string(enum_type.coded_name);
    validator_func_ref = "&" + validator_func;
    Emit(&tables_file_, "static bool ");
    Emit(&tables_file_, validator_func);
    Emit(&tables_file_, "(uint64_t v) {\n  switch (v) {\n");
    for (const auto& member : enum_type.members) {
      Emit(&tables_file_, "    case ");
      Emit(&tables_file_, member);
      Emit(&tables_file_, ":\n");
    }
    Emit(&tables_file_, "      return true;\n");
    Emit(&tables_file_, "    default:\n      return false;\n");
    Emit(&tables_file_, "  }\n}\n\n");
  } else {
    validator_func_ref = "NULL";
  }

  Emit(&tables_file_, "const struct FidlCodedEnum ");
  Emit(&tables_file_, NameTable(enum_type.coded_name));
  Emit(&tables_file_, " = {.tag=kFidlTypeEnum, .underlying_type=kFidlCodedPrimitiveSubtype_");
  Emit(&tables_file_, PrimitiveSubtypeToString(enum_type.subtype));
  Emit(&tables_file_, ", .strictness=");
  Emit(&tables_file_, enum_type.strictness);
  Emit(&tables_file_, ", .validate=" + validator_func_ref + ", .name=\"");
  Emit(&tables_file_, enum_type.qname);
  Emit(&tables_file_, "\"};\n\n");
}

void TablesGenerator::Generate(const coded::BitsType& bits_type) {
  Emit(&tables_file_, "const struct FidlCodedBits ");
  Emit(&tables_file_, NameTable(bits_type.coded_name));
  Emit(&tables_file_, " = {.tag=kFidlTypeBits, .underlying_type=kFidlCodedPrimitiveSubtype_");
  Emit(&tables_file_, PrimitiveSubtypeToString(bits_type.subtype));
  Emit(&tables_file_, ", .strictness=");
  Emit(&tables_file_, bits_type.strictness);
  Emit(&tables_file_, ", .mask=");
  Emit(&tables_file_, bits_type.mask);
  Emit(&tables_file_, ", .name=\"");
  Emit(&tables_file_, bits_type.qname);
  Emit(&tables_file_, "\"};\n\n");
}

// TODO(fxbug.dev/56727) Consider filtering out structs that are not used because they are
// only referenced by channel transports.
void TablesGenerator::Generate(const coded::StructType& struct_type) {
  std::string fields_array_name = NameFields(struct_type.coded_name);

  if (!struct_type.elements.empty()) {
    Emit(&tables_file_, "static const struct FidlStructElement ");
    Emit(&tables_file_, fields_array_name);
    Emit(&tables_file_, "[] = ");
    GenerateArray(struct_type.elements);
    Emit(&tables_file_, ";\n");
  }

  Emit(&tables_file_, "const struct FidlCodedStruct ");
  Emit(&tables_file_, NameTable(struct_type.coded_name));
  Emit(&tables_file_, " = {.tag=kFidlTypeStruct, .contains_envelope=");
  Emit(&tables_file_, struct_type.contains_envelope
                          ? "kFidlContainsEnvelope_ContainsEnvelope"
                          : "kFidlContainsEnvelope_DoesNotContainEnvelope");
  Emit(&tables_file_, ", .is_empty=");
  Emit(&tables_file_, struct_type.is_empty ? "kFidlEmpty_IsEmpty" : "kFidlEmpty_IsNotEmpty");
  Emit(&tables_file_, ", .elements=");
  Emit(&tables_file_, struct_type.elements.empty() ? "NULL" : fields_array_name);
  Emit(&tables_file_, ", .element_count=");
  Emit(&tables_file_, static_cast<uint32_t>(struct_type.elements.size()));
  Emit(&tables_file_, ", .size_v2=");
  Emit(&tables_file_, struct_type.size_v2);
  Emit(&tables_file_, ", .name=\"");
  Emit(&tables_file_, struct_type.qname);
  Emit(&tables_file_, "\"};\n\n");
}

void TablesGenerator::Generate(const coded::TableType& table_type) {
  std::string fields_array_name = NameFields(table_type.coded_name);

  if (!table_type.fields.empty()) {
    Emit(&tables_file_, "static const struct FidlTableField ");
    Emit(&tables_file_, NameFields(table_type.coded_name));
    Emit(&tables_file_, "[] = ");
    GenerateArray(table_type.fields);
    Emit(&tables_file_, ";\n");
  }

  Emit(&tables_file_, "const struct FidlCodedTable ");
  Emit(&tables_file_, NameTable(table_type.coded_name));
  Emit(&tables_file_, " = {.tag=kFidlTypeTable, .fields=");
  Emit(&tables_file_, table_type.fields.empty() ? "NULL" : fields_array_name);
  Emit(&tables_file_, ", .field_count=");
  Emit(&tables_file_, static_cast<uint32_t>(table_type.fields.size()));
  Emit(&tables_file_, ", .is_resource=");
  Emit(&tables_file_, table_type.resourceness);
  Emit(&tables_file_, ", .name=\"");
  Emit(&tables_file_, table_type.qname);
  Emit(&tables_file_, "\"};\n\n");
}

void TablesGenerator::Generate(const coded::UnionType& union_type) {
  std::string fields_array_name = NameFields(union_type.coded_name);

  if (!union_type.fields.empty()) {
    Emit(&tables_file_, "static const struct FidlUnionField ");
    Emit(&tables_file_, fields_array_name);
    Emit(&tables_file_, "[] = ");
    GenerateArray(union_type.fields);
    Emit(&tables_file_, ";\n");
  }

  Emit(&tables_file_, "const struct FidlCodedUnion ");
  Emit(&tables_file_, NameTable(union_type.coded_name));
  Emit(&tables_file_, " = {.tag=kFidlTypeUnion, .field_count=");
  Emit(&tables_file_, static_cast<uint32_t>(union_type.fields.size()));
  Emit(&tables_file_, ", .fields=");
  Emit(&tables_file_, union_type.fields.empty() ? "NULL" : fields_array_name);
  Emit(&tables_file_, ", .nullable=");
  Emit(&tables_file_, union_type.nullability);
  Emit(&tables_file_, ", .name=\"");
  Emit(&tables_file_, union_type.qname);
  Emit(&tables_file_, "\", .strictness=");
  Emit(&tables_file_, union_type.strictness);
  Emit(&tables_file_, ", .is_resource=");
  Emit(&tables_file_, union_type.resourceness);
  Emit(&tables_file_, "};\n");
}

void TablesGenerator::Generate(const coded::StructPointerType& pointer) {
  Emit(&tables_file_, "static const struct FidlCodedStructPointer ");
  Emit(&tables_file_, NameTable(pointer.coded_name));
  Emit(&tables_file_, " = {.tag=kFidlTypeStructPointer, .struct_type=");
  Generate(pointer.element_type, CastToFidlType::kNoCast);
  Emit(&tables_file_, "};\n");
}

void TablesGenerator::Generate(const coded::HandleType& handle_type) {
  Emit(&tables_file_, "static const struct FidlCodedHandle ");
  Emit(&tables_file_, NameTable(handle_type.coded_name));
  Emit(&tables_file_, " = {.tag=kFidlTypeHandle, .handle_subtype=");
  Emit(&tables_file_, NameHandleZXObjType(handle_type.subtype));
  Emit(&tables_file_, ", .handle_rights=");
  Emit(&tables_file_, handle_type.rights);
  Emit(&tables_file_, ", .nullable=");
  Emit(&tables_file_, handle_type.nullability);
  Emit(&tables_file_, "};\n\n");
}

void TablesGenerator::Generate(const coded::RequestHandleType& request_type) {
  Emit(&tables_file_, "static const struct FidlCodedHandle ");
  Emit(&tables_file_, NameTable(request_type.coded_name));
  Emit(&tables_file_,
       " = {.tag=kFidlTypeHandle, .handle_subtype=ZX_OBJ_TYPE_CHANNEL, "
       ".handle_rights=ZX_DEFAULT_CHANNEL_RIGHTS, "
       ".nullable=");
  Emit(&tables_file_, request_type.nullability);
  Emit(&tables_file_, "};\n\n");
}

void TablesGenerator::Generate(const coded::ProtocolHandleType& protocol_type) {
  Emit(&tables_file_, "static const struct FidlCodedHandle ");
  Emit(&tables_file_, NameTable(protocol_type.coded_name));
  Emit(&tables_file_,
       " = {.tag=kFidlTypeHandle, .handle_subtype=ZX_OBJ_TYPE_CHANNEL, "
       ".handle_rights=ZX_DEFAULT_CHANNEL_RIGHTS, "
       ".nullable=");
  Emit(&tables_file_, protocol_type.nullability);
  Emit(&tables_file_, "};\n\n");
}

void TablesGenerator::Generate(const coded::ArrayType& array_type) {
  Emit(&tables_file_, "static const struct FidlCodedArray ");
  Emit(&tables_file_, NameTable(array_type.coded_name));
  // Array defs can be unused when they only appear in structs where the field is optimized out.
  Emit(&tables_file_, " __attribute__((unused)) = {.tag=kFidlTypeArray, .element=");
  Generate(array_type.element_type);
  Emit(&tables_file_, ", .array_size_v2=");
  Emit(&tables_file_, array_type.size_v2);
  Emit(&tables_file_, ", .element_size_v2=");
  Emit(&tables_file_, array_type.element_size_v2);
  Emit(&tables_file_, "};\n\n");
}

void TablesGenerator::Generate(const coded::StringType& string_type) {
  Emit(&tables_file_, "static const struct FidlCodedString ");
  Emit(&tables_file_, NameTable(string_type.coded_name));
  Emit(&tables_file_, " = {.tag=kFidlTypeString, .max_size=");
  Emit(&tables_file_, string_type.max_size);
  Emit(&tables_file_, ", .nullable=");
  Emit(&tables_file_, string_type.nullability);
  Emit(&tables_file_, "};\n\n");
}

void TablesGenerator::Generate(const coded::VectorType& vector_type) {
  Emit(&tables_file_, "static const struct FidlCodedVector ");
  Emit(&tables_file_, NameTable(vector_type.coded_name));
  Emit(&tables_file_, " = {.tag=kFidlTypeVector, .element=");
  Generate(vector_type.element_type);
  Emit(&tables_file_, ", .max_count=");
  Emit(&tables_file_, vector_type.max_count);
  Emit(&tables_file_, ", .element_size_v2=");
  Emit(&tables_file_, vector_type.element_size_v2);
  Emit(&tables_file_, ", .nullable=");
  Emit(&tables_file_, vector_type.nullability);
  Emit(&tables_file_, ", .element_memcpy_compatibility=");
  Emit(&tables_file_, vector_type.element_memcpy_compatibility);
  Emit(&tables_file_, "};\n\n");
}

void TablesGenerator::Generate(const coded::ZxExperimentalPointerType& pointer_type) {
  // TODO(fxbug.dev/111200): Ideally we would panic here and just never
  // generate output for this case.
}

void TablesGenerator::Generate(const coded::Type* type, CastToFidlType cast_to_fidl_type) {
  if (type && type->is_coding_needed) {
    if (cast_to_fidl_type == CastToFidlType::kCast) {
      Emit(&tables_file_, "(fidl_type_t*)(&");
    } else {
      Emit(&tables_file_, "&");
    }
    Emit(&tables_file_, NameTable(CodedNameForEnvelope(type)));
    if (cast_to_fidl_type == CastToFidlType::kCast) {
      Emit(&tables_file_, ")");
    }
  } else {
    Emit(&tables_file_, "NULL");
  }
}

void TablesGenerator::Generate(const coded::StructField& field) {
  Emit(&tables_file_, "/*FidlStructField*/{.header=");
  Emit(&tables_file_, "/*FidlStructElementHeader*/{.element_type=");
  Emit(&tables_file_, "kFidlStructElementType_Field");
  Emit(&tables_file_, ", ");
  Emit(&tables_file_, ".is_resource=");
  Emit(&tables_file_, field.resourceness);
  Emit(&tables_file_, "},");
  Emit(&tables_file_, ".offset_v2=");
  Emit(&tables_file_, field.offset_v2);
  Emit(&tables_file_, ", ");
  Emit(&tables_file_, ".field_type=");
  Generate(field.type);
  Emit(&tables_file_, "}");
}

void TablesGenerator::Generate(const coded::StructPadding& padding) {
  Emit(&tables_file_, "/*FidlStructPadding*/{.offset_v2=");
  Emit(&tables_file_, padding.offset_v2);
  Emit(&tables_file_, ", .header=/*FidlStructElementHeader*/{");
  if (std::holds_alternative<uint64_t>(padding.mask)) {
    Emit(&tables_file_, ".element_type=");
    Emit(&tables_file_, "kFidlStructElementType_Padding64");
    Emit(&tables_file_, ",.is_resource=kFidlIsResource_NotResource},");
    Emit(&tables_file_, ".mask_64=");
    Emit(&tables_file_, std::get<uint64_t>(padding.mask));
  } else if (std::holds_alternative<uint32_t>(padding.mask)) {
    Emit(&tables_file_, ".element_type=");
    Emit(&tables_file_, "kFidlStructElementType_Padding32");
    Emit(&tables_file_, ",.is_resource=kFidlIsResource_NotResource},");
    Emit(&tables_file_, ".mask_32=");
    Emit(&tables_file_, std::get<uint32_t>(padding.mask));
  } else if (std::holds_alternative<uint16_t>(padding.mask)) {
    Emit(&tables_file_, ".element_type=");
    Emit(&tables_file_, "kFidlStructElementType_Padding16");
    Emit(&tables_file_, ",.is_resource=kFidlIsResource_NotResource},");
    Emit(&tables_file_, ".mask_16=");
    Emit(&tables_file_, std::get<uint16_t>(padding.mask));
  } else {
    ZX_PANIC("invalid mask variant");
  }
  Emit(&tables_file_, "}");
}

void TablesGenerator::Generate(const coded::StructElement& element) {
  if (std::holds_alternative<const coded::StructField>(element)) {
    Emit(&tables_file_, "/*FidlStructPadding*/{.field=");
    Generate(std::get<const coded::StructField>(element));
    Emit(&tables_file_, "}");
  } else if (std::holds_alternative<const coded::StructPadding>(element)) {
    Emit(&tables_file_, "/*FidlStructPadding*/{.padding=");
    Generate(std::get<const coded::StructPadding>(element));
    Emit(&tables_file_, "}");
  } else {
    ZX_PANIC("invalid StructElement variant");
  }
}

void TablesGenerator::Generate(const coded::TableField& field) {
  Emit(&tables_file_, "/*FidlTableField*/{.type=");
  Generate(field.type);
  Emit(&tables_file_, ", .ordinal=");
  Emit(&tables_file_, field.ordinal);
  Emit(&tables_file_, "}");
}

void TablesGenerator::Generate(const coded::UnionField& field) {
  Emit(&tables_file_, "/*FidlUnionField*/{.type=");
  Generate(field.type);
  Emit(&tables_file_, "}");
}

template <class T>
std::string ForwardDecls(const T& t) {
  // Since we always generate nullable unions they may be unused
  return "__LOCAL extern const struct " + TableTypeName(t) + " " + NameTable(t.coded_name) + ";\n";
}

void TablesGenerator::GenerateForward(const coded::EnumType& enum_type) {
  Emit(&tables_file_, ForwardDecls(enum_type));
}

void TablesGenerator::GenerateForward(const coded::BitsType& bits_type) {
  Emit(&tables_file_, ForwardDecls(bits_type));
}

void TablesGenerator::GenerateForward(const coded::StructType& struct_type) {
  Emit(&tables_file_, ForwardDecls(struct_type));
}

void TablesGenerator::GenerateForward(const coded::TableType& table_type) {
  Emit(&tables_file_, ForwardDecls(table_type));
}

void TablesGenerator::GenerateForward(const coded::UnionType& union_type) {
  Emit(&tables_file_, ForwardDecls(union_type));
}

void TablesGenerator::Produce(CodedTypesGenerator* coded_types_generator) {
  // Generate forward declarations of coding tables for named declarations.
  for (const auto& decl : compilation_->all_libraries_declaration_order) {
    auto coded_type = coded_types_generator->CodedTypeFor(decl->name);
    if (!coded_type)
      continue;
    switch (coded_type->kind) {
      case coded::Type::Kind::kEnum:
        GenerateForward(*static_cast<const coded::EnumType*>(coded_type));
        break;
      case coded::Type::Kind::kBits:
        GenerateForward(*static_cast<const coded::BitsType*>(coded_type));
        break;
      case coded::Type::Kind::kStruct:
        GenerateForward(*static_cast<const coded::StructType*>(coded_type));
        break;
      case coded::Type::Kind::kTable:
        GenerateForward(*static_cast<const coded::TableType*>(coded_type));
        break;
      case coded::Type::Kind::kUnion: {
        // Generate forward declarations for both the non-nullable and nullable variants
        const auto& union_type = *static_cast<const coded::UnionType*>(coded_type);
        GenerateForward(union_type);
        if (union_type.maybe_reference_type)
          GenerateForward(*union_type.maybe_reference_type);
        break;
      }
      case coded::Type::Kind::kProtocol: {
        const auto* protocol_coded_type = static_cast<const coded::ProtocolType*>(coded_type);
        for (const auto* message : protocol_coded_type->messages_after_compile) {
          switch (message->kind) {
            case coded::Type::Kind::kStruct: {
              GenerateForward(*static_cast<const coded::StructType*>(message));
              break;
            }
            case coded::Type::Kind::kTable: {
              GenerateForward(*static_cast<const coded::TableType*>(message));
              break;
            }
            case coded::Type::Kind::kUnion: {
              GenerateForward(*static_cast<const coded::UnionType*>(message));
              break;
            }
            default: {
              ZX_PANIC("only structs, tables, and unions may be used as message payloads");
            }
          }
        }
        break;
      }
      default:
        break;
    }
  }

  Emit(&tables_file_, "\n");

  // Generate pointer coding tables necessary for nullable types.
  for (const auto& decl : compilation_->all_libraries_declaration_order) {
    auto coded_type = coded_types_generator->CodedTypeFor(decl->name);
    if (!coded_type)
      continue;
    switch (coded_type->kind) {
      case coded::Type::Kind::kStruct: {
        const auto& struct_type = *static_cast<const coded::StructType*>(coded_type);
        if (auto pointer_type = struct_type.maybe_reference_type; pointer_type) {
          Generate(*pointer_type);
        }
        break;
      }
      case coded::Type::Kind::kUnion: {
        // Nullable unions have the same wire representation as non-nullable ones,
        // hence have the same fields and dependencies in their coding tables.
        // As such, we will generate them in the next phase, to maintain the correct
        // declaration order.
        break;
      }
      default:
        break;
    }
  }

  Emit(&tables_file_, "\n");

  // Generate coding table definitions for unnamed declarations.
  // These are composed in an ad-hoc way in FIDL source, hence we generate "static" coding tables
  // local to the translation unit.
  for (const auto& coded_type : coded_types_generator->coded_types()) {
    if (!coded_type->is_coding_needed)
      continue;

    switch (coded_type->kind) {
      case coded::Type::Kind::kEnum:
      case coded::Type::Kind::kBits:
      case coded::Type::Kind::kStruct:
      case coded::Type::Kind::kTable:
      case coded::Type::Kind::kStructPointer:
      case coded::Type::Kind::kUnion:
      case coded::Type::Kind::kOverlay:
        // These are generated in the next phase.
        break;
      case coded::Type::Kind::kProtocol:
        // Nothing to generate for protocols. We've already moved the
        // messages from the protocol into coded_types_ directly.
        break;
      case coded::Type::Kind::kHandle:
        Generate(*static_cast<const coded::HandleType*>(coded_type.get()));
        break;
      case coded::Type::Kind::kProtocolHandle:
        Generate(*static_cast<const coded::ProtocolHandleType*>(coded_type.get()));
        break;
      case coded::Type::Kind::kRequestHandle:
        Generate(*static_cast<const coded::RequestHandleType*>(coded_type.get()));
        break;
      case coded::Type::Kind::kArray:
        Generate(*static_cast<const coded::ArrayType*>(coded_type.get()));
        break;
      case coded::Type::Kind::kString:
        Generate(*static_cast<const coded::StringType*>(coded_type.get()));
        break;
      case coded::Type::Kind::kVector:
        Generate(*static_cast<const coded::VectorType*>(coded_type.get()));
        break;
      case coded::Type::Kind::kZxExperimentalPointer:
        Generate(*static_cast<const coded::ZxExperimentalPointerType*>(coded_type.get()));
        break;
      case coded::Type::Kind::kPrimitive:
      case coded::Type::Kind::kInternal:
        // Nothing to generate for primitives and internals. We intern all
        // primitive and internal coding tables, and therefore directly
        // reference them.
        break;
    }
  }

  Emit(&tables_file_, "\n");

  // Generate coding table definitions for named declarations.
  for (const auto& decl : compilation_->declaration_order) {
    const coded::Type* coded_type = coded_types_generator->CodedTypeFor(decl->name);
    if (!coded_type)
      continue;
    switch (coded_type->kind) {
      case coded::Type::Kind::kEnum:
        Generate(*static_cast<const coded::EnumType*>(coded_type));
        break;
      case coded::Type::Kind::kBits:
        Generate(*static_cast<const coded::BitsType*>(coded_type));
        break;
      case coded::Type::Kind::kStruct:
        Generate(*static_cast<const coded::StructType*>(coded_type));
        break;
      case coded::Type::Kind::kTable:
        Generate(*static_cast<const coded::TableType*>(coded_type));
        break;
      case coded::Type::Kind::kUnion: {
        const auto& union_type = *static_cast<const coded::UnionType*>(coded_type);
        Generate(union_type);

        if (union_type.maybe_reference_type)
          Generate(*union_type.maybe_reference_type);

        break;
      }
      case coded::Type::Kind::kProtocol: {
        const auto* protocol_coded_type = static_cast<const coded::ProtocolType*>(coded_type);
        for (const auto* message : protocol_coded_type->messages_after_compile) {
          switch (message->kind) {
            case coded::Type::Kind::kStruct: {
              Generate(*static_cast<const coded::StructType*>(message));
              break;
            }
            case coded::Type::Kind::kTable: {
              Generate(*static_cast<const coded::TableType*>(message));
              break;
            }
            case coded::Type::Kind::kUnion: {
              Generate(*static_cast<const coded::UnionType*>(message));
              break;
            }
            default: {
              ZX_PANIC("only structs, tables, and unions may be used as message payloads");
            }
          }
        }
        break;
      }
      default:
        break;
    }
  }
}

std::ostringstream TablesGenerator::Produce() {
  CodedTypesGenerator ctg_v1(compilation_);
  ctg_v1.CompileCodedTypes();
  Produce(&ctg_v1);

  std::ostringstream result;
  Emit(&result, "// WARNING: This file is machine generated by fidlc.\n\n");
  Emit(&result, "#include <lib/fidl/internal.h>\n\n");
  result << std::move(forward_decls_).str();
  result << "\n";
  result << std::move(tables_file_).str();
  return result;
}

}  // namespace fidl
