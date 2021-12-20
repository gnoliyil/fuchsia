// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fidl/flat_ast.h"

#include <assert.h>
#include <stdio.h>

#include <algorithm>
#include <iostream>
#include <sstream>
#include <type_traits>
#include <utility>

#include "fidl/diagnostic_types.h"
#include "fidl/diagnostics.h"
#include "fidl/experimental_flags.h"
#include "fidl/flat/name.h"
#include "fidl/flat/types.h"
#include "fidl/lexer.h"
#include "fidl/names.h"
#include "fidl/ordinals.h"
#include "fidl/raw_ast.h"
#include "fidl/types.h"
#include "fidl/utils.h"

namespace fidl::flat {

using namespace diagnostics;

namespace {

class ScopeInsertResult {
 public:
  explicit ScopeInsertResult(std::unique_ptr<SourceSpan> previous_occurrence)
      : previous_occurrence_(std::move(previous_occurrence)) {}

  static ScopeInsertResult Ok() { return ScopeInsertResult(nullptr); }
  static ScopeInsertResult FailureAt(SourceSpan previous) {
    return ScopeInsertResult(std::make_unique<SourceSpan>(previous));
  }

  bool ok() const { return previous_occurrence_ == nullptr; }

  const SourceSpan& previous_occurrence() const {
    assert(!ok());
    return *previous_occurrence_;
  }

 private:
  std::unique_ptr<SourceSpan> previous_occurrence_;
};

template <typename T>
class Scope {
 public:
  ScopeInsertResult Insert(const T& t, SourceSpan span) {
    auto iter = scope_.find(t);
    if (iter != scope_.end()) {
      return ScopeInsertResult::FailureAt(iter->second);
    } else {
      scope_.emplace(t, span);
      return ScopeInsertResult::Ok();
    }
  }

  typename std::map<T, SourceSpan>::const_iterator begin() const { return scope_.begin(); }

  typename std::map<T, SourceSpan>::const_iterator end() const { return scope_.end(); }

 private:
  std::map<T, SourceSpan> scope_;
};

using Ordinal64Scope = Scope<uint64_t>;

std::optional<std::pair<uint64_t, SourceSpan>> FindFirstNonDenseOrdinal(
    const Ordinal64Scope& scope) {
  uint64_t last_ordinal_seen = 0;
  for (const auto& ordinal_and_loc : scope) {
    uint64_t next_expected_ordinal = last_ordinal_seen + 1;
    if (ordinal_and_loc.first != next_expected_ordinal) {
      return std::optional{std::make_pair(next_expected_ordinal, ordinal_and_loc.second)};
    }
    last_ordinal_seen = ordinal_and_loc.first;
  }
  return std::nullopt;
}

struct MethodScope {
  Ordinal64Scope ordinals;
  Scope<std::string> canonical_names;
  Scope<const Protocol*> protocols;
};

// A helper class to derive the resourceness of synthesized decls based on their
// members. If the given std::optional<types::Resourceness> is already set
// (meaning the decl is user-defined, not synthesized), this does nothing.
//
// Types added via AddType must already be compiled. In other words, there must
// not be cycles among the synthesized decls.
class DeriveResourceness {
 public:
  explicit DeriveResourceness(std::optional<types::Resourceness>* target)
      : target_(target), derive_(!target->has_value()), result_(types::Resourceness::kValue) {}

  ~DeriveResourceness() {
    if (derive_) {
      *target_ = result_;
    }
  }

  void AddType(const Type* type) {
    if (derive_ && result_ == types::Resourceness::kValue &&
        type->Resourceness() == types::Resourceness::kResource) {
      result_ = types::Resourceness::kResource;
    }
  }

 private:
  std::optional<types::Resourceness>* const target_;
  const bool derive_;
  types::Resourceness result_;
};

// A helper class to track when a Decl is compiling and compiled.
class Compiling {
 public:
  explicit Compiling(Decl* decl) : decl_(decl) { decl_->compiling = true; }

  ~Compiling() {
    decl_->compiling = false;
    decl_->compiled = true;
  }

 private:
  Decl* decl_;
};

}  // namespace

uint32_t PrimitiveType::SubtypeSize(types::PrimitiveSubtype subtype) {
  switch (subtype) {
    case types::PrimitiveSubtype::kBool:
    case types::PrimitiveSubtype::kInt8:
    case types::PrimitiveSubtype::kUint8:
      return 1u;

    case types::PrimitiveSubtype::kInt16:
    case types::PrimitiveSubtype::kUint16:
      return 2u;

    case types::PrimitiveSubtype::kFloat32:
    case types::PrimitiveSubtype::kInt32:
    case types::PrimitiveSubtype::kUint32:
      return 4u;

    case types::PrimitiveSubtype::kFloat64:
    case types::PrimitiveSubtype::kInt64:
    case types::PrimitiveSubtype::kUint64:
      return 8u;
  }
}

const AttributeArg* Attribute::GetArg(std::string_view arg_name) const {
  std::string name = utils::canonicalize(arg_name);
  for (const auto& arg : args) {
    if (arg->name.value().data() == name) {
      return arg.get();
    }
  }
  return nullptr;
}

AttributeArg* Attribute::GetStandaloneAnonymousArg() {
  assert(!compiled &&
         "if calling after attribute compilation, use GetArg(...) with the resolved name instead");
  if (args.size() == 1 && !args[0]->name.has_value()) {
    return args[0].get();
  }
  return nullptr;
}

const Attribute* AttributeList::Get(std::string_view attribute_name) const {
  for (const auto& attribute : attributes) {
    if (attribute->name.data() == attribute_name)
      return attribute.get();
  }
  return nullptr;
}

Attribute* AttributeList::Get(std::string_view attribute_name) {
  for (const auto& attribute : attributes) {
    if (attribute->name.data() == attribute_name)
      return attribute.get();
  }
  return nullptr;
}

std::string Decl::GetName() const { return std::string(name.decl_name()); }

const std::set<std::pair<std::string, std::string_view>> allowed_simple_unions{{
    {"fuchsia.io", "NodeInfo"},
}};

bool IsSimple(const Type* type, Reporter* reporter) {
  auto depth = fidl::OldWireFormatDepth(type);
  switch (type->kind) {
    case Type::Kind::kVector: {
      auto vector_type = static_cast<const VectorType*>(type);
      if (*vector_type->element_count == Size::Max())
        return false;
      switch (vector_type->element_type->kind) {
        case Type::Kind::kHandle:
        case Type::Kind::kTransportSide:
        case Type::Kind::kPrimitive:
          return true;
        case Type::Kind::kArray:
        case Type::Kind::kVector:
        case Type::Kind::kString:
        case Type::Kind::kIdentifier:
        case Type::Kind::kBox:
          return false;
        case Type::Kind::kUntypedNumeric:
          assert(false && "compiler bug: should not have untyped numeric here");
          return false;
      }
    }
    case Type::Kind::kString: {
      auto string_type = static_cast<const StringType*>(type);
      return *string_type->max_size < Size::Max();
    }
    case Type::Kind::kArray:
    case Type::Kind::kHandle:
    case Type::Kind::kTransportSide:
    case Type::Kind::kPrimitive:
      return depth == 0u;
    case Type::Kind::kIdentifier: {
      auto identifier_type = static_cast<const IdentifierType*>(type);
      if (identifier_type->type_decl->kind == Decl::Kind::kUnion) {
        auto union_name = std::make_pair<const std::string&, const std::string_view&>(
            LibraryName(identifier_type->name.library(), "."), identifier_type->name.decl_name());
        if (allowed_simple_unions.find(union_name) == allowed_simple_unions.end()) {
          // Any unions not in the allow-list are treated as non-simple.
          return reporter->Fail(ErrUnionCannotBeSimple, identifier_type->name.span().value(),
                                identifier_type->name);
        }
      }
      // TODO(fxbug.dev/70186): This only applies to nullable structs, which should
      // be handled as box.
      switch (identifier_type->nullability) {
        case types::Nullability::kNullable:
          // If the identifier is nullable, then we can handle a depth of 1
          // because the secondary object is directly accessible.
          return depth <= 1u;
        case types::Nullability::kNonnullable:
          return depth == 0u;
      }
    }
    case Type::Kind::kBox:
      // we can handle a depth of 1 because the secondary object is directly accessible.
      return depth <= 1u;
    case Type::Kind::kUntypedNumeric:
      assert(false && "compiler bug: should not have untyped numeric here");
      return false;
  }
}

FieldShape Struct::Member::fieldshape(WireFormat wire_format) const {
  return FieldShape(*this, wire_format);
}

FieldShape Table::Member::Used::fieldshape(WireFormat wire_format) const {
  return FieldShape(*this, wire_format);
}

FieldShape Union::Member::Used::fieldshape(WireFormat wire_format) const {
  return FieldShape(*this, wire_format);
}

std::vector<std::reference_wrapper<const Union::Member>> Union::MembersSortedByXUnionOrdinal()
    const {
  std::vector<std::reference_wrapper<const Member>> sorted_members(members.cbegin(),
                                                                   members.cend());
  std::sort(sorted_members.begin(), sorted_members.end(),
            [](const auto& member1, const auto& member2) {
              return member1.get().ordinal->value < member2.get().ordinal->value;
            });
  return sorted_members;
}

bool Typespace::Create(const LibraryMediator& lib, const flat::Name& name,
                       const std::unique_ptr<LayoutParameterList>& parameters,
                       const std::unique_ptr<TypeConstraints>& constraints, const Type** out_type,
                       LayoutInvocation* out_params) {
  std::unique_ptr<Type> type;
  if (!CreateNotOwned(lib, name, parameters, constraints, &type, out_params))
    return false;
  types_.push_back(std::move(type));
  *out_type = types_.back().get();
  return true;
}

bool Typespace::CreateNotOwned(const LibraryMediator& lib, const flat::Name& name,
                               const std::unique_ptr<LayoutParameterList>& parameters,
                               const std::unique_ptr<TypeConstraints>& constraints,
                               std::unique_ptr<Type>* out_type, LayoutInvocation* out_params) {
  // TODO(pascallouis): lookup whether we've already created the type, and
  // return it rather than create a new one. Lookup must be by name,
  // arg_type, size, and nullability.

  auto type_template = LookupTemplate(name);
  if (type_template == nullptr) {
    return Fail(ErrUnknownType, name.span().value(), name);
  }
  if (type_template->HasGeneratedName() && (name.as_anonymous() == nullptr)) {
    return Fail(ErrAnonymousNameReference, name.span().value(), name);
  }
  return type_template->Create(lib, {.parameters = parameters, .constraints = constraints},
                               out_type, out_params);
}

const Size* Typespace::InternSize(uint32_t size) {
  sizes_.push_back(std::make_unique<Size>(size));
  return sizes_.back().get();
}

const Type* Typespace::Intern(std::unique_ptr<Type> type) {
  types_.push_back(std::move(type));
  return types_.back().get();
}

void Typespace::AddTemplate(std::unique_ptr<TypeTemplate> type_template) {
  templates_.emplace(type_template->name(), std::move(type_template));
}

const TypeTemplate* Typespace::LookupTemplate(const flat::Name& name) const {
  auto global_name = Name::Key(nullptr, name.decl_name());
  if (auto iter = templates_.find(global_name); iter != templates_.end()) {
    return iter->second.get();
  }

  if (auto iter = templates_.find(name); iter != templates_.end()) {
    return iter->second.get();
  }

  return nullptr;
}

bool TypeTemplate::HasGeneratedName() const { return name_.as_anonymous() != nullptr; }

class PrimitiveTypeTemplate : public TypeTemplate {
 public:
  PrimitiveTypeTemplate(Typespace* typespace, Reporter* reporter, std::string name,
                        types::PrimitiveSubtype subtype)
      : TypeTemplate(Name::CreateIntrinsic(std::move(name)), typespace, reporter),
        subtype_(subtype) {}

  bool Create(const LibraryMediator& lib, const ParamsAndConstraints& unresolved_args,
              std::unique_ptr<Type>* out_type, LayoutInvocation* out_params) const override {
    size_t num_params = unresolved_args.parameters->items.size();
    if (num_params != 0) {
      return Fail(ErrWrongNumberOfLayoutParameters, unresolved_args.parameters->span.value(), this,
                  0, num_params);
    }

    // TODO(fxbug.dev/76219): Should instead use the static const types provided
    // on Typespace, e.g. Typespace::kBoolType.
    PrimitiveType type(name_, subtype_);
    return type.ApplyConstraints(lib, *unresolved_args.constraints, this, out_type, out_params);
  }

 private:
  const types::PrimitiveSubtype subtype_;
};

bool PrimitiveType::ApplyConstraints(const flat::LibraryMediator& lib,
                                     const TypeConstraints& constraints, const TypeTemplate* layout,
                                     std::unique_ptr<Type>* out_type,
                                     LayoutInvocation* out_params) const {
  size_t num_constraints = constraints.items.size();
  // assume that a lone constraint was an attempt at specifying `optional` and provide a more
  // specific error
  // TOOD(fxbug.dev/75112): actually try to compile the optional constraint
  if (num_constraints == 1)
    return lib.Fail(ErrCannotBeNullable, constraints.items[0]->span, layout);
  if (num_constraints > 1)
    return lib.Fail(ErrTooManyConstraints, constraints.span.value(), layout, 0, num_constraints);
  *out_type = std::make_unique<PrimitiveType>(name, subtype);
  return true;
}

class ArrayTypeTemplate final : public TypeTemplate {
 public:
  ArrayTypeTemplate(Typespace* typespace, Reporter* reporter)
      : TypeTemplate(Name::CreateIntrinsic("array"), typespace, reporter) {}

  bool Create(const LibraryMediator& lib, const ParamsAndConstraints& unresolved_args,
              std::unique_ptr<Type>* out_type, LayoutInvocation* out_params) const override {
    size_t num_params = unresolved_args.parameters->items.size();
    size_t expected_params = 2;
    if (num_params != expected_params) {
      return Fail(ErrWrongNumberOfLayoutParameters, unresolved_args.parameters->span.value(), this,
                  expected_params, num_params);
    }

    const Type* element_type = nullptr;
    if (!lib.ResolveParamAsType(this, unresolved_args.parameters->items[0], &element_type))
      return false;
    out_params->element_type_resolved = element_type;
    out_params->element_type_raw = unresolved_args.parameters->items[0]->AsTypeCtor();

    const Size* size = nullptr;
    if (!lib.ResolveParamAsSize(this, unresolved_args.parameters->items[1], &size))
      return false;
    out_params->size_resolved = size;
    out_params->size_raw = unresolved_args.parameters->items[1]->AsConstant();

    ArrayType type(name_, element_type, size);
    return type.ApplyConstraints(lib, *unresolved_args.constraints, this, out_type, out_params);
  }
};

bool ArrayType::ApplyConstraints(const flat::LibraryMediator& lib,
                                 const TypeConstraints& constraints, const TypeTemplate* layout,
                                 std::unique_ptr<Type>* out_type,
                                 LayoutInvocation* out_params) const {
  size_t num_constraints = constraints.items.size();
  // assume that a lone constraint was an attempt at specifying `optional` and provide a more
  // specific error
  // TOOD(fxbug.dev/75112): actually try to compile the optional constraint
  if (num_constraints == 1)
    return lib.Fail(ErrCannotBeNullable, constraints.items[0]->span, layout);
  if (num_constraints > 1)
    return lib.Fail(ErrTooManyConstraints, constraints.span.value(), layout, 0, num_constraints);
  *out_type = std::make_unique<ArrayType>(name, element_type, element_count);
  return true;
}

class BytesTypeTemplate final : public TypeTemplate {
 public:
  BytesTypeTemplate(Typespace* typespace, Reporter* reporter)
      : TypeTemplate(Name::CreateIntrinsic("vector"), typespace, reporter),
        uint8_type_(kUint8Type) {}

  bool Create(const LibraryMediator& lib, const ParamsAndConstraints& unresolved_args,
              std::unique_ptr<Type>* out_type, LayoutInvocation* out_params) const override {
    size_t num_params = unresolved_args.parameters->items.size();
    if (num_params != 0) {
      return Fail(ErrWrongNumberOfLayoutParameters, unresolved_args.parameters->span.value(), this,
                  0, num_params);
    }

    VectorType type(name_, &uint8_type_);
    return type.ApplyConstraints(lib, *unresolved_args.constraints, this, out_type, out_params);
  }

 private:
  // TODO(fxbug.dev/7724): Remove when canonicalizing types.
  const Name kUint8TypeName = Name::CreateIntrinsic("uint8");
  const PrimitiveType kUint8Type = PrimitiveType(kUint8TypeName, types::PrimitiveSubtype::kUint8);

  const PrimitiveType uint8_type_;
};

bool VectorBaseType::ResolveSizeAndNullability(const LibraryMediator& lib,
                                               const TypeConstraints& constraints,
                                               const TypeTemplate* layout,
                                               LayoutInvocation* out_params) {
  size_t num_constraints = constraints.items.size();
  if (num_constraints == 1) {
    LibraryMediator::ResolvedConstraint resolved;
    if (!lib.ResolveConstraintAs(
            constraints.items[0],
            {LibraryMediator::ConstraintKind::kSize, LibraryMediator::ConstraintKind::kNullability},
            nullptr /* resource_decl */, &resolved))
      return lib.Fail(ErrUnexpectedConstraint, constraints.items[0]->span, layout);
    switch (resolved.kind) {
      case LibraryMediator::ConstraintKind::kSize:
        out_params->size_resolved = resolved.value.size;
        out_params->size_raw = constraints.items[0].get();
        break;
      case LibraryMediator::ConstraintKind::kNullability:
        out_params->nullability = types::Nullability::kNullable;
        break;
      default:
        assert(false && "Compiler bug: resolved to wrong constraint kind");
    }
  } else if (num_constraints == 2) {
    // first constraint must be size, followed by optional
    if (!lib.ResolveSizeBound(constraints.items[0].get(), &out_params->size_resolved))
      return lib.Fail(ErrCouldNotParseSizeBound, constraints.items[0]->span);
    out_params->size_raw = constraints.items[0].get();
    if (!lib.ResolveAsOptional(constraints.items[1].get())) {
      return lib.Fail(ErrUnexpectedConstraint, constraints.items[1]->span, layout);
    }
    out_params->nullability = types::Nullability::kNullable;
  } else if (num_constraints >= 3) {
    return lib.Fail(ErrTooManyConstraints, constraints.span.value(), layout, 2, num_constraints);
  }
  return true;
}

const Size VectorBaseType::kMaxSize = Size::Max();

class VectorTypeTemplate final : public TypeTemplate {
 public:
  VectorTypeTemplate(Typespace* typespace, Reporter* reporter)
      : TypeTemplate(Name::CreateIntrinsic("vector"), typespace, reporter) {}

  bool Create(const LibraryMediator& lib, const ParamsAndConstraints& unresolved_args,
              std::unique_ptr<Type>* out_type, LayoutInvocation* out_params) const override {
    size_t num_params = unresolved_args.parameters->items.size();
    if (num_params != 1) {
      return Fail(ErrWrongNumberOfLayoutParameters, unresolved_args.parameters->span.value(), this,
                  1, num_params);
    }

    const Type* element_type = nullptr;
    if (!lib.ResolveParamAsType(this, unresolved_args.parameters->items[0], &element_type))
      return false;
    out_params->element_type_resolved = element_type;
    out_params->element_type_raw = unresolved_args.parameters->items[0]->AsTypeCtor();

    VectorType type(name_, element_type);
    return type.ApplyConstraints(lib, *unresolved_args.constraints, this, out_type, out_params);
  }
};

bool VectorType::ApplyConstraints(const flat::LibraryMediator& lib,
                                  const TypeConstraints& constraints, const TypeTemplate* layout,
                                  std::unique_ptr<Type>* out_type,
                                  LayoutInvocation* out_params) const {
  if (!ResolveSizeAndNullability(lib, constraints, layout, out_params))
    return false;

  bool is_already_nullable = nullability == types::Nullability::kNullable;
  bool is_nullability_applied = out_params->nullability == types::Nullability::kNullable;
  if (is_already_nullable && is_nullability_applied)
    return lib.Fail(ErrCannotIndicateNullabilityTwice, constraints.span.value(), layout);
  auto merged_nullability = is_already_nullable || is_nullability_applied
                                ? types::Nullability::kNullable
                                : types::Nullability::kNonnullable;

  if (element_count != &kMaxSize && out_params->size_resolved)
    return lib.Fail(ErrCannotBoundTwice, constraints.span.value(), layout);
  auto merged_size = out_params->size_resolved ? out_params->size_resolved : element_count;

  *out_type = std::make_unique<VectorType>(name, element_type, merged_size, merged_nullability);
  return true;
}

class StringTypeTemplate final : public TypeTemplate {
 public:
  StringTypeTemplate(Typespace* typespace, Reporter* reporter)
      : TypeTemplate(Name::CreateIntrinsic("string"), typespace, reporter) {}

  bool Create(const LibraryMediator& lib, const ParamsAndConstraints& unresolved_args,
              std::unique_ptr<Type>* out_type, LayoutInvocation* out_params) const override {
    size_t num_params = unresolved_args.parameters->items.size();
    if (num_params != 0) {
      return Fail(ErrWrongNumberOfLayoutParameters, unresolved_args.parameters->span.value(), this,
                  0, num_params);
    }

    StringType type(name_);
    return type.ApplyConstraints(lib, *unresolved_args.constraints, this, out_type, out_params);
  }
};

bool StringType::ApplyConstraints(const flat::LibraryMediator& lib,
                                  const TypeConstraints& constraints, const TypeTemplate* layout,
                                  std::unique_ptr<Type>* out_type,
                                  LayoutInvocation* out_params) const {
  if (!ResolveSizeAndNullability(lib, constraints, layout, out_params))
    return false;

  bool is_already_nullable = nullability == types::Nullability::kNullable;
  bool is_nullability_applied = out_params->nullability == types::Nullability::kNullable;
  if (is_already_nullable && is_nullability_applied)
    return lib.Fail(ErrCannotIndicateNullabilityTwice, constraints.span.value(), layout);
  auto merged_nullability = is_already_nullable || is_nullability_applied
                                ? types::Nullability::kNullable
                                : types::Nullability::kNonnullable;

  if (max_size != &kMaxSize && out_params->size_resolved)
    return lib.Fail(ErrCannotBoundTwice, constraints.span.value(), layout);
  auto merged_size = out_params->size_resolved ? out_params->size_resolved : max_size;

  *out_type = std::make_unique<StringType>(name, merged_size, merged_nullability);
  return true;
}

class HandleTypeTemplate final : public TypeTemplate {
 public:
  HandleTypeTemplate(Name name, Typespace* typespace, Reporter* reporter, Resource* resource_decl_)
      : TypeTemplate(std::move(name), typespace, reporter), resource_decl_(resource_decl_) {}

  bool Create(const LibraryMediator& lib, const ParamsAndConstraints& unresolved_args,
              std::unique_ptr<Type>* out_type, LayoutInvocation* out_params) const override {
    size_t num_params = !unresolved_args.parameters->items.empty();
    if (num_params != 0) {
      return Fail(ErrWrongNumberOfLayoutParameters, unresolved_args.parameters->span.value(), this,
                  0, num_params);
    }

    HandleType type(name_, resource_decl_);
    return type.ApplyConstraints(lib, *unresolved_args.constraints, this, out_type, out_params);
  }

 private:
  const static HandleRights kSameRights;

