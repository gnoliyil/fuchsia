// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fidl/json_generator.h"

#include "fidl/diagnostic_types.h"
#include "fidl/flat/types.h"
#include "fidl/flat_ast.h"
#include "fidl/names.h"
#include "fidl/types.h"

namespace fidl {

void JSONGenerator::Generate(const flat::Decl* decl) { Generate(decl->name); }

void JSONGenerator::Generate(SourceSpan value) { EmitString(value.data()); }

void JSONGenerator::Generate(NameSpan value) {
  if (omit_locations_)
    return;
  GenerateObject([&]() {
    GenerateObjectMember("filename", value.filename, Position::kFirst);
    GenerateObjectMember("line", (uint32_t)value.position.line);
    GenerateObjectMember("column", (uint32_t)value.position.column);
    GenerateObjectMember("length", (uint32_t)value.length);
  });
}

void JSONGenerator::Generate(const flat::ConstantValue& value) {
  switch (value.kind) {
    case flat::ConstantValue::Kind::kUint8: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<uint8_t>&>(value);
      EmitNumeric(static_cast<uint64_t>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kUint16: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<uint16_t>&>(value);
      EmitNumeric(static_cast<uint16_t>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kUint32: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<uint32_t>&>(value);
      EmitNumeric(static_cast<uint32_t>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kUint64: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<uint64_t>&>(value);
      EmitNumeric(static_cast<uint64_t>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kInt8: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<int8_t>&>(value);
      EmitNumeric(static_cast<int64_t>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kInt16: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<int16_t>&>(value);
      EmitNumeric(static_cast<int16_t>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kInt32: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<int32_t>&>(value);
      EmitNumeric(static_cast<int32_t>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kInt64: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<int64_t>&>(value);
      EmitNumeric(static_cast<int64_t>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kFloat32: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<float>&>(value);
      EmitNumeric(static_cast<float>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kFloat64: {
      auto numeric_constant = reinterpret_cast<const flat::NumericConstantValue<double>&>(value);
      EmitNumeric(static_cast<double>(numeric_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kBool: {
      auto bool_constant = reinterpret_cast<const flat::BoolConstantValue&>(value);
      EmitBoolean(static_cast<bool>(bool_constant), kAsString);
      break;
    }
    case flat::ConstantValue::Kind::kDocComment: {
      auto doc_comment_constant = reinterpret_cast<const flat::DocCommentConstantValue&>(value);
      EmitLiteral(doc_comment_constant.MakeContents());
      break;
    }
    case flat::ConstantValue::Kind::kString: {
      auto string_constant = reinterpret_cast<const flat::StringConstantValue&>(value);
      EmitLiteral(string_constant.value);
      break;
    }
  }  // switch
}

void JSONGenerator::Generate(types::HandleSubtype value) { EmitString(NameHandleSubtype(value)); }

void JSONGenerator::Generate(types::Nullability value) {
  switch (value) {
    case types::Nullability::kNullable:
      EmitBoolean(true);
      break;
    case types::Nullability::kNonnullable:
      EmitBoolean(false);
      break;
  }
}

void JSONGenerator::Generate(const raw::Identifier& value) { EmitString(value.span().data()); }

void JSONGenerator::Generate(const flat::LiteralConstant& value) {
  GenerateObject([&]() {
    GenerateObjectMember("kind", NameRawLiteralKind(value.literal->kind), Position::kFirst);
    GenerateObjectMember("value", value.Value());
    GenerateObjectMember("expression", value.literal->span().data());
  });
}

void JSONGenerator::Generate(const flat::Constant& value) {
  GenerateObject([&]() {
    // TODO(pascallouis): We should explore exposing these in the JSON IR, such that the
    // implicit bounds are made explicit by fidlc, rather than sprinkled throughout all
    // backends.
    //
    // For now, do not emit synthesized constants
    if (value.kind == flat::Constant::Kind::kSynthesized)
      return;
    GenerateObjectMember("kind", NameFlatConstantKind(value.kind), Position::kFirst);
    GenerateObjectMember("value", value.Value());
    GenerateObjectMember("expression", value.span);
    switch (value.kind) {
      case flat::Constant::Kind::kIdentifier: {
        auto type = static_cast<const flat::IdentifierConstant*>(&value);
        GenerateObjectMember("identifier", type->name);
        break;
      }
      case flat::Constant::Kind::kLiteral: {
        auto& type = static_cast<const flat::LiteralConstant&>(value);
        GenerateObjectMember("literal", type);
        break;
      }
      case flat::Constant::Kind::kBinaryOperator: {
        // Avoid emitting a structure for binary operators in favor of "expression".
        break;
      }
      case flat::Constant::Kind::kSynthesized:
        break;
    }
  });
}

void JSONGenerator::Generate(const flat::Type* value) {
  GenerateObject([&]() {
    GenerateObjectMember("kind", NameFlatTypeKind(value->kind), Position::kFirst);

    switch (value->kind) {
      case flat::Type::Kind::kArray: {
        auto type = static_cast<const flat::ArrayType*>(value);
        GenerateObjectMember("element_type", type->element_type);
        GenerateObjectMember("element_count", type->element_count->value);
        break;
      }
      case flat::Type::Kind::kVector: {
        // This code path should only be exercised if the type is "bytes." All
        // other handling of kVector is handled in GenerateParameterizedType.
        auto type = static_cast<const flat::VectorType*>(value);
        GenerateObjectMember("element_type", type->element_type);
        if (*type->element_count < flat::Size::Max())
          GenerateObjectMember("maybe_element_count", type->element_count->value);
        GenerateObjectMember("nullable", type->nullability);
        break;
      }
      case flat::Type::Kind::kString: {
        auto type = static_cast<const flat::StringType*>(value);
        if (*type->max_size < flat::Size::Max())
          GenerateObjectMember("maybe_element_count", type->max_size->value);
        GenerateObjectMember("nullable", type->nullability);
        break;
      }
      case flat::Type::Kind::kHandle: {
        auto type = static_cast<const flat::HandleType*>(value);
        GenerateObjectMember("obj_type", type->obj_type);
        GenerateObjectMember("subtype", type->subtype);
        GenerateObjectMember(
            "rights",
            static_cast<const flat::NumericConstantValue<uint32_t>*>(type->rights)->value);
        GenerateObjectMember("nullable", type->nullability);
        break;
      }
      case flat::Type::Kind::kPrimitive: {
        auto type = static_cast<const flat::PrimitiveType*>(value);
        GenerateObjectMember("subtype", type->name);
        break;
      }
      case flat::Type::Kind::kIdentifier: {
        auto type = static_cast<const flat::IdentifierType*>(value);
        GenerateObjectMember("identifier", type->name);
        GenerateObjectMember("nullable", type->nullability);
        break;
      }
      default: {
        assert(false &&
               "expected non-parameterized type (neither array<T>, vector<T>, nor request<P>)");
      }
    }
  });
}

void JSONGenerator::Generate(const flat::Attribute& value) {
  GenerateObject([&]() {
    // TODO(fxbug.dev/74955): force lower_snake_case in a subsequent CL.
    auto name = value.name;
    if (value.syntax == utils::Syntax::kNew) {
      name = fidl::utils::to_upper_camel_case(name);
    }
    GenerateObjectMember("name", name, Position::kFirst);

    // TODO(fxbug.dev/74955): the rest of this block currently assumes a single
    //  string argument, as is the case in the old syntax. This should be
    //  expanded to parse multiple arguments, and produce an "arguments" JSON
    //  field, to support the new syntax.
    if (value.args.size() > 1) {
      assert(value.args.size() <= 1 && "multiple argument attributes not yet supported");
    }

    std::string attribute_value;
    auto arg_constant = value.GetArg();
    if (arg_constant.has_value()) {
      switch (arg_constant.value().get().kind) {
        case flat::ConstantValue::Kind::kDocComment: {
          auto arg_value =
              static_cast<const flat::DocCommentConstantValue&>(arg_constant.value().get());
          GenerateObjectMember("value", arg_value.MakeContents());
          break;
        }
        case flat::ConstantValue::Kind::kString: {
          auto arg_value =
              static_cast<const flat::StringConstantValue&>(arg_constant.value().get());
          GenerateObjectMember("value", arg_value.MakeContents());
          break;
        }
        default: {
          assert(false && "non-string/doc-comment attribute arguments not yet supported");
        }
      }
    } else {
      GenerateObjectMember("value", std::string_view());
    }
  });
}

void JSONGenerator::Generate(const flat::AttributeList& value) { Generate(value.attributes); }

void JSONGenerator::Generate(const raw::Ordinal64& value) { EmitNumeric(value.value); }

void JSONGenerator::Generate(const flat::Name& value) {
  // These look like (when there is a library)
  //     { "LIB.LIB.LIB", "ID" }
  // or (when there is not)
  //     { "ID" }
  Generate(NameFlatName(value));
}

void JSONGenerator::Generate(const flat::Bits& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    // TODO(fxbug.dev/75895): refactor this to only have a single null state.
    if (value.attributes && !value.attributes->attributes.empty())
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateTypeAndFromTypeAlias(value.subtype_ctor);
    // TODO(fxbug.dev/7660): When all numbers are wrapped as string, we can simply
    // call GenerateObjectMember directly.
    GenerateObjectPunctuation(Position::kSubsequent);
    EmitObjectKey("mask");
    EmitNumeric(value.mask, kAsString);
    GenerateObjectMember("members", value.members);
    GenerateObjectMember("strict", value.strictness == types::Strictness::kStrict);
  });
}

void JSONGenerator::Generate(const flat::Bits::Member& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    GenerateObjectMember("value", value.value);
    if (value.attributes && !value.attributes->attributes.empty())
      GenerateObjectMember("maybe_attributes", value.attributes);
  });
}

void JSONGenerator::Generate(const flat::Const& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes && !value.attributes->attributes.empty())
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateTypeAndFromTypeAlias(value.type_ctor);
    GenerateObjectMember("value", value.value);
  });
}

void JSONGenerator::Generate(const flat::Enum& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes && !value.attributes->attributes.empty())
      GenerateObjectMember("maybe_attributes", value.attributes);
    // TODO(fxbug.dev/7660): Due to legacy reasons, the 'type' of enums is actually
    // the primitive subtype, and therefore cannot use
    // GenerateTypeAndFromTypeAlias here.
    GenerateObjectMember("type", value.type->name);
    GenerateExperimentalMaybeFromTypeAlias(flat::GetLayoutInvocation(value.subtype_ctor));
    GenerateObjectMember("members", value.members);
    GenerateObjectMember("strict", value.strictness == types::Strictness::kStrict);
    if (value.strictness == types::Strictness::kFlexible) {
      if (value.unknown_value_signed) {
        GenerateObjectMember("maybe_unknown_value", value.unknown_value_signed.value());
      } else {
        GenerateObjectMember("maybe_unknown_value", value.unknown_value_unsigned.value());
      }
    }
  });
}

void JSONGenerator::Generate(const flat::Enum::Member& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    GenerateObjectMember("value", value.value);
    if (value.attributes && !value.attributes->attributes.empty())
      GenerateObjectMember("maybe_attributes", value.attributes);
  });
}

void JSONGenerator::Generate(const flat::Protocol& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes && !value.attributes->attributes.empty())
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateObjectMember("methods", value.all_methods);
  });
}

