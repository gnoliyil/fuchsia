// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/devicetree/devicetree.h>
#include <lib/fit/result.h>
#include <lib/stdcompat/array.h>
#include <lib/stdcompat/source_location.h>
#include <lib/stdcompat/span.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <filesystem>
#include <memory>

#include <zxtest/zxtest.h>

#include "test_helper.h"

namespace {

constexpr size_t kMaxSize = 1024;

TEST(DevicetreeTest, SplitNodeName) {
  {
    auto [name, unit_addr] = devicetree::SplitNodeName("abc");
    EXPECT_STREQ("abc", name);
    EXPECT_STREQ("", unit_addr);
  }
  {
    auto [name, unit_addr] = devicetree::SplitNodeName("abc@");
    EXPECT_STREQ("abc", name);
    EXPECT_STREQ("", unit_addr);
  }
  {
    auto [name, unit_addr] = devicetree::SplitNodeName("abc@def");
    EXPECT_STREQ("abc", name);
    EXPECT_STREQ("def", unit_addr);
  }
  {
    auto [name, unit_addr] = devicetree::SplitNodeName("@def");
    EXPECT_STREQ("", name);
    EXPECT_STREQ("def", unit_addr);
  }
}

TEST(DevicetreeTest, EmptyTree) {
  uint8_t fdt[kMaxSize];
  ReadTestData("empty.dtb", fdt);
  devicetree::Devicetree dt(cpp20::as_bytes(cpp20::span{fdt}));

  size_t seen = 0;
  auto walker = [&seen](const devicetree::NodePath& path, const devicetree::PropertyDecoder&) {
    if (seen++ == 0) {
      size_t size = path.size_slow();
      EXPECT_EQ(1, size);
      if (size > 0) {
        EXPECT_TRUE(path.back().empty());  // Root node.
      }
    }
    return true;
  };
  dt.Walk(walker);
  EXPECT_EQ(1, seen);
}

struct Node {
  std::string_view name;
  size_t size = 0;
  bool prune = false;
};

// List of nodes are in pre order.
// post order does a translation for node[i](preorder) -> node[post_order[i]](postorder)
void DoAndVerifyWalk(devicetree::Devicetree& tree, cpp20::span<const Node> nodes,
                     cpp20::span<const size_t> post_order) {
  // PreWalk Only check.
  size_t seen = 0;
  auto pre_walker = [&](const devicetree::NodePath& path,
                        const devicetree::PropertyDecoder& props) {
    bool prune = false;
    if (seen < nodes.size()) {
      auto node = nodes[seen];
      size_t size = path.size_slow();
      EXPECT_EQ(node.size, path.size_slow());
      if (size > 0) {
        EXPECT_STREQ(node.name, path.back());
      }
      prune = node.prune;
    }
    ++seen;
    return !prune;
  };

  // Only pre walk.
  tree.Walk(pre_walker);
  EXPECT_EQ(seen, nodes.size());
  seen = 0;

  size_t post_seen = 0;
  auto post_walker = [&](const devicetree::NodePath& path,
                         const devicetree::PropertyDecoder& props) {
    bool prune = false;
    if (post_seen < nodes.size()) {
      auto node = nodes[post_order[post_seen]];
      size_t size = path.size_slow();
      EXPECT_EQ(node.size, path.size_slow());
      if (size > 0) {
        EXPECT_STREQ(node.name, path.back());
      }
      prune = node.prune;
    }
    ++post_seen;
    return !prune;
  };

  tree.Walk(pre_walker, post_walker);
  EXPECT_EQ(seen, nodes.size());
  EXPECT_EQ(seen, post_seen);
}

TEST(DevicetreeTest, NodesAreVisitedDepthFirst) {
  /*
         *
        / \
       A   E
      / \   \
     B   C   F
        /   / \
       D   G   I
          /
         H
  */
  uint8_t fdt[kMaxSize];
  ReadTestData("complex_no_properties.dtb", fdt);
  devicetree::Devicetree dt(cpp20::as_bytes(cpp20::span{fdt}));

  constexpr auto nodes = cpp20::to_array<Node>({
      {.name = "", .size = 1},
      {.name = "A", .size = 2},
      {.name = "B", .size = 3},
      {.name = "C", .size = 3},
      {.name = "D", .size = 4},
      {.name = "E", .size = 2},
      {.name = "F", .size = 3},
      {.name = "G", .size = 4},
      {.name = "H", .size = 5},
      {.name = "I", .size = 4},
  });

  constexpr auto post_order = cpp20::to_array<size_t>({2, 4, 3, 1, 8, 7, 9, 6, 5, 0});

  DoAndVerifyWalk(dt, nodes, post_order);
}

TEST(DevicetreeTest, SubtreesArePruned) {
  /*
         *
        / \
       A   E
      / \   \
     B   C^  F^
        /   / \
       D   G   I
          /
         H

   ^ = root of pruned subtree
  */
  uint8_t fdt[kMaxSize];
  ReadTestData("complex_no_properties.dtb", fdt);
  devicetree::Devicetree dt(cpp20::as_bytes(cpp20::span{fdt}));

  constexpr auto nodes = cpp20::to_array<Node>({
      {.name = "", .size = 1},
      {.name = "A", .size = 2},
      {.name = "B", .size = 3},
      {.name = "C", .size = 3, .prune = true},
      {.name = "E", .size = 2},
      {.name = "F", .size = 3, .prune = true},
  });

  constexpr auto post_order = cpp20::to_array<size_t>({2, 3, 1, 5, 4, 0});

  DoAndVerifyWalk(dt, nodes, post_order);
}

TEST(DevicetreeTest, WholeTreeIsPruned) {
  /*
           *^
          / \
         A   E
        / \   \
       B   C   F
          /   / \
         D   G   I
            /
           H

     ^ = root of pruned subtree
    */

  uint8_t fdt[kMaxSize];
  ReadTestData("complex_no_properties.dtb", fdt);
  devicetree::Devicetree dt(cpp20::as_bytes(cpp20::span{fdt}));

  constexpr auto nodes = cpp20::to_array<Node>({
      {.name = "", .size = 1, .prune = true},
  });

  constexpr auto post_order = cpp20::to_array<size_t>({0});

  DoAndVerifyWalk(dt, nodes, post_order);
}

TEST(DevicetreeTest, AliaesNodeIsKept) {
  uint8_t fdt[kMaxSize];
  ReadTestData("complex_with_alias_first.dtb", fdt);
  // aliases:
  // foo = "/A/C";
  // bar = "/E/F";
  devicetree::Devicetree dt(cpp20::as_bytes(cpp20::span{fdt}));

  std::optional<devicetree::ResolvedPath> resolved;
  dt.Walk([&resolved](const auto& path, const auto& decoder) {
    if (path.back() == "A") {
      if (auto res = decoder.ResolvePath("foo"); res.is_ok()) {
        resolved = *res;
      }
      return false;
    }
    return true;
  });

  ASSERT_TRUE(resolved);
  auto [foo, empty] = *resolved;
  EXPECT_EQ(foo, "/A/C");
  EXPECT_TRUE(empty.empty());
}

TEST(DevicetreeTest, PropertiesAreTranslated) {
  /*
         *
        / \
       A   C
      /     \
     B       D
  */
  uint8_t fdt[kMaxSize];
  ReadTestData("simple_with_properties.dtb", fdt);
  devicetree::Devicetree dt(cpp20::as_bytes(cpp20::span{fdt}));

  size_t seen = 0;
  auto walker = [&seen](const devicetree::NodePath& path,
                        const devicetree::PropertyDecoder& decoder) {
    auto& props = decoder.properties();
    switch (seen++) {
      case 0: {  // root
        size_t size = path.size_slow();
        EXPECT_EQ(1, size);
        if (size > 0) {
          EXPECT_TRUE(path.back().empty());
        }

        devicetree::Properties::iterator begin;
        begin = props.begin();  // Can copy-assign.
        EXPECT_EQ(begin, props.end());

        break;
      }
      case 1: {  // A
        size_t size = path.size_slow();
        EXPECT_EQ(2, size);
        if (size > 0) {
          EXPECT_STREQ("A", path.back());
        }
        EXPECT_EQ(props.end(), std::next(props.begin(), 2));  // 2 properties.

        auto prop1 = *props.begin();
        EXPECT_STREQ("a1", prop1.name);
        EXPECT_TRUE(prop1.value.AsBool().value());
        auto prop2 = *std::next(props.begin());
        EXPECT_STREQ("a2", prop2.name);
        EXPECT_STREQ("root", *prop2.value.AsString());
        break;
      }
      case 2: {  // B
        size_t size = path.size_slow();
        EXPECT_EQ(3, size);
        if (size > 0) {
          EXPECT_STREQ("B", path.back());
        }
        EXPECT_EQ(props.end(), std::next(props.begin(), 3));  // 3 properties.

        auto prop1 = *props.begin();
        EXPECT_STREQ("b1", prop1.name);
        EXPECT_EQ(0x1, prop1.value.AsUint32());
        auto prop2 = *std::next(props.begin());
        EXPECT_STREQ("b2", prop2.name);
        EXPECT_EQ(0x10, prop2.value.AsUint32());
        auto prop3 = *std::next(props.begin(), 2);
        EXPECT_STREQ("b3", prop3.name);
        EXPECT_EQ(0x100, prop3.value.AsUint32());
        break;
      }
      case 3: {  // C
        size_t size = path.size_slow();
        EXPECT_EQ(2, size);
        if (size > 0) {
          EXPECT_STREQ("C", path.back());
        }
        EXPECT_EQ(props.end(), std::next(props.begin(), 2));  // 2 properties.

        auto prop1 = *props.begin();
        EXPECT_STREQ("c1", prop1.name);
        EXPECT_STREQ("hello", *prop1.value.AsString());
        auto prop2 = *std::next(props.begin());
        EXPECT_STREQ("c2", prop2.name);
        EXPECT_STREQ("world", *prop2.value.AsString());
        break;
      }
      case 4: {  // D
        size_t size = path.size_slow();
        EXPECT_EQ(3, size);
        if (size > 0) {
          EXPECT_STREQ("D", path.back());
        }
        EXPECT_EQ(props.end(), std::next(props.begin(), 3));  // 3 properties.

        auto prop1 = *props.begin();
        EXPECT_STREQ("d1", prop1.name);
        EXPECT_EQ(0x1000, prop1.value.AsUint64());
        auto prop2 = *std::next(props.begin());
        EXPECT_STREQ("d2", prop2.name);
        EXPECT_EQ(0x10000, prop2.value.AsUint64());
        auto prop3 = *std::next(props.begin(), 2);
        EXPECT_STREQ("d3", prop3.name);
        EXPECT_EQ(0x100000, prop3.value.AsUint64());
        break;
      }
    }
    return true;
  };
  dt.Walk(walker);
  EXPECT_EQ(5, seen);
}

TEST(DevicetreeTest, MemoryReservations) {
  uint8_t fdt[kMaxSize];
  ReadTestData("memory_reservations.dtb", fdt);
  const devicetree::Devicetree dt(cpp20::as_bytes(cpp20::span{fdt}));

  unsigned int i = 0;
  for (auto [start, size] : dt.memory_reservations()) {
    switch (i++) {
      case 0:
        EXPECT_EQ(start, 0x12340000);
        EXPECT_EQ(size, 0x2000);
        break;
      case 1:
        EXPECT_EQ(start, 0x56780000);
        EXPECT_EQ(size, 0x3000);
        break;
      case 2:
        EXPECT_EQ(start, 0x7fffffff12340000);
        EXPECT_EQ(size, 0x400000000);
        break;
      case 3:
        EXPECT_EQ(start, 0x00ffffff56780000);
        EXPECT_EQ(size, 0x500000000);
        break;
      default:
        EXPECT_LT(i, 4, "too many entries");
        break;
    }
  }
  EXPECT_EQ(i, 4, "wrong number of entries");
}

TEST(DevicetreeTest, StringList) {
  using namespace std::literals;

  unsigned int i = 0;
  for (auto str : devicetree::StringList(""sv)) {
    ++i;
    EXPECT_FALSE(true, "list should be empty");
    EXPECT_TRUE(str.empty());
  }
  EXPECT_EQ(i, 0);

  i = 0;
  for (auto str : devicetree::StringList("one"sv)) {
    ++i;
    EXPECT_STREQ("one", str);
  }
  EXPECT_EQ(i, 1);

  i = 0;
  for (auto str : devicetree::StringList("one\0two\0three"sv)) {
    switch (i++) {
      case 0:
        EXPECT_STREQ("one", str);
        break;
      case 1:
        EXPECT_STREQ("two", str);
        break;
      case 2:
        EXPECT_STREQ("three", str);
        break;
    }
  }
  EXPECT_EQ(i, 3);

  i = 0;
  for (auto str : devicetree::StringList("one\0\0two\0"sv)) {
    switch (i++) {
      case 0:
        EXPECT_STREQ("one", str);
        break;
      case 2:
        EXPECT_STREQ("two", str);
        break;
      default:
        EXPECT_EQ(0, str.size());
    }
  }
  EXPECT_EQ(i, 4);

  i = 0;
  for (auto str : devicetree::StringList<'/'>("foo/bar/baz"sv)) {
    switch (i++) {
      case 0:
        EXPECT_STREQ("foo", str);
        break;
      case 1:
        EXPECT_STREQ("bar", str);
        break;
      case 3:
        EXPECT_STREQ("baz", str);
        break;
    }
  }
  EXPECT_EQ(i, 3);
}

auto as_bytes = [](auto& val) {
  using byte_type = std::conditional_t<std::is_const_v<std::remove_reference_t<decltype(val)>>,
                                       const uint8_t, uint8_t>;
  return cpp20::span<byte_type>(reinterpret_cast<byte_type*>(&val), sizeof(val));
};

auto append = [](auto& vec, auto&& other) { vec.insert(vec.end(), other.begin(), other.end()); };

uint32_t byte_swap(uint32_t val) {
  if constexpr (cpp20::endian::native == cpp20::endian::big) {
    return val;
  } else {
    auto bytes = as_bytes(val);
    return static_cast<uint32_t>(bytes[0]) << 24 | static_cast<uint32_t>(bytes[1]) << 16 |
           static_cast<uint32_t>(bytes[2]) << 8 | static_cast<uint32_t>(bytes[3]);
  }
}

// Small helper so we can verify the behavior of CachedProperties.
struct PropertyBuilder {
  devicetree::Properties Build() {
    return devicetree::Properties(
        {property_block.data(), property_block.size()},
        std::string_view(reinterpret_cast<const char*>(string_block.data()), string_block.size()));
  }