  Resource* resource_decl_;
};

const HandleRights HandleType::kSameRights = HandleRights(kHandleSameRights);

bool HandleType::ApplyConstraints(const flat::LibraryMediator& lib,
                                  const TypeConstraints& constraints, const TypeTemplate* layout,
                                  std::unique_ptr<Type>* out_type,
                                  LayoutInvocation* out_params) const {
  assert(resource_decl);

  // We need to store this separately from out_params, because out_params doesn't
  // store the raw Constant that gets resolved to a nullability constraint.
  std::optional<SourceSpan> applied_nullability_span;

  size_t num_constraints = constraints.items.size();
  if (num_constraints == 0) {
    // no constraints: set to default subtype below
  } else if (num_constraints == 1) {
    // lone constraint can be either subtype or optional
    auto constraint_span = constraints.items[0]->span;
    LibraryMediator::ResolvedConstraint resolved;
    if (!lib.ResolveConstraintAs(constraints.items[0],
                                 {LibraryMediator::ConstraintKind::kHandleSubtype,
                                  LibraryMediator::ConstraintKind::kNullability},
                                 resource_decl, &resolved))
      return lib.Fail(ErrUnexpectedConstraint, constraint_span, layout);
    switch (resolved.kind) {
      case LibraryMediator::ConstraintKind::kHandleSubtype:
        out_params->subtype_resolved = resolved.value.handle_subtype;
        out_params->subtype_raw = constraints.items[0].get();
        break;
      case LibraryMediator::ConstraintKind::kNullability:
        out_params->nullability = types::Nullability::kNullable;
        applied_nullability_span = constraint_span;
        break;
      default:
        assert(false && "Compiler bug: resolved to wrong constraint kind");
    }
  } else if (num_constraints == 2) {
    // the first constraint must be subtype
    auto constraint_span = constraints.items[0]->span;
    uint32_t obj_type = 0;
    if (!lib.ResolveAsHandleSubtype(resource_decl, constraints.items[0], &obj_type))
      return lib.Fail(ErrUnexpectedConstraint, constraint_span, layout);
    out_params->subtype_resolved = obj_type;
    out_params->subtype_raw = constraints.items[0].get();

    // the second constraint can either be rights or optional
    constraint_span = constraints.items[1]->span;
    LibraryMediator::ResolvedConstraint resolved;
    if (!lib.ResolveConstraintAs(constraints.items[1],
                                 {LibraryMediator::ConstraintKind::kHandleRights,
                                  LibraryMediator::ConstraintKind::kNullability},
                                 resource_decl, &resolved))
      return lib.Fail(ErrUnexpectedConstraint, constraint_span, layout);
    switch (resolved.kind) {
      case LibraryMediator::ConstraintKind::kHandleRights:
        out_params->rights_resolved = resolved.value.handle_rights;
        out_params->rights_raw = constraints.items[1].get();
        break;
      case LibraryMediator::ConstraintKind::kNullability:
        out_params->nullability = types::Nullability::kNullable;
        applied_nullability_span = constraint_span;
        break;
      default:
        assert(false && "Compiler bug: resolved to wrong constraint kind");
    }
  } else if (num_constraints == 3) {
    // no degrees of freedom: must be subtype, followed by rights, then optional
    uint32_t obj_type = 0;
    if (!lib.ResolveAsHandleSubtype(resource_decl, constraints.items[0], &obj_type))
      return lib.Fail(ErrUnexpectedConstraint, constraints.items[0]->span, layout);
    out_params->subtype_resolved = obj_type;
    out_params->subtype_raw = constraints.items[0].get();
    const HandleRights* rights = nullptr;
    if (!lib.ResolveAsHandleRights(resource_decl, constraints.items[1].get(), &rights))
      return lib.Fail(ErrUnexpectedConstraint, constraints.items[1]->span, layout);
    out_params->rights_resolved = rights;
    out_params->rights_raw = constraints.items[1].get();
    if (!lib.ResolveAsOptional(constraints.items[2].get()))
      return lib.Fail(ErrUnexpectedConstraint, constraints.items[2]->span, layout);
    out_params->nullability = types::Nullability::kNullable;
    applied_nullability_span = constraints.items[2]->span;
  } else {
    return lib.Fail(ErrTooManyConstraints, constraints.span.value(), layout, 3, num_constraints);
  }

  bool has_obj_type = subtype != types::HandleSubtype::kHandle;
  if (has_obj_type && out_params->subtype_resolved)
    return lib.Fail(ErrCannotConstrainTwice, out_params->subtype_raw->span, layout);
  // TODO(fxbug.dev/64629): We need to allow setting a default obj_type in
  // resource_definition declarations rather than hard-coding.
  uint32_t merged_obj_type = obj_type;
  if (out_params->subtype_resolved) {
    merged_obj_type = out_params->subtype_resolved.value();
  }

  bool has_nullability = nullability == types::Nullability::kNullable;
  if (has_nullability && out_params->nullability == types::Nullability::kNullable)
    return lib.Fail(ErrCannotIndicateNullabilityTwice, applied_nullability_span.value(), layout);
  auto merged_nullability =
      has_nullability || out_params->nullability == types::Nullability::kNullable
          ? types::Nullability::kNullable
          : types::Nullability::kNonnullable;

  bool has_rights = rights != &kSameRights;
  if (has_rights && out_params->rights_resolved)
    return lib.Fail(ErrCannotConstrainTwice, out_params->rights_raw->span, layout);
  auto merged_rights = rights;
  if (out_params->rights_resolved) {
    merged_rights = out_params->rights_resolved;
  }

  *out_type = std::make_unique<HandleType>(name, resource_decl, merged_obj_type,
                                           types::HandleSubtype(merged_obj_type), merged_rights,
                                           merged_nullability);
  return true;
}

class TransportSideTypeTemplate final : public TypeTemplate {
 public:
  TransportSideTypeTemplate(Typespace* typespace, Reporter* reporter, TransportSide end)
      : TypeTemplate(end == TransportSide::kClient ? Name::CreateIntrinsic("client_end")
                                                   : Name::CreateIntrinsic("server_end"),
                     typespace, reporter),
        end_(end) {}

  bool Create(const LibraryMediator& lib, const ParamsAndConstraints& unresolved_args,
              std::unique_ptr<Type>* out_type, LayoutInvocation* out_params) const override {
    size_t num_params = !unresolved_args.parameters->items.empty();
    if (num_params != 0) {
      return Fail(ErrWrongNumberOfLayoutParameters, unresolved_args.parameters->span.value(), this,
                  0, num_params);
    }

    TransportSideType type(name_, end_);
    return type.ApplyConstraints(lib, *unresolved_args.constraints, this, out_type, out_params);
  }

 private:
  TransportSide end_;
};

bool TransportSideType::ApplyConstraints(const flat::LibraryMediator& lib,
                                         const TypeConstraints& constraints,
                                         const TypeTemplate* layout,
                                         std::unique_ptr<Type>* out_type,
                                         LayoutInvocation* out_params) const {
  size_t num_constraints = constraints.items.size();

  // We need to store this separately from out_params, because out_params doesn't
  // store the raw Constant that gets resolved to a nullability constraint.
  std::optional<SourceSpan> applied_nullability_span;

  if (num_constraints == 1) {
    // could either be a protocol or optional
    auto constraint_span = constraints.items[0]->span;
    LibraryMediator::ResolvedConstraint resolved;
    if (!lib.ResolveConstraintAs(constraints.items[0],
                                 {LibraryMediator::ConstraintKind::kProtocol,
                                  LibraryMediator::ConstraintKind::kNullability},
                                 /* resource_decl */ nullptr, &resolved))
      return lib.Fail(ErrUnexpectedConstraint, constraint_span, layout);
    switch (resolved.kind) {
      case LibraryMediator::ConstraintKind::kProtocol:
        out_params->protocol_decl = resolved.value.protocol_decl;
        out_params->protocol_decl_raw = constraints.items[0].get();
        break;
      case LibraryMediator::ConstraintKind::kNullability:
        out_params->nullability = types::Nullability::kNullable;
        applied_nullability_span = constraint_span;
        break;
      default:
        assert(false && "Compiler bug: resolved to wrong constraint kind");
    }
  } else if (num_constraints == 2) {
    // first constraint must be protocol
    if (!lib.ResolveAsProtocol(constraints.items[0].get(), &out_params->protocol_decl))
      return lib.Fail(ErrMustBeAProtocol, constraints.items[0]->span, layout);
    out_params->protocol_decl_raw = constraints.items[0].get();

    // second constraint must be optional
    if (!lib.ResolveAsOptional(constraints.items[1].get()))
      return lib.Fail(ErrUnexpectedConstraint, constraints.items[1]->span, layout);
    out_params->nullability = types::Nullability::kNullable;
    applied_nullability_span = constraints.items[1]->span;
  } else if (num_constraints > 2) {
    return lib.Fail(ErrTooManyConstraints, constraints.span.value(), layout, 2, num_constraints);
  }

  if (protocol_decl && out_params->protocol_decl)
    return lib.Fail(ErrCannotConstrainTwice, constraints.items[0]->span, layout);
  if (!protocol_decl && !out_params->protocol_decl)
    return lib.Fail(ErrProtocolConstraintRequired, constraints.span.value(), layout);
  const Decl* merged_protocol = protocol_decl;
  if (out_params->protocol_decl)
    merged_protocol = out_params->protocol_decl;

  bool has_nullability = nullability == types::Nullability::kNullable;
  if (has_nullability && out_params->nullability == types::Nullability::kNullable)
    return lib.Fail(ErrCannotIndicateNullabilityTwice, applied_nullability_span.value(), layout);
  auto merged_nullability =
      has_nullability || out_params->nullability == types::Nullability::kNullable
          ? types::Nullability::kNullable
          : types::Nullability::kNonnullable;

  *out_type = std::make_unique<TransportSideType>(name, merged_protocol, merged_nullability, end);
  return true;
}

class TypeDeclTypeTemplate final : public TypeTemplate {
 public:
  TypeDeclTypeTemplate(Name name, Typespace* typespace, Reporter* reporter, TypeDecl* type_decl)
      : TypeTemplate(std::move(name), typespace, reporter), type_decl_(type_decl) {}

  bool Create(const LibraryMediator& lib, const ParamsAndConstraints& unresolved_args,
              std::unique_ptr<Type>* out_type, LayoutInvocation* out_params) const override {
    if (!type_decl_->compiled && type_decl_->kind != Decl::Kind::kProtocol) {
      if (type_decl_->compiling) {
        type_decl_->recursive = true;
      } else {
        lib.CompileDecl(type_decl_);
      }
    }

    size_t num_params = unresolved_args.parameters->items.size();
    if (num_params != 0) {
      return Fail(ErrWrongNumberOfLayoutParameters, unresolved_args.parameters->span.value(), this,
                  0, num_params);
    }

    IdentifierType type(name_, type_decl_);
    return type.ApplyConstraints(lib, *unresolved_args.constraints, this, out_type, out_params);
  }

 private:
  TypeDecl* type_decl_;
};

bool IdentifierType::ApplyConstraints(const flat::LibraryMediator& lib,
                                      const TypeConstraints& constraints,
                                      const TypeTemplate* layout, std::unique_ptr<Type>* out_type,
                                      LayoutInvocation* out_params) const {
  size_t num_constraints = constraints.items.size();
  switch (type_decl->kind) {
    // These types have no allowed constraints
    case Decl::Kind::kBits:
    case Decl::Kind::kEnum:
    case Decl::Kind::kTable:
      // assume that a lone constraint was an attempt at specifying `optional` and provide a more
      // specific error
      // TOOD(fxbug.dev/75112): actually try to compile the optional constraint
      if (num_constraints == 1)
        return lib.Fail(ErrCannotBeNullable, constraints.items[0]->span, layout);
      if (num_constraints > 1) {
        return lib.Fail(ErrTooManyConstraints, constraints.span.value(), layout, 0,
                        num_constraints);
      }
      break;

    // These types have one allowed constraint (`optional`). For type aliases,
    // we need to allow the possibility that the concrete type does allow `optional`,
    // if it doesn't the Type itself will catch the error.
    case Decl::Kind::kTypeAlias:
    case Decl::Kind::kStruct:
    case Decl::Kind::kUnion:
      if (num_constraints > 1) {
        return lib.Fail(ErrTooManyConstraints, constraints.span.value(), layout, 1,
                        num_constraints);
      }
      break;

    case Decl::Kind::kConst:
    case Decl::Kind::kResource:
      // Cannot have const: entries for constants do not exist in the typespace, so
      // they're caught earlier.
      // Cannot have resource: resource types should have resolved to the HandleTypeTemplate
      assert(false && "Compiler bug: unexpected identifier type decl kind");
      break;

    // TODO(fxbug.dev/75837):
    // These can't be used as types. This will be caught later, in VerifyTypeCategory.
    case Decl::Kind::kService:
    case Decl::Kind::kProtocol:
      break;
  }

  types::Nullability applied_nullability = types::Nullability::kNonnullable;
  if (num_constraints == 1) {
    // must be optional
    if (!lib.ResolveAsOptional(constraints.items[0].get()))
      return lib.Fail(ErrUnexpectedConstraint, constraints.items[0]->span, layout);
    applied_nullability = types::Nullability::kNullable;
  }

  if (nullability == types::Nullability::kNullable &&
      applied_nullability == types::Nullability::kNullable)
    return lib.Fail(ErrCannotIndicateNullabilityTwice, constraints.span.value(), layout);
  auto merged_nullability = nullability;
  if (applied_nullability == types::Nullability::kNullable)
    merged_nullability = applied_nullability;

  out_params->nullability = applied_nullability;
  *out_type = std::make_unique<IdentifierType>(name, type_decl, merged_nullability);
  return true;
}

class TypeAliasTypeTemplate final : public TypeTemplate {
 public:
  TypeAliasTypeTemplate(Name name, Typespace* typespace, Reporter* reporter, TypeAlias* decl)
      : TypeTemplate(std::move(name), typespace, reporter), decl_(decl) {}

  bool Create(const LibraryMediator& lib, const ParamsAndConstraints& unresolved_args,
              std::unique_ptr<Type>* out_type, LayoutInvocation* out_params) const override {
    if (!decl_->compiled) {
      if (decl_->compiling) {
        return FailNoSpan(ErrIncludeCycle);
      }
      lib.CompileDecl(decl_);
    }

    size_t num_params = unresolved_args.parameters->items.size();
    if (num_params != 0) {
      return Fail(ErrWrongNumberOfLayoutParameters, unresolved_args.parameters->span.value(), this,
                  0, num_params);
    }

    // Compilation failed while trying to resolve something farther up the chain;
    // exit early
    if (decl_->partial_type_ctor->type == nullptr)
      return false;
    const auto& aliased_type = decl_->partial_type_ctor->type;
    out_params->from_type_alias = decl_;
    return aliased_type->ApplyConstraints(lib, *unresolved_args.constraints, this, out_type,
                                          out_params);
  }

 private:
  TypeAlias* decl_;
};

class BoxTypeTemplate final : public TypeTemplate {
 public:
  BoxTypeTemplate(Typespace* typespace, Reporter* reporter)
      : TypeTemplate(Name::CreateIntrinsic("box"), typespace, reporter) {}

  static bool IsStruct(const Type* boxed_type) {
    if (!boxed_type || boxed_type->kind != Type::Kind::kIdentifier)
      return false;

    return static_cast<const IdentifierType*>(boxed_type)->type_decl->kind == Decl::Kind::kStruct;
  }