void JSONGenerator::Generate(const flat::Protocol::MethodWithInfo& method_with_info) {
  assert(method_with_info.method != nullptr);
  const auto& value = *method_with_info.method;
  GenerateObject([&]() {
    GenerateObjectMember("ordinal", value.generated_ordinal64, Position::kFirst);
    GenerateObjectMember("name", value.name);
    GenerateObjectMember("location", NameSpan(value.name));
    GenerateObjectMember("has_request", value.maybe_request != nullptr);
    if (value.attributes && !value.attributes->attributes.empty())
      GenerateObjectMember("maybe_attributes", value.attributes);
    if (value.maybe_request != nullptr) {
      GenerateRequest("maybe_request", *value.maybe_request);
    }
    GenerateObjectMember("has_response", value.maybe_response != nullptr);
    if (value.maybe_response != nullptr) {
      GenerateRequest("maybe_response", *value.maybe_response);
    }
    GenerateObjectMember("is_composed", method_with_info.is_composed);
  });
}

void JSONGenerator::GenerateTypeAndFromTypeAlias(const flat::TypeConstructor& value,
                                                 Position position) {
  GenerateTypeAndFromTypeAlias(TypeKind::kConcrete, flat::GetTypeCtorAsPtr(value), position);
}

bool ShouldExposeTypeAliasOfParametrizedType(const flat::Type& type) {
  // TODO(fxbug.dev/68667): The last of these three conditionals will need to
  //  change slightly for the new syntax once client and server ends are added.
  return type.kind == flat::Type::Kind::kArray || type.kind == flat::Type::Kind::kVector ||
         type.kind == flat::Type::Kind::kRequestHandle;
}