  void Add(std::string_view name, uint32_t value) {
    uint32_t name_off = byte_swap(static_cast<uint32_t>(string_block.size()));
    // String must be null terminated.
    append(string_block, name);
    string_block.push_back('\0');

    uint32_t len = byte_swap(sizeof(uint32_t));

    if (!property_block.empty()) {
      const uint32_t kFdtPropToken = byte_swap(0x00000003);
      append(property_block, as_bytes(kFdtPropToken));
    }
    // this are all 32b aliagned, no padding need.
    append(property_block, as_bytes(len));
    append(property_block, as_bytes(name_off));
    uint32_t be_value = byte_swap(value);
    append(property_block, as_bytes(be_value));
  }

  void Add(std::string_view alias, std::string_view absolute_path) {
    constexpr std::array<uint8_t, sizeof(uint32_t) - 1> kPadding = {};

    uint32_t name_off = byte_swap(static_cast<uint32_t>(string_block.size()));
    // String must be null terminated.
    append(string_block, alias);
    string_block.push_back('\0');

    // Add the null terminator.
    uint32_t len = static_cast<uint32_t>(absolute_path.size()) + 1;
    cpp20::span<const uint8_t> padding;
    if (auto remainder = len % sizeof(uint32_t); remainder != 0) {
      padding = cpp20::span(kPadding).subspan(0, sizeof(uint32_t) - remainder);
    }
    len = byte_swap(len);

    if (!property_block.empty()) {
      const uint32_t kFdtPropToken = byte_swap(0x00000003);
      append(property_block, as_bytes(kFdtPropToken));
    }
    append(property_block, as_bytes(len));
    append(property_block, as_bytes(name_off));
    append(property_block, absolute_path);
    property_block.push_back('\0');
    append(property_block, padding);
  }