  bool Create(const LibraryMediator& lib, const ParamsAndConstraints& unresolved_args,
              std::unique_ptr<Type>* out_type, LayoutInvocation* out_params) const override {
    size_t num_params = unresolved_args.parameters->items.size();
    if (num_params == 0) {
      return Fail(ErrWrongNumberOfLayoutParameters, unresolved_args.parameters->span.value(), this,
                  1, num_params);
    }

    const Type* boxed_type = nullptr;
    if (!lib.ResolveParamAsType(this, unresolved_args.parameters->items[0], &boxed_type))
      return false;
    if (!IsStruct(boxed_type))
      return FailNoSpan(ErrCannotBeBoxed, boxed_type->name);
    const auto* inner = static_cast<const IdentifierType*>(boxed_type);
    if (inner->nullability == types::Nullability::kNullable) {
      return Fail(ErrBoxedTypeCannotBeNullable, unresolved_args.parameters->items[0]->span);
    }
    // We disallow specifying the boxed type as nullable in FIDL source but
    // then mark the boxed type as nullable, so that internally it shares the
    // same code path as its old syntax equivalent (a nullable struct). This
    // allows us to call `f(type->boxed_type)` wherever we used to call `f(type)`
    // in the old code.
    // As a temporary workaround for piping unconst-ness everywhere or having
    // box types own their own boxed types, we cast away the const to be able
    // to change the boxed type to be mutable.
    auto* mutable_inner = const_cast<IdentifierType*>(inner);
    mutable_inner->nullability = types::Nullability::kNullable;

    out_params->boxed_type_resolved = boxed_type;
    out_params->boxed_type_raw = unresolved_args.parameters->items[0]->AsTypeCtor();

    BoxType type(name_, boxed_type);
    return type.ApplyConstraints(lib, *unresolved_args.constraints, this, out_type, out_params);
  }
};

bool BoxType::ApplyConstraints(const flat::LibraryMediator& lib, const TypeConstraints& constraints,
                               const TypeTemplate* layout, std::unique_ptr<Type>* out_type,
                               LayoutInvocation* out_params) const {
  size_t num_constraints = constraints.items.size();
  // assume that a lone constraint was an attempt at specifying `optional` and provide a more
  // specific error
  // TOOD(fxbug.dev/75112): actually try to compile the optional constraint
  if (num_constraints == 1)
    return lib.Fail(ErrBoxCannotBeNullable, constraints.items[0]->span);
  if (num_constraints > 1)
    return lib.Fail(ErrTooManyConstraints, constraints.span.value(), layout, 0, num_constraints);
  *out_type = std::make_unique<BoxType>(name, boxed_type);
  return true;
}

bool UntypedNumericType::ApplyConstraints(const flat::LibraryMediator& lib,
                                          const TypeConstraints& constraints,
                                          const TypeTemplate* layout,
                                          std::unique_ptr<Type>* out_type,
                                          LayoutInvocation* out_params) const {
  assert(false && "compiler bug: should not have untyped numeric here");
  return false;
}

Typespace Typespace::RootTypes(Reporter* reporter) {
  Typespace root_typespace(reporter);

  auto add_template = [&](std::unique_ptr<TypeTemplate> type_template) {
    const Name& name = type_template->name();
    root_typespace.templates_.emplace(name, std::move(type_template));
  };

  auto add_primitive = [&](std::string name, types::PrimitiveSubtype subtype) {
    add_template(std::make_unique<PrimitiveTypeTemplate>(&root_typespace, reporter, std::move(name),
                                                         subtype));
  };

  add_primitive("bool", types::PrimitiveSubtype::kBool);

  add_primitive("int8", types::PrimitiveSubtype::kInt8);
  add_primitive("int16", types::PrimitiveSubtype::kInt16);
  add_primitive("int32", types::PrimitiveSubtype::kInt32);
  add_primitive("int64", types::PrimitiveSubtype::kInt64);
  add_primitive("uint8", types::PrimitiveSubtype::kUint8);
  add_primitive("uint16", types::PrimitiveSubtype::kUint16);
  add_primitive("uint32", types::PrimitiveSubtype::kUint32);
  add_primitive("uint64", types::PrimitiveSubtype::kUint64);

  add_primitive("float32", types::PrimitiveSubtype::kFloat32);
  add_primitive("float64", types::PrimitiveSubtype::kFloat64);

  // TODO(fxbug.dev/7807): Remove when there is generalized support.
  const static auto kByteName = Name::CreateIntrinsic("byte");
  const static auto kBytesName = Name::CreateIntrinsic("bytes");
  root_typespace.templates_.emplace(
      kByteName, std::make_unique<PrimitiveTypeTemplate>(&root_typespace, reporter, "uint8",
                                                         types::PrimitiveSubtype::kUint8));
  root_typespace.templates_.emplace(kBytesName,
                                    std::make_unique<BytesTypeTemplate>(&root_typespace, reporter));

  add_template(std::make_unique<ArrayTypeTemplate>(&root_typespace, reporter));
  add_template(std::make_unique<VectorTypeTemplate>(&root_typespace, reporter));
  add_template(std::make_unique<StringTypeTemplate>(&root_typespace, reporter));
  add_template(std::make_unique<TransportSideTypeTemplate>(&root_typespace, reporter,
                                                           TransportSide::kServer));
  add_template(std::make_unique<TransportSideTypeTemplate>(&root_typespace, reporter,
                                                           TransportSide::kClient));
  add_template(std::make_unique<BoxTypeTemplate>(&root_typespace, reporter));
  return root_typespace;
}

// static
const Name Typespace::kBoolTypeName = Name::CreateIntrinsic("bool");
const Name Typespace::kInt8TypeName = Name::CreateIntrinsic("int8");
const Name Typespace::kInt16TypeName = Name::CreateIntrinsic("int16");
const Name Typespace::kInt32TypeName = Name::CreateIntrinsic("int32");
const Name Typespace::kInt64TypeName = Name::CreateIntrinsic("int64");
const Name Typespace::kUint8TypeName = Name::CreateIntrinsic("uint8");
const Name Typespace::kUint16TypeName = Name::CreateIntrinsic("uint16");
const Name Typespace::kUint32TypeName = Name::CreateIntrinsic("uint32");
const Name Typespace::kUint64TypeName = Name::CreateIntrinsic("uint64");
const Name Typespace::kFloat32TypeName = Name::CreateIntrinsic("float32");
const Name Typespace::kFloat64TypeName = Name::CreateIntrinsic("float64");
const Name Typespace::kUntypedNumericTypeName = Name::CreateIntrinsic("untyped numeric");
const Name Typespace::kStringTypeName = Name::CreateIntrinsic("string");
const PrimitiveType Typespace::kBoolType =
    PrimitiveType(kBoolTypeName, types::PrimitiveSubtype::kBool);
const PrimitiveType Typespace::kInt8Type =
    PrimitiveType(kInt8TypeName, types::PrimitiveSubtype::kInt8);
const PrimitiveType Typespace::kInt16Type =
    PrimitiveType(kInt16TypeName, types::PrimitiveSubtype::kInt16);
const PrimitiveType Typespace::kInt32Type =
    PrimitiveType(kInt32TypeName, types::PrimitiveSubtype::kInt32);
const PrimitiveType Typespace::kInt64Type =
    PrimitiveType(kInt64TypeName, types::PrimitiveSubtype::kInt64);
const PrimitiveType Typespace::kUint8Type =
    PrimitiveType(kUint8TypeName, types::PrimitiveSubtype::kUint8);
const PrimitiveType Typespace::kUint16Type =
    PrimitiveType(kUint16TypeName, types::PrimitiveSubtype::kUint16);
const PrimitiveType Typespace::kUint32Type =
    PrimitiveType(kUint32TypeName, types::PrimitiveSubtype::kUint32);
const PrimitiveType Typespace::kUint64Type =
    PrimitiveType(kUint64TypeName, types::PrimitiveSubtype::kUint64);
const PrimitiveType Typespace::kFloat32Type =
    PrimitiveType(kFloat32TypeName, types::PrimitiveSubtype::kFloat32);
const PrimitiveType Typespace::kFloat64Type =
    PrimitiveType(kFloat64TypeName, types::PrimitiveSubtype::kFloat64);
const UntypedNumericType Typespace::kUntypedNumericType =
    UntypedNumericType(kUntypedNumericTypeName);
const StringType Typespace::kUnboundedStringType = StringType(
    Typespace::kStringTypeName, &VectorBaseType::kMaxSize, types::Nullability::kNonnullable);

AttributeSchema& AttributeSchema::RestrictTo(std::set<AttributePlacement> placements) {
  assert(!placements.empty() && "must allow some placements");
  assert(kind_ == AttributeSchema::Kind::kValidateOnly ||
         kind_ == AttributeSchema::Kind::kUseEarly ||
         kind_ == AttributeSchema::Kind::kCompileEarly && "wrong kind");
  assert(placement_ == AttributeSchema::Placement::kAnywhere && "already set placements");
  assert(specific_placements_.empty() && "already set placements");
  placement_ = AttributeSchema::Placement::kSpecific;
  specific_placements_ = std::move(placements);
  return *this;
}

AttributeSchema& AttributeSchema::RestrictToAnonymousLayouts() {
  assert(kind_ == AttributeSchema::Kind::kValidateOnly ||
         kind_ == AttributeSchema::Kind::kUseEarly ||
         kind_ == AttributeSchema::Kind::kCompileEarly && "wrong kind");
  assert(placement_ == AttributeSchema::Placement::kAnywhere && "already set placements");
  assert(specific_placements_.empty() && "already set placements");
  placement_ = AttributeSchema::Placement::kAnonymousLayout;
  return *this;
}

AttributeSchema& AttributeSchema::AddArg(AttributeArgSchema arg_schema) {
  assert(kind_ == AttributeSchema::Kind::kValidateOnly ||
         kind_ == AttributeSchema::Kind::kUseEarly ||
         kind_ == AttributeSchema::Kind::kCompileEarly && "wrong kind");
  assert(arg_schemas_.empty() && "can only have one unnamed arg");
  arg_schemas_.emplace(AttributeArg::kDefaultAnonymousName, arg_schema);
  return *this;
}

AttributeSchema& AttributeSchema::AddArg(std::string name, AttributeArgSchema arg_schema) {
  assert(kind_ == AttributeSchema::Kind::kValidateOnly ||
         kind_ == AttributeSchema::Kind::kUseEarly ||
         kind_ == AttributeSchema::Kind::kCompileEarly && "wrong kind");
  [[maybe_unused]] const auto& [it, inserted] =
      arg_schemas_.try_emplace(std::move(name), arg_schema);
  assert(inserted && "duplicate argument name");
  return *this;
}

AttributeSchema& AttributeSchema::Constrain(AttributeSchema::Constraint constraint) {
  assert(constraint != nullptr && "constraint must be non-null");
  assert(constraint_ == nullptr && "already set constraint");
  assert(kind_ == AttributeSchema::Kind::kValidateOnly &&
         "constraints only allowed on kValidateOnly attributes");
  constraint_ = std::move(constraint);
  return *this;
}

AttributeSchema& AttributeSchema::UseEarly() {
  assert(kind_ == AttributeSchema::Kind::kValidateOnly && "already changed kind");
  assert(constraint_ == nullptr && "use-early attribute should not specify constraint");
  kind_ = AttributeSchema::Kind::kUseEarly;
  return *this;
}

AttributeSchema& AttributeSchema::CompileEarly() {
  assert(kind_ == AttributeSchema::Kind::kValidateOnly && "already changed kind");
  assert(constraint_ == nullptr && "compile-early attribute should not specify constraint");
  kind_ = AttributeSchema::Kind::kCompileEarly;
  return *this;
}

AttributeSchema& AttributeSchema::Deprecate() {
  assert(kind_ == AttributeSchema::Kind::kValidateOnly && "wrong kind");
  assert(placement_ == AttributeSchema::Placement::kAnywhere &&
         "deprecated attribute should not specify placement");
  assert(arg_schemas_.empty() && "deprecated attribute should not specify arguments");
  assert(constraint_ == nullptr && "deprecated attribute should not specify constraint");
  kind_ = AttributeSchema::Kind::kDeprecated;
  return *this;
}

// static
const AttributeSchema AttributeSchema::kUserDefined(Kind::kUserDefined);

void AttributeSchema::Validate(Reporter* reporter, const Attribute* attribute,
                               const Attributable* attributable) const {
  switch (kind_) {
    case Kind::kValidateOnly:
      break;
    case Kind::kUseEarly:
    case Kind::kCompileEarly:
      assert(constraint_ == nullptr &&
             "use-early and compile-early schemas should not have a constraint");
      break;
    case Kind::kDeprecated:
      reporter->Fail(ErrDeprecatedAttribute, attribute->span, attribute);
      return;
    case Kind::kUserDefined:
      return;
  }

  switch (placement_) {
    case Placement::kAnywhere:
      break;
    case Placement::kSpecific:
      if (specific_placements_.count(attributable->placement) == 0) {
        reporter->Fail(ErrInvalidAttributePlacement, attribute->span, attribute);
        return;
      }
      break;
    case Placement::kAnonymousLayout:
      switch (attributable->placement) {
        case AttributePlacement::kBitsDecl:
        case AttributePlacement::kEnumDecl:
        case AttributePlacement::kStructDecl:
        case AttributePlacement::kTableDecl:
        case AttributePlacement::kUnionDecl:
          if (static_cast<const Decl*>(attributable)->name.as_anonymous()) {
            // Good: the attribute is on an anonymous layout.
            break;
          }
          [[fallthrough]];
        default:
          reporter->Fail(ErrInvalidAttributePlacement, attribute->span, attribute);
          return;
      }
      break;
  }

  if (constraint_ == nullptr) {
    return;
  }
  auto check = reporter->Checkpoint();
  auto passed = constraint_(reporter, attribute, attributable);
  if (passed) {
    assert(check.NoNewErrors() && "cannot add errors and pass");
    return;
  }
  if (check.NoNewErrors()) {
    reporter->Fail(ErrAttributeConstraintNotSatisfied, attribute->span, attribute);
  }
}

void AttributeSchema::ResolveArgs(CompileStep* step, Attribute* attribute) const {
  switch (kind_) {
    case Kind::kValidateOnly:
    case Kind::kUseEarly:
    case Kind::kCompileEarly:
      break;
    case Kind::kDeprecated:
      // Don't attempt to resolve arguments, as we don't store arument schemas
      // for deprecated attributes. Instead, rely on AttributeSchema::Validate
      // to report the error.
      return;
    case Kind::kUserDefined:
      ResolveArgsWithoutSchema(step, attribute);
      return;
  }

  // Name the anonymous argument (if present).
  if (auto anon_arg = attribute->GetStandaloneAnonymousArg()) {
    if (arg_schemas_.empty()) {
      step->Fail(ErrAttributeDisallowsArgs, attribute->span, attribute);
      return;
    }
    if (arg_schemas_.size() > 1) {
      step->Fail(ErrAttributeArgNotNamed, attribute->span, anon_arg);
      return;
    }
    anon_arg->name = step->library_->GeneratedSimpleName(arg_schemas_.begin()->first);
  } else if (arg_schemas_.size() == 1 && attribute->args.size() == 1) {
    step->Fail(ErrAttributeArgMustNotBeNamed, attribute->span);
  }

  // Resolve each argument by name.
  for (auto& arg : attribute->args) {
    const auto it = arg_schemas_.find(arg->name.value().data());
    if (it == arg_schemas_.end()) {
      step->Fail(ErrUnknownAttributeArg, attribute->span, attribute, arg->name.value().data());
      continue;
    }
    const auto& [name, schema] = *it;
    const bool literal_only = kind_ == Kind::kCompileEarly;
    schema.ResolveArg(step, attribute, arg.get(), literal_only);
  }

  // Check for missing arguments.
  for (const auto& [name, schema] : arg_schemas_) {
    if (schema.IsOptional() || attribute->GetArg(name) != nullptr) {
      continue;
    }
    if (arg_schemas_.size() == 1) {
      step->Fail(ErrMissingRequiredAnonymousAttributeArg, attribute->span, attribute);
    } else {
      step->Fail(ErrMissingRequiredAttributeArg, attribute->span, attribute, name);
    }
  }
}

void AttributeArgSchema::ResolveArg(CompileStep* step, Attribute* attribute, AttributeArg* arg,
                                    bool literal_only) const {
  Constant* constant = arg->value.get();

  if (literal_only && constant->kind != Constant::Kind::kLiteral) {
    step->Fail(ErrAttributeArgRequiresLiteral, constant->span, arg->name.value().data(), attribute);
    return;
  }

  const Type* target_type;
  switch (type_) {
    case ConstantValue::Kind::kDocComment:
      assert(false && "we know the target type of doc comments, and should not end up here");
      return;
    case ConstantValue::Kind::kString:
      target_type = &Typespace::kUnboundedStringType;
      break;
    case ConstantValue::Kind::kBool:
      target_type = &Typespace::kBoolType;
      break;
    case ConstantValue::Kind::kInt8:
      target_type = &Typespace::kInt8Type;
      break;
    case ConstantValue::Kind::kInt16:
      target_type = &Typespace::kInt16Type;
      break;
    case ConstantValue::Kind::kInt32:
      target_type = &Typespace::kInt32Type;
      break;
    case ConstantValue::Kind::kInt64:
      target_type = &Typespace::kInt64Type;
      break;
    case ConstantValue::Kind::kUint8:
      target_type = &Typespace::kUint8Type;
      break;
    case ConstantValue::Kind::kUint16:
      target_type = &Typespace::kUint16Type;
      break;
    case ConstantValue::Kind::kUint32:
      target_type = &Typespace::kUint32Type;
      break;
    case ConstantValue::Kind::kUint64:
      target_type = &Typespace::kUint64Type;
      break;
    case ConstantValue::Kind::kFloat32:
      target_type = &Typespace::kFloat32Type;
      break;
    case ConstantValue::Kind::kFloat64:
      target_type = &Typespace::kFloat64Type;
      break;
  }
  if (!step->ResolveConstant(constant, target_type)) {
    step->Fail(ErrCouldNotResolveAttributeArg, arg->span);
  }
}

// static
void AttributeSchema::ResolveArgsWithoutSchema(CompileStep* step, Attribute* attribute) {
  // For attributes with a single, anonymous argument like `@foo("bar")`, assign
  // a default name so that arguments are always named after compilation.
  if (auto anon_arg = attribute->GetStandaloneAnonymousArg()) {
    anon_arg->name = step->library_->GeneratedSimpleName(AttributeArg::kDefaultAnonymousName);
  }

  // Try resolving each argument as string or bool. We don't allow numerics
  // because it's not clear what type (int8, uint32, etc.) we should infer.
  for (const auto& arg : attribute->args) {
    assert(arg->value->kind != Constant::Kind::kBinaryOperator &&
           "attribute arg with a binary operator is a parse error");

    auto inferred_type = step->InferType(arg->value.get());
    if (!inferred_type) {
      step->Fail(ErrCouldNotResolveAttributeArg, attribute->span);
      continue;
    }
    // Only string or bool supported.
    switch (inferred_type->kind) {
      case Type::Kind::kString:
        break;
      case Type::Kind::kPrimitive:
        if (static_cast<const PrimitiveType*>(inferred_type)->subtype ==
            types::PrimitiveSubtype::kBool) {
          break;
        }
        [[fallthrough]];
      case Type::Kind::kIdentifier:
      case Type::Kind::kArray:
      case Type::Kind::kBox:
      case Type::Kind::kVector:
      case Type::Kind::kHandle:
      case Type::Kind::kTransportSide:
      case Type::Kind::kUntypedNumeric:
        step->Fail(ErrCanOnlyUseStringOrBool, attribute->span, arg.get(), attribute);
        continue;
    }
    if (!step->ResolveConstant(arg->value.get(), inferred_type)) {
      // Since we've inferred the type, it must resolve correctly.
      __builtin_unreachable();
    }
  }
}

bool SimpleLayoutConstraint(Reporter* reporter, const Attribute* attr,
                            const Attributable* attributable) {
  assert(attributable);
  bool ok = true;
  switch (attributable->placement) {
    case AttributePlacement::kProtocolDecl: {
      auto protocol = static_cast<const Protocol*>(attributable);
      for (const auto& method_with_info : protocol->all_methods) {
        auto* method = method_with_info.method;
        if (!SimpleLayoutConstraint(reporter, attr, method)) {
          ok = false;
        }
      }
      break;
    }
    case AttributePlacement::kMethod: {
      auto method = static_cast<const Protocol::Method*>(attributable);
      if (method->maybe_request) {
        auto id = static_cast<const flat::IdentifierType*>(method->maybe_request->type);

        // TODO(fxbug.dev/88343): switch on union/table when those are enabled.
        auto as_struct = static_cast<const flat::Struct*>(id->type_decl);
        if (!SimpleLayoutConstraint(reporter, attr, as_struct)) {
          ok = false;
        }
      }
      if (method->maybe_response) {
        auto id = static_cast<const flat::IdentifierType*>(method->maybe_response->type);

        // TODO(fxbug.dev/88343): switch on union/table when those are enabled.
        auto as_struct = static_cast<const flat::Struct*>(id->type_decl);
        if (!SimpleLayoutConstraint(reporter, attr, as_struct)) {
          ok = false;
        }
      }
      break;
    }
    case AttributePlacement::kStructDecl: {
      auto struct_decl = static_cast<const Struct*>(attributable);
      for (const auto& member : struct_decl->members) {
        if (!IsSimple(member.type_ctor->type, reporter)) {
          reporter->Fail(ErrMemberMustBeSimple, member.name, member.name.data());
          ok = false;
        }
      }
      break;
    }
    default:
      assert(false && "unexpected kind");
  }
  return ok;
}

bool ParseBound(Reporter* reporter, const Attribute* attribute, std::string_view input,
                uint32_t* out_value) {
  auto result = utils::ParseNumeric(input, out_value, 10);
  switch (result) {
    case utils::ParseNumericResult::kOutOfBounds:
      reporter->Fail(ErrBoundIsTooBig, attribute->span, attribute, input);
      return false;
    case utils::ParseNumericResult::kMalformed: {
      reporter->Fail(ErrUnableToParseBound, attribute->span, attribute, input);
      return false;
    }
    case utils::ParseNumericResult::kSuccess:
      return true;
  }
}

void VerifyInlineSizeStep::RunImpl() {
  for (const Decl* decl : library_->declaration_order_) {
    if (decl->kind == Decl::Kind::kStruct) {
      auto struct_decl = static_cast<const Struct*>(decl);
      if (struct_decl->typeshape(WireFormat::kV1NoEe).InlineSize() >= 65536) {
        Fail(ErrInlineSizeExceeds64k, struct_decl->name.span().value());
      }
    }
  }
}

bool MaxBytesConstraint(Reporter* reporter, const Attribute* attribute,
                        const Attributable* attributable) {
  assert(attributable);
  auto arg = attribute->GetArg(AttributeArg::kDefaultAnonymousName);
  auto arg_value = static_cast<const flat::StringConstantValue&>(arg->value->Value());

  uint32_t bound;
  if (!ParseBound(reporter, attribute, std::string(arg_value.MakeContents()), &bound))
    return false;
  uint32_t max_bytes = std::numeric_limits<uint32_t>::max();
  switch (attributable->placement) {
    case AttributePlacement::kProtocolDecl: {
      auto protocol = static_cast<const Protocol*>(attributable);
      bool ok = true;
      for (const auto& method_with_info : protocol->all_methods) {
        auto* method = method_with_info.method;
        if (!MaxBytesConstraint(reporter, attribute, method)) {
          ok = false;
        }
      }
      return ok;
    }
    case AttributePlacement::kMethod: {
      auto method = static_cast<const Protocol::Method*>(attributable);
      bool ok = true;
      if (method->maybe_request) {
        auto id = static_cast<const flat::IdentifierType*>(method->maybe_request->type);

        // TODO(fxbug.dev/88343): switch on union/table when those are enabled.
        auto as_struct = static_cast<const flat::Struct*>(id->type_decl);
        if (!MaxBytesConstraint(reporter, attribute, as_struct)) {
          ok = false;
        }
      }
      if (method->maybe_response) {
        auto id = static_cast<const flat::IdentifierType*>(method->maybe_response->type);

        // TODO(fxbug.dev/88343): switch on union/table when those are enabled.
        auto as_struct = static_cast<const flat::Struct*>(id->type_decl);
        if (!MaxBytesConstraint(reporter, attribute, as_struct)) {
          ok = false;
        }
      }
      return ok;
    }
    case AttributePlacement::kStructDecl: {
      auto struct_decl = static_cast<const Struct*>(attributable);
      max_bytes = struct_decl->typeshape(WireFormat::kV1NoEe).InlineSize() +
                  struct_decl->typeshape(WireFormat::kV1NoEe).MaxOutOfLine();
      break;
    }
    case AttributePlacement::kTableDecl: {
      auto table_decl = static_cast<const Table*>(attributable);
      max_bytes = table_decl->typeshape(WireFormat::kV1NoEe).InlineSize() +
                  table_decl->typeshape(WireFormat::kV1NoEe).MaxOutOfLine();
      break;
    }
    case AttributePlacement::kUnionDecl: {
      auto union_decl = static_cast<const Union*>(attributable);
      max_bytes = union_decl->typeshape(WireFormat::kV1NoEe).InlineSize() +
                  union_decl->typeshape(WireFormat::kV1NoEe).MaxOutOfLine();
      break;
    }
    default:
      assert(false && "unexpected kind");
      return false;
  }
  if (max_bytes > bound) {
    reporter->Fail(ErrTooManyBytes, attribute->span, bound, max_bytes);
    return false;
  }
  return true;
}

bool MaxHandlesConstraint(Reporter* reporter, const Attribute* attribute,
                          const Attributable* attributable) {
  assert(attributable);
  auto arg = attribute->GetArg(AttributeArg::kDefaultAnonymousName);
  auto arg_value = static_cast<const flat::StringConstantValue&>(arg->value->Value());

  uint32_t bound;
  if (!ParseBound(reporter, attribute, std::string(arg_value.MakeContents()), &bound))
    return false;
  uint32_t max_handles = std::numeric_limits<uint32_t>::max();
  switch (attributable->placement) {
    case AttributePlacement::kProtocolDecl: {
      auto protocol = static_cast<const Protocol*>(attributable);
      bool ok = true;
      for (const auto& method_with_info : protocol->all_methods) {
        auto* method = method_with_info.method;
        if (!MaxHandlesConstraint(reporter, attribute, method)) {
          ok = false;
        }
      }
      return ok;
    }
    case AttributePlacement::kMethod: {
      auto method = static_cast<const Protocol::Method*>(attributable);
      bool ok = true;
      if (method->maybe_request) {
        auto id = static_cast<const flat::IdentifierType*>(method->maybe_request->type);

        // TODO(fxbug.dev/88343): switch on union/table when those are enabled.
        auto as_struct = static_cast<const flat::Struct*>(id->type_decl);
        if (!MaxHandlesConstraint(reporter, attribute, as_struct)) {
          ok = false;
        }
      }
      if (method->maybe_response) {
        auto id = static_cast<const flat::IdentifierType*>(method->maybe_response->type);

        // TODO(fxbug.dev/88343): switch on union/table when those are enabled.
        auto as_struct = static_cast<const flat::Struct*>(id->type_decl);
        if (!MaxHandlesConstraint(reporter, attribute, as_struct)) {
          ok = false;
        }
      }
      return ok;
    }
    case AttributePlacement::kStructDecl: {
      auto struct_decl = static_cast<const Struct*>(attributable);
      max_handles = struct_decl->typeshape(WireFormat::kV1NoEe).MaxHandles();
      break;
    }
    case AttributePlacement::kTableDecl: {
      auto table_decl = static_cast<const Table*>(attributable);
      max_handles = table_decl->typeshape(WireFormat::kV1NoEe).MaxHandles();
      break;
    }
    case AttributePlacement::kUnionDecl: {
      auto union_decl = static_cast<const Union*>(attributable);
      max_handles = union_decl->typeshape(WireFormat::kV1NoEe).MaxHandles();
      break;
    }
    default:
      assert(false && "unexpected kind");
      return false;
  }
  if (max_handles > bound) {
    reporter->Fail(ErrTooManyHandles, attribute->span, bound, max_handles);
    return false;
  }
  return true;
}

bool ResultShapeConstraint(Reporter* reporter, const Attribute* attribute,
                           const Attributable* attributable) {
  assert(attributable);
  assert(attributable->placement == AttributePlacement::kUnionDecl);
  auto union_decl = static_cast<const Union*>(attributable);
  assert(union_decl->members.size() == 2);
  auto& error_member = union_decl->members.at(1);
  assert(error_member.maybe_used && "must have an error member");
  auto error_type = error_member.maybe_used->type_ctor->type;

  const PrimitiveType* error_primitive = nullptr;
  if (error_type->kind == Type::Kind::kPrimitive) {
    error_primitive = static_cast<const PrimitiveType*>(error_type);
  } else if (error_type->kind == Type::Kind::kIdentifier) {
    auto identifier_type = static_cast<const IdentifierType*>(error_type);
    if (identifier_type->type_decl->kind == Decl::Kind::kEnum) {
      auto error_enum = static_cast<const Enum*>(identifier_type->type_decl);
      assert(error_enum->subtype_ctor->type->kind == Type::Kind::kPrimitive);
      error_primitive = static_cast<const PrimitiveType*>(error_enum->subtype_ctor->type);
    }
  }

  if (!error_primitive || (error_primitive->subtype != types::PrimitiveSubtype::kInt32 &&
                           error_primitive->subtype != types::PrimitiveSubtype::kUint32)) {
    reporter->Fail(ErrInvalidErrorType, union_decl->name.span().value());
    return false;
  }

  return true;
}

static std::string Trim(std::string s) {
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) {
            return !utils::IsWhitespace(static_cast<char>(ch));
          }));
  s.erase(std::find_if(s.rbegin(), s.rend(),
                       [](int ch) { return !utils::IsWhitespace(static_cast<char>(ch)); })
              .base(),
          s.end());
  return s;
}

bool TransportConstraint(Reporter* reporter, const Attribute* attribute,
                         const Attributable* attributable) {
  assert(attributable);
  assert(attributable->placement == AttributePlacement::kProtocolDecl);

  // function-local static pointer to non-trivially-destructible type
  // is allowed by styleguide
  static const auto kValidTransports = new std::set<std::string>{
      "Banjo",
      "Channel",
      "Driver",
      "Syscall",
  };

  auto arg = attribute->GetArg(AttributeArg::kDefaultAnonymousName);
  auto arg_value = static_cast<const flat::StringConstantValue&>(arg->value->Value());

  // Parse comma separated transports
  const std::string& value = arg_value.MakeContents();
  std::string::size_type prev_pos = 0;
  std::string::size_type pos;
  std::vector<std::string> transports;
  while ((pos = value.find(',', prev_pos)) != std::string::npos) {
    transports.emplace_back(Trim(value.substr(prev_pos, pos - prev_pos)));
    prev_pos = pos + 1;
  }
  transports.emplace_back(Trim(value.substr(prev_pos)));

  // Validate that they're ok
  for (const auto& transport : transports) {
    if (kValidTransports->count(transport) == 0) {
      reporter->Fail(ErrInvalidTransportType, attribute->span, transport, *kValidTransports);
      return false;
    }
  }
  return true;
}

Resource::Property* Resource::LookupProperty(std::string_view name) {
  for (Property& property : properties) {
    if (property.name.data() == name.data()) {
      return &property;
    }
  }
  return nullptr;
}

Libraries::Libraries() {
  AddAttributeSchema("discoverable")
      .RestrictTo({
          AttributePlacement::kProtocolDecl,
      });
  AddAttributeSchema(std::string(Attribute::kDocCommentName))
      .AddArg(AttributeArgSchema(ConstantValue::Kind::kString));
  AddAttributeSchema("layout").Deprecate();
  AddAttributeSchema("for_deprecated_c_bindings")
      .RestrictTo({
          AttributePlacement::kProtocolDecl,
          AttributePlacement::kStructDecl,
      })
      .Constrain(SimpleLayoutConstraint);
  AddAttributeSchema("generated_name")
      .RestrictToAnonymousLayouts()
      .AddArg(AttributeArgSchema(ConstantValue::Kind::kString))
      .CompileEarly();
  AddAttributeSchema("max_bytes")
      .RestrictTo({
          AttributePlacement::kProtocolDecl,
          AttributePlacement::kMethod,
          AttributePlacement::kStructDecl,
          AttributePlacement::kTableDecl,
          AttributePlacement::kUnionDecl,
      })
      .AddArg(AttributeArgSchema(ConstantValue::Kind::kString))
      .Constrain(MaxBytesConstraint);
  AddAttributeSchema("max_handles")
      .RestrictTo({
          AttributePlacement::kProtocolDecl,
          AttributePlacement::kMethod,
          AttributePlacement::kStructDecl,
          AttributePlacement::kTableDecl,
          AttributePlacement::kUnionDecl,
      })
      .AddArg(AttributeArgSchema(ConstantValue::Kind::kString))
      .Constrain(MaxHandlesConstraint);
  AddAttributeSchema("result")
      .RestrictTo({
          AttributePlacement::kUnionDecl,
      })
      .Constrain(ResultShapeConstraint);
  AddAttributeSchema("selector")
      .RestrictTo({
          AttributePlacement::kMethod,
      })
      .AddArg(AttributeArgSchema(ConstantValue::Kind::kString))
      .UseEarly();
  AddAttributeSchema("transitional")
      .RestrictTo({
          AttributePlacement::kMethod,
      })
      .AddArg(AttributeArgSchema(ConstantValue::Kind::kString,
                                 AttributeArgSchema::Optionality::kOptional));
  AddAttributeSchema("transport")
      .RestrictTo({
          AttributePlacement::kProtocolDecl,
      })
      .AddArg(AttributeArgSchema(ConstantValue::Kind::kString))
      .Constrain(TransportConstraint);
  AddAttributeSchema("unknown").RestrictTo({
      AttributePlacement::kEnumMember,
  });
}

bool Libraries::Insert(std::unique_ptr<Library> library) {
  std::vector<std::string_view> library_name = library->name();
  auto iter = all_libraries_.emplace(library_name, std::move(library));
  return iter.second;
}

bool Libraries::Lookup(const std::vector<std::string_view>& library_name,
                       Library** out_library) const {
  auto iter = all_libraries_.find(library_name);
  if (iter == all_libraries_.end()) {
    return false;
  }

  *out_library = iter->second.get();
  return true;
}

std::set<std::vector<std::string_view>> Libraries::Unused(const Library* target_library) const {
  std::set<std::vector<std::string_view>> unused;
  for (auto& name_library : all_libraries_)
    unused.insert(name_library.first);
  unused.erase(target_library->name());
  std::set<const Library*> worklist = {target_library};
  while (worklist.size() != 0) {
    auto it = worklist.begin();
    auto next = *it;
    worklist.erase(it);
    for (const auto dependency : next->dependencies()) {
      unused.erase(dependency->name());
      worklist.insert(dependency);
    }
  }
  return unused;
}

static size_t EditDistance(std::string_view sequence1, std::string_view sequence2) {
  size_t s1_length = sequence1.length();
  size_t s2_length = sequence2.length();
  size_t row1[s1_length + 1];
  size_t row2[s1_length + 1];
  size_t* last_row = row1;
  size_t* this_row = row2;
  for (size_t i = 0; i <= s1_length; i++)
    last_row[i] = i;
  for (size_t j = 0; j < s2_length; j++) {
    this_row[0] = j + 1;
    auto s2c = sequence2[j];
    for (size_t i = 1; i <= s1_length; i++) {
      auto s1c = sequence1[i - 1];
      this_row[i] = std::min(std::min(last_row[i] + 1, this_row[i - 1] + 1),
                             last_row[i - 1] + (s1c == s2c ? 0 : 1));
    }
    std::swap(last_row, this_row);
  }
  return last_row[s1_length];
}

const AttributeSchema& Libraries::RetrieveAttributeSchema(Reporter* reporter,
                                                          const Attribute* attribute,
                                                          bool warn_on_typo) const {
  auto attribute_name = attribute->name.data();
  auto iter = attribute_schemas_.find(attribute_name);
  if (iter != attribute_schemas_.end()) {
    return iter->second;
  }

  if (warn_on_typo) {
    // Match against all known attributes.
    for (const auto& [suspected_name, schema] : attribute_schemas_) {
      auto supplied_name = attribute_name;
      auto edit_distance = EditDistance(supplied_name, suspected_name);
      if (0 < edit_distance && edit_distance < 2) {
        reporter->Warn(WarnAttributeTypo, attribute->span, supplied_name, suspected_name);
      }
    }
  }
  return AttributeSchema::kUserDefined;
}

Dependencies::RegisterResult Dependencies::Register(
    const SourceSpan& span, std::string_view filename, Library* dep_library,
    const std::unique_ptr<raw::Identifier>& maybe_alias) {
  refs_.push_back(std::make_unique<LibraryRef>(span, dep_library));
  LibraryRef* ref = refs_.back().get();

  const std::vector<std::string_view> name =
      maybe_alias ? std::vector{maybe_alias->span().data()} : dep_library->name();
  auto iter = by_filename_.find(filename);
  if (iter == by_filename_.end()) {
    iter = by_filename_.emplace(filename, std::make_unique<PerFile>()).first;
  }
  PerFile& per_file = *iter->second;
  if (!per_file.libraries.insert(dep_library).second) {
    return RegisterResult::kDuplicate;
  }
  if (!per_file.refs.emplace(name, ref).second) {
    return RegisterResult::kCollision;
  }
  dependencies_aggregate_.insert(dep_library);
  return RegisterResult::kSuccess;
}