void JSONGenerator::GenerateTypeAndFromTypeAlias(TypeKind parent_type_kind,
                                                 flat::TypeConstructorPtr value,
                                                 Position position) {
  const auto* type = flat::GetType(value);
  const auto& invocation = flat::GetLayoutInvocation(value);
  if (fidl::ShouldExposeTypeAliasOfParametrizedType(*type)) {
    const auto& invocation = flat::GetLayoutInvocation(value);
    if (invocation.from_type_alias) {
      GenerateParameterizedType(
          parent_type_kind, type,
          flat::GetTypeCtorAsPtr(invocation.from_type_alias->partial_type_ctor), position);
    } else {
      GenerateParameterizedType(parent_type_kind, type, value, position);
    }
    GenerateExperimentalMaybeFromTypeAlias(invocation);
    return;
  }

  std::string key = parent_type_kind == TypeKind::kConcrete ? "type" : "element_type";
  GenerateObjectMember(key, type, position);
  GenerateExperimentalMaybeFromTypeAlias(invocation);
}

void JSONGenerator::GenerateExperimentalMaybeFromTypeAlias(
    const flat::LayoutInvocation& invocation) {
  if (invocation.from_type_alias)
    GenerateObjectMember("experimental_maybe_from_type_alias", invocation);
}

