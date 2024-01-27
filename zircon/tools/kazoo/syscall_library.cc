// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/kazoo/syscall_library.h"

#include <stdio.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>

#include <algorithm>
#include <cctype>

#include "tools/kazoo/output_util.h"
#include "tools/kazoo/string_util.h"

namespace {

using MaybeValue = std::optional<std::reference_wrapper<const rapidjson::Value>>;

// TODO(fxbug.dev/81390): Attribute values may only be string literals for now. Make sure to fix
//  this API once that changes to resolve the constant value for all constant types.
MaybeValue GetConstantValueAsString(const rapidjson::Value& constant) {
  if (constant["kind"] == "literal") {
    return constant["value"];
  }
  return std::nullopt;
}

// Check that an attribute exists, and return true if it has no arguments.
bool HasAttributeWithNoArgs(const rapidjson::Value& element, const std::string& attribute_name) {
  if (!element.HasMember("maybe_attributes")) {
    return false;
  }
  for (const auto& attrib : element["maybe_attributes"].GetArray()) {
    if (CamelToSnake(attrib.GetObject()["name"].Get<std::string>()) == attribute_name) {
      const auto& args = attrib.GetObject()["arguments"];
      if (args.GetArray().Empty()) {
        return true;
      }
    }
  }
  return false;
}

// Check that an attribute exists. If the attribute only has one argument, retrieve that argument's
// value.
MaybeValue GetAttributeStandaloneArgValue(const rapidjson::Value& element,
                                          const std::string& attribute_name) {
  if (!element.HasMember("maybe_attributes")) {
    return std::nullopt;
  }
  for (const auto& attrib : element["maybe_attributes"].GetArray()) {
    if (CamelToSnake(attrib.GetObject()["name"].Get<std::string>()) == attribute_name) {
      const auto& args = attrib.GetObject()["arguments"];
      if (args.Size() == 1) {
        return GetConstantValueAsString(args.GetArray()[0].GetObject()["value"]);
      }
    }
  }
  return std::nullopt;
}

bool ValidateTransport(const rapidjson::Value& protocol) {
  const MaybeValue maybe_value = GetAttributeStandaloneArgValue(protocol, "transport");
  return maybe_value.has_value() && maybe_value.value().get().Get<std::string>() == "Syscall";
}

std::string StripLibraryName(const std::string& full_name) {
  auto prefix_pos = full_name.find_first_of('/');
  ZX_ASSERT_MSG(prefix_pos != full_name.npos, "%s has no library prefix", full_name.c_str());
  size_t prefix_len = prefix_pos + 1;
  return full_name.substr(prefix_len, full_name.size() - prefix_len);
}

std::string GetCategory(const rapidjson::Value& protocol, const std::string& protocol_name) {
  if (protocol.HasMember("maybe_attributes")) {
    for (const auto& attrib : protocol["maybe_attributes"].GetArray()) {
      if (CamelToSnake(attrib.GetObject()["name"].Get<std::string>()) == "no_protocol_prefix") {
        return std::string();
      }
    }
  }

  return ToLowerAscii(StripLibraryName(protocol_name));
}

std::string GetDocAttribute(const rapidjson::Value& method) {
  const MaybeValue maybe_value = GetAttributeStandaloneArgValue(method, "doc");
  if (maybe_value.has_value()) {
    return maybe_value.value().get().GetString();
  }
  return std::string();
}

Required GetRequiredAttribute(const rapidjson::Value& field) {
  return HasAttributeWithNoArgs(field, "required") ? Required::kYes : Required::kNo;
}

std::vector<std::string> GetCleanDocAttribute(const std::string& full_doc_attribute) {
  auto lines = SplitString(full_doc_attribute, '\n', kTrimWhitespace);
  if (!lines.empty() && lines[lines.size() - 1].empty()) {
    lines.pop_back();
  }
  std::for_each(lines.begin(), lines.end(),
                [](std::string& line) { return TrimString(line, " \t\n"); });
  return lines;
}

std::optional<Type> PrimitiveTypeFromName(std::string subtype) {
  if (subtype == "int8") {
    return Type(TypeInt8{});
  }
  if (subtype == "uint8") {
    return Type(TypeUint8{});
  }
  if (subtype == "int16") {
    return Type(TypeInt16{});
  }
  if (subtype == "uint16") {
    return Type(TypeUint16{});
  }
  if (subtype == "int32") {
    return Type(TypeInt32{});
  }
  if (subtype == "uint32") {
    return Type(TypeUint32{});
  }
  if (subtype == "int64") {
    return Type(TypeInt64{});
  }
  if (subtype == "uint64") {
    return Type(TypeUint64{});
  }
  if (subtype == "bool") {
    return Type(TypeBool{});
  }
  if (subtype == "uchar") {
    return Type(TypeUchar{});
  }
  if (subtype == "uintptr") {
    return Type(TypeUintptr{});
  }
  if (subtype == "usize") {
    return Type(TypeUsize{});
  }

  return std::nullopt;
}

Type TypeFromJson(const SyscallLibrary& library, const rapidjson::Value& type,
                  const std::map<std::string, std::string> attributes,
                  const rapidjson::Value* alias) {
  if (alias) {
    // If the "experimental_maybe_from_alias" field is non-null, then the source-level has used
    // a type that's declared as "using x = y;". Here, treat various "x"s as special types. This
    // is likely mostly (?) temporary until there's 1) a more nailed down alias implementation in
    // the front end (fidlc) and 2) we move various parts of zx.fidl from being built-in to fidlc to
    // actual source level fidl and shared between the syscall definitions and normal FIDL.
    const std::string full_name((*alias)["name"].GetString());
    if (full_name.substr(0, 3) == "zx/") {
      const std::string name = full_name.substr(3);
      if (name == "duration" || name == "Futex" || name == "koid" || name == "paddr" ||
          name == "rights" || name == "signals" || name == "status" || name == "time" ||
          name == "ticks" || name == "vaddr" || name == "VmOption") {
        return Type(TypeZxBasicAlias(name));
      }
    }

    const std::string name = StripLibraryName(full_name);
    return *library.TypeFromIdentifier(full_name);
  }

  if (!type.HasMember("kind")) {
    fprintf(stderr, "type has no 'kind'\n");
    return Type();
  }

  std::string kind = type["kind"].GetString();
  std::optional<Type> typ;
  if (kind == "primitive") {
    const rapidjson::Value& subtype_value = type["subtype"];
    ZX_ASSERT(subtype_value.IsString());
    std::string subtype = subtype_value.GetString();
    typ = PrimitiveTypeFromName(subtype).value();
  } else if (kind == "identifier") {
    std::string id = type["identifier"].GetString();
    typ = library.TypeFromIdentifier(type["identifier"].GetString());
  } else if (kind == "handle") {
    typ = Type(TypeHandle(type["subtype"].GetString()));
  } else if (kind == "vector") {
    Type contained_type = TypeFromJson(library, type["element_type"], {}, nullptr);
    typ = Type(TypeVector(contained_type));
  } else if (kind == "experimental_pointer") {
    Type pointee_type = TypeFromJson(library, type["pointee_type"], {}, nullptr);
    typ = Type(TypePointer(pointee_type));
  }
  ZX_ASSERT_MSG(typ, "TODO: kind=%s", kind.c_str());

  if (attributes.find("voidptr") != attributes.end()) {
    ZX_ASSERT(typ->IsVector() || typ->IsPointer());
    if (typ->IsVector()) {
      const auto& contained_type = typ->DataAsVector().contained_type();
      ZX_ASSERT(std::holds_alternative<TypeUint8>(contained_type.type_data()));
      typ = Type(TypeVector(Type(TypeVoid{})));
    } else {
      const auto& pointee_type = typ->DataAsPointer().pointed_to_type();
      ZX_ASSERT(std::holds_alternative<TypeUint8>(pointee_type.type_data()));
      typ = Type(TypePointer(Type(TypeVoid{})));
    }
  }

  bool inout = attributes.find("inout") != attributes.end();
  bool out = attributes.find("out") != attributes.end();
  ZX_ASSERT_MSG(!inout || !out, "@inout and @out cannot both be specified");
  if (inout) {
    if (typ->IsVector() || typ->IsPointer()) {
      typ->set_constness(Constness::kMutable);
    } else {
      typ = Type(TypePointer(*typ), Constness::kMutable);
    }
  } else if (out) {
    typ->set_constness(Constness::kMutable);
    typ->set_optionality(Optionality::kOutputOptional);
  }

  if (attributes.find("size32") != attributes.end()) {
    ZX_ASSERT(typ->IsVector());
    typ = Type(TypeVector(typ->DataAsVector().contained_type(), UseUint32ForVectorSizeTag{}),
               typ->constness());
  }

  if (auto it = attributes.find("embedded_alias"); it != attributes.end()) {
    ZX_ASSERT(typ->IsPointer() || typ->IsVector());

    auto aliased = library.TypeFromIdentifier(it->second);
    if (typ->IsPointer()) {
      typ = Type(TypePointer(*aliased), typ->constness());
    } else {
      typ = Type(TypeVector(*aliased), typ->constness());
    }
  }
  return *typ;
}

}  // namespace