bool Dependencies::Contains(std::string_view filename, const std::vector<std::string_view>& name) {
  const auto iter = by_filename_.find(filename);
  if (iter == by_filename_.end()) {
    return false;
  }
  const PerFile& per_file = *iter->second;
  return per_file.refs.find(name) != per_file.refs.end();
}

bool Dependencies::Lookup(std::string_view filename, const std::vector<std::string_view>& name,
                          Dependencies::LookupMode mode, Library** out_library) const {
  auto iter1 = by_filename_.find(filename);
  if (iter1 == by_filename_.end()) {
    return false;
  }

  auto iter2 = iter1->second->refs.find(name);
  if (iter2 == iter1->second->refs.end()) {
    return false;
  }

  auto ref = iter2->second;
  if (mode == Dependencies::LookupMode::kUse) {
    ref->used = true;
  }
  *out_library = ref->library;
  return true;
}

void Dependencies::VerifyAllDependenciesWereUsed(const Library& for_library, Reporter* reporter) {
  for (const auto& [filename, per_file] : by_filename_) {
    for (const auto& [name, ref] : per_file->refs) {
      if (!ref->used) {
        reporter->Fail(ErrUnusedImport, ref->span, for_library.name(), ref->library->name(),
                       ref->library->name());
      }
    }
  }
}

// Consuming the AST is primarily concerned with walking the tree and
// flattening the representation. The AST's declaration nodes are
// converted into the Library's foo_declaration structures. This means pulling
// a struct declaration inside a protocol out to the top level and
// so on.

std::string LibraryName(const Library* library, std::string_view separator) {
  if (library != nullptr) {
    return utils::StringJoin(library->name(), separator);
  }
  return std::string();
}

void VerifyAttributesStep::VerifyAttributes(const Attributable* attributable) {
  for (const auto& attribute : attributable->attributes->attributes) {
    const AttributeSchema& schema =
        library_->all_libraries_->RetrieveAttributeSchema(reporter(), attribute.get());
    schema.Validate(reporter(), attribute.get(), attributable);
  }
}

SourceSpan Library::GeneratedSimpleName(std::string_view name) {
  return generated_source_file_.AddLine(name);
}

std::optional<Name> ConsumeStep::CompileCompoundIdentifier(
    const raw::CompoundIdentifier* compound_identifier) {
  const auto& components = compound_identifier->components;
  assert(components.size() >= 1);

  SourceSpan decl_name = components.back()->span();

  // First try resolving the identifier in the library.
  if (components.size() == 1) {
    return Name::CreateSourced(library_, decl_name);
  }

  std::vector<std::string_view> library_name;
  for (auto iter = components.begin(); iter != components.end() - 1; ++iter) {
    library_name.push_back((*iter)->span().data());
  }

  auto filename = compound_identifier->span().source_file().filename();
  Library* dep_library = nullptr;
  if (library_->dependencies_.Lookup(filename, library_name, Dependencies::LookupMode::kUse,
                                     &dep_library)) {
    return Name::CreateSourced(dep_library, decl_name);
  }

  // If the identifier is not found in the library it might refer to a
  // declaration with a member (e.g. library.EnumX.val or BitsY.val).
  SourceSpan member_name = decl_name;
  SourceSpan member_decl_name = components.rbegin()[1]->span();

  if (components.size() == 2) {
    return Name::CreateSourced(library_, member_decl_name, std::string(member_name.data()));
  }

  std::vector<std::string_view> member_library_name(library_name);
  member_library_name.pop_back();

  Library* member_dep_library = nullptr;
  if (library_->dependencies_.Lookup(filename, member_library_name, Dependencies::LookupMode::kUse,
                                     &member_dep_library)) {
    return Name::CreateSourced(member_dep_library, member_decl_name,
                               std::string(member_name.data()));
  }

  Fail(ErrUnknownDependentLibrary, components[0]->span(), library_name, member_library_name);
  return std::nullopt;
}

namespace {

template <typename T>
void StoreDecl(Decl* decl_ptr, std::vector<std::unique_ptr<T>>* declarations) {
  std::unique_ptr<T> t_decl;
  t_decl.reset(static_cast<T*>(decl_ptr));
  declarations->push_back(std::move(t_decl));
}

}  // namespace

bool ConsumeStep::RegisterDecl(std::unique_ptr<Decl> decl) {
  assert(decl);

  auto decl_ptr = decl.release();
  auto kind = decl_ptr->kind;
  switch (kind) {
    case Decl::Kind::kBits:
      StoreDecl(decl_ptr, &library_->bits_declarations_);
      break;
    case Decl::Kind::kConst:
      StoreDecl(decl_ptr, &library_->const_declarations_);
      break;
    case Decl::Kind::kEnum:
      StoreDecl(decl_ptr, &library_->enum_declarations_);
      break;
    case Decl::Kind::kProtocol:
      StoreDecl(decl_ptr, &library_->protocol_declarations_);
      break;
    case Decl::Kind::kResource:
      StoreDecl(decl_ptr, &library_->resource_declarations_);
      break;
    case Decl::Kind::kService:
      StoreDecl(decl_ptr, &library_->service_declarations_);
      break;
    case Decl::Kind::kStruct:
      StoreDecl(decl_ptr, &library_->struct_declarations_);
      break;
    case Decl::Kind::kTable:
      StoreDecl(decl_ptr, &library_->table_declarations_);
      break;
    case Decl::Kind::kTypeAlias:
      StoreDecl(decl_ptr, &library_->type_alias_declarations_);
      break;
    case Decl::Kind::kUnion:
      StoreDecl(decl_ptr, &library_->union_declarations_);
      break;
  }  // switch

  const Name& name = decl_ptr->name;
  {
    const auto it = library_->declarations_.emplace(name, decl_ptr);
    if (!it.second) {
      const auto previous_name = it.first->second->name;
      return Fail(ErrNameCollision, name.span().value(), name, previous_name.span().value());
    }
  }

  const auto canonical_decl_name = utils::canonicalize(name.decl_name());
  {
    const auto it = declarations_by_canonical_name_.emplace(canonical_decl_name, decl_ptr);
    if (!it.second) {
      const auto previous_name = it.first->second->name;
      return Fail(ErrNameCollisionCanonical, name.span().value(), name, previous_name,
                  previous_name.span().value(), canonical_decl_name);
    }
  }

  if (name.span()) {
    if (library_->dependencies_.Contains(name.span()->source_file().filename(),
                                         {name.span()->data()})) {
      return Fail(ErrDeclNameConflictsWithLibraryImport, name.span().value(), name);
    }
    if (library_->dependencies_.Contains(name.span()->source_file().filename(),
                                         {canonical_decl_name})) {
      return Fail(ErrDeclNameConflictsWithLibraryImportCanonical, name.span().value(), name,
                  canonical_decl_name);
    }
  }

  Typespace* typespace = library_->typespace_;
  switch (kind) {
    case Decl::Kind::kBits:
    case Decl::Kind::kEnum:
    case Decl::Kind::kService:
    case Decl::Kind::kStruct:
    case Decl::Kind::kTable:
    case Decl::Kind::kUnion:
    case Decl::Kind::kProtocol: {
      auto type_decl = static_cast<TypeDecl*>(decl_ptr);
      auto type_template =
          std::make_unique<TypeDeclTypeTemplate>(name, typespace, reporter(), type_decl);
      typespace->AddTemplate(std::move(type_template));
      break;
    }
    case Decl::Kind::kResource: {
      auto resource_decl = static_cast<Resource*>(decl_ptr);
      auto type_template =
          std::make_unique<HandleTypeTemplate>(name, typespace, reporter(), resource_decl);
      typespace->AddTemplate(std::move(type_template));
      break;
    }
    case Decl::Kind::kTypeAlias: {
      auto type_alias_decl = static_cast<TypeAlias*>(decl_ptr);
      auto type_alias_template =
          std::make_unique<TypeAliasTypeTemplate>(name, typespace, reporter(), type_alias_decl);
      typespace->AddTemplate(std::move(type_alias_template));
      break;
    }
    case Decl::Kind::kConst:
      break;
  }  // switch
  return true;
}

void ConsumeStep::ConsumeAttributeList(std::unique_ptr<raw::AttributeList> raw_attribute_list,
                                       std::unique_ptr<AttributeList>* out_attribute_list) {
  assert(out_attribute_list && "must provide out parameter");
  // Usually *out_attribute_list is null and we create the AttributeList here.
  // But for library declarations we consume attributes from each file into
  // library->attributes. In this case it's only null for the first file.
  if (*out_attribute_list == nullptr) {
    *out_attribute_list =
        std::make_unique<AttributeList>(std::vector<std::unique_ptr<Attribute>>());
  }
  if (!raw_attribute_list) {
    return;
  }
  auto& out_attributes = (*out_attribute_list)->attributes;
  for (auto& raw_attribute : raw_attribute_list->attributes) {
    std::unique_ptr<Attribute> attribute;
    ConsumeAttribute(std::move(raw_attribute), &attribute);
    out_attributes.push_back(std::move(attribute));
  }
}

void ConsumeStep::ConsumeAttribute(std::unique_ptr<raw::Attribute> raw_attribute,
                                   std::unique_ptr<Attribute>* out_attribute) {
  [[maybe_unused]] bool all_named = true;
  std::vector<std::unique_ptr<AttributeArg>> args;
  for (auto& raw_arg : raw_attribute->args) {
    std::unique_ptr<Constant> constant;
    if (!ConsumeConstant(std::move(raw_arg->value), &constant)) {
      continue;
    }
    std::optional<SourceSpan> name;
    if (raw_arg->maybe_name) {
      name = raw_arg->maybe_name->span();
    }
    all_named = all_named && name.has_value();
    args.emplace_back(std::make_unique<AttributeArg>(name, std::move(constant), raw_arg->span()));
  }
  assert(all_named ||
         args.size() == 1 && "parser should not allow an anonymous arg with other args");
  SourceSpan name;
  switch (raw_attribute->provenance) {
    case raw::Attribute::Provenance::kDefault:
      name = raw_attribute->maybe_name->span();
      break;
    case raw::Attribute::Provenance::kDocComment:
      name = library_->GeneratedSimpleName(Attribute::kDocCommentName);
      break;
  }
  *out_attribute = std::make_unique<Attribute>(name, std::move(args), raw_attribute->span());
}

bool ConsumeStep::ConsumeConstant(std::unique_ptr<raw::Constant> raw_constant,
                                  std::unique_ptr<Constant>* out_constant) {
  switch (raw_constant->kind) {
    case raw::Constant::Kind::kIdentifier: {
      auto identifier = static_cast<raw::IdentifierConstant*>(raw_constant.get());
      auto name = CompileCompoundIdentifier(identifier->identifier.get());
      if (!name)
        return false;
      *out_constant =
          std::make_unique<IdentifierConstant>(std::move(name.value()), identifier->span());
      break;
    }
    case raw::Constant::Kind::kLiteral: {
      auto literal = static_cast<raw::LiteralConstant*>(raw_constant.get());
      std::unique_ptr<LiteralConstant> out;
      ConsumeLiteralConstant(literal, &out);
      *out_constant = std::unique_ptr<Constant>(out.release());
      break;
    }
    case raw::Constant::Kind::kBinaryOperator: {
      auto binary_operator_constant = static_cast<raw::BinaryOperatorConstant*>(raw_constant.get());
      BinaryOperatorConstant::Operator op;
      switch (binary_operator_constant->op) {
        case raw::BinaryOperatorConstant::Operator::kOr:
          op = BinaryOperatorConstant::Operator::kOr;
          break;
      }
      std::unique_ptr<Constant> left_operand;
      if (!ConsumeConstant(std::move(binary_operator_constant->left_operand), &left_operand)) {
        return false;
      }
      std::unique_ptr<Constant> right_operand;
      if (!ConsumeConstant(std::move(binary_operator_constant->right_operand), &right_operand)) {
        return false;
      }
      *out_constant = std::make_unique<BinaryOperatorConstant>(
          std::move(left_operand), std::move(right_operand), op, binary_operator_constant->span());
      break;
    }
  }
  return true;
}

void ConsumeStep::ConsumeLiteralConstant(raw::LiteralConstant* raw_constant,
                                         std::unique_ptr<LiteralConstant>* out_constant) {
  *out_constant = std::make_unique<LiteralConstant>(std::move(raw_constant->literal));
}

void ConsumeStep::ConsumeUsing(std::unique_ptr<raw::Using> using_directive) {
  if (using_directive->attributes != nullptr) {
    Fail(ErrAttributesNotAllowedOnLibraryImport, using_directive->span(),
         using_directive->attributes.get());
    return;
  }

  std::vector<std::string_view> library_name;
  for (const auto& component : using_directive->using_path->components) {
    library_name.push_back(component->span().data());
  }

  Library* dep_library = nullptr;
  if (!library_->all_libraries_->Lookup(library_name, &dep_library)) {
    Fail(ErrUnknownLibrary, using_directive->using_path->components[0]->span(), library_name);
    return;
  }

  const auto filename = using_directive->span().source_file().filename();
  const auto result = library_->dependencies_.Register(using_directive->span(), filename,
                                                       dep_library, using_directive->maybe_alias);
  switch (result) {
    case Dependencies::RegisterResult::kSuccess:
      break;
    case Dependencies::RegisterResult::kDuplicate:
      FailNoSpan(ErrDuplicateLibraryImport, library_name);
      return;
    case Dependencies::RegisterResult::kCollision:
      if (using_directive->maybe_alias) {
        FailNoSpan(ErrConflictingLibraryImportAlias, library_name,
                   using_directive->maybe_alias->span().data());
        return;
      }
      FailNoSpan(ErrConflictingLibraryImport, library_name);
      return;
  }

  // Import declarations, and type aliases of dependent library.
  const auto& declarations = dep_library->declarations_;
  library_->declarations_.insert(declarations.begin(), declarations.end());
}

void ConsumeStep::ConsumeAliasDeclaration(
    std::unique_ptr<raw::AliasDeclaration> alias_declaration) {
  assert(alias_declaration->alias && alias_declaration->type_ctor != nullptr);

  std::unique_ptr<AttributeList> attributes;
  ConsumeAttributeList(std::move(alias_declaration->attributes), &attributes);

  auto alias_name = Name::CreateSourced(library_, alias_declaration->alias->span());
  std::unique_ptr<TypeConstructor> type_ctor_;

  if (!ConsumeTypeConstructor(std::move(alias_declaration->type_ctor),
                              NamingContext::Create(alias_name), &type_ctor_))
    return;

  RegisterDecl(std::make_unique<TypeAlias>(std::move(attributes), std::move(alias_name),
                                           std::move(type_ctor_)));
}

void ConsumeStep::ConsumeConstDeclaration(
    std::unique_ptr<raw::ConstDeclaration> const_declaration) {
  auto span = const_declaration->identifier->span();
  auto name = Name::CreateSourced(library_, span);
  std::unique_ptr<AttributeList> attributes;
  ConsumeAttributeList(std::move(const_declaration->attributes), &attributes);

  std::unique_ptr<TypeConstructor> type_ctor;
  if (!ConsumeTypeConstructor(std::move(const_declaration->type_ctor), NamingContext::Create(name),
                              &type_ctor))
    return;

  std::unique_ptr<Constant> constant;
  if (!ConsumeConstant(std::move(const_declaration->constant), &constant))
    return;

  RegisterDecl(std::make_unique<Const>(std::move(attributes), std::move(name), std::move(type_ctor),
                                       std::move(constant)));
}

namespace {

// Create a type constructor pointing to an anonymous layout.
std::unique_ptr<TypeConstructor> IdentifierTypeForDecl(const Decl* decl) {
  std::vector<std::unique_ptr<LayoutParameter>> no_params;
  std::vector<std::unique_ptr<Constant>> no_constraints;
  return std::make_unique<TypeConstructor>(
      decl->name, std::make_unique<LayoutParameterList>(std::move(no_params), std::nullopt),
      std::make_unique<TypeConstraints>(std::move(no_constraints), std::nullopt));
}

}  // namespace

bool ConsumeStep::CreateMethodResult(const std::shared_ptr<NamingContext>& err_variant_context,
                                     SourceSpan response_span, raw::ProtocolMethod* method,
                                     std::unique_ptr<TypeConstructor> success_variant,
                                     std::unique_ptr<TypeConstructor>* out_payload) {
  // Compile the error type.
  std::unique_ptr<TypeConstructor> error_type_ctor;
  if (!ConsumeTypeConstructor(std::move(method->maybe_error_ctor), err_variant_context,
                              &error_type_ctor))
    return false;

  raw::SourceElement sourceElement = raw::SourceElement(fidl::Token(), fidl::Token());
  assert(success_variant->name.as_anonymous() != nullptr);
  auto success_variant_context = success_variant->name.as_anonymous()->context;
  Union::Member success_member{
      std::make_unique<raw::Ordinal64>(sourceElement,
                                       1),  // success case explicitly has ordinal 1
      std::move(success_variant), success_variant_context->name(),
      std::make_unique<AttributeList>(std::vector<std::unique_ptr<Attribute>>{})};
  Union::Member error_member{
      std::make_unique<raw::Ordinal64>(sourceElement, 2),  // error case explicitly has ordinal 2
      std::move(error_type_ctor), err_variant_context->name(),
      std::make_unique<AttributeList>(std::vector<std::unique_ptr<Attribute>>{})};
  std::vector<Union::Member> result_members;
  result_members.push_back(std::move(success_member));
  result_members.push_back(std::move(error_member));
  std::vector<std::unique_ptr<Attribute>> result_attributes;
  result_attributes.emplace_back(
      std::make_unique<Attribute>(library_->GeneratedSimpleName("result")));

  // TODO(fxbug.dev/8027): Join spans of response and error constructor for `result_name`.
  auto result_context = err_variant_context->parent();
  auto result_name = Name::CreateAnonymous(library_, response_span, result_context);
  auto union_decl = std::make_unique<Union>(
      std::make_unique<AttributeList>(std::move(result_attributes)), std::move(result_name),
      std::move(result_members), types::Strictness::kStrict, std::nullopt /* resourceness */);
  auto result_decl = union_decl.get();
  if (!RegisterDecl(std::move(union_decl)))
    return false;

  // Make a new response struct for the method containing just the
  // result union.
  std::vector<Struct::Member> response_members;
  response_members.push_back(
      Struct::Member(IdentifierTypeForDecl(result_decl), result_context->name(), nullptr,
                     std::make_unique<AttributeList>(std::vector<std::unique_ptr<Attribute>>{})));

  const auto& response_context = result_context->parent();
  const Name response_name = Name::CreateAnonymous(library_, response_span, response_context);
  auto struct_decl = std::make_unique<Struct>(
      /* attributes = */ std::make_unique<AttributeList>(std::vector<std::unique_ptr<Attribute>>{}),
      response_name, std::move(response_members),
      /* resourceness = */ std::nullopt, /* is_request_or_response = */ true);
  auto payload = IdentifierTypeForDecl(struct_decl.get());
  if (!RegisterDecl(std::move(struct_decl)))
    return false;

  *out_payload = std::move(payload);
  return true;
}

void ConsumeStep::ConsumeProtocolDeclaration(
    std::unique_ptr<raw::ProtocolDeclaration> protocol_declaration) {
  auto protocol_name = Name::CreateSourced(library_, protocol_declaration->identifier->span());
  auto protocol_context = NamingContext::Create(protocol_name.span().value());

  std::vector<Protocol::ComposedProtocol> composed_protocols;
  std::set<Name> seen_composed_protocols;
  for (auto& raw_composed : protocol_declaration->composed_protocols) {
    std::unique_ptr<AttributeList> attributes;
    ConsumeAttributeList(std::move(raw_composed->attributes), &attributes);

    auto& raw_protocol_name = raw_composed->protocol_name;
    auto composed_protocol_name = CompileCompoundIdentifier(raw_protocol_name.get());
    if (!composed_protocol_name)
      return;
    if (!seen_composed_protocols.insert(composed_protocol_name.value()).second) {
      Fail(ErrProtocolComposedMultipleTimes, composed_protocol_name->span().value());
      return;
    }

    composed_protocols.emplace_back(std::move(attributes),
                                    std::move(composed_protocol_name.value()));
  }

  std::vector<Protocol::Method> methods;
  for (auto& method : protocol_declaration->methods) {
    std::unique_ptr<AttributeList> attributes;
    ConsumeAttributeList(std::move(method->attributes), &attributes);

    SourceSpan method_name = method->identifier->span();
    bool has_request = method->maybe_request != nullptr;
    std::unique_ptr<TypeConstructor> maybe_request;
    if (has_request) {
      bool result = ConsumeParameterList(method_name, protocol_context->EnterRequest(method_name),
                                         std::move(method->maybe_request), true, &maybe_request);
      if (!result)
        return;
    }

    std::unique_ptr<TypeConstructor> maybe_response;
    bool has_response = method->maybe_response != nullptr;
    bool has_error = false;
    if (has_response) {
      has_error = method->maybe_error_ctor != nullptr;

      SourceSpan response_span = method->maybe_response->span();
      auto response_context = has_request ? protocol_context->EnterResponse(method_name)
                                          : protocol_context->EnterEvent(method_name);

      std::shared_ptr<NamingContext> result_context, success_variant_context, err_variant_context;
      if (has_error) {
        // The error syntax for protocol P and method M desugars to the following type:
        //
        // // the "response"
        // struct {
        //   // the "result"
        //   result @generated_name("P_M_Result") union {
        //     // the "success variant"
        //     response @generated_name("P_M_Response") [user specified response type];
        //     // the "error variant"
        //     err @generated_name("P_M_Error") [user specified error type];
        //   };
        // };
        //
        // Note that this can lead to ambiguity with the success variant, since its member
        // name within the union is "response". The naming convention within fidlc
        // is to refer to each type using the name provided in the comments
        // above (i.e. "response" refers to the top level struct, not the success variant).
        //
        // The naming scheme for the result type and the success variant in a response
        // with an error type predates the design of the anonymous name flattening
        // algorithm, and we therefore they are overridden to be backwards compatible.
        result_context = response_context->EnterMember(library_->GeneratedSimpleName("result"));
        result_context->set_name_override(
            utils::StringJoin({protocol_name.decl_name(), method_name.data(), "Result"}, "_"));
        success_variant_context =
            result_context->EnterMember(library_->GeneratedSimpleName("response"));
        success_variant_context->set_name_override(
            utils::StringJoin({protocol_name.decl_name(), method_name.data(), "Response"}, "_"));
        err_variant_context = result_context->EnterMember(library_->GeneratedSimpleName("err"));
        err_variant_context->set_name_override(
            utils::StringJoin({protocol_name.decl_name(), method_name.data(), "Error"}, "_"));
      }

      // The context for the user specified type within the response part of the method
      // (i.e. `Foo() -> («this source») ...`) is either the top level response context
      // or that of the success variant of the result type
      std::unique_ptr<TypeConstructor> result_payload;
      auto ctx = has_error ? success_variant_context : response_context;
      bool result = ConsumeParameterList(method_name, ctx, std::move(method->maybe_response),
                                         !has_error, &result_payload);
      if (!result)
        return;

      if (has_error) {
        assert(err_variant_context != nullptr &&
               "compiler bug: error type contexts should have been computed");
        // we move out of `response_context` only if !has_error, so it's safe to use here
        if (!CreateMethodResult(err_variant_context, response_span, method.get(),
                                std::move(result_payload), &maybe_response))
          return;
      } else {
        maybe_response = std::move(result_payload);
      }
    }

    assert(has_request || has_response);
    methods.emplace_back(std::move(attributes), std::move(method->identifier), method_name,
                         has_request, std::move(maybe_request), has_response,
                         std::move(maybe_response), has_error);
  }

  std::unique_ptr<AttributeList> attributes;
  ConsumeAttributeList(std::move(protocol_declaration->attributes), &attributes);

  RegisterDecl(std::make_unique<Protocol>(std::move(attributes), std::move(protocol_name),
                                          std::move(composed_protocols), std::move(methods)));
}