void JSONGenerator::GenerateParameterizedType(TypeKind parent_type_kind, const flat::Type* type,
                                              flat::TypeConstructorPtr type_ctor,
                                              Position position) {
  const auto& invocation = flat::GetLayoutInvocation(type_ctor);
  std::string key = parent_type_kind == TypeKind::kConcrete ? "type" : "element_type";

  // Special case: type "bytes" is a builtin alias, so it will have no
  // user-specified arg type.
  if (!flat::IsTypeConstructorDefined(invocation.element_type_raw)) {
    GenerateObjectMember(key, type, position);
    return;
  }

  GenerateObjectPunctuation(position);
  EmitObjectKey(key);
  GenerateObject([&]() {
    GenerateObjectMember("kind", NameFlatTypeKind(type->kind), Position::kFirst);

    switch (type->kind) {
      case flat::Type::Kind::kArray: {
        auto array_type = static_cast<const flat::ArrayType*>(type);
        GenerateTypeAndFromTypeAlias(TypeKind::kParameterized, invocation.element_type_raw);
        GenerateObjectMember("element_count", array_type->element_count->value);
        break;
      }
      case flat::Type::Kind::kVector: {
        auto vector_type = static_cast<const flat::VectorType*>(type);
        GenerateTypeAndFromTypeAlias(TypeKind::kParameterized, invocation.element_type_raw);
        if (*vector_type->element_count < flat::Size::Max())
          GenerateObjectMember("maybe_element_count", vector_type->element_count->value);
        GenerateObjectMember("nullable", vector_type->nullability);
        break;
      }
      case flat::Type::Kind::kRequestHandle: {
        auto request_type = static_cast<const flat::RequestHandleType*>(type);
        GenerateObjectMember("subtype", request_type->protocol_type->name);
        if (flat::IsTypeConstructorDefined(invocation.element_type_raw)) {
          GenerateExperimentalMaybeFromTypeAlias(
              flat::GetLayoutInvocation(invocation.element_type_raw));
        }
        // TODO(fxbug.dev/43803): Add required and optional rights.
        GenerateObjectMember("nullable", request_type->nullability);
        break;
      }
      default: {
        assert(false && "expected parameterized type (either array<T>, vector<T>, or request<P>)");
      }
    }
  });
}

void JSONGenerator::GenerateRequest(const std::string& prefix, const flat::Struct& value) {
  // Temporarily hardcode the generation of request/response struct members to use the old
  // wire format, in order to maintain compatibility during the transition for fxbug.dev/7704.
  // This block of code is copied from JsonWriter::GenerateArray (with the difference
  // noted below), and will be removed once backends are updated to use anonymous structs.
  GenerateObjectPunctuation(Position::kSubsequent);
  EmitObjectKey(prefix);
  EmitArrayBegin();
  if (value.members.begin() != value.members.end()) {
    Indent();
    EmitNewlineWithIndent();
  }
  for (auto it = value.members.begin(); it != value.members.end(); ++it) {
    if (it != value.members.begin())
      EmitArraySeparator();
    // call Generate with is_request_response = true on each struct member
    Generate(*it, true);
  }
  if (value.members.begin() != value.members.end()) {
    Outdent();
    EmitNewlineWithIndent();
  }
  EmitArrayEnd();

  if (!value.members.empty()) {
    GenerateObjectMember(prefix + "_payload", value.name);
  }
  GenerateTypeShapes(prefix, value, true);
}

