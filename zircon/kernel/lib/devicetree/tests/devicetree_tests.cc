// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/devicetree/devicetree.h>
#include <lib/stdcompat/array.h>
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
  auto walker = [&seen](const devicetree::NodePath& path, devicetree::Properties) {
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
  auto pre_walker = [&](const devicetree::NodePath& path, devicetree::Properties props) {
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
  auto post_walker = [&](const devicetree::NodePath& path, devicetree::Properties props) {
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
  auto walker = [&seen](const devicetree::NodePath& path, devicetree::Properties props) {
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

}  // namespace