  std::vector<uint8_t> property_block;
  std::vector<uint8_t> string_block;
};

auto check_prop = [](auto& prop, uint32_t val,
                     cpp20::source_location loc = cpp20::source_location::current()) {
  ASSERT_TRUE(prop, "at %s:%u", loc.file_name(), loc.line());
  auto pv = prop->AsUint32();
  ASSERT_TRUE(pv, "at %s:%u", loc.file_name(), loc.line());
  EXPECT_EQ(*pv, val, "at %s:%u", loc.file_name(), loc.line());
};

TEST(PropertyDecoderTest, FindProperties) {
  PropertyBuilder builder;
  builder.Add("property_1", 1);
  builder.Add("property_2", 2);
  builder.Add("property_3", 3);
  auto props = builder.Build();

  devicetree::PropertyDecoder decoder(props);

  auto [p1, p2, p3, u4, rep_p3] =
      decoder.FindProperties("property_1", "property_2", "property_3", "unknown", "property_3");

  check_prop(p1, 1);
  check_prop(p2, 2);
  check_prop(p3, 3);
  ASSERT_FALSE(u4);
  ASSERT_FALSE(rep_p3);
}

TEST(PropertyDecoderTest, FindProperty) {
  PropertyBuilder builder;
  builder.Add("property_1", 1);
  builder.Add("property_2", 2);
  builder.Add("property_3", 3);
  auto props = builder.Build();

  devicetree::PropertyDecoder decoder(props);

  auto prop = decoder.FindProperty("property_1");
  auto not_found = decoder.FindProperty("not in there");

  check_prop(prop, 1);
  ASSERT_FALSE(not_found);
}

TEST(PropertyDecoderTest, ResolvePathWithAlias) {
  PropertyBuilder builder;
  builder.Add("foo", "/foo/bar");
  builder.Add("bar", "/bar/baz");
  builder.Add("baz", "/baz/foo");
  builder.Add("empty", "");
  std::optional<devicetree::Properties> aliases(builder.Build());

  devicetree::PropertyDecoder decoder(nullptr, devicetree::Properties(), aliases);

  auto foo_path = decoder.ResolvePath("foo");
  ASSERT_TRUE(foo_path.is_ok());

  auto [foo_prefix, foo_suffix] = *foo_path;
  EXPECT_EQ(foo_prefix, "/foo/bar");
  EXPECT_TRUE(foo_suffix.empty());

  auto bar_path = decoder.ResolvePath("bar/baz");
  ASSERT_TRUE(bar_path.is_ok());

  auto [bar_prefix, bar_suffix] = *bar_path;
  EXPECT_EQ(bar_prefix, "/bar/baz");
  EXPECT_EQ(bar_suffix, "baz");

  auto baz_path = decoder.ResolvePath("baz/baz/foo");
  ASSERT_TRUE(baz_path.is_ok());

  auto [baz_prefix, baz_suffix] = *baz_path;
  EXPECT_EQ(baz_prefix, "/baz/foo");
  EXPECT_EQ(baz_suffix, "baz/foo");

  // Alias not found.
  auto not_found = decoder.ResolvePath("foobar");
  ASSERT_TRUE(not_found.is_error());
  EXPECT_EQ(not_found.error_value(), devicetree::PropertyDecoder::PathResolveError::kBadAlias);

  // Absolute path with alias
  auto absolute_path = decoder.ResolvePath("/foo");
  ASSERT_TRUE(absolute_path.is_ok());

  auto [abs_prefix, empty_suffix] = *absolute_path;
  EXPECT_EQ(abs_prefix, "/foo");
  EXPECT_TRUE(empty_suffix.empty());

  // empty path
  auto empty_path = decoder.ResolvePath("empty/baz");
  ASSERT_TRUE(empty_path.is_error());
  EXPECT_EQ(empty_path.error_value(), devicetree::PropertyDecoder::PathResolveError::kBadAlias);
}

TEST(PropertyDecoderTest, ResolvePathNoAlias) {
  std::optional<devicetree::Properties> aliases;

  devicetree::PropertyDecoder decoder(nullptr, devicetree::Properties(), aliases);
  auto path_with_alias = decoder.ResolvePath("foo");

  ASSERT_TRUE(path_with_alias.is_error());
  ASSERT_EQ(path_with_alias.error_value(),
            devicetree::PropertyDecoder::PathResolveError::kNoAliases);

  auto absolute_path = decoder.ResolvePath("/foo");

  ASSERT_TRUE(absolute_path.is_ok());
  auto [abs_prefix, empty_suffix] = *absolute_path;

  EXPECT_EQ(abs_prefix, "/foo");
  EXPECT_TRUE(empty_suffix.empty());
}

TEST(PropertyDecoderTest, CellCountsAreCached) {
  // This test relies on manipulating the underlying data, to verify
  // at which point are the properties actually cached.
  PropertyBuilder builder;
  builder.Add("#address-cells", 1);
  builder.Add("#size-cells", 2);
  builder.Add("#interrupt-cells", 3);
  auto properties = builder.Build();

  devicetree::PropertyDecoder decoder(properties);
  {
    auto address_cells = decoder.num_address_cells();
    ASSERT_TRUE(address_cells);
    EXPECT_EQ(*address_cells, 1);

    auto size_cells = decoder.num_size_cells();
    ASSERT_TRUE(size_cells);
    EXPECT_EQ(*size_cells, 2);

    auto interrupt_cells = decoder.num_interrupt_cells();
    ASSERT_TRUE(interrupt_cells);
    EXPECT_EQ(*interrupt_cells, 3);
  }
  // Update the underlying propeties, if the property is not cached, it should
  // return the updated value.
  PropertyBuilder mod_builder;
  mod_builder.Add("#address-cells", 3);
  mod_builder.Add("#size-cells", 4);
  mod_builder.Add("#interrupt-cells", 5);
  std::ignore = mod_builder.Build();
  // copy contents to the previous property block.
  builder.property_block = mod_builder.property_block;

  // Safe check that the underlying properties have been updated.
  {
    auto address_cells = decoder.FindProperty("#address-cells");
    ASSERT_TRUE(address_cells);
    auto val = address_cells->AsUint32();
    ASSERT_TRUE(val);
    EXPECT_EQ(*val, 3);
  }

  // Now double check address cell still has the previous value.
  {
    auto address_cells = decoder.num_address_cells();
    ASSERT_TRUE(address_cells);
    EXPECT_EQ(*address_cells, 1);

    auto size_cells = decoder.num_size_cells();
    ASSERT_TRUE(size_cells);
    EXPECT_EQ(*size_cells, 2);

    auto interrupt_cells = decoder.num_interrupt_cells();
    ASSERT_TRUE(interrupt_cells);
    EXPECT_EQ(*interrupt_cells, 3);
  }
}

}  // namespace