TypeZxBasicAlias::TypeZxBasicAlias(const std::string& name)
    : name_(std::string(1, static_cast<char>(toupper(name[0]))) + name.substr(1)),
      c_name_("zx_" + CamelToSnake(name) + "_t") {}

bool Type::IsSimpleType() const {
  if (IsStruct()) {
    return DataAsStruct().struct_data().id() == "zx/StringView";
  }
  return !IsVector();
}

bool Syscall::HasAttribute(const char* attrib_name) const {
  return attributes_.find(CamelToSnake(attrib_name)) != attributes_.end();
}

std::string Syscall::GetAttribute(const char* attrib_name) const {
  ZX_ASSERT(HasAttribute(attrib_name));
  return attributes_.find(CamelToSnake(attrib_name))->second;
}

// Converts from FIDL style to C/Kernel style:
// - string to pointer+size
// - vector to pointer+size
// - structs become pointer-to-struct (const on input, mutable on output)
// - etc.
void Syscall::MapRequestResponseToKernelAbi(SyscallLibrary* library) {
  ZX_ASSERT(kernel_arguments_.empty());

  // Used for input arguments, which default to const unless alread specified mutable.
  auto default_to_const = [](Constness constness) {
    if (constness == Constness::kUnspecified) {
      return Constness::kConst;
    }
    return constness;
  };

  auto output_optionality = [](Optionality optionality) {
    // If explicitly made optional then leave it alone, otherwise mark non-optional.
    if (optionality == Optionality::kOutputOptional) {
      return optionality;
    }
    return Optionality::kOutputNonOptional;
  };

  auto get_vector_size_name = [](const StructMember& member) {
    std::string prefix, suffix;
    // If it's a char* or void*, blah_size seems more natural, otherwise, num_blahs is moreso.
    if ((member.type().DataAsVector().contained_type().IsChar() ||
         member.type().DataAsVector().contained_type().IsVoid()) &&
        (member.name() != "bytes")) {
      suffix = "_size";
    } else {
      prefix = "num_";
    }
    return std::tuple<std::string, bool>(prefix + member.name() + suffix,
                                         member.type().DataAsVector().uint32_size());
  };

  // Map inputs first, converting vectors, strings, and structs to their corresponding input types
  // as we go.
  for (const auto& m : request_.members()) {
    Type type = m.type();
    Optionality opt = type.optionality();
    if (opt == Optionality::kUnspecified) {
      opt = Optionality::kInputArgument;
    }

    if (type.IsVector()) {
      Type pointer_to_subtype(
          TypePointer(type.DataAsVector().contained_type(), IsDecayedVectorTag{}),
          default_to_const(type.constness()), opt);
      kernel_arguments_.emplace_back(m.name(), pointer_to_subtype, m.attributes());
      auto [size_name, is_u32] = get_vector_size_name(m);
      kernel_arguments_.emplace_back(size_name, is_u32 ? Type(TypeUint32{}) : Type(TypeUsize{}),
                                     std::map<std::string, std::string>{});
    } else if (type.IsStruct()) {
      // If it's a struct, map to struct*, const unless otherwise specified. The pointer takes the
      // constness of the struct.
      kernel_arguments_.emplace_back(
          m.name(), Type(TypePointer(type), default_to_const(type.constness()), opt),
          m.attributes());
    } else {
      // This covers the case of a 'disjointed' buffer of handles, which was
      // certainly intended to be a "vector".
      if (type.IsPointer()) {
        const Type& pointed_to = type.DataAsPointer().pointed_to_type();
        if (std::string name = GetCKernelModeName(pointed_to);
            name == "zx_handle_t" || name == "zx_handle_info_t") {
          type = Type(TypePointer(pointed_to, IsDecayedVectorTag{}), type.constness());
        }
      }
      // Otherwise, copy it over, unchanged other than to tag it as input.
      kernel_arguments_.emplace_back(
          m.name(), Type(type.type_data(), default_to_const(m.type().constness()), opt),
          m.attributes());
    }
  }

  // Similarly for the outputs, but turning buffers into outparams, and with special handling for
  // the C return value.
  size_t start_at;
  if (error_type_) {
    kernel_return_type_ = *error_type_;
    start_at = 0;
  } else if (auto resp_type = library->TypeFromIdentifier(response_.id());
             resp_type && resp_type->IsSimpleType()) {
    kernel_return_type_ = *resp_type;
    start_at = response_.members().size();
  } else if (response_.members().size() == 0 || !response_.members()[0].type().IsSimpleType()) {
    kernel_return_type_ = Type(TypeVoid{});
    start_at = 0;
  } else {
    Type resp_type = Type(TypeStruct{&response_});
    const auto& first_type = response_.members()[0].type();
    if (first_type.IsZxBasicAlias() && first_type.DataAsZxBasicAlias().name() == "Status") {
      ZX_PANIC("%s.%s: `status` should be specified as `error status`", id().c_str(),
               name().c_str());
    }
    kernel_return_type_ = first_type;
    start_at = 1;
  }
  for (size_t i = start_at; i < response_.members().size(); ++i) {
    const StructMember& m = response_.members()[i];
    const Type& type = m.type();
    if (type.IsVector()) {
      // TODO(syscall-fidl-transition): These vector types aren't marked as non-optional in
      // abigen, but generally they probably are.
      Type pointer_to_subtype(
          TypePointer(type.DataAsVector().contained_type(), IsDecayedVectorTag{}),
          Constness::kMutable, Optionality::kOutputOptional);
      kernel_arguments_.emplace_back(m.name(), pointer_to_subtype, m.attributes());
      auto [size_name, is_u32] = get_vector_size_name(m);
      kernel_arguments_.emplace_back(size_name, is_u32 ? Type(TypeUint32{}) : Type(TypeUsize{}),
                                     std::map<std::string, std::string>{});
    } else if (type.IsPointer()) {
      kernel_arguments_.emplace_back(
          m.name(), Type(type.type_data(), Constness::kMutable, Optionality::kOutputOptional),
          m.attributes());
    } else {
      // Everything else becomes a T* (to make it an out parameter).
      kernel_arguments_.emplace_back(
          m.name(),
          Type(TypePointer(type), Constness::kMutable, output_optionality(type.optionality())),
          m.attributes());
    }
  }
}

