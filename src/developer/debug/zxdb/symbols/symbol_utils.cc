// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/symbol_utils.h"

#include <lib/syslog/cpp/macros.h>

#include "src/developer/debug/zxdb/symbols/array_type.h"
#include "src/developer/debug/zxdb/symbols/base_type.h"
#include "src/developer/debug/zxdb/symbols/collection.h"
#include "src/developer/debug/zxdb/symbols/compile_unit.h"
#include "src/developer/debug/zxdb/symbols/data_member.h"
#include "src/developer/debug/zxdb/symbols/dwarf_tag.h"
#include "src/developer/debug/zxdb/symbols/modified_type.h"
#include "src/developer/debug/zxdb/symbols/namespace.h"
#include "src/developer/debug/zxdb/symbols/type.h"

namespace zxdb {

Identifier GetSymbolScopePrefix(const Symbol* symbol) {
  if (!symbol->parent().is_valid())
    return Identifier(IdentifierQualification::kGlobal);  // No prefix

  fxl::RefPtr<Symbol> parent = symbol->parent().Get();
  if (parent->tag() == DwarfTag::kCompileUnit)
    return Identifier(IdentifierQualification::kGlobal);  // Don't go above compilation units.

  if (parent->As<Namespace>() || parent->As<Collection>() || parent->As<Function>()) {
    // These are the types that get qualified.
    return parent->GetIdentifier();
  }
  // Anything else is skipped and we just return the parent's prefix. This
  // will include things like lexical blocks.
  return GetSymbolScopePrefix(parent.get());
}

fxl::RefPtr<Collection> MakeRustTuple(const std::string& name,
                                      const std::vector<fxl::RefPtr<Type>>& members) {
  auto coll = fxl::MakeRefCounted<Collection>(DwarfTag::kStructureType, name);
  auto unit = fxl::MakeRefCounted<CompileUnit>(fxl::WeakPtr<ModuleSymbols>(), nullptr,
                                               DwarfLang::kRust, "<no file>", std::nullopt);
  coll->set_parent(UncachedLazySymbol::MakeUnsafe(unit));

  uint32_t offset = 0;
  std::vector<LazySymbol> data_members;
  for (size_t i = 0; i < members.size(); i++) {
    auto& type = members[i];
    auto data = fxl::MakeRefCounted<DataMember>("__" + std::to_string(i), type, offset);

    data_members.emplace_back(std::move(data));
    offset += type->byte_size();
  }

  coll->set_byte_size(offset);
  coll->set_data_members(std::move(data_members));
  return coll;
}

fxl::RefPtr<Type> MakeStringLiteralType(size_t length) {
  auto char_type = fxl::MakeRefCounted<BaseType>(BaseType::kBaseTypeSignedChar, 1, "char");
  return fxl::MakeRefCounted<ArrayType>(std::move(char_type), length);
}

fxl::RefPtr<Type> AddCVQualifiersToMatch(const Type* reference, fxl::RefPtr<Type> modified) {
  const Type* source = reference;
  while (source) {
    const ModifiedType* mod_source = source->As<ModifiedType>();
    if (!mod_source || !DwarfTagIsCVQualifier(mod_source->tag()))
      break;

    modified = fxl::MakeRefCounted<ModifiedType>(mod_source->tag(), std::move(modified));
    source = mod_source->modified().Get()->As<Type>();
  }

  return modified;
}

}  // namespace zxdb