bool ConsumeStep::ConsumeParameterList(SourceSpan method_name,
                                       std::shared_ptr<NamingContext> context,
                                       std::unique_ptr<raw::ParameterList> parameter_layout,
                                       bool is_request_or_response,
                                       std::unique_ptr<TypeConstructor>* out_payload) {
  // If the payload is empty, like the request in `Foo()` or the response in
  // `Foo(...) -> ()` or the success variant in `Foo(...) -> () error uint32`:
  if (!parameter_layout->type_ctor) {
    // If this is not a request or response, but a success variant:
    if (!is_request_or_response) {
      // Fail because we want `Foo(...) -> (struct {}) error uint32` instead.
      return Fail(ErrResponsesWithErrorsMustNotBeEmpty, parameter_layout->span(), method_name);
    }
    // Otherwise, there is nothing to do for an empty payload.
    return true;
  }

  std::unique_ptr<TypeConstructor> type_ctor;
  if (!ConsumeTypeConstructor(std::move(parameter_layout->type_ctor), context,
                              /*raw_attribute_list=*/nullptr, is_request_or_response, &type_ctor))
    return false;

  auto* decl = library_->LookupDeclByName(type_ctor->name);
  assert(decl);

  switch (decl->kind) {
    case Decl::Kind::kStruct: {
      auto struct_decl = static_cast<Struct*>(decl);
      if (is_request_or_response && struct_decl->members.empty()) {
        return Fail(ErrEmptyPayloadStructs, method_name, method_name.data());
      }
      break;
    }
    case Decl::Kind::kBits:
    case Decl::Kind::kEnum: {
      return Fail(ErrInvalidParameterListType, method_name, decl);
    }
    case Decl::Kind::kTable:
    case Decl::Kind::kUnion: {
      return Fail(ErrNotYetSupportedParameterListType, method_name, decl);
    }
    default: {
      assert(false && "unexpected decl kind");
    }
  }

  *out_payload = std::move(type_ctor);
  return true;
}

void ConsumeStep::ConsumeResourceDeclaration(
    std::unique_ptr<raw::ResourceDeclaration> resource_declaration) {
  auto name = Name::CreateSourced(library_, resource_declaration->identifier->span());
  std::vector<Resource::Property> properties;
  for (auto& property : resource_declaration->properties) {
    std::unique_ptr<AttributeList> attributes;
    ConsumeAttributeList(std::move(property->attributes), &attributes);

    std::unique_ptr<TypeConstructor> type_ctor;
    if (!ConsumeTypeConstructor(std::move(property->type_ctor), NamingContext::Create(name),
                                &type_ctor))
      return;
    properties.emplace_back(std::move(type_ctor), property->identifier->span(),
                            std::move(attributes));
  }

  std::unique_ptr<AttributeList> attributes;
  ConsumeAttributeList(std::move(resource_declaration->attributes), &attributes);

  std::unique_ptr<TypeConstructor> type_ctor;
  if (resource_declaration->maybe_type_ctor != nullptr) {
    if (!ConsumeTypeConstructor(std::move(resource_declaration->maybe_type_ctor),
                                NamingContext::Create(name), &type_ctor))
      return;
  } else {
    type_ctor = TypeConstructor::CreateSizeType();
  }

  RegisterDecl(std::make_unique<Resource>(std::move(attributes), std::move(name),
                                          std::move(type_ctor), std::move(properties)));
}

void ConsumeStep::ConsumeServiceDeclaration(std::unique_ptr<raw::ServiceDeclaration> service_decl) {
  auto name = Name::CreateSourced(library_, service_decl->identifier->span());
  auto context = NamingContext::Create(name);
  std::vector<Service::Member> members;
  for (auto& member : service_decl->members) {
    std::unique_ptr<AttributeList> attributes;
    ConsumeAttributeList(std::move(member->attributes), &attributes);

    std::unique_ptr<TypeConstructor> type_ctor;
    if (!ConsumeTypeConstructor(std::move(member->type_ctor), context->EnterMember(member->span()),
                                &type_ctor))
      return;
    members.emplace_back(std::move(type_ctor), member->identifier->span(), std::move(attributes));
  }

  std::unique_ptr<AttributeList> attributes;
  ConsumeAttributeList(std::move(service_decl->attributes), &attributes);

  RegisterDecl(
      std::make_unique<Service>(std::move(attributes), std::move(name), std::move(members)));
}

void ConsumeStep::MaybeOverrideName(AttributeList& attributes, NamingContext* context) {
  auto attr = attributes.Get("generated_name");
  if (attr == nullptr)
    return;

  CompileStep::CompileAttributeEarly(library_, attr);
  const auto* arg = attr->GetArg(AttributeArg::kDefaultAnonymousName);
  if (arg == nullptr || !arg->value->IsResolved()) {
    return;
  }
  const ConstantValue& value = arg->value->Value();
  assert(value.kind == ConstantValue::Kind::kString);
  std::string str = static_cast<const StringConstantValue&>(value).MakeContents();
  if (utils::IsValidIdentifierComponent(str)) {
    context->set_name_override(std::move(str));
  } else {
    Fail(ErrInvalidGeneratedName, arg->span);
  }
}

// TODO(fxbug.dev/77853): these conversion methods may need to be refactored
//  once the new flat AST lands, and such coercion  is no longer needed.
template <typename T>
bool ConsumeStep::ConsumeValueLayout(std::unique_ptr<raw::Layout> layout,
                                     const std::shared_ptr<NamingContext>& context,
                                     std::unique_ptr<raw::AttributeList> raw_attribute_list) {
  std::vector<typename T::Member> members;
  for (auto& mem : layout->members) {
    auto member = static_cast<raw::ValueLayoutMember*>(mem.get());
    auto span = member->identifier->span();

    std::unique_ptr<AttributeList> attributes;
    ConsumeAttributeList(std::move(member->attributes), &attributes);

    std::unique_ptr<Constant> value;
    if (!ConsumeConstant(std::move(member->value), &value))
      return false;

    members.emplace_back(span, std::move(value), std::move(attributes));
  }

  std::unique_ptr<TypeConstructor> subtype_ctor;
  if (layout->subtype_ctor != nullptr) {
    if (!ConsumeTypeConstructor(std::move(layout->subtype_ctor), context,
                                /*raw_attribute_list=*/nullptr, /*is_request_or_response=*/false,
                                &subtype_ctor))
      return false;
  } else {
    subtype_ctor = TypeConstructor::CreateSizeType();
  }

  std::unique_ptr<AttributeList> attributes;
  ConsumeAttributeList(std::move(raw_attribute_list), &attributes);
  MaybeOverrideName(*attributes, context.get());

  auto strictness = types::Strictness::kFlexible;
  if (layout->modifiers != nullptr)
    strictness = layout->modifiers->maybe_strictness.value_or(types::Strictness::kFlexible);

  if (layout->members.size() == 0) {
    if (!std::is_same<T, Enum>::value || strictness != types::Strictness::kFlexible)
      return Fail(ErrMustHaveOneMember, layout->span());
  }

  RegisterDecl(std::make_unique<T>(std::move(attributes), context->ToName(library_, layout->span()),
                                   std::move(subtype_ctor), std::move(members), strictness));
  return true;
}

template <typename T>
bool ConsumeStep::ConsumeOrdinaledLayout(std::unique_ptr<raw::Layout> layout,
                                         const std::shared_ptr<NamingContext>& context,
                                         std::unique_ptr<raw::AttributeList> raw_attribute_list) {
  std::vector<typename T::Member> members;
  for (auto& mem : layout->members) {
    auto member = static_cast<raw::OrdinaledLayoutMember*>(mem.get());
    std::unique_ptr<AttributeList> attributes;
    ConsumeAttributeList(std::move(member->attributes), &attributes);
    if (member->reserved) {
      members.emplace_back(std::move(member->ordinal), member->span(), std::move(attributes));
      continue;
    }

    std::unique_ptr<TypeConstructor> type_ctor;
    if (!ConsumeTypeConstructor(
            std::move(member->type_ctor), context->EnterMember(member->identifier->span()),
            /*raw_attribute_list=*/nullptr, /*is_request_or_response=*/false, &type_ctor))
      return false;

    members.emplace_back(std::move(member->ordinal), std::move(type_ctor),
                         member->identifier->span(), std::move(attributes));
  }

  std::unique_ptr<AttributeList> attributes;
  ConsumeAttributeList(std::move(raw_attribute_list), &attributes);
  MaybeOverrideName(*attributes, context.get());

  auto strictness = types::Strictness::kFlexible;
  if (layout->modifiers != nullptr)
    strictness = layout->modifiers->maybe_strictness.value_or(types::Strictness::kFlexible);

  auto resourceness = types::Resourceness::kValue;
  if (layout->modifiers != nullptr && layout->modifiers->maybe_resourceness != std::nullopt)
    resourceness = layout->modifiers->maybe_resourceness.value_or(types::Resourceness::kValue);

  RegisterDecl(std::make_unique<T>(std::move(attributes), context->ToName(library_, layout->span()),
                                   std::move(members), strictness, resourceness));
  return true;
}

bool ConsumeStep::ConsumeStructLayout(std::unique_ptr<raw::Layout> layout,
                                      const std::shared_ptr<NamingContext>& context,
                                      std::unique_ptr<raw::AttributeList> raw_attribute_list,
                                      bool is_request_or_response) {
  std::vector<Struct::Member> members;
  for (auto& mem : layout->members) {
    auto member = static_cast<raw::StructLayoutMember*>(mem.get());

    std::unique_ptr<AttributeList> attributes;
    ConsumeAttributeList(std::move(member->attributes), &attributes);

    std::unique_ptr<TypeConstructor> type_ctor;
    if (!ConsumeTypeConstructor(
            std::move(member->type_ctor), context->EnterMember(member->identifier->span()),
            /*raw_attribute_list=*/nullptr, /*is_request_or_response=*/false, &type_ctor))
      return false;

    std::unique_ptr<Constant> default_value;
    if (member->default_value != nullptr) {
      ConsumeConstant(std::move(member->default_value), &default_value);
    }

    members.emplace_back(std::move(type_ctor), member->identifier->span(), std::move(default_value),
                         std::move(attributes));
  }

  std::unique_ptr<AttributeList> attributes;
  ConsumeAttributeList(std::move(raw_attribute_list), &attributes);
  MaybeOverrideName(*attributes, context.get());

  auto resourceness = types::Resourceness::kValue;
  if (layout->modifiers != nullptr && layout->modifiers->maybe_resourceness != std::nullopt)
    resourceness = layout->modifiers->maybe_resourceness.value_or(types::Resourceness::kValue);

  RegisterDecl(std::make_unique<Struct>(std::move(attributes),
                                        context->ToName(library_, layout->span()),
                                        std::move(members), resourceness, is_request_or_response));
  return true;
}

bool ConsumeStep::ConsumeLayout(std::unique_ptr<raw::Layout> layout,
                                const std::shared_ptr<NamingContext>& context,
                                std::unique_ptr<raw::AttributeList> raw_attribute_list,
                                bool is_request_or_response) {
  switch (layout->kind) {
    case raw::Layout::Kind::kBits: {
      return ConsumeValueLayout<Bits>(std::move(layout), context, std::move(raw_attribute_list));
    }
    case raw::Layout::Kind::kEnum: {
      return ConsumeValueLayout<Enum>(std::move(layout), context, std::move(raw_attribute_list));
    }
    case raw::Layout::Kind::kStruct: {
      return ConsumeStructLayout(std::move(layout), context, std::move(raw_attribute_list),
                                 is_request_or_response);
    }
    case raw::Layout::Kind::kTable: {
      return ConsumeOrdinaledLayout<Table>(std::move(layout), context,
                                           std::move(raw_attribute_list));
    }
    case raw::Layout::Kind::kUnion: {
      return ConsumeOrdinaledLayout<Union>(std::move(layout), context,
                                           std::move(raw_attribute_list));
    }
  }
  assert(false && "layouts must be of type bits, enum, struct, table, or union");
  return true;
}

bool ConsumeStep::ConsumeTypeConstructor(std::unique_ptr<raw::TypeConstructor> raw_type_ctor,
                                         const std::shared_ptr<NamingContext>& context,
                                         std::unique_ptr<raw::AttributeList> raw_attribute_list,
                                         bool is_request_or_response,
                                         std::unique_ptr<TypeConstructor>* out_type_ctor) {
  std::vector<std::unique_ptr<LayoutParameter>> params;
  // Fallback to name span. See LayoutParameterList's comment on `span`.
  SourceSpan params_span = raw_type_ctor->layout_ref->span();
  if (raw_type_ctor->parameters) {
    params_span = raw_type_ctor->parameters->span();
    for (auto& p : raw_type_ctor->parameters->items) {
      auto param = std::move(p);
      auto span = param->span();
      switch (param->kind) {
        case raw::LayoutParameter::Kind::kLiteral: {
          auto literal_param = static_cast<raw::LiteralLayoutParameter*>(param.get());
          std::unique_ptr<LiteralConstant> constant;
          ConsumeLiteralConstant(literal_param->literal.get(), &constant);

          std::unique_ptr<LayoutParameter> consumed =
              std::make_unique<LiteralLayoutParameter>(std::move(constant), span);
          params.push_back(std::move(consumed));
          break;
        }
        case raw::LayoutParameter::Kind::kType: {
          auto type_param = static_cast<raw::TypeLayoutParameter*>(param.get());
          std::unique_ptr<TypeConstructor> type_ctor;
          if (!ConsumeTypeConstructor(std::move(type_param->type_ctor), context,
                                      /*raw_attribute_list=*/nullptr, is_request_or_response,
                                      &type_ctor))
            return false;

          std::unique_ptr<LayoutParameter> consumed =
              std::make_unique<TypeLayoutParameter>(std::move(type_ctor), span);
          params.push_back(std::move(consumed));
          break;
        }
        case raw::LayoutParameter::Kind::kIdentifier: {
          auto id_param = static_cast<raw::IdentifierLayoutParameter*>(param.get());
          auto name = CompileCompoundIdentifier(id_param->identifier.get());
          if (!name)
            return false;

          std::unique_ptr<LayoutParameter> consumed =
              std::make_unique<IdentifierLayoutParameter>(std::move(name.value()), span);
          params.push_back(std::move(consumed));
          break;
        }
      }
    }
  }

  std::vector<std::unique_ptr<Constant>> constraints;
  // Fallback to name span. See TypeConstraints's comment on `span`.
  SourceSpan constraints_span = raw_type_ctor->layout_ref->span();
  if (raw_type_ctor->constraints) {
    constraints_span = raw_type_ctor->constraints->span();
    for (auto& c : raw_type_ctor->constraints->items) {
      std::unique_ptr<Constant> constraint;
      if (!ConsumeConstant(std::move(c), &constraint))
        return false;
      constraints.push_back(std::move(constraint));
    }
  }

  if (raw_type_ctor->layout_ref->kind == raw::LayoutReference::Kind::kInline) {
    auto inline_ref = static_cast<raw::InlineLayoutReference*>(raw_type_ctor->layout_ref.get());
    auto attributes = std::move(raw_attribute_list);
    if (inline_ref->attributes != nullptr)
      attributes = std::move(inline_ref->attributes);
    if (!ConsumeLayout(std::move(inline_ref->layout), context, std::move(attributes),
                       is_request_or_response))
      return false;

    if (out_type_ctor)
      *out_type_ctor = std::make_unique<TypeConstructor>(
          context->ToName(library_, raw_type_ctor->layout_ref->span()),
          std::make_unique<LayoutParameterList>(std::move(params), params_span),
          std::make_unique<TypeConstraints>(std::move(constraints), constraints_span));
    return true;
  }

  // TODO(fxbug.dev/76349): named parameter lists are not yet allowed, so we
  //  need to ensure that is_request_or_response is false at this point.  Once
  //  that feature is enabled, this check can be removed.
  if (is_request_or_response) {
    return Fail(ErrNamedParameterListTypesNotYetSupported, raw_type_ctor->span());
  }

  auto named_ref = static_cast<raw::NamedLayoutReference*>(raw_type_ctor->layout_ref.get());
  auto name = CompileCompoundIdentifier(named_ref->identifier.get());
  if (!name)
    return false;

  assert(out_type_ctor && "out type ctors should always be provided for a named type ctor");
  *out_type_ctor = std::make_unique<TypeConstructor>(
      std::move(name.value()),
      std::make_unique<LayoutParameterList>(std::move(params), params_span),
      std::make_unique<TypeConstraints>(std::move(constraints), constraints_span));
  return true;
}

bool ConsumeStep::ConsumeTypeConstructor(std::unique_ptr<raw::TypeConstructor> raw_type_ctor,
                                         const std::shared_ptr<NamingContext>& context,
                                         std::unique_ptr<TypeConstructor>* out_type) {
  return ConsumeTypeConstructor(std::move(raw_type_ctor), context, /*raw_attribute_list=*/nullptr,
                                /*is_request_or_response=*/false, out_type);
}

void ConsumeStep::ConsumeTypeDecl(std::unique_ptr<raw::TypeDecl> type_decl) {
  auto name = Name::CreateSourced(library_, type_decl->identifier->span());
  auto& layout_ref = type_decl->type_ctor->layout_ref;
  // TODO(fxbug.dev/7807)
  if (layout_ref->kind == raw::LayoutReference::Kind::kNamed) {
    auto named_ref = static_cast<raw::NamedLayoutReference*>(layout_ref.get());
    Fail(ErrNewTypesNotAllowed, type_decl->span(), name, named_ref->span().data());
    return;
  }

  ConsumeTypeConstructor(std::move(type_decl->type_ctor), NamingContext::Create(name),
                         std::move(type_decl->attributes),
                         /*is_request_or_response=*/false,
                         /*out_type=*/nullptr);
}

bool Library::ConsumeFile(std::unique_ptr<raw::File> file) {
  return ConsumeStep(this, std::move(file)).Run();
}

void ConsumeStep::RunImpl() {
  // All fidl files in a library should agree on the library name.
  std::vector<std::string_view> new_name;
  for (const auto& part : file_->library_decl->path->components) {
    new_name.push_back(part->span().data());
  }
  if (!library_->library_name_.empty()) {
    if (new_name != library_->library_name_) {
      library_->Fail(ErrFilesDisagreeOnLibraryName,
                     file_->library_decl->path->components[0]->span());
      return;
    }
  } else {
    library_->library_name_ = new_name;
  }

  ConsumeAttributeList(std::move(file_->library_decl->attributes), &library_->attributes);

  for (auto& using_directive : std::move(file_->using_list)) {
    ConsumeUsing(std::move(using_directive));
  }
  for (auto& alias_declaration : std::move(file_->alias_list)) {
    ConsumeAliasDeclaration(std::move(alias_declaration));
  }
  for (auto& const_declaration : std::move(file_->const_declaration_list)) {
    ConsumeConstDeclaration(std::move(const_declaration));
  }
  for (auto& protocol_declaration : std::move(file_->protocol_declaration_list)) {
    ConsumeProtocolDeclaration(std::move(protocol_declaration));
  }
  for (auto& resource_declaration : std::move(file_->resource_declaration_list)) {
    ConsumeResourceDeclaration(std::move(resource_declaration));
  }
  for (auto& service_declaration : std::move(file_->service_declaration_list)) {
    ConsumeServiceDeclaration(std::move(service_declaration));
  }
  for (auto& type_decl : std::move(file_->type_decls)) {
    ConsumeTypeDecl(std::move(type_decl));
  }
}

bool CompileStep::ResolveOrOperatorConstant(Constant* constant, std::optional<const Type*> opt_type,
                                            const ConstantValue& left_operand,
                                            const ConstantValue& right_operand) {
  assert(left_operand.kind == right_operand.kind &&
         "left and right operands of or operator must be of the same kind");
  assert(opt_type && "compiler bug: type inference not implemented for or operator");
  const auto type = TypeResolve(opt_type.value());
  if (type == nullptr)
    return false;
  if (type->kind != Type::Kind::kPrimitive) {
    return FailNoSpan(ErrOrOperatorOnNonPrimitiveValue);
  }
  std::unique_ptr<ConstantValue> left_operand_u64;
  std::unique_ptr<ConstantValue> right_operand_u64;
  if (!left_operand.Convert(ConstantValue::Kind::kUint64, &left_operand_u64))
    return false;
  if (!right_operand.Convert(ConstantValue::Kind::kUint64, &right_operand_u64))
    return false;
  NumericConstantValue<uint64_t> result =
      *static_cast<NumericConstantValue<uint64_t>*>(left_operand_u64.get()) |
      *static_cast<NumericConstantValue<uint64_t>*>(right_operand_u64.get());
  std::unique_ptr<ConstantValue> converted_result;
  if (!result.Convert(ConstantValuePrimitiveKind(static_cast<const PrimitiveType*>(type)->subtype),
                      &converted_result))
    return false;
  constant->ResolveTo(std::move(converted_result), type);
  return true;
}

bool CompileStep::ResolveConstant(Constant* constant, std::optional<const Type*> opt_type) {
  assert(constant != nullptr);

  // Prevent re-entry.
  if (constant->compiled)
    return constant->IsResolved();
  constant->compiled = true;

  switch (constant->kind) {
    case Constant::Kind::kIdentifier:
      return ResolveIdentifierConstant(static_cast<IdentifierConstant*>(constant), opt_type);
    case Constant::Kind::kLiteral:
      return ResolveLiteralConstant(static_cast<LiteralConstant*>(constant), opt_type);
    case Constant::Kind::kBinaryOperator: {
      auto binary_operator_constant = static_cast<BinaryOperatorConstant*>(constant);
      if (!ResolveConstant(binary_operator_constant->left_operand.get(), opt_type)) {
        return false;
      }
      if (!ResolveConstant(binary_operator_constant->right_operand.get(), opt_type)) {
        return false;
      }
      switch (binary_operator_constant->op) {
        case BinaryOperatorConstant::Operator::kOr:
          return ResolveOrOperatorConstant(constant, opt_type,
                                           binary_operator_constant->left_operand->Value(),
                                           binary_operator_constant->right_operand->Value());
        default:
          assert(false && "Compiler bug: unhandled binary operator");
      }
      break;
    }
  }
  __builtin_unreachable();
}

ConstantValue::Kind CompileStep::ConstantValuePrimitiveKind(
    const types::PrimitiveSubtype primitive_subtype) {
  switch (primitive_subtype) {
    case types::PrimitiveSubtype::kBool:
      return ConstantValue::Kind::kBool;
    case types::PrimitiveSubtype::kInt8:
      return ConstantValue::Kind::kInt8;
    case types::PrimitiveSubtype::kInt16:
      return ConstantValue::Kind::kInt16;
    case types::PrimitiveSubtype::kInt32:
      return ConstantValue::Kind::kInt32;
    case types::PrimitiveSubtype::kInt64:
      return ConstantValue::Kind::kInt64;
    case types::PrimitiveSubtype::kUint8:
      return ConstantValue::Kind::kUint8;
    case types::PrimitiveSubtype::kUint16:
      return ConstantValue::Kind::kUint16;
    case types::PrimitiveSubtype::kUint32:
      return ConstantValue::Kind::kUint32;
    case types::PrimitiveSubtype::kUint64:
      return ConstantValue::Kind::kUint64;
    case types::PrimitiveSubtype::kFloat32:
      return ConstantValue::Kind::kFloat32;
    case types::PrimitiveSubtype::kFloat64:
      return ConstantValue::Kind::kFloat64;
  }
  assert(false && "Compiler bug: unhandled primitive subtype");
}

bool CompileStep::ResolveIdentifierConstant(IdentifierConstant* identifier_constant,
                                            std::optional<const Type*> opt_type) {
  if (opt_type) {
    assert(TypeCanBeConst(opt_type.value()) &&
           "Compiler bug: resolving identifier constant to non-const-able type!");
  }

  auto decl = library_->LookupDeclByName(identifier_constant->name.memberless_key());
  if (!decl)
    return false;
  CompileDecl(decl);

  const Type* const_type = nullptr;
  const ConstantValue* const_val = nullptr;
  switch (decl->kind) {
    case Decl::Kind::kConst: {
      auto const_decl = static_cast<Const*>(decl);
      if (!const_decl->value->IsResolved()) {
        return false;
      }
      const_type = const_decl->type_ctor->type;
      const_val = &const_decl->value->Value();
      break;
    }
    case Decl::Kind::kEnum: {
      // If there is no member name, fallthrough to default.
      if (auto member_name = identifier_constant->name.member_name(); member_name) {
        auto enum_decl = static_cast<Enum*>(decl);
        const_type = enum_decl->subtype_ctor->type;
        for (auto& member : enum_decl->members) {
          if (member.name.data() == member_name) {
            if (!member.value->IsResolved()) {
              return false;
            }
            const_val = &member.value->Value();
          }
        }
        if (!const_val) {
          return Fail(ErrUnknownEnumMember, identifier_constant->name.span().value(), *member_name);
        }
        break;
      }
      [[fallthrough]];
    }
    case Decl::Kind::kBits: {
      // If there is no member name, fallthrough to default.
      if (auto member_name = identifier_constant->name.member_name(); member_name) {
        auto bits_decl = static_cast<Bits*>(decl);
        const_type = bits_decl->subtype_ctor->type;
        for (auto& member : bits_decl->members) {
          if (member.name.data() == member_name) {
            if (!member.value->IsResolved()) {
              return false;
            }
            const_val = &member.value->Value();
          }
        }
        if (!const_val) {
          return Fail(ErrUnknownBitsMember, identifier_constant->name.span().value(), *member_name);
        }
        break;
      }
      [[fallthrough]];
    }
    default: {
      return Fail(ErrExpectedValueButGotType, identifier_constant->name.span().value(),
                  identifier_constant->name);
    }
  }

  assert(const_val && "Compiler bug: did not set const_val");
  assert(const_type && "Compiler bug: did not set const_type");

  std::unique_ptr<ConstantValue> resolved_val;
  const auto type = opt_type ? opt_type.value() : const_type;
  switch (type->kind) {
    case Type::Kind::kString: {
      if (!TypeIsConvertibleTo(const_type, type))
        goto fail_cannot_convert;

      if (!const_val->Convert(ConstantValue::Kind::kString, &resolved_val))
        goto fail_cannot_convert;
      break;
    }
    case Type::Kind::kPrimitive: {
      auto primitive_type = static_cast<const PrimitiveType*>(type);
      if (!const_val->Convert(ConstantValuePrimitiveKind(primitive_type->subtype), &resolved_val))
        goto fail_cannot_convert;
      break;
    }
    case Type::Kind::kIdentifier: {
      auto identifier_type = static_cast<const IdentifierType*>(type);
      const PrimitiveType* primitive_type;
      switch (identifier_type->type_decl->kind) {
        case Decl::Kind::kEnum: {
          auto enum_decl = static_cast<const Enum*>(identifier_type->type_decl);
          if (!enum_decl->subtype_ctor->type) {
            return false;
          }
          assert(enum_decl->subtype_ctor->type->kind == Type::Kind::kPrimitive);
          primitive_type = static_cast<const PrimitiveType*>(enum_decl->subtype_ctor->type);
          break;
        }
        case Decl::Kind::kBits: {
          auto bits_decl = static_cast<const Bits*>(identifier_type->type_decl);
          assert(bits_decl->subtype_ctor->type->kind == Type::Kind::kPrimitive);
          if (!bits_decl->subtype_ctor->type) {
            return false;
          }
          primitive_type = static_cast<const PrimitiveType*>(bits_decl->subtype_ctor->type);
          break;
        }
        default: {
          assert(false && "Compiler bug: identifier not of const-able type.");
        }
      }

      auto fail_with_mismatched_type = [this, identifier_type](const Name& type_name) {
        return FailNoSpan(ErrMismatchedNameTypeAssignment, identifier_type->type_decl->name,
                          type_name);
      };

      switch (decl->kind) {
        case Decl::Kind::kConst: {
          if (const_type->name != identifier_type->type_decl->name)
            return fail_with_mismatched_type(const_type->name);
          break;
        }
        case Decl::Kind::kBits:
        case Decl::Kind::kEnum: {
          if (decl->name != identifier_type->type_decl->name)
            return fail_with_mismatched_type(decl->name);
          break;
        }
        default: {
          assert(false && "Compiler bug: identifier not of const-able type.");
        }
      }

      if (!const_val->Convert(ConstantValuePrimitiveKind(primitive_type->subtype), &resolved_val))
        goto fail_cannot_convert;
      break;
    }
    default: {
      assert(false && "Compiler bug: identifier not of const-able type.");
    }
  }

  identifier_constant->ResolveTo(std::move(resolved_val), type);
  return true;

fail_cannot_convert:
  return Fail(ErrTypeCannotBeConvertedToType, identifier_constant->name.span().value(),
              identifier_constant, const_type, type);
}