void JSONGenerator::Generate(const flat::Resource::Property& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    GenerateTypeAndFromTypeAlias(value.type_ctor);
    if (value.attributes && !value.attributes->attributes.empty())
      GenerateObjectMember("maybe_attributes", value.attributes);
  });
}

void JSONGenerator::Generate(const flat::Resource& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes && !value.attributes->attributes.empty())
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateTypeAndFromTypeAlias(value.subtype_ctor);
    GenerateObjectMember("properties", value.properties);
  });
}

void JSONGenerator::Generate(const flat::Service& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes && !value.attributes->attributes.empty())
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateObjectMember("members", value.members);
  });
}

void JSONGenerator::Generate(const flat::Service::Member& value) {
  GenerateObject([&]() {
    GenerateTypeAndFromTypeAlias(value.type_ctor, Position::kFirst);
    GenerateObjectMember("name", value.name);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes && !value.attributes->attributes.empty())
      GenerateObjectMember("maybe_attributes", value.attributes);
  });
}

void JSONGenerator::Generate(const flat::Struct& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    GenerateObjectMember("anonymous", value.is_request_or_response);
    if (value.attributes && !value.attributes->attributes.empty())
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateObjectMember("members", value.members);
    GenerateObjectMember("resource", value.resourceness == types::Resourceness::kResource);
    GenerateTypeShapes(value);
  });
}

void JSONGenerator::Generate(const flat::Struct* value) { Generate(*value); }

void JSONGenerator::Generate(const flat::Struct::Member& value, bool is_request_or_response) {
  GenerateObject([&]() {
    GenerateTypeAndFromTypeAlias(value.type_ctor, Position::kFirst);
    GenerateObjectMember("name", value.name);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes && !value.attributes->attributes.empty())
      GenerateObjectMember("maybe_attributes", value.attributes);
    if (value.maybe_default_value)
      GenerateObjectMember("maybe_default_value", value.maybe_default_value);
    GenerateFieldShapes(value, is_request_or_response);
  });
}

void JSONGenerator::Generate(const flat::Table& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes && !value.attributes->attributes.empty())
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateObjectMember("members", value.members);
    GenerateObjectMember("strict", value.strictness == types::Strictness::kStrict);
    GenerateObjectMember("resource", value.resourceness == types::Resourceness::kResource);
    GenerateTypeShapes(value);
  });
}

void JSONGenerator::Generate(const flat::Table::Member& value) {
  GenerateObject([&]() {
    GenerateObjectMember("ordinal", *value.ordinal, Position::kFirst);
    if (value.maybe_used) {
      assert(!value.span);
      GenerateObjectMember("reserved", false);
      GenerateTypeAndFromTypeAlias(value.maybe_used->type_ctor);
      GenerateObjectMember("name", value.maybe_used->name);
      GenerateObjectMember("location", NameSpan(value.maybe_used->name));
      if (value.maybe_used->attributes && !value.maybe_used->attributes->attributes.empty())
        GenerateObjectMember("maybe_attributes", value.maybe_used->attributes);
      // TODO(fxbug.dev/7932): Support defaults on tables.
    } else {
      assert(value.span);
      GenerateObjectMember("reserved", true);
      GenerateObjectMember("location", NameSpan(value.span.value()));
    }
  });
}

void JSONGenerator::Generate(const TypeShape& type_shape) {
  GenerateObject([&]() {
    GenerateObjectMember("inline_size", type_shape.inline_size, Position::kFirst);
    GenerateObjectMember("alignment", type_shape.alignment);
    GenerateObjectMember("depth", type_shape.depth);
    GenerateObjectMember("max_handles", type_shape.max_handles);
    GenerateObjectMember("max_out_of_line", type_shape.max_out_of_line);
    GenerateObjectMember("has_padding", type_shape.has_padding);
    GenerateObjectMember("has_flexible_envelope", type_shape.has_flexible_envelope);
  });
}

void JSONGenerator::Generate(const FieldShape& field_shape) {
  GenerateObject([&]() {
    GenerateObjectMember("offset", field_shape.offset, Position::kFirst);
    GenerateObjectMember("padding", field_shape.padding);
  });
}