void Enum::AddMember(const std::string& member_name, EnumMember member) {
  ZX_ASSERT(!HasMember(member_name));
  members_[member_name] = std::move(member);
  insertion_order_.push_back(member_name);
}

bool Enum::HasMember(const std::string& member_name) const {
  return members_.find(member_name) != members_.end();
}

const EnumMember& Enum::ValueForMember(const std::string& member_name) const {
  ZX_ASSERT(HasMember(member_name));
  return members_.find(member_name)->second;
}

std::optional<Type> SyscallLibrary::TypeFromIdentifier(const std::string& id) const {
  for (const auto& bits : bits_) {
    if (bits->id() == id) {
      // TODO(scottmg): Consider if we need to separate bits from enum here.
      return Type(TypeEnum{bits.get()});
    }
  }

  for (const auto& enm : enums_) {
    if (enm->id() == id) {
      return Type(TypeEnum{enm.get()});
    }
  }

  for (const auto& alias : aliases_) {
    if (alias->id() == id) {
      return Type(TypeAlias(alias.get()));
    }
  }

  for (const auto& strukt : structs_) {
    if (strukt->id() == id) {
      return Type(TypeStruct(strukt.get()));
    }
  }

  return {};
}

std::optional<Type> SyscallLibrary::TypeFromName(const std::string& name) const {
  if (auto primitive = PrimitiveTypeFromName(name); primitive.has_value()) {
    return primitive.value();
  }
  return TypeFromIdentifier(name);
}

