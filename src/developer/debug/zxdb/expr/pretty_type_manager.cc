// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/expr/pretty_type_manager.h"

#include <lib/syslog/cpp/macros.h>

#include <limits>

#include "src/developer/debug/zxdb/common/string_util.h"
#include "src/developer/debug/zxdb/expr/format_node.h"
#include "src/developer/debug/zxdb/expr/pretty_rust_tuple.h"
#include "src/developer/debug/zxdb/expr/pretty_std_string.h"
#include "src/developer/debug/zxdb/expr/pretty_type.h"
#include "src/developer/debug/zxdb/symbols/collection.h"

namespace zxdb {

namespace {

using GetterList = std::initializer_list<std::pair<std::string, std::string>>;

// Used for internal hardcoded type globs, this parses the given identifier string and asserts if it
// can't be parsed. Since the built-in globs should always be parseable, this helps clean up the
// syntax.
IdentifierGlob InternalGlob(const char* glob) {
  IdentifierGlob result;
  Err err = result.Init(glob);
  FX_CHECK(!err.has_error()) << "Internal pretty-printer parse failure for\" " << glob
                             << "\": " << err.msg();
  return result;
}

}  // namespace

PrettyTypeManager::PrettyTypeManager() {
  AddDefaultCppPrettyTypes();
  AddDefaultRustPrettyTypes();
  AddDefaultFuchsiaCppPrettyTypes();
}

PrettyTypeManager::~PrettyTypeManager() = default;

void PrettyTypeManager::Add(ExprLanguage lang, IdentifierGlob glob,
                            std::unique_ptr<PrettyType> pretty) {
  switch (lang) {
    case ExprLanguage::kC:
      cpp_.emplace_back(std::move(glob), std::move(pretty));
      break;
    case ExprLanguage::kRust:
      rust_.emplace_back(std::move(glob), std::move(pretty));
      break;
  }
}

PrettyType* PrettyTypeManager::GetForType(const Type* in_type) const {
  if (!in_type)
    return nullptr;

  // Strip const-volatile qualifiers for the name comparison, but don't follow typedefs or make
  // the type concrete. Typedefs will change the name and some pretty-printers are defined for
  // typedefs of other values. We need to maintain the original name for this comparison.
  const Type* type = in_type->StripCV();
  ParsedIdentifier type_ident = ToParsedIdentifier(type->GetIdentifier());

  // Pick the language-specific lookup.
  const auto* lookup = type->GetLanguage() == DwarfLang::kRust ? &rust_ : &cpp_;

  // Tracks the best one found so far. Lower scores are better.
  int best_score = std::numeric_limits<int>::max();
  PrettyType* best_type = nullptr;

  for (const auto& [glob, pretty_ptr] : *lookup) {
    if (auto match = glob.Matches(type_ident)) {
      if (*match < best_score) {
        // Got a new best match.
        best_score = *match;
        best_type = pretty_ptr.get();
      }
    }
  }

  if (best_type) {
    return best_type;
  }

  if (type->GetLanguage() == DwarfLang::kRust) {
    const Collection* coll = type->As<Collection>();
    if (!coll) {
      return nullptr;
    }

    auto special = coll->GetSpecialType();
    if (special == Collection::kRustTuple || special == Collection::kRustTupleStruct) {
      return rust_tuple_type_.get();
    }
  }

  return nullptr;
}

bool PrettyTypeManager::Format(FormatNode* node, const Type* type, const FormatOptions& options,
                               const fxl::RefPtr<EvalContext>& context,
                               fit::deferred_callback& cb) const {
  if (PrettyType* pretty = GetForType(type)) {
    pretty->Format(node, options, context, std::move(cb));
    return true;
  }
  return false;
}

void PrettyTypeManager::AddDefaultCppPrettyTypes() {
  // std::string
  //
  // Because of the weirdness of std::string's definition, we need to check for both the typedef
  // source and the resolved value. The typedef won't always map to something.
  //
  // Furthermore, different versions of the compiler or library have included the "__2" on the
  // template type names or not, so we also encode both variants there.
  cpp_.emplace_back(
      InternalGlob(
          "std::__2::basic_string<char, std::__2::char_traits<char>, std::__2::allocator<char> >"),
      std::make_unique<PrettyStdString>());
  cpp_.emplace_back(
      InternalGlob("std::__2::basic_string<char, std::char_traits<char>, std::allocator<char> >"),
      std::make_unique<PrettyStdString>());
  cpp_.emplace_back(InternalGlob("std::__2::string"), std::make_unique<PrettyStdString>());

  // std::string_view. Like std::string, we encode variants for both "__2" and not for the nested
  // template type names.
  PrettyHeapString pretty_string_view("__data", "__size",
                                      GetterList{{"back", "__data[__size - 1]"},
                                                 {"data", "__data"},
                                                 {"front", "*__data"},
                                                 {"size", "__size"},
                                                 {"length", "__size"},
                                                 {"empty", "__size == 0"}});
  cpp_.emplace_back(InternalGlob("std::__2::basic_string_view<char, std::char_traits<char> >"),
                    std::make_unique<PrettyHeapString>(pretty_string_view));
  cpp_.emplace_back(InternalGlob("std::__2::basic_string_view<char, std::__2::char_traits<char> >"),
                    std::make_unique<PrettyHeapString>(pretty_string_view));

  // std::vector
  //
  // Note that we don't have vector<bool> yet but need to add a pretty-printer for it to
  // preferentially match over the non-bool version (the longest match will be taken). This will
  // result in errors but it will be better than misleading results.
  cpp_.emplace_back(
      InternalGlob("std::__2::vector<*>"),
      std::make_unique<PrettyArray>("__begin_", "__end_ - __begin_",
                                    GetterList{{"size", "__end_ - __begin_"},
                                               {"capacity", "__end_cap_.__value_ - __begin_"},
                                               {"data", "__begin_"},
                                               {"empty", "__end_ == __begin_"},
                                               {"front", "*__begin_"},
                                               {"back", "__begin_[__end_ - __begin_ - 1]"}}));
  cpp_.emplace_back(InternalGlob("std::__2::vector<bool, *>"),
                    std::make_unique<PrettyArray>("vector_bool_printer_not_implemented_yet",
                                                  "vector_bool_printer_not_implemented_yet"));

  // Smart pointers.
  cpp_.emplace_back(InternalGlob("std::__2::unique_ptr<*>"),
                    std::make_unique<PrettyPointer>("__ptr_.__value_"));
  cpp_.emplace_back(InternalGlob("std::__2::shared_ptr<*>"),
                    std::make_unique<PrettyPointer>("__ptr_"));
  cpp_.emplace_back(InternalGlob("std::__2::weak_ptr<*>"),
                    std::make_unique<PrettyPointer>("__ptr_"));

  cpp_.emplace_back(InternalGlob("std::__2::optional<*>"),
                    std::make_unique<PrettyOptional>(
                        "std::optional", "__engaged_", "__val_", "std::nullopt",
                        GetterList{{"value", "__val_"}, {"has_value", "__engaged_"}}));

  cpp_.emplace_back(
      InternalGlob("std::__2::variant<*>"),
      std::make_unique<PrettyRecursiveVariant>(
          "std::variant", "__impl.__data", "__impl.__index", "__tail", "__head.__value",
          "std::variant::valueless_by_exception()", GetterList({{"index", "__impl.__index"}})));

  // std::atomic
  cpp_.emplace_back(
      InternalGlob("std::__2::atomic<*>"),
      std::make_unique<PrettyWrappedValue>("std::atomic", "(", ")", "__a_.__a_value"));

  // std::mutex. std::mutex has a member __m_ which is a __libcpp_mutex_t a.k.a. pthread_mutex_t.
  // Our pthread implementation stores the owning thread handle (not koid) in the "_m_lock" member.
  // Valid handles always have the low bit set. This is cleared to mark the contested state so we
  // need to set it back to get the valid handle. This is delicate but the information is extremely
  // useful for certain kinds of debugging.
  cpp_.emplace_back(InternalGlob("std::__2::mutex"),
                    std::make_unique<PrettyStruct>(GetterList{
                        {"owning_thread_handle", "__m_._m_lock ? (__m_._m_lock | 1) : 0"}}));

  // These locking primitives cause a lot of useless variable spew so just hide the internals. We
  // can probably provide some more useful information with some research about their workings.
  cpp_.emplace_back(InternalGlob("std::__2::condition_variable"),
                    std::make_unique<PrettyStruct>(GetterList{}));
  cpp_.emplace_back(InternalGlob("std::__2::shared_mutex"),
                    std::make_unique<PrettyStruct>(GetterList{}));

  // Streams. Show istringstreams as their current input location, ostringstreams and stringstreams
  // as their full string contents. All other streams get elided as their contents is very long and
  // not very interesting.
  cpp_.emplace_back(InternalGlob("std::__2::basic_ostringstream<*>"),
                    std::make_unique<PrettyStruct>(GetterList{{"str", "__sb_.__str_"}}));
  cpp_.emplace_back(InternalGlob("std::__2::basic_stringstream<*>"),
                    std::make_unique<PrettyStruct>(GetterList{{"str", "__sb_.__str_"}}));
  cpp_.emplace_back(InternalGlob("std::__2::basic_istringstream<*>"),
                    std::make_unique<PrettyStruct>(GetterList{{"next", "__sb_.__ninp_"}}));
  cpp_.emplace_back(InternalGlob("std::__2::basic_stringbuf<*>"),
                    std::make_unique<PrettyStruct>(GetterList{{"buf", "__str_"}}));
  cpp_.emplace_back(InternalGlob("std::__2::basic_ostream<*>"),
                    std::make_unique<PrettyStruct>(GetterList{}));
  cpp_.emplace_back(InternalGlob("std::__2::basic_istream<*>"),
                    std::make_unique<PrettyStruct>(GetterList{}));
  cpp_.emplace_back(InternalGlob("std::__2::basic_istream<*>"),
                    std::make_unique<PrettyStruct>(GetterList{}));
  cpp_.emplace_back(InternalGlob("std::__2::basic_streambuf<*>"),
                    std::make_unique<PrettyStruct>(GetterList{}));

  // Set and map.
  //
  // This is a tree iterator. Map and set are the same except for the line that actually emits the
  // pretty-printing row. So these two strings make one program with the line between them being
  // substituted below.
  std::string tree_before_emit = R"(
    auto size = __tree_.__pair3_.__value_;
    // Uncomment to emit size record (unsure if this is confusing).
    // $zxdb::AppendNameValueRow("[size]", size);

    // We need the pointer to the current node for comparison, but keeping the dereferenced copy
    // makes execution faster because it brings down the memory from the target only once.
    auto cur_node_ptr = static_cast<__base::__node_pointer>(__tree_.__begin_node_);
    auto cur_node = *cur_node_ptr;
    auto nodes_emitted = 0;
    while (nodes_emitted < size) {
      if (nodes_emitted > $zxdb::GetMaxArraySize()) {
        $zxdb::AppendNameRow("...");
        break;
      }
  )";
  // (Emit line goes here.)
  std::string tree_after_emit = R"(
      nodes_emitted = nodes_emitted + 1;

      if (cur_node.__right_) {
        // Move to the right child of this node and go to the leftmost child inside of it.
        cur_node_ptr = static_cast<__base::__node_pointer>(cur_node.__right_);
        cur_node = *cur_node_ptr;
        while (cur_node.__left_) {
          cur_node_ptr = static_cast<__base::__node_pointer>(cur_node.__left_);
          cur_node = *cur_node_ptr;
        }
      } else {
        // Go up until we're no longer a left child.
        for (;;) {
          auto parent_ptr = static_cast<__base::__node_pointer>(cur_node.__parent_);
          auto parent = *parent_ptr;
          if (parent.__left_ == cur_node_ptr)
            break;

          cur_node_ptr = parent_ptr;
          cur_node = parent;
        }
        cur_node_ptr = static_cast<__base::__node_pointer>(cur_node.__parent_);
        cur_node = *cur_node_ptr;
      }
    }
  )";
  GetterList tree_getter_list{{"size", "__tree_.__pair3_.__value_"},
                              {"empty", "__tree_.__pair3_.__value_ == 0"}};
  cpp_.emplace_back(InternalGlob("std::__2::set<*>"),
                    std::make_unique<PrettyGenericContainer>(
                        tree_before_emit + "$zxdb::AppendNameValueRow(\"\", cur_node.__value_);\n" +
                            tree_after_emit,
                        tree_getter_list));
  // This formats map entries as "<index> = {key, value}"
  // This is done instead of "<key> = <value>" because the key can be a complex structure. The
  // FormatNodes can't accommodate non-string values here and text formatting complex structures is
  // done at a higher level (the "console" library) than this code. It also creates a formatting
  // problem as it's not clear how to present a complex nested struct as a "key".
  cpp_.emplace_back(
      InternalGlob("std::__2::map<*>"),
      std::make_unique<PrettyGenericContainer>(
          tree_before_emit + "$zxdb::AppendNameValueRow(\"\", cur_node.__value_.__cc_);\n" +
              tree_after_emit,
          tree_getter_list));

  // Set (tree) and map iterators.
  std::string tree_iterator_dereference = "static_cast<__node_pointer>(__ptr_)->__value_";
  cpp_.emplace_back(InternalGlob("std::__2::__tree_iterator<*>"),
                    std::make_unique<PrettyIterator>(tree_iterator_dereference));
  cpp_.emplace_back(InternalGlob("std::__2::__tree_const_iterator<*>"),
                    std::make_unique<PrettyIterator>(tree_iterator_dereference));

  std::string map_iterator_dereference =
      "static_cast<_TreeIterator::__node_pointer>(__i_.__ptr_)->__value_.__cc_";
  cpp_.emplace_back(InternalGlob("std::__2::__map_iterator<*>"),
                    std::make_unique<PrettyIterator>(map_iterator_dereference));
  cpp_.emplace_back(InternalGlob("std::__2::__map_const_iterator<*>"),
                    std::make_unique<PrettyIterator>(map_iterator_dereference));

  // Unordered_set and unordered_map. See "tree" versions (for set and map) above.
  std::string hash_table_before_emit = R"(
    auto size = __table_.__p2_.__value_;
    // Uncomment to emit size record (unsure if this is confusing).
    // $zxdb::AppendNameValueRow("[size]", size);

    auto cur_node_ptr = __table_.__p1_.__value_.__next_;
    auto nodes_emitted = 0;
    while (cur_node_ptr && nodes_emitted < size) {
      if (nodes_emitted > $zxdb::GetMaxArraySize()) {
        $zxdb::AppendNameRow("...");
        break;
      }

      auto cur_node = *static_cast<__table::__node_pointer>(cur_node_ptr);
  )";
  // (Emit line goes here.)
  std::string hash_table_after_emit = R"(
      cur_node_ptr = cur_node.__next_;
    }
  )";
  GetterList hash_table_getter_list{{"size", "__tree_.__p2_.__value_"},
                                    {"empty", "__tree_.__p2_.__value_ == 0"}};
  cpp_.emplace_back(
      InternalGlob("std::__2::unordered_set<*>"),
      std::make_unique<PrettyGenericContainer>(
          hash_table_before_emit + "$zxdb::AppendNameValueRow(\"\", cur_node.__value_);" +
              hash_table_after_emit,
          hash_table_getter_list));
  cpp_.emplace_back(
      InternalGlob("std::__2::unordered_map<*>"),
      std::make_unique<PrettyGenericContainer>(
          hash_table_before_emit + "$zxdb::AppendNameValueRow(\"\", cur_node.__value_.__cc_);" +
              hash_table_after_emit,
          hash_table_getter_list));

  // Unordered_set and unordered_map iterators.
  std::string hash_set_iterator_dereference = "static_cast<_NodePtr>(__node_)->__value_";
  cpp_.emplace_back(InternalGlob("std::__2::unordered_set<*>::iterator"),
                    std::make_unique<PrettyIterator>(hash_set_iterator_dereference));
  cpp_.emplace_back(InternalGlob("std::__2::unordered_set<*>::const_iterator"),
                    std::make_unique<PrettyIterator>(hash_set_iterator_dereference));

  // TODO support unordered_map iterators:
  //
  // For "std::__2::unordered_map<*>::iterator" ideally we'd say
  //   static_cast<_HashIterator::_NodePtr>(__i_.__node_)->__value_.__cc_
  // But _NodePtr is a template parameter so can't be referenced as a child of _HashIterator using
  // "::". The compiler doesn't emit other typedefs that make it convenient. Our expression system
  // doesn't support decltype, but I think
  //   static_cast<decltype(__i_.__node_)::__node_pointer)>(__i_.__node_)->__value_.__cc_
  // might also work. We could also just hardcode this case.
}