bool CompileStep::ResolveLiteralConstant(LiteralConstant* literal_constant,
                                         std::optional<const Type*> opt_type) {
  auto inferred_type = InferType(static_cast<flat::Constant*>(literal_constant));
  const Type* type = opt_type ? opt_type.value() : inferred_type;
  if (!TypeIsConvertibleTo(inferred_type, type)) {
    return Fail(ErrTypeCannotBeConvertedToType, literal_constant->literal->span(), literal_constant,
                inferred_type, type);
  }
  switch (literal_constant->literal->kind) {
    case raw::Literal::Kind::kDocComment: {
      auto doc_comment_literal =
          static_cast<raw::DocCommentLiteral*>(literal_constant->literal.get());
      literal_constant->ResolveTo(
          std::make_unique<DocCommentConstantValue>(doc_comment_literal->span().data()),
          &Typespace::kUnboundedStringType);
      return true;
    }
    case raw::Literal::Kind::kString: {
      literal_constant->ResolveTo(
          std::make_unique<StringConstantValue>(literal_constant->literal->span().data()),
          &Typespace::kUnboundedStringType);
      return true;
    }
    case raw::Literal::Kind::kBool: {
      auto bool_literal = static_cast<raw::BoolLiteral*>(literal_constant->literal.get());
      literal_constant->ResolveTo(std::make_unique<BoolConstantValue>(bool_literal->value),
                                  &Typespace::kBoolType);
      return true;
    }
    case raw::Literal::Kind::kNumeric: {
      // Even though `untyped numeric` is convertible to any numeric type, we
      // still need to check for overflows which is done in
      // ResolveLiteralConstantKindNumericLiteral.
      switch (static_cast<const PrimitiveType*>(type)->subtype) {
        case types::PrimitiveSubtype::kInt8:
          return ResolveLiteralConstantKindNumericLiteral<int8_t>(literal_constant, type);
        case types::PrimitiveSubtype::kInt16:
          return ResolveLiteralConstantKindNumericLiteral<int16_t>(literal_constant, type);
        case types::PrimitiveSubtype::kInt32:
          return ResolveLiteralConstantKindNumericLiteral<int32_t>(literal_constant, type);
        case types::PrimitiveSubtype::kInt64:
          return ResolveLiteralConstantKindNumericLiteral<int64_t>(literal_constant, type);
        case types::PrimitiveSubtype::kUint8:
          return ResolveLiteralConstantKindNumericLiteral<uint8_t>(literal_constant, type);
        case types::PrimitiveSubtype::kUint16:
          return ResolveLiteralConstantKindNumericLiteral<uint16_t>(literal_constant, type);
        case types::PrimitiveSubtype::kUint32:
          return ResolveLiteralConstantKindNumericLiteral<uint32_t>(literal_constant, type);
        case types::PrimitiveSubtype::kUint64:
          return ResolveLiteralConstantKindNumericLiteral<uint64_t>(literal_constant, type);
        case types::PrimitiveSubtype::kFloat32:
          return ResolveLiteralConstantKindNumericLiteral<float>(literal_constant, type);
        case types::PrimitiveSubtype::kFloat64:
          return ResolveLiteralConstantKindNumericLiteral<double>(literal_constant, type);
        default:
          assert(false && "compiler bug: should not have any other primitive type reachable");
          return false;
      }
    }
  }  // switch
}

template <typename NumericType>
bool CompileStep::ResolveLiteralConstantKindNumericLiteral(LiteralConstant* literal_constant,
                                                           const Type* type) {
  NumericType value;
  const auto span = literal_constant->literal->span();
  std::string string_data(span.data().data(), span.data().data() + span.data().size());
  switch (utils::ParseNumeric(string_data, &value)) {
    case utils::ParseNumericResult::kSuccess:
      literal_constant->ResolveTo(std::make_unique<NumericConstantValue<NumericType>>(value), type);
      return true;
    case utils::ParseNumericResult::kMalformed:
      // The caller (ResolveLiteralConstant) ensures that the constant kind is
      // a numeric literal, which means that it follows the grammar for
      // numerical types. As a result, an error to parse the data here is due
      // to the data being too large, rather than bad input.
      [[fallthrough]];
    case utils::ParseNumericResult::kOutOfBounds:
      return Fail(ErrConstantOverflowsType, span, literal_constant, type);
  }
}

const Type* CompileStep::InferType(Constant* constant) {
  switch (constant->kind) {
    case Constant::Kind::kLiteral: {
      auto literal = static_cast<const raw::Literal*>(
          static_cast<const LiteralConstant*>(constant)->literal.get());
      switch (literal->kind) {
        case raw::Literal::Kind::kString: {
          auto string_literal = static_cast<const raw::StringLiteral*>(literal);
          auto inferred_size = utils::string_literal_length(string_literal->span().data());
          auto inferred_type = std::make_unique<StringType>(
              Typespace::kUnboundedStringType.name, library_->typespace_->InternSize(inferred_size),
              types::Nullability::kNonnullable);
          return library_->typespace_->Intern(std::move(inferred_type));
        }
        case raw::Literal::Kind::kNumeric:
          return &Typespace::kUntypedNumericType;
        case raw::Literal::Kind::kBool:
          return &Typespace::kBoolType;
        case raw::Literal::Kind::kDocComment:
          return &Typespace::kUnboundedStringType;
      }
      return nullptr;
    }
    case Constant::Kind::kIdentifier:
      if (!ResolveConstant(constant, std::nullopt)) {
        return nullptr;
      }
      return constant->type;
    case Constant::Kind::kBinaryOperator:
      assert(false && "compiler bug: type inference not implemented for binops");
      __builtin_unreachable();
  }
}

bool CompileStep::ResolveAsOptional(Constant* constant) const {
  assert(constant);

  if (constant->kind != Constant::Kind::kIdentifier)
    return false;

  // This refers to the `optional` constraint only if it is "optional" AND
  // it is not shadowed by a previous definition.
  // Note that as we improve scoping rules, we would need to allow `fidl.optional`
  // to be the FQN for the `optional` constant.
  auto identifier_constant = static_cast<IdentifierConstant*>(constant);
  auto decl = library_->LookupDeclByName(identifier_constant->name.memberless_key());
  if (decl)
    return false;

  return identifier_constant->name.decl_name() == "optional";
}

void CompileStep::CompileAttributeList(AttributeList* attributes) {
  Scope<std::string> scope;
  for (auto& attribute : attributes->attributes) {
    const auto original_name = attribute->name.data();
    const auto canonical_name = utils::canonicalize(original_name);
    const auto result = scope.Insert(canonical_name, attribute->name);
    if (!result.ok()) {
      const auto previous_span = result.previous_occurrence();
      if (original_name == previous_span.data()) {
        Fail(ErrDuplicateAttribute, attribute->name, original_name, previous_span);
      } else {
        Fail(ErrDuplicateAttributeCanonical, attribute->name, original_name, previous_span.data(),
             previous_span, canonical_name);
      }
    }
    CompileAttribute(attribute.get());
  }
}

void CompileStep::CompileAttribute(Attribute* attribute, bool early) {
  if (attribute->compiled) {
    return;
  }

  Scope<std::string> scope;
  for (auto& arg : attribute->args) {
    if (!arg->name.has_value()) {
      continue;
    }
    const auto original_name = arg->name.value().data();
    const auto canonical_name = utils::canonicalize(original_name);
    const auto result = scope.Insert(canonical_name, arg->name.value());
    if (!result.ok()) {
      const auto previous_span = result.previous_occurrence();
      if (original_name == previous_span.data()) {
        Fail(ErrDuplicateAttributeArg, attribute->span, attribute, original_name, previous_span);
      } else {
        Fail(ErrDuplicateAttributeArgCanonical, attribute->span, attribute, original_name,
             previous_span.data(), previous_span, canonical_name);
      }
    }
  }

  const AttributeSchema& schema =
      library_->all_libraries_->RetrieveAttributeSchema(reporter(), attribute,
                                                        /* warn_on_typo = */ true);
  if (early) {
    assert(schema.CanCompileEarly() && "attribute is not allowed to be compiled early");
  }
  schema.ResolveArgs(this, attribute);
  attribute->compiled = true;
}

// static
void CompileStep::CompileAttributeEarly(Library* library, Attribute* attribute) {
  CompileStep(library).CompileAttribute(attribute, /* early = */ true);
}

const Type* CompileStep::TypeResolve(const Type* type) {
  if (type->kind != Type::Kind::kIdentifier) {
    return type;
  }
  auto identifier_type = static_cast<const IdentifierType*>(type);
  Decl* decl = library_->LookupDeclByName(identifier_type->name);
  if (!decl) {
    FailNoSpan(ErrCouldNotResolveIdentifierToType);
    return nullptr;
  }
  CompileDecl(decl);
  switch (decl->kind) {
    case Decl::Kind::kBits:
      return static_cast<const Bits*>(decl)->subtype_ctor->type;
    case Decl::Kind::kEnum:
      return static_cast<const Enum*>(decl)->subtype_ctor->type;
    default:
      return type;
  }
}

bool CompileStep::TypeCanBeConst(const Type* type) {
  switch (type->kind) {
    case flat::Type::Kind::kString:
      return type->nullability != types::Nullability::kNullable;
    case flat::Type::Kind::kPrimitive:
      return true;
    case flat::Type::Kind::kIdentifier: {
      auto identifier_type = static_cast<const IdentifierType*>(type);
      switch (identifier_type->type_decl->kind) {
        case Decl::Kind::kEnum:
        case Decl::Kind::kBits:
          return true;
        default:
          return false;
      }
    }
    default:
      return false;
  }  // switch
}

bool CompileStep::TypeIsConvertibleTo(const Type* from_type, const Type* to_type) {
  switch (to_type->kind) {
    case flat::Type::Kind::kString: {
      if (from_type->kind != flat::Type::Kind::kString)
        return false;

      auto from_string_type = static_cast<const flat::StringType*>(from_type);
      auto to_string_type = static_cast<const flat::StringType*>(to_type);

      if (to_string_type->nullability == types::Nullability::kNonnullable &&
          from_string_type->nullability != types::Nullability::kNonnullable)
        return false;

      if (to_string_type->max_size->value < from_string_type->max_size->value)
        return false;

      return true;
    }
    case flat::Type::Kind::kPrimitive: {
      auto to_primitive_type = static_cast<const flat::PrimitiveType*>(to_type);
      switch (from_type->kind) {
        case flat::Type::Kind::kUntypedNumeric:
          return to_primitive_type->subtype != types::PrimitiveSubtype::kBool;
        case flat::Type::Kind::kPrimitive:
          break;  // handled below
        default:
          return false;
      }
      auto from_primitive_type = static_cast<const flat::PrimitiveType*>(from_type);
      switch (to_primitive_type->subtype) {
        case types::PrimitiveSubtype::kBool:
          return from_primitive_type->subtype == types::PrimitiveSubtype::kBool;
        default:
          // TODO(pascallouis): be more precise about convertibility, e.g. it
          // should not be allowed to convert a float to an int.
          return from_primitive_type->subtype != types::PrimitiveSubtype::kBool;
      }
    }
    default:
      return false;
  }  // switch
}

// Library resolution is concerned with resolving identifiers to their
// declarations, and with computing type sizes and alignments.

Decl* Library::LookupDeclByName(Name::Key name) const {
  auto iter = declarations_.find(name);
  if (iter == declarations_.end()) {
    return nullptr;
  }
  return iter->second;
}

bool SortStep::AddConstantDependencies(const Constant* constant, std::set<const Decl*>* out_edges) {
  switch (constant->kind) {
    case Constant::Kind::kIdentifier: {
      auto identifier = static_cast<const flat::IdentifierConstant*>(constant);
      auto decl = library_->LookupDeclByName(identifier->name.memberless_key());
      if (decl == nullptr) {
        return Fail(ErrFailedConstantLookup, identifier->name.span().value(), identifier->name);
      }
      out_edges->insert(decl);
      break;
    }
    case Constant::Kind::kLiteral: {
      // Literal and synthesized constants have no dependencies on other declarations.
      break;
    }
    case Constant::Kind::kBinaryOperator: {
      auto op = static_cast<const flat::BinaryOperatorConstant*>(constant);
      return AddConstantDependencies(op->left_operand.get(), out_edges) &&
             AddConstantDependencies(op->right_operand.get(), out_edges);
    }
  }
  return true;
}

// Calculating declaration dependencies is largely serving the C/C++ family of languages bindings.
// For instance, the declaration of a struct member type must be defined before the containing
// struct if that member is stored inline.
// Given the FIDL declarations:
//
//     struct D2 { D1 d; }
//     struct D1 { int32 x; }
//
// We must first declare D1, followed by D2 when emitting C code.
//
// Below, an edge from D1 to D2 means that we must see the declaration of of D1 before
// the declaration of D2, i.e. the calculated set of |out_edges| represents all the declarations
// that |decl| depends on.
//
// Notes:
// - Nullable structs do not require dependency edges since they are boxed via a
// pointer indirection, and their content placed out-of-line.
bool SortStep::DeclDependencies(const Decl* decl, std::set<const Decl*>* out_edges) {
  std::set<const Decl*> edges;

  auto maybe_add_decl = [&edges](const TypeConstructor* type_ctor) {
    const TypeConstructor* current = type_ctor;
    for (;;) {
      const auto& invocation = current->resolved_params;
      if (invocation.from_type_alias) {
        assert(!invocation.element_type_resolved &&
               "Compiler bug: partial aliases should be disallowed");
        edges.insert(invocation.from_type_alias);
        return;
      }

      const Type* type = current->type;
      if (type->nullability == types::Nullability::kNullable)
        return;

      switch (type->kind) {
        case Type::Kind::kHandle: {
          auto handle_type = static_cast<const HandleType*>(type);
          assert(handle_type->resource_decl);
          auto decl = static_cast<const Decl*>(handle_type->resource_decl);
          edges.insert(decl);
          return;
        }
        case Type::Kind::kPrimitive:
        case Type::Kind::kString:
        case Type::Kind::kTransportSide:
        case Type::Kind::kBox:
          return;
        case Type::Kind::kArray:
        case Type::Kind::kVector: {
          if (invocation.element_type_raw != nullptr) {
            current = invocation.element_type_raw;
            break;
          }
          // The type_ctor won't have an arg_type_ctor if the type is Bytes.
          // In that case, just return since there are no edges
          return;
        }
        case Type::Kind::kIdentifier: {
          // should have been caught above and returned early.
          assert(type->nullability != types::Nullability::kNullable);
          auto identifier_type = static_cast<const IdentifierType*>(type);
          auto decl = static_cast<const Decl*>(identifier_type->type_decl);
          if (decl->kind != Decl::Kind::kProtocol) {
            edges.insert(decl);
          }
          return;
        }
        case Type::Kind::kUntypedNumeric:
          assert(false && "compiler bug: should not have untyped numeric here");
      }
    }
  };

  switch (decl->kind) {
    case Decl::Kind::kBits: {
      auto bits_decl = static_cast<const Bits*>(decl);
      maybe_add_decl(bits_decl->subtype_ctor.get());
      for (const auto& member : bits_decl->members) {
        if (!AddConstantDependencies(member.value.get(), &edges)) {
          return false;
        }
      }
      break;
    }
    case Decl::Kind::kConst: {
      auto const_decl = static_cast<const Const*>(decl);
      maybe_add_decl(const_decl->type_ctor.get());
      if (!AddConstantDependencies(const_decl->value.get(), &edges)) {
        return false;
      }
      break;
    }
    case Decl::Kind::kEnum: {
      auto enum_decl = static_cast<const Enum*>(decl);
      maybe_add_decl(enum_decl->subtype_ctor.get());
      for (const auto& member : enum_decl->members) {
        if (!AddConstantDependencies(member.value.get(), &edges)) {
          return false;
        }
      }
      break;
    }
    case Decl::Kind::kProtocol: {
      auto protocol_decl = static_cast<const Protocol*>(decl);
      for (const auto& composed_protocol : protocol_decl->composed_protocols) {
        if (auto type_decl = library_->LookupDeclByName(composed_protocol.name); type_decl) {
          edges.insert(type_decl);
        }
      }
      for (const auto& method : protocol_decl->methods) {
        if (method.maybe_request != nullptr) {
          if (auto type_decl = library_->LookupDeclByName(method.maybe_request->name); type_decl) {
            edges.insert(type_decl);
          }
        }
        if (method.maybe_response != nullptr) {
          if (auto type_decl = library_->LookupDeclByName(method.maybe_response->name); type_decl) {
            edges.insert(type_decl);
          }
        }
      }
      break;
    }
    case Decl::Kind::kResource: {
      auto resource_decl = static_cast<const Resource*>(decl);
      maybe_add_decl(resource_decl->subtype_ctor.get());
      break;
    }
    case Decl::Kind::kService: {
      auto service_decl = static_cast<const Service*>(decl);
      for (const auto& member : service_decl->members) {
        maybe_add_decl(member.type_ctor.get());
      }
      break;
    }
    case Decl::Kind::kStruct: {
      auto struct_decl = static_cast<const Struct*>(decl);
      for (const auto& member : struct_decl->members) {
        maybe_add_decl(member.type_ctor.get());
        if (member.maybe_default_value) {
          if (!AddConstantDependencies(member.maybe_default_value.get(), &edges)) {
            return false;
          }
        }
      }
      break;
    }
    case Decl::Kind::kTable: {
      auto table_decl = static_cast<const Table*>(decl);
      for (const auto& member : table_decl->members) {
        if (!member.maybe_used)
          continue;
        maybe_add_decl(member.maybe_used->type_ctor.get());
        if (member.maybe_used->maybe_default_value) {
          if (!AddConstantDependencies(member.maybe_used->maybe_default_value.get(), &edges)) {
            return false;
          }
        }
      }
      break;
    }
    case Decl::Kind::kUnion: {
      auto union_decl = static_cast<const Union*>(decl);
      for (const auto& member : union_decl->members) {
        if (!member.maybe_used)
          continue;
        maybe_add_decl(member.maybe_used->type_ctor.get());
      }
      break;
    }
    case Decl::Kind::kTypeAlias: {
      auto type_alias_decl = static_cast<const TypeAlias*>(decl);
      maybe_add_decl(type_alias_decl->partial_type_ctor.get());
    }
  }  // switch
  *out_edges = std::move(edges);
  return true;
}

namespace {
// Declaration comparator.
//
// (1) To compare two Decl's in the same library, it suffices to compare the
//     unqualified names of the Decl's. (This is faster.)
//
// (2) To compare two Decl's across libraries, we rely on the fully qualified
//     names of the Decl's. (This is slower.)
struct CmpDeclInLibrary {
  bool operator()(const Decl* a, const Decl* b) const {
    assert(a->name != b->name || a == b);
    const Library* a_library = a->name.library();
    const Library* b_library = b->name.library();
    if (a_library != b_library) {
      return NameFlatName(a->name) < NameFlatName(b->name);
    } else {
      return a->name.decl_name() < b->name.decl_name();
    }
  }
};
}  // namespace

void SortStep::RunImpl() {
  // |degree| is the number of undeclared dependencies for each decl.
  std::map<const Decl*, uint32_t, CmpDeclInLibrary> degrees;
  // |inverse_dependencies| records the decls that depend on each decl.
  std::map<const Decl*, std::vector<const Decl*>, CmpDeclInLibrary> inverse_dependencies;
  for (const auto& name_and_decl : library_->declarations_) {
    const Decl* decl = name_and_decl.second;
    std::set<const Decl*> deps;
    if (!DeclDependencies(decl, &deps))
      return;
    degrees[decl] = static_cast<uint32_t>(deps.size());
    for (const Decl* dep : deps) {
      inverse_dependencies[dep].push_back(decl);
    }
  }

  // Start with all decls that have no incoming edges.
  std::vector<const Decl*> decls_without_deps;
  for (const auto& decl_and_degree : degrees) {
    if (decl_and_degree.second == 0u) {
      decls_without_deps.push_back(decl_and_degree.first);
    }
  }

  while (!decls_without_deps.empty()) {
    // Pull one out of the queue.
    auto decl = decls_without_deps.back();
    decls_without_deps.pop_back();
    assert(degrees[decl] == 0u);
    library_->declaration_order_.push_back(decl);

    // Decrement the incoming degree of all the other decls it
    // points to.
    auto& inverse_deps = inverse_dependencies[decl];
    for (const Decl* inverse_dep : inverse_deps) {
      uint32_t& degree = degrees[inverse_dep];
      assert(degree != 0u);
      degree -= 1;
      if (degree == 0u)
        decls_without_deps.push_back(inverse_dep);
    }
  }

  if (library_->declaration_order_.size() != degrees.size()) {
    // We didn't visit all the edges! There was a cycle.
    library_->FailNoSpan(ErrIncludeCycle);
  }
}

void CompileStep::CompileDecl(Decl* decl) {
  if (decl->compiled) {
    return;
  }
  if (decl->compiling) {
    FailNoSpan(ErrIncludeCycle);
    return;
  }
  Compiling guard(decl);
  switch (decl->kind) {
    case Decl::Kind::kBits:
      CompileBits(static_cast<Bits*>(decl));
      break;
    case Decl::Kind::kConst:
      CompileConst(static_cast<Const*>(decl));
      break;
    case Decl::Kind::kEnum:
      CompileEnum(static_cast<Enum*>(decl));
      break;
    case Decl::Kind::kProtocol:
      CompileProtocol(static_cast<Protocol*>(decl));
      break;
    case Decl::Kind::kResource:
      CompileResource(static_cast<Resource*>(decl));
      break;
    case Decl::Kind::kService:
      CompileService(static_cast<Service*>(decl));
      break;
    case Decl::Kind::kStruct:
      CompileStruct(static_cast<Struct*>(decl));
      break;
    case Decl::Kind::kTable:
      CompileTable(static_cast<Table*>(decl));
      break;
    case Decl::Kind::kUnion:
      CompileUnion(static_cast<Union*>(decl));
      break;
    case Decl::Kind::kTypeAlias:
      CompileTypeAlias(static_cast<TypeAlias*>(decl));
      break;
  }  // switch
}

template <template <typename> typename Constness, typename MemberFn>
static void ForEachDeclMemberHelper(Constness<Decl>* decl, MemberFn f) {
  switch (decl->kind) {
    case Decl::Kind::kBits:
      for (auto& member : static_cast<Constness<Bits>*>(decl)->members) {
        f(&member);
      }
      break;
    case Decl::Kind::kConst:
      break;
    case Decl::Kind::kEnum:
      for (auto& member : static_cast<Constness<Enum>*>(decl)->members) {
        f(&member);
      }
      break;
    case Decl::Kind::kProtocol:
      for (auto& method_with_info : static_cast<Constness<Protocol>*>(decl)->all_methods) {
        f(method_with_info.method);
      }
      break;
    case Decl::Kind::kResource:
      for (auto& member : static_cast<Constness<Resource>*>(decl)->properties) {
        f(&member);
      }
      break;
    case Decl::Kind::kService:
      for (auto& member : static_cast<Constness<Service>*>(decl)->members) {
        f(&member);
      }
      break;
    case Decl::Kind::kStruct:
      for (auto& member : static_cast<Constness<Struct>*>(decl)->members) {
        f(&member);
      }
      break;
    case Decl::Kind::kTable:
      for (auto& member : static_cast<Constness<Table>*>(decl)->members) {
        f(&member);
      }
      break;
    case Decl::Kind::kUnion:
      for (auto& member : static_cast<Constness<Union>*>(decl)->members) {
        f(&member);
      }
      break;
    case Decl::Kind::kTypeAlias:
      break;
  }  // switch
}