void JSONGenerator::Generate(const flat::Union& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes && !value.attributes->attributes.empty())
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateObjectMember("members", value.members);
    GenerateObjectMember("strict", value.strictness == types::Strictness::kStrict);
    GenerateObjectMember("resource", value.resourceness == types::Resourceness::kResource);
    GenerateTypeShapes(value);
  });
}

void JSONGenerator::Generate(const flat::Union::Member& value) {
  GenerateObject([&]() {
    GenerateObjectMember("ordinal", value.ordinal, Position::kFirst);
    if (value.maybe_used) {
      assert(!value.span);
      GenerateObjectMember("reserved", false);
      GenerateObjectMember("name", value.maybe_used->name);
      GenerateTypeAndFromTypeAlias(value.maybe_used->type_ctor);
      GenerateObjectMember("location", NameSpan(value.maybe_used->name));
      if (value.maybe_used->attributes && !value.maybe_used->attributes->attributes.empty())
        GenerateObjectMember("maybe_attributes", value.maybe_used->attributes);
    } else {
      GenerateObjectMember("reserved", true);
      GenerateObjectMember("location", NameSpan(value.span.value()));
    }
  });
}

void JSONGenerator::Generate(const flat::LayoutInvocation& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.from_type_alias->name, Position::kFirst);
    GenerateObjectPunctuation(Position::kSubsequent);
    EmitObjectKey("args");

    // In preparation of template support, it is better to expose a
    // heterogeneous argument list to backends, rather than the currently
    // limited internal view.
    EmitArrayBegin();
    if (value.element_type_resolved) {
      Indent();
      EmitNewlineWithIndent();
      Generate(flat::GetName(value.element_type_raw));
      Outdent();
      EmitNewlineWithIndent();
    }
    EmitArrayEnd();

    GenerateObjectMember("nullable", value.nullability);

    if (value.size_resolved)
      GenerateObjectMember("maybe_size", *value.size_resolved);
  });
}

void JSONGenerator::Generate(const flat::TypeConstructor& value) {
  GenerateTypeCtor(GetTypeCtorAsPtr(value));
}

void JSONGenerator::GenerateTypeCtor(const flat::TypeConstructorPtr& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", GetType(value) ? GetType(value)->name : GetName(value),
                         Position::kFirst);
    GenerateObjectPunctuation(Position::kSubsequent);
    EmitObjectKey("args");
    const auto& invocation = flat::GetLayoutInvocation(value);

    // In preparation of template support, it is better to expose a
    // heterogenous argument list to backends, rather than the currently
    // limited internal view.
    EmitArrayBegin();
    if (invocation.element_type_resolved) {
      Indent();
      EmitNewlineWithIndent();
      GenerateTypeCtor(invocation.element_type_raw);
      Outdent();
      EmitNewlineWithIndent();
    }
    EmitArrayEnd();

    GenerateObjectMember("nullable", invocation.nullability);

    if (invocation.size_raw)
      GenerateObjectMember("maybe_size", *invocation.size_raw);
    if (invocation.rights_raw)
      GenerateObjectMember("handle_rights", *invocation.rights_raw);
  });
}

void JSONGenerator::Generate(const flat::TypeAlias& value) {
  GenerateObject([&]() {
    GenerateObjectMember("name", value.name, Position::kFirst);
    GenerateObjectMember("location", NameSpan(value.name));
    if (value.attributes && !value.attributes->attributes.empty())
      GenerateObjectMember("maybe_attributes", value.attributes);
    GenerateObjectMember("partial_type_ctor", value.partial_type_ctor);
  });
}

void JSONGenerator::Generate(const flat::Library* library) {
  GenerateObject([&]() {
    auto library_name = flat::LibraryName(library, ".");
    GenerateObjectMember("name", library_name, Position::kFirst);
    GenerateExternalDeclarationsMember(library);
  });
}

void JSONGenerator::GenerateTypeShapes(const flat::Object& object) {
  GenerateTypeShapes("", object);
}

void JSONGenerator::GenerateTypeShapes(std::string prefix, const flat::Object& object,
                                       bool is_request_or_response) {
  if (prefix.size() > 0) {
    prefix.push_back('_');
  }

  // NOTE: while the transition for fxbug.dev/7024 is ongoing, we need to treat request/responses
  // specially as before, but this will be removed once the transition is complete
  const auto& v1 = is_request_or_response ? WireFormat::kV1NoEe : WireFormat::kV1Header;
  GenerateObjectMember(prefix + "type_shape_v1", TypeShape(object, v1));
}