void PrettyTypeManager::AddDefaultRustPrettyTypes() {
  rust_tuple_type_ = std::make_unique<PrettyRustTuple>();

  // Borrowed version of Str, OsStr.
  // Rust's "&str" type won't parse as an identifier, construct an Identifier manually.
  rust_.emplace_back(IdentifierGlob(ParsedIdentifier(IdentifierQualification::kRelative,
                                                     ParsedIdentifierComponent("&str"))),
                     std::make_unique<PrettyHeapString>("data_ptr", "length",
                                                        GetterList{{"as_ptr", "data_ptr"},
                                                                   {"as_mut_ptr", "data_ptr"},
                                                                   {"len", "length"},
                                                                   {"is_empty", "length == 0"}}));
  rust_.emplace_back(
      IdentifierGlob(ParsedIdentifier(IdentifierQualification::kRelative,
                                      ParsedIdentifierComponent("&std::ffi::os_str::OsStr"))),
      std::make_unique<PrettyHeapString>("data_ptr", "length",
                                         GetterList{{"as_ptr", "data_ptr"},
                                                    {"as_mut_ptr", "data_ptr"},
                                                    {"len", "length"},
                                                    {"is_empty", "length == 0"}}));
  // TODO(fxbug.dev/79506): Support Rust slices.

  // Owned version of String, OsString and Vec.
  rust_.emplace_back(InternalGlob("alloc::string::String"),
                     std::make_unique<PrettyHeapString>(
                         "vec.buf.ptr.pointer.pointer as *u8", "vec.len",
                         GetterList{{"as_ptr", "vec.buf.ptr.pointer.pointer as *u8"},
                                    {"as_mut_ptr", "vec.buf.ptr.pointer.pointer as *u8"},
                                    {"len", "vec.len"},
                                    {"capacity", "vec.buf.cap"},
                                    {"is_empty", "vec.len == 0"}}));
  rust_.emplace_back(InternalGlob("std::ffi::os_str::OsString"),
                     std::make_unique<PrettyHeapString>(
                         "inner.inner.buf.ptr.pointer.pointer as *u8", "inner.inner.len",
                         GetterList{{"as_ptr", "inner.inner.buf.ptr.pointer.pointer as *u8"},
                                    {"as_mut_ptr", "inner.inner.buf.ptr.pointer.pointer as *u8"},
                                    {"len", "inner.inner.len"},
                                    {"capacity", "inner.inner.buf.cap"},
                                    {"is_empty", "inner.inner.len == 0"}}));
  rust_.emplace_back(
      InternalGlob("alloc::vec::Vec<*>"),
      std::make_unique<PrettyArray>("buf.ptr.pointer.pointer", "len",
                                    GetterList{{"as_ptr", "buf.ptr.pointer.pointer"},
                                               {"as_mut_ptr", "buf.ptr.pointer.pointer"},
                                               {"len", "len"},
                                               {"capacity", "buf.cap"},
                                               {"is_empty", "len == 0"}}));
  // RawVec is in the inside of a Vec (used for some other containers so we have this pretty-printer
  // to show the user). The RawVec holds only the capacity and doesn't know its length so typically
  // contains some uninitialized memory. To avoid misleading data, only show the capacity.
  rust_.emplace_back(InternalGlob("alloc::raw_vec::RawVec<*>"),
                     std::make_unique<PrettyStruct>(GetterList{{"capacity", "cap"}}));

  // A BinaryHeap is a wrapper around a "Vec" named "data".
  rust_.emplace_back(InternalGlob("alloc::collections::binary_heap::BinaryHeap<*>"),
                     std::make_unique<PrettyArray>("data.buf.ptr.pointer", "data.len",
                                                   GetterList{{"len", "data.len"},
                                                              {"capacity", "data.buf.cap"},
                                                              {"is_empty", "data.len == 0"}}));

  // Smart pointers.
  rust_.emplace_back(
      InternalGlob("alloc::sync::Arc<*>"),
      std::make_unique<PrettyPointer>("ptr.pointer",
                                      GetterList{{"weak_count", "ptr.pointer->weak.v.value"},
                                                 {"strong_count", "ptr.pointer->strong.v.value"}}));
  rust_.emplace_back(
      InternalGlob("core::ptr::non_null::NonNull<*>"),
      std::make_unique<PrettyPointer>("pointer", GetterList{{"as_ptr", "ptr.pointer"},
                                                            {"as_ref", "*ptr.pointer"},
                                                            {"as_mut", "*ptr.pointer"}}));

  // Atomic values (bool is special because it's stored as an 8-bit int, and "ptr" is a template).
  // These are otherwise all the same, they just put the value in "v.value".
  for (const auto& name : {"AtomicI8", "AtomicI16", "AtomicI32", "AtomicI64", "AtomicIsize",
                           "AtomicU8", "AtomicU16", "AtomicU32", "AtomicU64", "AtomicUsize"}) {
    rust_.emplace_back(InternalGlob((std::string("core::sync::atomic::") + name).c_str()),
                       std::make_unique<PrettyWrappedValue>(name, "{", "}", "v.value"));
  }
  rust_.emplace_back(InternalGlob("core::sync::atomic::AtomicBool"),
                     std::make_unique<PrettyWrappedValue>("AtomicBool", "{", "}", "(bool)v.value"));
  rust_.emplace_back(InternalGlob("core::sync::atomic::AtomicPtr<*>"),
                     std::make_unique<PrettyWrappedValue>("AtomicPtr", "{", "}", "v.value"));

  // Remove the type annotations and ancillary data for some common wrapper objects. UnsafeCell is
  // used for many internal mutable values. ArcInner is what alloc::sync::Arc holds a reference to.
  rust_.emplace_back(InternalGlob("core::cell::UnsafeCell<*>"),
                     std::make_unique<PrettyWrappedValue>("UnsafeCell", "{", "}", "value"));
  rust_.emplace_back(InternalGlob("alloc::sync::ArcInner<*>"),
                     std::make_unique<PrettyWrappedValue>("ArcInner", "{", "}", "data"));
  rust_.emplace_back(InternalGlob("lock_api::mutex::Mutex<*>"),
                     std::make_unique<PrettyWrappedValue>("Mutex", "{", "}", "data.value"));

  // Rust's wrapper for zx_status_t
  rust_.emplace_back(InternalGlob("fuchsia_zircon_status::Status"),
                     std::make_unique<PrettyRustZirconStatus>());

  // Channels appear in many places and typically don't contain useful information.
  rust_.emplace_back(InternalGlob("fuchsia_async::handle::zircon::channel::Channel"),
                     std::make_unique<PrettyStruct>(GetterList{{"handle", "__0.handle.__0.__0"}}));
}