template <typename MemberFn>
static void ForEachDeclMember(const Decl* decl, MemberFn f) {
  ForEachDeclMemberHelper<std::add_const_t>(decl, f);
}

template <typename MemberFn>
static void ForEachDeclMember(Decl* decl, MemberFn f) {
  ForEachDeclMemberHelper<std::remove_const_t>(decl, f);
}

void VerifyAttributesStep::VerifyDecl(const Decl* decl) {
  assert(decl->compiled && "verification must happen after compilation of decls");
  VerifyAttributes(decl);
  ForEachDeclMember(decl, [this](const Attributable* member) { VerifyAttributes(member); });
}

void VerifyResourcenessStep::VerifyDecl(const Decl* decl) {
  assert(decl->compiled && "verification must happen after compilation of decls");
  switch (decl->kind) {
    case Decl::Kind::kStruct: {
      const auto* struct_decl = static_cast<const Struct*>(decl);
      if (struct_decl->resourceness == types::Resourceness::kValue) {
        for (const auto& member : struct_decl->members) {
          if (EffectiveResourceness(member.type_ctor->type) == types::Resourceness::kResource) {
            Fail(ErrTypeMustBeResource, struct_decl->name.span().value(), struct_decl->name,
                 member.name.data(), "struct", struct_decl->name);
          }
        }
      }
      break;
    }
    case Decl::Kind::kTable: {
      const auto* table_decl = static_cast<const Table*>(decl);
      if (table_decl->resourceness == types::Resourceness::kValue) {
        for (const auto& member : table_decl->members) {
          if (member.maybe_used) {
            const auto& used = *member.maybe_used;
            if (EffectiveResourceness(used.type_ctor->type) == types::Resourceness::kResource) {
              Fail(ErrTypeMustBeResource, table_decl->name.span().value(), table_decl->name,
                   used.name.data(), "table", table_decl->name);
            }
          }
        }
      }
      break;
    }
    case Decl::Kind::kUnion: {
      const auto* union_decl = static_cast<const Union*>(decl);
      if (union_decl->resourceness == types::Resourceness::kValue) {
        for (const auto& member : union_decl->members) {
          if (member.maybe_used) {
            const auto& used = *member.maybe_used;
            if (EffectiveResourceness(used.type_ctor->type) == types::Resourceness::kResource) {
              Fail(ErrTypeMustBeResource, union_decl->name.span().value(), union_decl->name,
                   used.name.data(), "union", union_decl->name);
            }
          }
        }
      }
      break;
    }
    default:
      break;
  }
}

types::Resourceness Type::Resourceness() const {
  switch (this->kind) {
    case Type::Kind::kPrimitive:
    case Type::Kind::kString:
      return types::Resourceness::kValue;
    case Type::Kind::kHandle:
    case Type::Kind::kTransportSide:
      return types::Resourceness::kResource;
    case Type::Kind::kArray:
      return static_cast<const ArrayType*>(this)->element_type->Resourceness();
    case Type::Kind::kVector:
      return static_cast<const VectorType*>(this)->element_type->Resourceness();
    case Type::Kind::kIdentifier:
      break;
    case Type::Kind::kBox:
      return static_cast<const BoxType*>(this)->boxed_type->Resourceness();
    case Type::Kind::kUntypedNumeric:
      assert(false && "compiler bug: should not have untyped numeric here");
  }

  const auto* decl = static_cast<const IdentifierType*>(this)->type_decl;

  switch (decl->kind) {
    case Decl::Kind::kBits:
    case Decl::Kind::kEnum:
      return types::Resourceness::kValue;
    case Decl::Kind::kProtocol:
      return types::Resourceness::kResource;
    case Decl::Kind::kStruct:
      assert(decl->compiled && "Compiler bug: accessing resourceness of not-yet-compiled struct");
      return static_cast<const Struct*>(decl)->resourceness.value();
    case Decl::Kind::kTable:
      return static_cast<const Table*>(decl)->resourceness;
    case Decl::Kind::kUnion:
      assert(decl->compiled && "Compiler bug: accessing resourceness of not-yet-compiled union");
      return static_cast<const Union*>(decl)->resourceness.value();
    case Decl::Kind::kConst:
    case Decl::Kind::kResource:
    case Decl::Kind::kService:
    case Decl::Kind::kTypeAlias:
      assert(false && "Compiler bug: unexpected kind");
  }

  __builtin_unreachable();
}

types::Resourceness VerifyResourcenessStep::EffectiveResourceness(const Type* type) {
  switch (type->kind) {
    case Type::Kind::kPrimitive:
    case Type::Kind::kString:
      return types::Resourceness::kValue;
    case Type::Kind::kHandle:
    case Type::Kind::kTransportSide:
      return types::Resourceness::kResource;
    case Type::Kind::kArray:
      return EffectiveResourceness(static_cast<const ArrayType*>(type)->element_type);
    case Type::Kind::kVector:
      return EffectiveResourceness(static_cast<const VectorType*>(type)->element_type);
    case Type::Kind::kIdentifier:
      break;
    case Type::Kind::kBox:
      return EffectiveResourceness(static_cast<const BoxType*>(type)->boxed_type);
    case Type::Kind::kUntypedNumeric:
      assert(false && "compiler bug: should not have untyped numeric here");
  }

  const auto* decl = static_cast<const IdentifierType*>(type)->type_decl;

  switch (decl->kind) {
    case Decl::Kind::kBits:
    case Decl::Kind::kEnum:
      return types::Resourceness::kValue;
    case Decl::Kind::kProtocol:
      return types::Resourceness::kResource;
    case Decl::Kind::kStruct:
      if (static_cast<const Struct*>(decl)->resourceness.value() ==
          types::Resourceness::kResource) {
        return types::Resourceness::kResource;
      }
      break;
    case Decl::Kind::kTable:
      if (static_cast<const Table*>(decl)->resourceness == types::Resourceness::kResource) {
        return types::Resourceness::kResource;
      }
      break;
    case Decl::Kind::kUnion:
      if (static_cast<const Union*>(decl)->resourceness.value() == types::Resourceness::kResource) {
        return types::Resourceness::kResource;
      }
      break;
    case Decl::Kind::kService:
      return types::Resourceness::kValue;
    case Decl::Kind::kConst:
    case Decl::Kind::kResource:
    case Decl::Kind::kTypeAlias:
      assert(false && "Compiler bug: unexpected kind");
  }

  const auto [it, inserted] = effective_resourceness_.try_emplace(decl, std::nullopt);
  if (!inserted) {
    const auto& maybe_value = it->second;
    // If we already computed effective resourceness, return it. If we started
    // computing it but did not complete (nullopt), we're in a cycle, so return
    // kValue as the default assumption.
    return maybe_value.value_or(types::Resourceness::kValue);
  }

  switch (decl->kind) {
    case Decl::Kind::kStruct:
      for (const auto& member : static_cast<const Struct*>(decl)->members) {
        if (EffectiveResourceness(member.type_ctor->type) == types::Resourceness::kResource) {
          effective_resourceness_[decl] = types::Resourceness::kResource;
          return types::Resourceness::kResource;
        }
      }
      break;
    case Decl::Kind::kTable:
      for (const auto& member : static_cast<const Table*>(decl)->members) {
        const auto& used = member.maybe_used;
        if (used &&
            EffectiveResourceness(used->type_ctor->type) == types::Resourceness::kResource) {
          effective_resourceness_[decl] = types::Resourceness::kResource;
          return types::Resourceness::kResource;
        }
      }
      break;
    case Decl::Kind::kUnion:
      for (const auto& member : static_cast<const Union*>(decl)->members) {
        const auto& used = member.maybe_used;
        if (used &&
            EffectiveResourceness(used->type_ctor->type) == types::Resourceness::kResource) {
          effective_resourceness_[decl] = types::Resourceness::kResource;
          return types::Resourceness::kResource;
        }
      }
      break;
    default:
      assert(false && "Compiler bug: unexpected kind");
  }

  effective_resourceness_[decl] = types::Resourceness::kValue;
  return types::Resourceness::kValue;
}

void CompileStep::CompileBits(Bits* bits_declaration) {
  CompileAttributeList(bits_declaration->attributes.get());
  for (auto& member : bits_declaration->members) {
    CompileAttributeList(member.attributes.get());
  }

  CompileTypeConstructor(bits_declaration->subtype_ctor.get());
  if (!bits_declaration->subtype_ctor->type) {
    return;
  }

  if (bits_declaration->subtype_ctor->type->kind != Type::Kind::kPrimitive) {
    Fail(ErrBitsTypeMustBeUnsignedIntegralPrimitive, bits_declaration->name.span().value(),
         bits_declaration->subtype_ctor->type);
    return;
  }

  // Validate constants.
  auto primitive_type = static_cast<const PrimitiveType*>(bits_declaration->subtype_ctor->type);
  switch (primitive_type->subtype) {
    case types::PrimitiveSubtype::kUint8: {
      uint8_t mask;
      if (!ValidateBitsMembersAndCalcMask<uint8_t>(bits_declaration, &mask))
        return;
      bits_declaration->mask = mask;
      break;
    }
    case types::PrimitiveSubtype::kUint16: {
      uint16_t mask;
      if (!ValidateBitsMembersAndCalcMask<uint16_t>(bits_declaration, &mask))
        return;
      bits_declaration->mask = mask;
      break;
    }
    case types::PrimitiveSubtype::kUint32: {
      uint32_t mask;
      if (!ValidateBitsMembersAndCalcMask<uint32_t>(bits_declaration, &mask))
        return;
      bits_declaration->mask = mask;
      break;
    }
    case types::PrimitiveSubtype::kUint64: {
      uint64_t mask;
      if (!ValidateBitsMembersAndCalcMask<uint64_t>(bits_declaration, &mask))
        return;
      bits_declaration->mask = mask;
      break;
    }
    case types::PrimitiveSubtype::kBool:
    case types::PrimitiveSubtype::kInt8:
    case types::PrimitiveSubtype::kInt16:
    case types::PrimitiveSubtype::kInt32:
    case types::PrimitiveSubtype::kInt64:
    case types::PrimitiveSubtype::kFloat32:
    case types::PrimitiveSubtype::kFloat64:
      Fail(ErrBitsTypeMustBeUnsignedIntegralPrimitive, bits_declaration->name.span().value(),
           bits_declaration->subtype_ctor->type);
      return;
  }
}

void CompileStep::CompileConst(Const* const_declaration) {
  CompileAttributeList(const_declaration->attributes.get());
  CompileTypeConstructor(const_declaration->type_ctor.get());
  const auto* const_type = const_declaration->type_ctor->type;
  if (!const_type) {
    return;
  }
  if (!TypeCanBeConst(const_type)) {
    Fail(ErrInvalidConstantType, const_declaration->name.span().value(), const_type);
  } else if (!ResolveConstant(const_declaration->value.get(), const_type)) {
    Fail(ErrCannotResolveConstantValue, const_declaration->name.span().value());
  }
}

void CompileStep::CompileEnum(Enum* enum_declaration) {
  CompileAttributeList(enum_declaration->attributes.get());
  for (auto& member : enum_declaration->members) {
    CompileAttributeList(member.attributes.get());
  }

  CompileTypeConstructor(enum_declaration->subtype_ctor.get());
  if (!enum_declaration->subtype_ctor->type) {
    return;
  }

  if (enum_declaration->subtype_ctor->type->kind != Type::Kind::kPrimitive) {
    Fail(ErrEnumTypeMustBeIntegralPrimitive, enum_declaration->name.span().value(),
         enum_declaration->subtype_ctor->type);
    return;
  }

  // Validate constants.
  auto primitive_type = static_cast<const PrimitiveType*>(enum_declaration->subtype_ctor->type);
  enum_declaration->type = primitive_type;
  switch (primitive_type->subtype) {
    case types::PrimitiveSubtype::kInt8: {
      int8_t unknown_value;
      if (ValidateEnumMembersAndCalcUnknownValue<int8_t>(enum_declaration, &unknown_value)) {
        enum_declaration->unknown_value_signed = unknown_value;
      }
      break;
    }
    case types::PrimitiveSubtype::kInt16: {
      int16_t unknown_value;
      if (ValidateEnumMembersAndCalcUnknownValue<int16_t>(enum_declaration, &unknown_value)) {
        enum_declaration->unknown_value_signed = unknown_value;
      }
      break;
    }
    case types::PrimitiveSubtype::kInt32: {
      int32_t unknown_value;
      if (ValidateEnumMembersAndCalcUnknownValue<int32_t>(enum_declaration, &unknown_value)) {
        enum_declaration->unknown_value_signed = unknown_value;
      }
      break;
    }
    case types::PrimitiveSubtype::kInt64: {
      int64_t unknown_value;
      if (ValidateEnumMembersAndCalcUnknownValue<int64_t>(enum_declaration, &unknown_value)) {
        enum_declaration->unknown_value_signed = unknown_value;
      }
      break;
    }
    case types::PrimitiveSubtype::kUint8: {
      uint8_t unknown_value;
      if (ValidateEnumMembersAndCalcUnknownValue<uint8_t>(enum_declaration, &unknown_value)) {
        enum_declaration->unknown_value_unsigned = unknown_value;
      }
      break;
    }
    case types::PrimitiveSubtype::kUint16: {
      uint16_t unknown_value;
      if (ValidateEnumMembersAndCalcUnknownValue<uint16_t>(enum_declaration, &unknown_value)) {
        enum_declaration->unknown_value_unsigned = unknown_value;
      }
      break;
    }
    case types::PrimitiveSubtype::kUint32: {
      uint32_t unknown_value;
      if (ValidateEnumMembersAndCalcUnknownValue<uint32_t>(enum_declaration, &unknown_value)) {
        enum_declaration->unknown_value_unsigned = unknown_value;
      }
      break;
    }
    case types::PrimitiveSubtype::kUint64: {
      uint64_t unknown_value;
      if (ValidateEnumMembersAndCalcUnknownValue<uint64_t>(enum_declaration, &unknown_value)) {
        enum_declaration->unknown_value_unsigned = unknown_value;
      }
      break;
    }
    case types::PrimitiveSubtype::kBool:
    case types::PrimitiveSubtype::kFloat32:
    case types::PrimitiveSubtype::kFloat64:
      Fail(ErrEnumTypeMustBeIntegralPrimitive, enum_declaration->name.span().value(),
           enum_declaration->subtype_ctor->type);
      break;
  }
}

bool HasSimpleLayout(const Decl* decl) {
  return decl->attributes->Get("for_deprecated_c_bindings") != nullptr;
}

void CompileStep::CompileResource(Resource* resource_declaration) {
  Scope<std::string_view> scope;

  CompileAttributeList(resource_declaration->attributes.get());
  CompileTypeConstructor(resource_declaration->subtype_ctor.get());
  if (!resource_declaration->subtype_ctor->type) {
    return;
  }

  if (resource_declaration->subtype_ctor->type->kind != Type::Kind::kPrimitive) {
    Fail(ErrEnumTypeMustBeIntegralPrimitive, resource_declaration->name.span().value(),
         resource_declaration->subtype_ctor->type);
    return;
  }

  for (auto& property : resource_declaration->properties) {
    CompileAttributeList(property.attributes.get());
    auto name_result = scope.Insert(property.name.data(), property.name);
    if (!name_result.ok()) {
      Fail(ErrDuplicateResourcePropertyName, property.name, name_result.previous_occurrence());
      return;
    }
    CompileTypeConstructor(property.type_ctor.get());
  }
}

void CompileStep::CompileProtocol(Protocol* protocol_declaration) {
  CompileAttributeList(protocol_declaration->attributes.get());

  MethodScope method_scope;
  auto CheckScopes = [this, &protocol_declaration, &method_scope](const Protocol* protocol,
                                                                  auto Visitor) -> void {
    for (const auto& composed_protocol : protocol->composed_protocols) {
      auto name = composed_protocol.name;
      auto decl = library_->LookupDeclByName(name);
      // TODO(fxbug.dev/7926): Special handling here should not be required, we
      // should first rely on creating the types representing composed
      // protocols.
      if (!decl || decl->kind != Decl::Kind::kProtocol) {
        // No need to report an error here since it was already done by the loop
        // after the definition of CheckScopes (before calling CheckScopes).
        continue;
      }
      auto composed_protocol_declaration = static_cast<const Protocol*>(decl);
      auto span = composed_protocol_declaration->name.span();
      assert(span);
      if (method_scope.protocols.Insert(composed_protocol_declaration, span.value()).ok()) {
        Visitor(composed_protocol_declaration, Visitor);
      } else {
        // Otherwise we have already seen this protocol in
        // the inheritance graph.
      }
    }
    for (const auto& method : protocol->methods) {
      const auto original_name = method.name.data();
      const auto canonical_name = utils::canonicalize(original_name);
      const auto name_result = method_scope.canonical_names.Insert(canonical_name, method.name);
      if (!name_result.ok()) {
        const auto previous_span = name_result.previous_occurrence();
        if (original_name == previous_span.data()) {
          Fail(ErrDuplicateMethodName, method.name, original_name, previous_span);
        } else {
          Fail(ErrDuplicateMethodNameCanonical, method.name, original_name, previous_span.data(),
               previous_span, canonical_name);
        }
      }
      if (!method.generated_ordinal64) {
        // If a composed method failed to compile, we do not associate have a
        // generated ordinal, and proceeding leads to a segfault. Instead,
        // continue to the next method, without reporting additional errors (the
        // error emitted when compiling the composed method is sufficient).
        continue;
      }
      if (method.generated_ordinal64->value == 0) {
        Fail(ErrGeneratedZeroValueOrdinal, method.generated_ordinal64->span());
      }
      auto ordinal_result =
          method_scope.ordinals.Insert(method.generated_ordinal64->value, method.name);
      if (!ordinal_result.ok()) {
        std::string replacement_method(
            fidl::ordinals::GetSelector(method.attributes.get(), method.name));
        replacement_method.push_back('_');
        Fail(ErrDuplicateMethodOrdinal, method.generated_ordinal64->span(),
             ordinal_result.previous_occurrence(), replacement_method);
      }

      // Add a pointer to this method to the protocol_declarations list.
      bool is_composed = protocol_declaration != protocol;
      protocol_declaration->all_methods.emplace_back(&method, is_composed);
    }
  };

  // Before scope checking can occur, ordinals must be generated for each of the
  // protocol's methods, including those that were composed from transitive
  // child protocols.  This means that child protocols must be compiled prior to
  // this one, or they will not have generated_ordinal64s on their methods, and
  // will fail the scope check.
  for (const auto& composed_protocol : protocol_declaration->composed_protocols) {
    CompileAttributeList(composed_protocol.attributes.get());
    auto decl = library_->LookupDeclByName(composed_protocol.name);
    if (!decl) {
      Fail(ErrUnknownType, composed_protocol.name.span().value(), composed_protocol.name);
      continue;
    }
    if (decl->kind != Decl::Kind::kProtocol) {
      Fail(ErrComposingNonProtocol, composed_protocol.name.span().value());
      continue;
    }
    CompileDecl(decl);
  }
  for (auto& method : protocol_declaration->methods) {
    CompileAttributeList(method.attributes.get());
    auto selector = fidl::ordinals::GetSelector(method.attributes.get(), method.name);
    if (!utils::IsValidIdentifierComponent(selector) &&
        !utils::IsValidFullyQualifiedMethodIdentifier(selector)) {
      Fail(ErrInvalidSelectorValue,
           method.attributes->Get("selector")->GetArg(AttributeArg::kDefaultAnonymousName)->span);
      continue;
    }
    // TODO(fxbug.dev/77623): Remove.
    auto library_name = library_->library_name_;
    if (library_name.size() == 2 && library_name[0] == "fuchsia" && library_name[1] == "io" &&
        selector.find("/") == selector.npos) {
      Fail(ErrFuchsiaIoExplicitOrdinals, method.name);
      continue;
    }
    method.generated_ordinal64 = std::make_unique<raw::Ordinal64>(library_->method_hasher_(
        library_name, protocol_declaration->name.decl_name(), selector, *method.identifier));
  }

  CheckScopes(protocol_declaration, CheckScopes);

  for (auto& method : protocol_declaration->methods) {
    if (method.maybe_request != nullptr) {
      const Name name = method.maybe_request->name;
      CompileTypeConstructor(method.maybe_request.get());
      Decl* decl = library_->LookupDeclByName(name);
      if (!method.maybe_request->type || !decl) {
        Fail(ErrUnknownType, name.span().value(), name);
        continue;
      }
      CompileDecl(decl);
    }
    if (method.maybe_response != nullptr) {
      const Name name = method.maybe_response->name;
      CompileTypeConstructor(method.maybe_response.get());
      Decl* decl = library_->LookupDeclByName(name);
      if (!method.maybe_response->type || !decl) {
        Fail(ErrUnknownType, name.span().value(), name);
        continue;
      }
      CompileDecl(decl);
    }
  }
}

void CompileStep::CompileService(Service* service_decl) {
  Scope<std::string> scope;

  CompileAttributeList(service_decl->attributes.get());
  for (auto& member : service_decl->members) {
    CompileAttributeList(member.attributes.get());
    const auto original_name = member.name.data();
    const auto canonical_name = utils::canonicalize(original_name);
    const auto name_result = scope.Insert(canonical_name, member.name);
    if (!name_result.ok()) {
      const auto previous_span = name_result.previous_occurrence();
      if (original_name == previous_span.data()) {
        Fail(ErrDuplicateServiceMemberName, member.name, original_name, previous_span);
      } else {
        Fail(ErrDuplicateServiceMemberNameCanonical, member.name, original_name,
             previous_span.data(), previous_span, canonical_name);
      }
    }
    CompileTypeConstructor(member.type_ctor.get());
    if (!member.type_ctor->type) {
      continue;
    }
    if (member.type_ctor->type->kind != Type::Kind::kTransportSide) {
      Fail(ErrOnlyClientEndsInServices, member.name);
      continue;
    }
    const auto transport_side_type = static_cast<const TransportSideType*>(member.type_ctor->type);
    if (transport_side_type->end != TransportSide::kClient) {
      Fail(ErrOnlyClientEndsInServices, member.name);
    }
    if (member.type_ctor->type->nullability != types::Nullability::kNonnullable) {
      Fail(ErrNullableServiceMember, member.name);
    }
  }
}

void CompileStep::CompileStruct(Struct* struct_declaration) {
  Scope<std::string> scope;
  DeriveResourceness derive_resourceness(&struct_declaration->resourceness);

  CompileAttributeList(struct_declaration->attributes.get());
  for (auto& member : struct_declaration->members) {
    CompileAttributeList(member.attributes.get());
    const auto original_name = member.name.data();
    const auto canonical_name = utils::canonicalize(original_name);
    const auto name_result = scope.Insert(canonical_name, member.name);
    if (!name_result.ok()) {
      const auto previous_span = name_result.previous_occurrence();
      if (original_name == previous_span.data()) {
        Fail(struct_declaration->is_request_or_response ? ErrDuplicateMethodParameterName
                                                        : ErrDuplicateStructMemberName,
             member.name, original_name, previous_span);
      } else {
        Fail(struct_declaration->is_request_or_response ? ErrDuplicateMethodParameterNameCanonical
                                                        : ErrDuplicateStructMemberNameCanonical,
             member.name, original_name, previous_span.data(), previous_span, canonical_name);
      }
    }

    CompileTypeConstructor(member.type_ctor.get());
    if (!member.type_ctor->type) {
      continue;
    }
    assert(!(struct_declaration->is_request_or_response && member.maybe_default_value) &&
           "method parameters cannot have default values");
    if (member.maybe_default_value) {
      const auto* default_value_type = member.type_ctor->type;
      if (!TypeCanBeConst(default_value_type)) {
        Fail(ErrInvalidStructMemberType, struct_declaration->name.span().value(),
             NameIdentifier(member.name), default_value_type);
      } else if (!ResolveConstant(member.maybe_default_value.get(), default_value_type)) {
        Fail(ErrCouldNotResolveMemberDefault, member.name, NameIdentifier(member.name));
      }
    }
    derive_resourceness.AddType(member.type_ctor->type);
  }
}