void SyscallLibrary::FilterSyscalls(const std::set<std::string>& attributes_to_exclude) {
  std::vector<std::unique_ptr<Syscall>> filtered;
  for (auto& syscall : syscalls_) {
    if (std::any_of(
            attributes_to_exclude.begin(), attributes_to_exclude.end(),
            [&syscall](const std::string& x) { return syscall->HasAttribute(x.c_str()); })) {
      continue;
    }

    filtered.push_back(std::move(syscall));
  }

  std::sort(filtered.begin(), filtered.end(),
            [](const std::unique_ptr<Syscall>& a, const std::unique_ptr<Syscall>& b) -> bool {
              // TODO(fxbug.dev/110295): The default lexicographic order
              // between digits and letters is different in C++ than in Go.
              // Compare the Go way here for easier diffs when comparing
              // kazoo's outputs with zither's.
              auto comp = [](char c1, char c2) -> bool {
                bool isdigit1 = std::isdigit(c1);
                bool isdigit2 = std::isdigit(c2);
                if (isdigit1 != isdigit2) {
                  return !isdigit1;
                }
                return c1 < c2;
              };
              const std::string& name_a = a->snake_name();
              const std::string& name_b = b->snake_name();
              return std::lexicographical_compare(name_a.begin(), name_a.end(), name_b.begin(),
                                                  name_b.end(), comp);
            });
  syscalls_ = std::move(filtered);
}

