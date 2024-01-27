// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/fidl/fidlc/include/fidl/flat/availability_step.h"

#include <zircon/assert.h>

#include "tools/fidl/fidlc/include/fidl/diagnostics.h"
#include "tools/fidl/fidlc/include/fidl/flat/compile_step.h"
#include "tools/fidl/fidlc/include/fidl/flat_ast.h"
#include "tools/fidl/fidlc/include/fidl/versioning_types.h"

namespace fidl::flat {

void AvailabilityStep::RunImpl() {
  PopulateLexicalParents();
  library()->TraverseElements([&](Element* element) { CompileAvailability(element); });
  VerifyNoDeclOverlaps();
}

void AvailabilityStep::PopulateLexicalParents() {
  // First, map members to the Decl they occur in.
  for (const auto& entry : library()->declarations.all) {
    Decl* decl = entry.second;
    decl->ForEachMember(
        [this, decl](const Element* member) { lexical_parents_.emplace(member, decl); });
  }

  // Second, map anonymous layouts to the struct/table/union member or method
  // whose type constructor they occur in. We do this with a helpful function
  // that recursively visits all anonymous types in `type_ctor`.
  std::function<void(Element*, const TypeConstructor*)> link_anonymous =
      [&](Element* member, const TypeConstructor* type_ctor) -> void {
    if (type_ctor->layout.IsSynthetic()) {
      auto anon_layout = type_ctor->layout.raw_synthetic().target.element();
      lexical_parents_.emplace(anon_layout, member);
    }
    for (const auto& param : type_ctor->parameters->items) {
      if (auto param_type_ctor = param->AsTypeCtor()) {
        link_anonymous(member, param_type_ctor);
      }
    }
  };

  for (auto& decl : library()->declarations.structs) {
    for (auto& member : decl->members) {
      link_anonymous(&member, member.type_ctor.get());
    }
  }
  for (auto& decl : library()->declarations.tables) {
    for (auto& member : decl->members) {
      if (member.maybe_used) {
        link_anonymous(&member, member.maybe_used->type_ctor.get());
      }
    }
  }
  for (auto& decl : library()->declarations.unions) {
    for (auto& member : decl->members) {
      if (member.maybe_used) {
        link_anonymous(&member, member.maybe_used->type_ctor.get());
      }
    }
  }
  for (auto& protocol : library()->declarations.protocols) {
    for (auto& method : protocol->methods) {
      if (auto& request = method.maybe_request) {
        link_anonymous(&method, request.get());
      }
      if (auto& response = method.maybe_response) {
        link_anonymous(&method, response.get());
      }
    }
  }
  for (auto& decl : library()->declarations.resources) {
    for (auto& property : decl->properties) {
      link_anonymous(&property, property.type_ctor.get());
    }
  }
}

void AvailabilityStep::CompileAvailability(Element* element) {
  if (element->availability.state() != Availability::State::kUnset) {
    // Already compiled.
    return;
  }

  // Inheritance relies on the parent being compiled first.
  if (auto parent = LexicalParent(element)) {
    CompileAvailability(parent);
  }

  // If this is an anonymous layout, don't attempt to compile the attribute
  // since it can result in misleading errors. Instead, rely on
  // VerifyAttributesStep to report an error about the attribute placement.
  if (!element->IsAnonymousLayout()) {
    if (auto* attribute = element->attributes->Get("available")) {
      CompileAvailabilityFromAttribute(element, attribute);
      return;
    }
  }

  // There is no attribute, so simulate an empty one -- unless this is the
  // library declaration, in which case we default to @available(added=HEAD).
  std::optional<Version> default_added;
  if (element->kind == Element::Kind::kLibrary) {
    ZX_ASSERT(element == library());
    library()->platform = GetDefaultPlatform();
    default_added = Version::Head();
  }
  bool valid = element->availability.Init({.added = default_added});
  ZX_ASSERT_MSG(valid, "initializing default availability should succeed");
  if (auto source = AvailabilityToInheritFrom(element)) {
    auto result = element->availability.Inherit(source.value());
    ZX_ASSERT_MSG(result.Ok(), "inheriting into default availability should succeed");
  }
}

void AvailabilityStep::CompileAvailabilityFromAttribute(Element* element, Attribute* attribute) {
  CompileStep::CompileAttributeEarly(compiler(), attribute);

  const bool is_library = element->kind == Element::Kind::kLibrary;
  ZX_ASSERT(is_library == (element == library()));

  const auto platform = attribute->GetArg("platform");
  const auto added = attribute->GetArg("added");
  const auto deprecated = attribute->GetArg("deprecated");
  const auto removed = attribute->GetArg("removed");
  const auto note = attribute->GetArg("note");
  const auto legacy = attribute->GetArg("legacy");

  if (attribute->args.empty()) {
    Fail(ErrAvailableMissingArguments, attribute->span);
  }
  if (note && !deprecated) {
    Fail(ErrNoteWithoutDeprecation, attribute->span);
  }
  if (!is_library && platform) {
    Fail(ErrPlatformNotOnLibrary, platform->span);
  }
  if (is_library && !added && !attribute->args.empty()) {
    Fail(ErrLibraryAvailabilityMissingAdded, attribute->span);
  }
  if (!is_library && !library()->attributes->Get("available")) {
    Fail(ErrMissingLibraryAvailability, attribute->span, library()->name);
    // Return early to avoid confusing error messages about inheritance
    // conflicts with the default @available(added=HEAD) on the library.
    element->availability.Fail();
    return;
  }

  const auto init_args = Availability::InitArgs{
      .added = GetVersion(added),
      .deprecated = GetVersion(deprecated),
      .removed = GetVersion(removed),
      .legacy = GetLegacy(legacy),
  };
  if (is_library) {
    library()->platform = GetPlatform(platform).value_or(GetDefaultPlatform());
    if (!init_args.added) {
      // Return early to avoid letting the -inf from Availability::Unbounded()
      // propagate any further, since .Inherit() asserts added != -inf.
      element->availability.Fail();
      return;
    }
  }
  if (!element->availability.Init(init_args)) {
    Fail(ErrInvalidAvailabilityOrder, attribute->span);
    // Return early to avoid confusing error messages about inheritance
    // conflicts for an availability that isn't even self-consistent.
    return;
  }

  // Reports an error for arg given its inheritance status.
  auto report = [&](const AttributeArg* arg, Availability::InheritResult::Status status) {
    std::string_view child_what, when, parent_what;
    switch (status) {
      case Availability::InheritResult::Status::kOk:
        return;
      case Availability::InheritResult::Status::kBeforeParentAdded:
        when = "before";
        parent_what = "added";
        break;
      case Availability::InheritResult::Status::kAfterParentDeprecated:
        when = "after";
        parent_what = "deprecated";
        break;
      case Availability::InheritResult::Status::kAfterParentRemoved:
        when = "after";
        parent_what = "removed";
        break;
    }
    child_what = arg->name.value().data();
    const auto* inherited_arg = AncestorArgument(element, parent_what);
    Fail(ErrAvailabilityConflictsWithParent, arg->span, arg, arg->value->span.data(), inherited_arg,
         inherited_arg->value->span.data(), inherited_arg->span, child_what, when, parent_what);
  };

  // Reports an error for the legacy arg given its status.
  auto report_legacy = [&](const AttributeArg* arg,
                           Availability::InheritResult::LegacyStatus status) {
    switch (status) {
      case Availability::InheritResult::LegacyStatus::kOk:
        break;
      case Availability::InheritResult::LegacyStatus::kNeverRemoved:
        Fail(ErrLegacyWithoutRemoval, arg->span, arg);
        break;
      case Availability::InheritResult::LegacyStatus::kWithoutParent: {
        const auto* inherited_arg = AncestorArgument(element, "removed");
        Fail(ErrLegacyConflictsWithParent, arg->span, arg, arg->value->span.data(), inherited_arg,
             inherited_arg->value->span.data(), inherited_arg->span);
        break;
      }
    }
  };

  if (auto source = AvailabilityToInheritFrom(element)) {
    const auto result = element->availability.Inherit(source.value());
    report(added, result.added);
    report(deprecated, result.deprecated);
    report(removed, result.removed);
    report_legacy(legacy, result.legacy);
  }
}

Platform AvailabilityStep::GetDefaultPlatform() {
  auto platform = Platform::Parse(std::string(library()->name.front()));
  ZX_ASSERT_MSG(platform, "library component should be valid platform");
  return platform.value();
}

std::optional<Platform> AvailabilityStep::GetPlatform(const AttributeArg* maybe_arg) {
  if (!(maybe_arg && maybe_arg->value->IsResolved())) {
    return std::nullopt;
  }
  ZX_ASSERT(maybe_arg->value->Value().kind == ConstantValue::Kind::kString);
  std::string str =
      static_cast<const StringConstantValue*>(&maybe_arg->value->Value())->MakeContents();
  auto platform = Platform::Parse(str);
  if (!platform) {
    Fail(ErrInvalidPlatform, maybe_arg->value->span, str);
    return std::nullopt;
  }
  return platform;
}

std::optional<Version> AvailabilityStep::GetVersion(const AttributeArg* maybe_arg) {
  if (!(maybe_arg && maybe_arg->value->IsResolved())) {
    return std::nullopt;
  }
  // Note: We only have to deal with integers here. If the argument was the
  // identifier `HEAD`, it will have been resolved to Version::Head().ordinal()
  // when we call CompileAttributeEarly. See AttributeArgSchema::ResolveArg.
  ZX_ASSERT(maybe_arg->value->Value().kind == ConstantValue::Kind::kUint64);
  uint64_t value =
      static_cast<const NumericConstantValue<uint64_t>*>(&maybe_arg->value->Value())->value;
  auto version = Version::From(value);
  // Do not allow referencing the LEGACY version directly. It may only be
  // specified on the command line, or in FIDL libraries via the `legacy`
  // argument to @available.
  if (!version || version == Version::Legacy()) {
    Fail(ErrInvalidVersion, maybe_arg->value->span, value);
    return std::nullopt;
  }
  return version;
}

std::optional<Availability::Legacy> AvailabilityStep::GetLegacy(const AttributeArg* maybe_arg) {
  if (!(maybe_arg && maybe_arg->value->IsResolved())) {
    return std::nullopt;
  }
  ZX_ASSERT(maybe_arg->value->Value().kind == ConstantValue::Kind::kBool);
  return static_cast<const BoolConstantValue*>(&maybe_arg->value->Value())->value
             ? Availability::Legacy::kYes
             : Availability::Legacy::kNo;
}

std::optional<Availability> AvailabilityStep::AvailabilityToInheritFrom(const Element* element) {
  const Element* parent = LexicalParent(element);
  if (!parent) {
    ZX_ASSERT_MSG(element == library(), "if it has no parent, it must be the library");
    return Availability::Unbounded();
  }
  if (parent->availability.state() == Availability::State::kInherited) {
    // The typical case: inherit from the parent.
    return parent->availability;
  }
  // The parent failed to compile, so don't try to inherit.
  return std::nullopt;
}

const AttributeArg* AvailabilityStep::AncestorArgument(const Element* element,
                                                       std::string_view arg_name) {
  while ((element = LexicalParent(element))) {
    if (auto attribute = element->attributes->Get("available")) {
      if (auto arg = attribute->GetArg(arg_name)) {
        return arg;
      }
    }
  }
  ZX_PANIC("no ancestor exists for this arg");
}

Element* AvailabilityStep::LexicalParent(const Element* element) {
  ZX_ASSERT(element);
  if (element == library()) {
    return nullptr;
  }
  if (auto it = lexical_parents_.find(element); it != lexical_parents_.end()) {
    return it->second;
  }
  // If it's not in lexical_parents_, it must be a top-level declaration.
  return library();
}

namespace {

struct CmpAvailability {
  bool operator()(const Element* lhs, const Element* rhs) const {
    return lhs->availability.set() < rhs->availability.set();
  }
};

}  // namespace

void AvailabilityStep::VerifyNoDeclOverlaps() {
  // Here we check for (canonical) name collisions on availabilities that
  // overlap. We report at most one error per element, even if it overlaps with
  // multiple elements, to allow the same code to work gracefully with libraries
  // that don't use @available (i.e. avoid too many redundant errors).

  std::map<std::string, std::set<const Decl*, CmpAvailability>> by_canonical_name;
  for (auto& [name, decl] : library()->declarations.all) {
    // Skip decls whose availabilities we failed to compile.
    if (decl->availability.state() != Availability::State::kInherited) {
      continue;
    }

    // TODO(fxbug.dev/67858): This is worst-case quadratic in the number of
    // declarations having the same name. It can be optimized to O(n*log(n)).
    auto canonical_name = utils::canonicalize(name);
    auto set = decl->availability.set();
    auto& same_canonical_name = by_canonical_name[canonical_name];
    for (auto other_decl : same_canonical_name) {
      auto other_set = other_decl->availability.set();
      auto overlap = VersionSet::Intersect(set, other_set);
      if (!overlap) {
        continue;
      }
      auto span = decl->name.span().value();
      auto other_name = other_decl->name.decl_name();
      auto other_span = other_decl->name.span().value();
      // Use a simplified error message for unversioned libraries, or for
      // versioned libraries where the version sets match exactly.
      if (set == other_set) {
        if (name == other_name) {
          Fail(ErrNameCollision, span, name, other_span);
        } else {
          Fail(ErrNameCollisionCanonical, span, name, other_name, other_span, canonical_name);
        }
      } else {
        if (name == other_name) {
          Fail(ErrNameOverlap, span, name, other_span, overlap.value(),
               library()->platform.value());
        } else {
          Fail(ErrNameOverlapCanonical, span, name, other_name, other_span, canonical_name,
               overlap.value(), library()->platform.value());
        }
      }
      break;
    }
    same_canonical_name.insert(decl);
  }
}

}  // namespace fidl::flat