void PrettyTypeManager::AddDefaultFuchsiaCppPrettyTypes() {
  // Zircon.
  cpp_.emplace_back(InternalGlob("zx_status_t"), std::make_unique<PrettyZxStatusT>());

// fbl
#define FBL_STRING_LENGTH_EXPRESSION \
  "*reinterpret_cast<size_t*>(data_ - kDataFieldOffset + kLengthFieldOffset)"
  cpp_.emplace_back(
      InternalGlob("fbl::String"),
      std::make_unique<PrettyHeapString>("data_", FBL_STRING_LENGTH_EXPRESSION,
                                         GetterList{{"data", "data_"},
                                                    {"c_str", "data_"},
                                                    {"length", FBL_STRING_LENGTH_EXPRESSION},
                                                    {"size", FBL_STRING_LENGTH_EXPRESSION},
                                                    {"empty", "!" FBL_STRING_LENGTH_EXPRESSION}}));
  cpp_.emplace_back(InternalGlob("cpp20::span<*>"),
                    std::make_unique<PrettyArray>(
                        "ptr_", "size_",
                        GetterList{{"size", "size_"}, {"data", "ptr_"}, {"empty", "size_ == 0"}}));
  cpp_.emplace_back(InternalGlob("fbl::Vector<*>"),
                    std::make_unique<PrettyArray>("ptr_", "size_",
                                                  GetterList{{"size", "size_"},
                                                             {"get", "ptr_"},
                                                             {"capacity", "capacity_"},
                                                             {"is_empty", "size_ == 0"}}));
  cpp_.emplace_back(InternalGlob("fbl::RefPtr<*>"),
                    std::make_unique<PrettyPointer>("ptr_", GetterList{{"get", "ptr_"}}));
  cpp_.emplace_back(
      InternalGlob("fbl::RefCounted<*>"),
      std::make_unique<PrettyStruct>(GetterList{{"ref_count_", "ref_count_.__a_.__a_value"}}));

  // fit
  cpp_.emplace_back(InternalGlob("fit::variant<*>"),
                    std::make_unique<PrettyRecursiveVariant>(
                        "fit::variant", "storage_.base_", "storage_.index_", "rest", "value",
                        "fit::variant::empty", GetterList({{"index", "storage_.index_"}})));

  // fxl
  cpp_.emplace_back(InternalGlob("fxl::RefPtr<*>"),
                    std::make_unique<PrettyPointer>("ptr_", GetterList{{"get", "ptr_"}}));
  cpp_.emplace_back(
      InternalGlob("fxl::RefCountedThreadSafe<*>"),
      std::make_unique<PrettyStruct>(GetterList{{"ref_count_", "ref_count_.__a_.__a_value"}}));

  // stdcompat
  cpp_.emplace_back(
      InternalGlob("cpp17::optional<*>"),
      std::make_unique<PrettyOptional>(
          "cpp17::optional", "storage_.index_ == 0", "storage_.base_.value", "cpp17::nullopt",
          GetterList{{"value", "storage_.base_.value"}, {"has_value", "storage_.index_ == 0"}}));
}

}  // namespace zxdb