// static
bool SyscallLibraryLoader::FromJson(const std::string& json_ir, SyscallLibrary* library) {
  rapidjson::Document document;
  document.Parse(json_ir);

  // Maybe do schema validation here, though we rely on fidlc for many details
  // and general sanity, so probably only in a diagnostic mode.

  if (!document.IsObject()) {
    fprintf(stderr, "Incorrect fidlc JSON IR, wasn't json object.\n");
    return false;
  }

  library->name_ = document["name"].GetString();
  if (library->name_ != "zx" && library->name_ != "zxio") {
    fprintf(stderr, "Library name %s wasn't zx or zxio as expected.\n", library->name_.c_str());
    return false;
  }

  ZX_ASSERT(library->syscalls_.empty());

  // The order of these loads is significant. For example, enums must be loaded to be able to be
  // referred to by protocol methods.

  if (!LoadBits(document, library)) {
    return false;
  }

  if (!LoadEnums(document, library)) {
    return false;
  }

  if (!LoadAliases(document, library)) {
    return false;
  }

  if (!LoadStructs(document, library)) {
    return false;
  }

  if (!LoadTables(document, library)) {
    return false;
  }

  if (!LoadProtocols(document, library)) {
    return false;
  }

  return true;
}

// 'bits' are currently handled the same as enums, so just use Enum for now as the underlying
// data storage.
//
// static
std::unique_ptr<Enum> SyscallLibraryLoader::ConvertBitsOrEnumMember(const rapidjson::Value& json) {
  auto obj = std::make_unique<Enum>();
  std::string full_name = json["name"].GetString();
  obj->id_ = full_name;
  std::string stripped = StripLibraryName(full_name);
  obj->original_name_ = stripped;
  obj->base_name_ = CamelToSnake(stripped);
  std::string doc_attribute = GetDocAttribute(json);
  obj->description_ = GetCleanDocAttribute(doc_attribute);
  const rapidjson::Value& type = json["type"];
  if (type.IsString()) {
    // Enum
    obj->underlying_type_ = PrimitiveTypeFromName(type.GetString()).value();
  } else {
    ZX_ASSERT(type.IsObject());
    // Bits
    ZX_ASSERT_MSG(type["kind"].GetString() == std::string("primitive"),
                  "Enum %s not backed by primitive type", full_name.c_str());
    const rapidjson::Value& subtype_value = type["subtype"];
    ZX_ASSERT(subtype_value.IsString());
    std::string subtype = subtype_value.GetString();
    obj->underlying_type_ = PrimitiveTypeFromName(subtype).value();
  }
  for (const auto& member : json["members"].GetArray()) {
    ZX_ASSERT_MSG(member["value"]["kind"] == "literal", "TODO: More complex value expressions");
    uint64_t member_value;
    std::string decimal = member["value"]["literal"]["value"].GetString();
    if (obj->underlying_type().IsUnsignedInt()) {
      member_value = StringToUInt(decimal);
    } else if (obj->underlying_type().IsSignedInt()) {
      member_value = StringToInt(decimal);
    } else {
      ZX_PANIC("Unreachable");
    }
    std::string doc_attribute = GetDocAttribute(member);
    obj->AddMember(
        member["name"].GetString(),
        EnumMember{.value = member_value, .description = GetCleanDocAttribute(doc_attribute)});
  }
  return obj;
}