void CompileStep::CompileTable(Table* table_declaration) {
  Scope<std::string> name_scope;
  Ordinal64Scope ordinal_scope;

  CompileAttributeList(table_declaration->attributes.get());
  if (table_declaration->members.size() > kMaxTableOrdinals) {
    FailNoSpan(ErrTooManyTableOrdinals);
  }

  for (size_t i = 0; i < table_declaration->members.size(); i++) {
    auto& member = table_declaration->members[i];
    CompileAttributeList(member.attributes.get());
    const auto ordinal_result = ordinal_scope.Insert(member.ordinal->value, member.ordinal->span());
    if (!ordinal_result.ok()) {
      Fail(ErrDuplicateTableFieldOrdinal, member.ordinal->span(),
           ordinal_result.previous_occurrence());
    }
    if (!member.maybe_used) {
      continue;
    }
    auto& member_used = *member.maybe_used;
    const auto original_name = member_used.name.data();
    const auto canonical_name = utils::canonicalize(original_name);
    const auto name_result = name_scope.Insert(canonical_name, member_used.name);
    if (!name_result.ok()) {
      const auto previous_span = name_result.previous_occurrence();
      if (original_name == previous_span.data()) {
        Fail(ErrDuplicateTableFieldName, member_used.name, original_name, previous_span);
      } else {
        Fail(ErrDuplicateTableFieldNameCanonical, member_used.name, original_name,
             previous_span.data(), previous_span, canonical_name);
      }
    }
    CompileTypeConstructor(member_used.type_ctor.get());
    if (!member_used.type_ctor->type) {
      continue;
    }
    if (member_used.type_ctor->type->nullability != types::Nullability::kNonnullable) {
      Fail(ErrNullableTableMember, member_used.name);
    }
    if (i == kMaxTableOrdinals - 1) {
      if (member_used.type_ctor->type->kind != Type::Kind::kIdentifier) {
        FailNoSpan(ErrMaxOrdinalNotTable);
      } else {
        auto identifier_type = static_cast<const IdentifierType*>(member_used.type_ctor->type);
        if (identifier_type->type_decl->kind != Decl::Kind::kTable) {
          FailNoSpan(ErrMaxOrdinalNotTable);
        }
      }
    }
  }

  if (auto ordinal_and_loc = FindFirstNonDenseOrdinal(ordinal_scope)) {
    auto [ordinal, span] = *ordinal_and_loc;
    Fail(ErrNonDenseOrdinal, span, ordinal);
  }
}

void CompileStep::CompileUnion(Union* union_declaration) {
  Scope<std::string> scope;
  Ordinal64Scope ordinal_scope;
  DeriveResourceness derive_resourceness(&union_declaration->resourceness);

  CompileAttributeList(union_declaration->attributes.get());
  for (const auto& member : union_declaration->members) {
    CompileAttributeList(member.attributes.get());
    const auto ordinal_result = ordinal_scope.Insert(member.ordinal->value, member.ordinal->span());
    if (!ordinal_result.ok()) {
      Fail(ErrDuplicateUnionMemberOrdinal, member.ordinal->span(),
           ordinal_result.previous_occurrence());
    }
    if (!member.maybe_used) {
      continue;
    }
    const auto& member_used = *member.maybe_used;
    const auto original_name = member_used.name.data();
    const auto canonical_name = utils::canonicalize(original_name);
    const auto name_result = scope.Insert(canonical_name, member_used.name);
    if (!name_result.ok()) {
      const auto previous_span = name_result.previous_occurrence();
      if (original_name == previous_span.data()) {
        Fail(ErrDuplicateUnionMemberName, member_used.name, original_name, previous_span);
      } else {
        Fail(ErrDuplicateUnionMemberNameCanonical, member_used.name, original_name,
             previous_span.data(), previous_span, canonical_name);
      }
    }

    CompileTypeConstructor(member_used.type_ctor.get());
    if (!member_used.type_ctor->type) {
      continue;
    }
    if (member_used.type_ctor->type->nullability != types::Nullability::kNonnullable) {
      Fail(ErrNullableUnionMember, member_used.name);
    }
    derive_resourceness.AddType(member_used.type_ctor->type);
  }

  if (auto ordinal_and_loc = FindFirstNonDenseOrdinal(ordinal_scope)) {
    auto [ordinal, span] = *ordinal_and_loc;
    Fail(ErrNonDenseOrdinal, span, ordinal);
  }
}

void CompileStep::CompileTypeAlias(TypeAlias* type_alias) {
  CompileAttributeList(type_alias->attributes.get());

  if (type_alias->partial_type_ctor->name == type_alias->name) {
    // fidlc's current semantics for cases like `alias foo = foo;` is to
    // include the LHS in the scope while compiling the RHS. Note that because
    // of an interaction with a fidlc scoping bug that prevents shadowing builtins,
    // this means that `alias Recursive = Recursive;` will fail with an includes
    // cycle error, but e.g. `alias uint32 = uint32;` won't because the user
    // defined `uint32` fails to shadow the builtin which means that we successfully
    // resolve the RHS. To avoid inconsistent semantics, we need to manually
    // catch this case and fail.
    FailNoSpan(ErrIncludeCycle);
    return;
  }
  CompileTypeConstructor(type_alias->partial_type_ctor.get());
}

bool Library::Compile() {
  if (!CompileStep(this).Run())
    return false;
  if (!SortStep(this).Run())
    return false;
  if (!VerifyResourcenessStep(this).Run())
    return false;
  if (!VerifyAttributesStep(this).Run())
    return false;
  if (!VerifyInlineSizeStep(this).Run())
    return false;
  if (!VerifyDependenciesStep(this).Run())
    return false;

  assert(reporter()->errors().empty() && "errors should have caused an early return");
  return true;
}

void CompileStep::RunImpl() {
  CompileAttributeList(library_->attributes.get());
  for (auto& [name, decl] : library_->declarations_) {
    CompileDecl(decl);
  }
}

void VerifyResourcenessStep::RunImpl() {
  for (const Decl* decl : library_->declaration_order_) {
    VerifyDecl(decl);
  }
}

void VerifyAttributesStep::RunImpl() {
  VerifyAttributes(library_);
  for (const Decl* decl : library_->declaration_order_) {
    VerifyDecl(decl);
  }
}

void VerifyDependenciesStep::RunImpl() {
  library_->dependencies_.VerifyAllDependenciesWereUsed(*library_, reporter());
}

void CompileStep::CompileTypeConstructor(TypeConstructor* type_ctor) {
  if (type_ctor->type != nullptr) {
    return;
  }
  if (!library_->typespace_->Create(LibraryMediator(library_, this, reporter()), type_ctor->name,
                                    type_ctor->parameters, type_ctor->constraints, &type_ctor->type,
                                    &type_ctor->resolved_params)) {
    return;
  }

  // postcondition: compilation sets the Type of the TypeConstructor
  assert(type_ctor->type && "type constructors' type not resolved after compilation");
  VerifyTypeCategory(type_ctor->type, type_ctor->name.span(), AllowedCategories::kTypeOnly);
}

bool CompileStep::VerifyTypeCategory(const Type* type, std::optional<SourceSpan> span,
                                     AllowedCategories category) {
  assert(type && "CompileTypeConstructor did not set Type");
  if (type->kind != Type::Kind::kIdentifier) {
    // we assume that all non-identifier types (i.e. builtins) are actually
    // types (and not e.g. protocols or services).
    return category == AllowedCategories::kProtocolOnly ? Fail(ErrCannotUseType, span.value())
                                                        : true;
  }

  auto identifier_type = static_cast<const IdentifierType*>(type);
  switch (identifier_type->type_decl->kind) {
    // services are never allowed in any context
    case Decl::Kind::kService:
      return Fail(ErrCannotUseService, span.value());
      break;
    case Decl::Kind::kProtocol:
      if (category == AllowedCategories::kTypeOnly)
        return Fail(ErrCannotUseProtocol, span.value());
      break;
    default:
      if (category == AllowedCategories::kProtocolOnly)
        return Fail(ErrCannotUseType, span.value());
      break;
  }
  return true;
}

bool CompileStep::ResolveHandleRightsConstant(Resource* resource, Constant* constant,
                                              const HandleRights** out_rights) {
  if (resource->subtype_ctor == nullptr || resource->subtype_ctor->name.full_name() != "uint32") {
    return FailNoSpan(ErrResourceMustBeUint32Derived, resource->name);
  }

  auto rights_property = resource->LookupProperty("rights");
  if (!rights_property) {
    return false;
  }

  Decl* rights_decl = library_->LookupDeclByName(rights_property->type_ctor->name);
  if (!rights_decl || rights_decl->kind != Decl::Kind::kBits) {
    return false;
  }

  CompileTypeConstructor(rights_property->type_ctor.get());
  const Type* rights_type = rights_property->type_ctor->type;
  if (!rights_type) {
    return false;
  }

  if (!ResolveConstant(constant, rights_type))
    return false;

  if (out_rights)
    *out_rights = static_cast<const HandleRights*>(&constant->Value());
  return true;
}

bool CompileStep::ResolveHandleSubtypeIdentifier(Resource* resource,
                                                 const std::unique_ptr<Constant>& constant,
                                                 uint32_t* out_obj_type) {
  // We only support an extremely limited form of resource suitable for
  // handles here, where it must be:
  // - derived from uint32
  // - have a single properties element
  // - the single property element must be a reference to an enum
  // - the single property must be named "subtype".
  if (constant->kind != Constant::Kind::kIdentifier) {
    return false;
  }
  auto identifier_constant = static_cast<IdentifierConstant*>(constant.get());
  const Name& handle_subtype_identifier = identifier_constant->name;

  if (resource->subtype_ctor == nullptr || resource->subtype_ctor->name.full_name() != "uint32") {
    return false;
  }
  auto subtype_property = resource->LookupProperty("subtype");
  if (!subtype_property) {
    return false;
  }

  Decl* subtype_decl = library_->LookupDeclByName(subtype_property->type_ctor->name);
  if (!subtype_decl || subtype_decl->kind != Decl::Kind::kEnum) {
    return false;
  }

  CompileTypeConstructor(subtype_property->type_ctor.get());
  const Type* subtype_type = subtype_property->type_ctor->type;
  if (!subtype_type) {
    return false;
  }

  auto* subtype_enum = static_cast<Enum*>(subtype_decl);
  for (const auto& member : subtype_enum->members) {
    if (member.name.data() == handle_subtype_identifier.span()->data()) {
      if (!ResolveConstant(member.value.get(), subtype_type)) {
        return false;
      }
      const flat::ConstantValue& value = member.value->Value();
      auto obj_type = static_cast<uint32_t>(
          reinterpret_cast<const flat::NumericConstantValue<uint32_t>&>(value));
      *out_obj_type = obj_type;
      return true;
    }
  }

  return false;
}

bool CompileStep::ResolveSizeBound(Constant* size_constant, const Size** out_size) {
  if (!ResolveConstant(size_constant, &Typespace::kUint32Type)) {
    if (size_constant->kind == Constant::Kind::kIdentifier) {
      auto name = static_cast<IdentifierConstant*>(size_constant)->name;
      if (name.library() == library_ && name.decl_name() == "MAX" && !name.member_name()) {
        size_constant->ResolveTo(std::make_unique<Size>(Size::Max()), &Typespace::kUint32Type);
      }
    }
  }
  if (!size_constant->IsResolved()) {
    return false;
  }
  if (out_size) {
    *out_size = static_cast<const Size*>(&size_constant->Value());
  }
  return true;
}

template <typename DeclType, typename MemberType>
bool CompileStep::ValidateMembers(DeclType* decl, MemberValidator<MemberType> validator) {
  assert(decl != nullptr);
  auto checkpoint = reporter()->Checkpoint();

  constexpr const char* decl_type = std::is_same_v<DeclType, Enum> ? "enum" : "bits";

  Scope<std::string> name_scope;
  Scope<MemberType> value_scope;
  for (const auto& member : decl->members) {
    assert(member.value != nullptr && "Compiler bug: member value is null!");

    // Check that the member identifier hasn't been used yet
    const auto original_name = member.name.data();
    const auto canonical_name = utils::canonicalize(original_name);
    const auto name_result = name_scope.Insert(canonical_name, member.name);
    if (!name_result.ok()) {
      const auto previous_span = name_result.previous_occurrence();
      // We can log the error and then continue validating for other issues in the decl
      if (original_name == name_result.previous_occurrence().data()) {
        Fail(ErrDuplicateMemberName, member.name, decl_type, original_name, previous_span);
      } else {
        Fail(ErrDuplicateMemberNameCanonical, member.name, decl_type, original_name,
             previous_span.data(), previous_span, canonical_name);
      }
    }

    if (!ResolveConstant(member.value.get(), decl->subtype_ctor->type)) {
      Fail(ErrCouldNotResolveMember, member.name, decl_type);
      continue;
    }

    MemberType value =
        static_cast<const NumericConstantValue<MemberType>&>(member.value->Value()).value;
    const auto value_result = value_scope.Insert(value, member.name);
    if (!value_result.ok()) {
      const auto previous_span = value_result.previous_occurrence();
      // We can log the error and then continue validating other members for other bugs
      Fail(ErrDuplicateMemberValue, member.name, decl_type, original_name, previous_span.data(),
           previous_span);
    }

    auto err = validator(value, member.attributes.get());
    if (err) {
      err->span = member.name;
      Report(std::move(err));
    }
  }

  return checkpoint.NoNewErrors();
}

template <typename T>
static bool IsPowerOfTwo(T t) {
  if (t == 0) {
    return false;
  }
  if ((t & (t - 1)) != 0) {
    return false;
  }
  return true;
}

template <typename MemberType>
bool CompileStep::ValidateBitsMembersAndCalcMask(Bits* bits_decl, MemberType* out_mask) {
  static_assert(std::is_unsigned<MemberType>::value && !std::is_same<MemberType, bool>::value,
                "Bits members must be an unsigned integral type!");
  // Each bits member must be a power of two.
  MemberType mask = 0u;
  auto validator = [&mask](MemberType member, const AttributeList*) -> std::unique_ptr<Diagnostic> {
    if (!IsPowerOfTwo(member)) {
      return Diagnostic::MakeError(ErrBitsMemberMustBePowerOfTwo, std::nullopt);
    }
    mask |= member;
    return nullptr;
  };
  if (!ValidateMembers<Bits, MemberType>(bits_decl, validator)) {
    return false;
  }
  *out_mask = mask;
  return true;
}

template <typename MemberType>
bool CompileStep::ValidateEnumMembersAndCalcUnknownValue(Enum* enum_decl,
                                                         MemberType* out_unknown_value) {
  static_assert(std::is_integral<MemberType>::value && !std::is_same<MemberType, bool>::value,
                "Enum members must be an integral type!");

  const auto default_unknown_value = std::numeric_limits<MemberType>::max();
  std::optional<MemberType> explicit_unknown_value;
  for (const auto& member : enum_decl->members) {
    if (!ResolveConstant(member.value.get(), enum_decl->subtype_ctor->type)) {
      // ValidateMembers will resolve each member and report errors.
      continue;
    }
    if (member.attributes->Get("unknown") != nullptr) {
      if (explicit_unknown_value.has_value()) {
        return Fail(ErrUnknownAttributeOnMultipleEnumMembers, member.name);
      }
      explicit_unknown_value =
          static_cast<const NumericConstantValue<MemberType>&>(member.value->Value()).value;
    }
  }

  auto validator = [enum_decl, &explicit_unknown_value](
                       MemberType member,
                       const AttributeList* attributes) -> std::unique_ptr<Diagnostic> {
    switch (enum_decl->strictness) {
      case types::Strictness::kStrict:
        if (attributes->Get("unknown") != nullptr) {
          return Diagnostic::MakeError(ErrUnknownAttributeOnStrictEnumMember, std::nullopt);
        }
        return nullptr;
      case types::Strictness::kFlexible:
        if (member == default_unknown_value && !explicit_unknown_value.has_value()) {
          return Diagnostic::MakeError(ErrFlexibleEnumMemberWithMaxValue, std::nullopt,
                                       std::to_string(default_unknown_value));
        }
        return nullptr;
    }
  };
  if (!ValidateMembers<Enum, MemberType>(enum_decl, validator)) {
    return false;
  }
  *out_unknown_value = explicit_unknown_value.value_or(default_unknown_value);
  return true;
}

const std::set<Library*>& Library::dependencies() const { return dependencies_.dependencies(); }

std::set<const Library*, LibraryComparator> Library::DirectDependencies() const {
  std::set<const Library*, LibraryComparator> direct_dependencies;
  auto add_constant_deps = [&](const Constant* constant) {
    if (constant->kind != Constant::Kind::kIdentifier)
      return;
    auto* dep_library = static_cast<const IdentifierConstant*>(constant)->name.library();
    assert(dep_library != nullptr && "all identifier constants have a library");
    direct_dependencies.insert(dep_library);
  };
  auto add_type_ctor_deps = [&](const TypeConstructor& type_ctor) {
    if (auto dep_library = type_ctor.name.library())
      direct_dependencies.insert(dep_library);

    // TODO(fxbug.dev/64629): Add dependencies introduced through handle constraints.
    // This code currently assumes the handle constraints are always defined in the same
    // library as the resource_definition and so does not check for them separately.
    const auto& invocation = type_ctor.resolved_params;
    if (invocation.size_raw)
      add_constant_deps(invocation.size_raw);
    if (invocation.protocol_decl_raw)
      add_constant_deps(invocation.protocol_decl_raw);
    if (invocation.element_type_raw != nullptr) {
      if (auto dep_library = invocation.element_type_raw->name.library())
        direct_dependencies.insert(dep_library);
    }
    if (invocation.boxed_type_raw != nullptr) {
      if (auto dep_library = invocation.boxed_type_raw->name.library())
        direct_dependencies.insert(dep_library);
    }
  };
  for (const auto& dep_library : dependencies()) {
    direct_dependencies.insert(dep_library);
  }
  // Discover additional dependencies that are required to support
  // cross-library protocol composition.
  for (const auto& protocol : protocol_declarations_) {
    for (const auto method_with_info : protocol->all_methods) {
      if (method_with_info.method->maybe_request) {
        auto id =
            static_cast<const flat::IdentifierType*>(method_with_info.method->maybe_request->type);

        // TODO(fxbug.dev/88343): switch on union/table when those are enabled.
        auto as_struct = static_cast<const flat::Struct*>(id->type_decl);
        for (const auto& member : as_struct->members) {
          add_type_ctor_deps(*member.type_ctor);
        }
      }
      if (method_with_info.method->maybe_response) {
        auto id =
            static_cast<const flat::IdentifierType*>(method_with_info.method->maybe_response->type);

        // TODO(fxbug.dev/88343): switch on union/table when those are enabled.
        auto as_struct = static_cast<const flat::Struct*>(id->type_decl);
        for (const auto& member : as_struct->members) {
          add_type_ctor_deps(*member.type_ctor);
        }
      }
      direct_dependencies.insert(method_with_info.method->owning_protocol->name.library());
    }
  }
  direct_dependencies.erase(this);
  return direct_dependencies;
}

std::unique_ptr<TypeConstructor> TypeConstructor::CreateSizeType() {
  std::vector<std::unique_ptr<LayoutParameter>> no_params;
  std::vector<std::unique_ptr<Constant>> no_constraints;
  return std::make_unique<TypeConstructor>(
      Name::CreateIntrinsic("uint32"),
      std::make_unique<LayoutParameterList>(std::move(no_params), std::nullopt /* span */),
      std::make_unique<TypeConstraints>(std::move(no_constraints), std::nullopt /* span */));
}

bool LibraryMediator::ResolveParamAsType(const flat::TypeTemplate* layout,
                                         const std::unique_ptr<LayoutParameter>& param,
                                         const Type** out_type) const {
  auto type_ctor = param->AsTypeCtor();
  auto check = reporter()->Checkpoint();
  if (!type_ctor || !ResolveType(type_ctor)) {
    // if there were no errors reported but we couldn't resolve to a type, it must
    // mean that the parameter referred to a non-type, so report a new error here.
    if (check.NoNewErrors()) {
      return Fail(ErrExpectedType, param->span);
    }
    // otherwise, there was an error during the type resolution process, so we
    // should just report that rather than add an extra error here
    return false;
  }
  *out_type = type_ctor->type;
  return true;
}

bool LibraryMediator::ResolveParamAsSize(const flat::TypeTemplate* layout,
                                         const std::unique_ptr<LayoutParameter>& param,
                                         const Size** out_size) const {
  // We could use param->AsConstant() here, leading to code similar to ResolveParamAsType.
  // However, unlike ErrExpectedType, ErrExpectedValueButGotType requires a name to be
  // reported, which would require doing a switch on the parameter kind anyway to find
  // its Name. So we just handle all the cases ourselves from the start.
  switch (param->kind) {
    case LayoutParameter::Kind::kLiteral: {
      auto literal_param = static_cast<LiteralLayoutParameter*>(param.get());
      if (!ResolveSizeBound(literal_param->literal.get(), out_size))
        return Fail(ErrCouldNotParseSizeBound, literal_param->span);
      break;
    }
    case LayoutParameter::kType: {
      auto type_param = static_cast<TypeLayoutParameter*>(param.get());
      return Fail(ErrExpectedValueButGotType, type_param->span, type_param->type_ctor->name);
    }
    case LayoutParameter::Kind::kIdentifier: {
      auto ambig_param = static_cast<IdentifierLayoutParameter*>(param.get());
      auto as_constant = ambig_param->AsConstant();
      if (!ResolveSizeBound(as_constant, out_size))
        return Fail(ErrExpectedValueButGotType, ambig_param->span, ambig_param->name);
      break;
    }
  }
  assert(*out_size);
  if ((*out_size)->value == 0)
    return Fail(ErrMustHaveNonZeroSize, param->span, layout);
  return true;
}

bool LibraryMediator::ResolveConstraintAs(const std::unique_ptr<Constant>& constraint,
                                          const std::vector<ConstraintKind>& interpretations,
                                          Resource* resource, ResolvedConstraint* out) const {
  for (const auto& constraint_kind : interpretations) {
    out->kind = constraint_kind;
    switch (constraint_kind) {
      case ConstraintKind::kHandleSubtype: {
        assert(resource &&
               "Compiler bug: must pass resource if trying to resolve to handle subtype");
        if (ResolveAsHandleSubtype(resource, constraint, &out->value.handle_subtype))
          return true;
        break;
      }
      case ConstraintKind::kHandleRights: {
        assert(resource &&
               "Compiler bug: must pass resource if trying to resolve to handle rights");
        if (ResolveAsHandleRights(resource, constraint.get(), &(out->value.handle_rights)))
          return true;
        break;
      }
      case ConstraintKind::kSize: {
        if (ResolveSizeBound(constraint.get(), &(out->value.size)))
          return true;
        break;
      }
      case ConstraintKind::kNullability: {
        if (ResolveAsOptional(constraint.get()))
          return true;
        break;
      }
      case ConstraintKind::kProtocol: {
        if (ResolveAsProtocol(constraint.get(), &(out->value.protocol_decl)))
          return true;
        break;
      }
    }
  }
  return false;
}

bool LibraryMediator::ResolveType(TypeConstructor* type) const {
  compile_step_->CompileTypeConstructor(type);
  return type->type != nullptr;
}

bool LibraryMediator::ResolveSizeBound(Constant* size_constant, const Size** out_size) const {
  return compile_step_->ResolveSizeBound(size_constant, out_size);
}

bool LibraryMediator::ResolveAsOptional(Constant* constant) const {
  return compile_step_->ResolveAsOptional(constant);
}

bool LibraryMediator::ResolveAsHandleSubtype(Resource* resource,
                                             const std::unique_ptr<Constant>& constant,
                                             uint32_t* out_obj_type) const {
  return compile_step_->ResolveHandleSubtypeIdentifier(resource, constant, out_obj_type);
}

bool LibraryMediator::ResolveAsHandleRights(Resource* resource, Constant* constant,
                                            const HandleRights** out_rights) const {
  return compile_step_->ResolveHandleRightsConstant(resource, constant, out_rights);
}

bool LibraryMediator::ResolveAsProtocol(const Constant* constant, const Protocol** out_decl) const {
  // TODO(fxbug.dev/75112): If/when this method is responsible for reporting errors, the
  // `return false` statements should fail with ErrConstraintMustBeProtocol instead.
  if (constant->kind != Constant::Kind::kIdentifier)
    return false;

  const auto* as_identifier = static_cast<const IdentifierConstant*>(constant);
  const auto* decl = LookupDeclByName(as_identifier->name);
  if (!decl || decl->kind != Decl::Kind::kProtocol)
    return false;
  *out_decl = static_cast<const Protocol*>(decl);
  return true;
}

Decl* LibraryMediator::LookupDeclByName(Name::Key name) const {
  return library_->LookupDeclByName(name);
}

TypeConstructor* LiteralLayoutParameter::AsTypeCtor() const { return nullptr; }
TypeConstructor* TypeLayoutParameter::AsTypeCtor() const { return type_ctor.get(); }
TypeConstructor* IdentifierLayoutParameter::AsTypeCtor() const {
  if (!as_type_ctor) {
    std::vector<std::unique_ptr<LayoutParameter>> no_params;
    std::vector<std::unique_ptr<Constant>> no_constraints;
    as_type_ctor = std::make_unique<TypeConstructor>(
        name, std::make_unique<LayoutParameterList>(std::move(no_params), std::nullopt),
        std::make_unique<TypeConstraints>(std::move(no_constraints), std::nullopt));
  }

  return as_type_ctor.get();
}

Constant* LiteralLayoutParameter::AsConstant() const { return literal.get(); }
Constant* TypeLayoutParameter::AsConstant() const { return nullptr; }
Constant* IdentifierLayoutParameter::AsConstant() const {
  if (!as_constant) {
    as_constant = std::make_unique<IdentifierConstant>(name, span);
  }
  return as_constant.get();
}

void LibraryMediator::CompileDecl(Decl* decl) const { compile_step_->CompileDecl(decl); }

}  // namespace fidl::flat