void JSONGenerator::GenerateFieldShapes(const flat::Struct::Member& struct_member,
                                        bool is_request_or_response) {
  // NOTE: while the transition for fxbug.dev/7024 is ongoing, we need to treat request/responses
  // specially as before, but this will be removed once the transition is complete
  const auto& v1 = is_request_or_response ? WireFormat::kV1NoEe : WireFormat::kV1Header;
  GenerateObjectMember("field_shape_v1", FieldShape(struct_member, v1));
}

void JSONGenerator::GenerateDeclarationsEntry(int count, const flat::Name& name,
                                              std::string_view decl_kind) {
  if (count == 0) {
    Indent();
    EmitNewlineWithIndent();
  } else {
    EmitObjectSeparator();
  }
  EmitObjectKey(NameFlatName(name));
  EmitString(decl_kind);
}

void JSONGenerator::GenerateDeclarationsMember(const flat::Library* library, Position position) {
  GenerateObjectPunctuation(position);
  EmitObjectKey("declarations");
  GenerateObject([&]() {
    int count = 0;
    for (const auto& decl : library->bits_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "bits");

    for (const auto& decl : library->const_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "const");

    for (const auto& decl : library->enum_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "enum");

    for (const auto& decl : library->resource_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "experimental_resource");

    for (const auto& decl : library->protocol_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "interface");

    for (const auto& decl : library->service_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "service");

    for (const auto& decl : library->struct_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "struct");

    for (const auto& decl : library->table_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "table");

    for (const auto& decl : library->union_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "union");

    for (const auto& decl : library->type_alias_declarations_)
      GenerateDeclarationsEntry(count++, decl->name, "type_alias");
  });
}

void JSONGenerator::GenerateExternalDeclarationsEntry(
    int count, const flat::Name& name, std::string_view decl_kind,
    std::optional<types::Resourceness> maybe_resourceness) {
  if (count == 0) {
    Indent();
    EmitNewlineWithIndent();
  } else {
    EmitObjectSeparator();
  }
  EmitObjectKey(NameFlatName(name));
  GenerateObject([&]() {
    GenerateObjectMember("kind", decl_kind, Position::kFirst);
    if (maybe_resourceness) {
      GenerateObjectMember("resource", *maybe_resourceness == types::Resourceness::kResource);
    }
  });
}

void JSONGenerator::GenerateExternalDeclarationsMember(const flat::Library* library,
                                                       Position position) {
  GenerateObjectPunctuation(position);
  EmitObjectKey("declarations");
  GenerateObject([&]() {
    int count = 0;
    for (const auto& decl : library->bits_declarations_)
      GenerateExternalDeclarationsEntry(count++, decl->name, "bits", std::nullopt);

    for (const auto& decl : library->const_declarations_)
      GenerateExternalDeclarationsEntry(count++, decl->name, "const", std::nullopt);

    for (const auto& decl : library->enum_declarations_)
      GenerateExternalDeclarationsEntry(count++, decl->name, "enum", std::nullopt);

    for (const auto& decl : library->resource_declarations_)
      GenerateExternalDeclarationsEntry(count++, decl->name, "experimental_resource", std::nullopt);

    for (const auto& decl : library->protocol_declarations_)
      GenerateExternalDeclarationsEntry(count++, decl->name, "interface", std::nullopt);

    for (const auto& decl : library->service_declarations_)
      GenerateExternalDeclarationsEntry(count++, decl->name, "service", std::nullopt);

    for (const auto& decl : library->struct_declarations_)
      GenerateExternalDeclarationsEntry(count++, decl->name, "struct", decl->resourceness);

    for (const auto& decl : library->table_declarations_)
      GenerateExternalDeclarationsEntry(count++, decl->name, "table", decl->resourceness);

    for (const auto& decl : library->union_declarations_)
      GenerateExternalDeclarationsEntry(count++, decl->name, "union", decl->resourceness);

    for (const auto& decl : library->type_alias_declarations_)
      GenerateExternalDeclarationsEntry(count++, decl->name, "type_alias", std::nullopt);
  });
}