// static
bool SyscallLibraryLoader::ExtractPayload(Struct& payload, const std::string& type_name,
                                          const rapidjson::Document& document,
                                          SyscallLibrary* library) {
  auto FindStructDecl = [&](const rapidjson::Value& struct_list) -> bool {
    for (const auto& struct_json : struct_list.GetArray()) {
      std::string struct_name = struct_json["name"].GetString();
      if (struct_name == type_name) {
        for (const auto& arg : struct_json["members"].GetArray()) {
          Struct* strukt = &payload;
          const auto* alias = arg.HasMember("experimental_maybe_from_alias")
                                  ? &arg["experimental_maybe_from_alias"]
                                  : nullptr;
          std::map<std::string, std::string> attributes;
          if (arg.HasMember("maybe_attributes")) {
            for (const auto& attrib : arg["maybe_attributes"].GetArray()) {
              const auto attrib_name = attrib["name"].GetString();
              const MaybeValue maybe_value = GetAttributeStandaloneArgValue(arg, attrib_name);
              attributes[CamelToSnake(attrib_name)] =
                  maybe_value.has_value() && maybe_value.value().get().IsString()
                      ? maybe_value.value().get().GetString()
                      : "";
            }
          }
          Type typ = TypeFromJson(*library, arg["type"], attributes, alias);
          strukt->members_.emplace_back(arg["name"].GetString(), std::move(typ),
                                        std::move(attributes));
        }
        payload.id_ = type_name;
        return true;
      }
    }

    return false;
  };

  return FindStructDecl(document["struct_declarations"]) ||
         FindStructDecl(document["external_struct_declarations"]);
}

// static
bool SyscallLibraryLoader::LoadBits(const rapidjson::Document& document, SyscallLibrary* library) {
  for (const auto& bits_json : document["bits_declarations"].GetArray()) {
    library->bits_.push_back(ConvertBitsOrEnumMember(bits_json));
  }
  return true;
}

// static
bool SyscallLibraryLoader::LoadEnums(const rapidjson::Document& document, SyscallLibrary* library) {
  for (const auto& enum_json : document["enum_declarations"].GetArray()) {
    library->enums_.push_back(ConvertBitsOrEnumMember(enum_json));
  }
  return true;
}

// static
bool SyscallLibraryLoader::LoadProtocols(const rapidjson::Document& document,
                                         SyscallLibrary* library) {
  for (const auto& protocol : document["protocol_declarations"].GetArray()) {
    if (!ValidateTransport(protocol)) {
      fprintf(stderr, "Expected Transport to be Syscall.\n");
      return false;
    }

    std::string protocol_name = protocol["name"].GetString();
    std::string category = GetCategory(protocol, protocol_name);
    bool protocol_prefix = !HasAttributeWithNoArgs(protocol, "no_protocol_prefix");

    for (const auto& method : protocol["methods"].GetArray()) {
      auto syscall = std::make_unique<Syscall>();
      syscall->id_ = protocol_name;
      syscall->category_ = category;
      syscall->name_ = method["name"].GetString();
      if (protocol_prefix) {
        syscall->snake_name_ = CamelToSnake(syscall->category_ + syscall->name_);
      } else {
        syscall->snake_name_ = CamelToSnake(syscall->name_);
      }
      syscall->is_noreturn_ = !method["has_response"].GetBool();
      const auto doc_attribute = GetDocAttribute(method);
      if (method.HasMember("maybe_attributes")) {
        for (const auto& attrib : method["maybe_attributes"].GetArray()) {
          const auto attrib_name = attrib["name"].GetString();
          const MaybeValue maybe_value = GetAttributeStandaloneArgValue(method, attrib_name);
          syscall->attributes_[CamelToSnake(attrib_name)] =
              maybe_value.has_value() && maybe_value.value().get().IsString()
                  ? maybe_value.value().get().GetString()
                  : "";
        }
      }

      ZX_ASSERT(method["has_request"].GetBool());  // Events are not expected in syscalls.

      Struct& req = syscall->request_;
      if (method.HasMember("maybe_request_payload")) {
        if (!ExtractPayload(req, method["maybe_request_payload"]["identifier"].GetString(),
                            document, library)) {
          return false;
        }
      }

      if (method["has_response"].GetBool()) {
        Struct& resp = syscall->response_;
        if (method.HasMember("maybe_response_success_type")) {
          if (!ExtractPayload(resp, method["maybe_response_success_type"]["identifier"].GetString(),
                              document, library)) {
            return false;
          }

          // If a "success" type is given, then so too has an error type been.
          ZX_ASSERT(method.HasMember("maybe_response_err_type"));

          // TODO(fxbug.dev/105758): Ideally, we would just naively translate
          // `method["maybe_response_err_type"]` into `syscall_->error_type`,
          // but that unfortunately does not work in the case of `zx.status`.
          // It does not work since `zx.status` is currently defined as a type
          // alias, but type aliases are currently lossy and all that would
          // survive into the IR is "uint32". Since this is our only real error
          // type use-case, we just hardcode its use when a method specifies
          // `error`.
          syscall->error_type_ = Type(TypeZxBasicAlias("status"));
        } else if (method.HasMember("maybe_response_payload")) {
          if (!ExtractPayload(resp, method["maybe_response_payload"]["identifier"].GetString(),
                              document, library)) {
            return false;
          }
        }
      }

      syscall->MapRequestResponseToKernelAbi(library);
      library->syscalls_.push_back(std::move(syscall));
    }
  }

  return true;
}