namespace {

struct LibraryComparator {
  bool operator()(const flat::Library* lhs, const flat::Library* rhs) const {
    assert(!lhs->name().empty());
    assert(!rhs->name().empty());
    return lhs->name() < rhs->name();
  }
};

std::set<const flat::Library*, LibraryComparator> TransitiveDependencies(
    const flat::Library* library) {
  std::set<const flat::Library*, LibraryComparator> dependencies;
  auto add_dependency = [&](const flat::Library* dep_library) {
    if (!dep_library->HasAttribute("Internal")) {
      dependencies.insert(dep_library);
    }
  };
  for (const auto& dep_library : library->dependencies()) {
    add_dependency(dep_library);
  }
  // Discover additional dependencies that are required to support
  // cross-library protocol composition.
  for (const auto& protocol : library->protocol_declarations_) {
    for (const auto method_with_info : protocol->all_methods) {
      if (auto request = method_with_info.method->maybe_request) {
        for (const auto& member : request->members) {
          if (auto dep_library = flat::GetName(member.type_ctor).library()) {
            add_dependency(dep_library);
          }
        }
      }
      if (auto response = method_with_info.method->maybe_response) {
        for (const auto& member : response->members) {
          if (auto dep_library = flat::GetName(member.type_ctor).library()) {
            add_dependency(dep_library);
          }
        }
      }
      add_dependency(method_with_info.method->owning_protocol->name.library());
    }
  }
  dependencies.erase(library);
  return dependencies;
}

// Return all structs that should be emitted in the JSON IR, which consists of
// two parts: all structs from this library (which includes all struct definitions
// and request/response payloads defined in this library), plus any request/response
// payloads defined in other libraries that are composed into a protocol in this
// library.
std::vector<const flat::Struct*> AllStructs(const flat::Library* library) {
  std::vector<const flat::Struct*> all_structs;

  for (const auto& struct_decl : library->struct_declarations_) {
    if (struct_decl->is_request_or_response && struct_decl->members.empty())
      continue;
    all_structs.push_back(struct_decl.get());
  }

  for (const auto& protocol : library->protocol_declarations_) {
    for (const auto method_with_info : protocol->all_methods) {
      // these are already included in the library's struct declarations
      if (!method_with_info.is_composed)
        continue;
      const auto& method = method_with_info.method;
      if (method->maybe_request && !method->maybe_request->members.empty()) {
        all_structs.push_back(method->maybe_request);
      }
      if (method->maybe_response && !method->maybe_response->members.empty()) {
        all_structs.push_back(method->maybe_response);
      }
    }
  }

  return all_structs;
}

}  // namespace

std::ostringstream JSONGenerator::Produce() {
  ResetIndentLevel();
  GenerateObject([&]() {
    GenerateObjectMember("version", std::string_view("0.0.1"), Position::kFirst);

    GenerateObjectMember("name", LibraryName(library_, "."));

    if (auto attributes = library_->attributes(); attributes) {
      GenerateObjectMember("maybe_attributes", *attributes);
    }

    GenerateObjectPunctuation(Position::kSubsequent);
    EmitObjectKey("library_dependencies");
    GenerateArray(TransitiveDependencies(library_));

    GenerateObjectMember("bits_declarations", library_->bits_declarations_);
    GenerateObjectMember("const_declarations", library_->const_declarations_);
    GenerateObjectMember("enum_declarations", library_->enum_declarations_);
    GenerateObjectMember("experimental_resource_declarations", library_->resource_declarations_);
    GenerateObjectMember("interface_declarations", library_->protocol_declarations_);
    GenerateObjectMember("service_declarations", library_->service_declarations_);
    GenerateObjectMember("struct_declarations", AllStructs(library_));
    GenerateObjectMember("table_declarations", library_->table_declarations_);
    GenerateObjectMember("union_declarations", library_->union_declarations_);
    GenerateObjectMember("type_alias_declarations", library_->type_alias_declarations_);

    // The library's declaration_order_ contains all the declarations for all
    // transitive dependencies. The backend only needs the declaration order
    // for this specific library.
    std::vector<std::string> declaration_order;
    for (const flat::Decl* decl : library_->declaration_order_) {
      if (decl->kind == flat::Decl::Kind::kStruct) {
        auto struct_decl = static_cast<const flat::Struct*>(decl);
        if (struct_decl->is_request_or_response)
          continue;
      }
      if (decl->name.library() == library_)
        declaration_order.push_back(NameFlatName(decl->name));
    }
    GenerateObjectMember("declaration_order", declaration_order);

    GenerateDeclarationsMember(library_);
  });
  GenerateEOF();

  return std::move(json_file_);
}

}  // namespace fidl