// static
bool SyscallLibraryLoader::LoadAliases(const rapidjson::Document& document,
                                       SyscallLibrary* library) {
  for (const auto& alias_json : document["alias_declarations"].GetArray()) {
    auto obj = std::make_unique<Alias>();
    std::string full_name = alias_json["name"].GetString();
    obj->id_ = full_name;
    std::string stripped = StripLibraryName(full_name);
    obj->original_name_ = stripped;
    obj->base_name_ = CamelToSnake(stripped);
    const rapidjson::Value& partial_type_ctor = alias_json["partial_type_ctor"];
    ZX_ASSERT(partial_type_ctor.IsObject());
    obj->partial_type_ctor_ = partial_type_ctor["name"].GetString();
    std::string doc_attribute = GetDocAttribute(alias_json);
    obj->description_ = GetCleanDocAttribute(doc_attribute);
    library->aliases_.push_back(std::move(obj));
  }
  return true;
}

// static
bool SyscallLibraryLoader::LoadStructs(const rapidjson::Document& document,
                                       SyscallLibrary* library) {
  // TODO(scottmg): In transition, we're still relying on the existing Zircon headers to define all
  // these structures. So we only load their names for the time being, which is enough for now to
  // know that there's something in the .fidl file where the struct is declared. Note also that
  // protocol parsing fills out request/response "structs", so that code should likely be shared
  // when this is implemented.
  for (const auto& struct_json : document["struct_declarations"].GetArray()) {
    auto obj = std::make_unique<Struct>();
    std::string full_name = struct_json["name"].GetString();
    obj->id_ = full_name;
    std::string stripped = StripLibraryName(full_name);
    obj->original_name_ = stripped;
    obj->base_name_ = CamelToSnake(stripped);
    library->structs_.push_back(std::move(obj));
  }
  return true;
}

// static
bool SyscallLibraryLoader::LoadTables(const rapidjson::Document& document,
                                      SyscallLibrary* library) {
  for (const auto& json : document["table_declarations"].GetArray()) {
    auto obj = std::make_unique<Table>();
    std::string full_name = json["name"].GetString();
    obj->id_ = full_name;
    std::string stripped = StripLibraryName(full_name);
    obj->original_name_ = stripped;
    obj->base_name_ = CamelToSnake(stripped);
    std::string doc_attribute = GetDocAttribute(json);
    obj->description_ = GetCleanDocAttribute(doc_attribute);
    std::vector<TableMember> members;
    for (const auto& member : json["members"].GetArray()) {
      std::string name = member["name"].GetString();
      const auto* alias = member.HasMember("experimental_maybe_from_alias")
                              ? &member["experimental_maybe_from_alias"]
                              : nullptr;
      Type type = TypeFromJson(*library, member["type"], {}, alias);
      Required required = GetRequiredAttribute(member);
      std::string doc_attribute = GetDocAttribute(member);
      std::vector<std::string> description = GetCleanDocAttribute(doc_attribute);
      members.emplace_back(std::move(name), std::move(type), std::move(description), required);
    }
    obj->members_ = std::move(members);
    library->tables_.push_back(std::move(obj));
  }
  return true;
}
