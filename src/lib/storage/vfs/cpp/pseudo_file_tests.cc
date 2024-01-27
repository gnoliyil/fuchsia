// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <initializer_list>
#include <string_view>

#include <fbl/vector.h>
#include <zxtest/zxtest.h>

#include "src/lib/storage/vfs/cpp/pseudo_file.h"

#define EXPECT_FSTREQ(expected, actual)                                                   \
  EXPECT_BYTES_EQ(reinterpret_cast<const uint8_t*>(expected.c_str()),                     \
                  reinterpret_cast<const uint8_t*>(actual.c_str()), expected.size() + 1u, \
                  "unequal fbl::String")

#define EXPECT_ATTR_EQ(expected, actual) \
  EXPECT_TRUE(expected == actual, "unequal fs::VnodeAttributes")

namespace {

using VnodeOptions = fs::VnodeConnectionOptions;

zx_status_t DummyReader(fbl::String* output) { return ZX_OK; }

zx_status_t DummyWriter(std::string_view input) { return ZX_OK; }

class VectorReader {
 public:
  VectorReader(std::initializer_list<fbl::String> strings) : strings_(strings) {}

  fs::PseudoFile::ReadHandler GetHandler() {
    return [this](fbl::String* output) {
      if (index_ >= strings_.size())
        return ZX_ERR_IO;
      *output = strings_[index_++];
      return ZX_OK;
    };
  }

  const fbl::Vector<fbl::String>& strings() const { return strings_; }

 private:
  fbl::Vector<fbl::String> strings_;
  size_t index_ = 0u;
};

class VectorWriter {
 public:
  explicit VectorWriter(size_t max_strings) : max_strings_(max_strings) {}

  fs::PseudoFile::WriteHandler GetHandler() {
    return [this](std::string_view input) {
      if (strings_.size() >= max_strings_)
        return ZX_ERR_IO;
      strings_.push_back(fbl::String(input));
      return ZX_OK;
    };
  }

  const fbl::Vector<fbl::String>& strings() const { return strings_; }

 private:
  const size_t max_strings_;
  fbl::Vector<fbl::String> strings_;
};

void CheckRead(const fbl::RefPtr<fs::Vnode>& file, zx_status_t status, size_t length, size_t offset,
               std::string_view expected) {
  std::vector<uint8_t> buf(length, '!');
  size_t actual = 0u;
  EXPECT_EQ(status, file->Read(buf.data(), length, offset, &actual));
  EXPECT_EQ(expected.size(), actual);
  EXPECT_BYTES_EQ(reinterpret_cast<const uint8_t*>(expected.data()), buf.data(), expected.size(),
                  "");
}

void CheckWrite(const fbl::RefPtr<fs::Vnode>& file, zx_status_t status, size_t offset,
                std::string_view content, size_t expected_actual) {
  size_t actual = 0u;
  EXPECT_EQ(status, file->Write(content.data(), content.size(), offset, &actual));
  EXPECT_EQ(expected_actual, actual);
}

void CheckAppend(const fbl::RefPtr<fs::Vnode>& file, zx_status_t status, std::string_view content,
                 size_t expected_end, size_t expected_actual) {
  size_t end = 0u;
  size_t actual = 0u;
  EXPECT_EQ(status, file->Append(content.data(), content.size(), &end, &actual));
  EXPECT_EQ(expected_end, end);
  EXPECT_EQ(expected_actual, actual);
}

#define EXPECT_RESULT_OK(expr) EXPECT_TRUE((expr).is_ok())
#define EXPECT_RESULT_ERROR(error_val, expr) \
  EXPECT_TRUE((expr).is_error());            \
  EXPECT_EQ(error_val, (expr).status_value())

TEST(PseudoFile, OpenValidationBuffered) {
  // no read handler, no write handler
  {
    auto file = fbl::MakeRefCounted<fs::BufferedPseudoFile>();
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadOnly()));
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadWrite()));
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::WriteOnly()));
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadExec()));
    EXPECT_RESULT_ERROR(ZX_ERR_NOT_DIR, file->ValidateOptions(VnodeOptions().set_directory()));
  }

  // read handler, no write handler
  {
    auto file = fbl::MakeRefCounted<fs::BufferedPseudoFile>(&DummyReader);
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadWrite()));
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::WriteOnly()));
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadExec()));
    EXPECT_RESULT_ERROR(ZX_ERR_NOT_DIR, file->ValidateOptions(VnodeOptions().set_directory()));

    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::ReadOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    EXPECT_TRUE(redirect);
  }

  // no read handler, write handler
  {
    auto file = fbl::MakeRefCounted<fs::BufferedPseudoFile>(nullptr, &DummyWriter);
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadOnly()));
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadWrite()));
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadExec()));
    EXPECT_RESULT_ERROR(ZX_ERR_NOT_DIR, file->ValidateOptions(VnodeOptions().set_directory()));

    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    EXPECT_TRUE(redirect);
  }

  // read handler, write handler
  {
    auto file = fbl::MakeRefCounted<fs::BufferedPseudoFile>(&DummyReader, &DummyWriter);
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadExec()));
    EXPECT_RESULT_ERROR(ZX_ERR_NOT_DIR, file->ValidateOptions(VnodeOptions().set_directory()));

    {
      fbl::RefPtr<fs::Vnode> redirect;
      auto result = file->ValidateOptions(VnodeOptions::ReadOnly());
      EXPECT_RESULT_OK(result);
      EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
      EXPECT_TRUE(redirect);
    }
    {
      fbl::RefPtr<fs::Vnode> redirect;
      auto result = file->ValidateOptions(VnodeOptions::ReadWrite());
      EXPECT_RESULT_OK(result);
      EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
      EXPECT_TRUE(redirect);
    }
    {
      fbl::RefPtr<fs::Vnode> redirect;
      auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
      EXPECT_RESULT_OK(result);
      EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
      EXPECT_TRUE(redirect);
    }
  }
}

TEST(PseudoFile, OpenValidationUnbuffered) {
  // no read handler, no write handler
  {
    auto file = fbl::MakeRefCounted<fs::UnbufferedPseudoFile>();
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadOnly()));
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadWrite()));
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::WriteOnly()));
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadExec()));
    EXPECT_RESULT_ERROR(ZX_ERR_NOT_DIR, file->ValidateOptions(VnodeOptions().set_directory()));
  }

  // read handler, no write handler
  {
    auto file = fbl::MakeRefCounted<fs::UnbufferedPseudoFile>(&DummyReader);
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadWrite()));
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::WriteOnly()));
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadExec()));
    EXPECT_RESULT_ERROR(ZX_ERR_NOT_DIR, file->ValidateOptions(VnodeOptions().set_directory()));
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::ReadOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    EXPECT_TRUE(redirect);
  }

  // no read handler, write handler
  {
    auto file = fbl::MakeRefCounted<fs::UnbufferedPseudoFile>(nullptr, &DummyWriter);
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadOnly()));
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadWrite()));
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadExec()));
    EXPECT_RESULT_ERROR(ZX_ERR_NOT_DIR, file->ValidateOptions(VnodeOptions().set_directory()));
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    EXPECT_TRUE(redirect);
  }

  // read handler, write handler
  {
    auto file = fbl::MakeRefCounted<fs::UnbufferedPseudoFile>(&DummyReader, &DummyWriter);
    EXPECT_RESULT_ERROR(ZX_ERR_ACCESS_DENIED, file->ValidateOptions(VnodeOptions::ReadExec()));
    EXPECT_RESULT_ERROR(ZX_ERR_NOT_DIR, file->ValidateOptions(VnodeOptions().set_directory()));
    {
      fbl::RefPtr<fs::Vnode> redirect;
      auto result = file->ValidateOptions(VnodeOptions::ReadOnly());
      EXPECT_RESULT_OK(result);
      EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
      EXPECT_TRUE(redirect);
    }
    {
      fbl::RefPtr<fs::Vnode> redirect;
      auto result = file->ValidateOptions(VnodeOptions::ReadWrite());
      EXPECT_RESULT_OK(result);
      EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
      EXPECT_TRUE(redirect);
    }
    {
      fbl::RefPtr<fs::Vnode> redirect;
      auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
      EXPECT_RESULT_OK(result);
      EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
      EXPECT_TRUE(redirect);
    }
  }
}

TEST(PseudoFile, GetattrBuffered) {
  const fbl::String kHello = "hello";

  auto reader = [kHello](fbl::String* out) {
    *out = kHello;
    return ZX_OK;
  };

  // no read handler, no write handler
  {
    auto file = fbl::MakeRefCounted<fs::BufferedPseudoFile>();
    fs::VnodeAttributes attr;
    EXPECT_EQ(ZX_OK, file->GetAttributes(&attr));
    EXPECT_EQ(V_TYPE_FILE, attr.mode);
    EXPECT_EQ(0, attr.content_size);
    EXPECT_EQ(1, attr.link_count);

    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions().set_node_reference());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    fs::VnodeAttributes open_attr;
    EXPECT_EQ(ZX_OK, redirect->GetAttributes(&open_attr));
    EXPECT_ATTR_EQ(attr, open_attr);
  }

  // read handler, no write handler
  {
    auto file = fbl::MakeRefCounted<fs::BufferedPseudoFile>(reader);
    fs::VnodeAttributes attr;
    EXPECT_EQ(ZX_OK, file->GetAttributes(&attr));
    EXPECT_EQ(V_TYPE_FILE | V_IRUSR, attr.mode);
    EXPECT_EQ(0, attr.content_size);
    EXPECT_EQ(1, attr.link_count);

    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::ReadOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    fs::VnodeAttributes open_attr;
    EXPECT_EQ(ZX_OK, redirect->GetAttributes(&open_attr));
    attr.content_size = kHello.size();
    EXPECT_ATTR_EQ(attr, open_attr);
  }

  // no read handler, write handler
  {
    auto file = fbl::MakeRefCounted<fs::BufferedPseudoFile>(nullptr, &DummyWriter);
    fs::VnodeAttributes attr;
    EXPECT_EQ(ZX_OK, file->GetAttributes(&attr));
    EXPECT_EQ(V_TYPE_FILE | V_IWUSR, attr.mode);
    EXPECT_EQ(0, attr.content_size);
    EXPECT_EQ(1, attr.link_count);

    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    fs::VnodeAttributes open_attr;
    EXPECT_EQ(ZX_OK, redirect->GetAttributes(&open_attr));
    EXPECT_ATTR_EQ(attr, open_attr);
  }

  // read handler, write handler
  {
    auto file = fbl::MakeRefCounted<fs::BufferedPseudoFile>(reader, &DummyWriter);
    fs::VnodeAttributes attr;
    EXPECT_EQ(ZX_OK, file->GetAttributes(&attr));
    EXPECT_EQ(V_TYPE_FILE | V_IRUSR | V_IWUSR, attr.mode);
    EXPECT_EQ(0, attr.content_size);
    EXPECT_EQ(1, attr.link_count);

    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::ReadWrite());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    fs::VnodeAttributes open_attr;
    EXPECT_EQ(ZX_OK, redirect->GetAttributes(&open_attr));
    attr.content_size = kHello.size();
    EXPECT_ATTR_EQ(attr, open_attr);
  }
}

TEST(PseudoFile, GetattrUnbuffered) {
  const fbl::String kHello = "hello";

  auto reader = [kHello](fbl::String* out) {
    *out = kHello;
    return ZX_OK;
  };

  // no read handler, no write handler
  {
    auto file = fbl::MakeRefCounted<fs::UnbufferedPseudoFile>();
    fs::VnodeAttributes attr;
    EXPECT_EQ(ZX_OK, file->GetAttributes(&attr));
    EXPECT_EQ(V_TYPE_FILE, attr.mode);
    EXPECT_EQ(0, attr.content_size);
    EXPECT_EQ(1, attr.link_count);

    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions().set_node_reference());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    fs::VnodeAttributes open_attr;
    EXPECT_EQ(ZX_OK, redirect->GetAttributes(&open_attr));
    EXPECT_ATTR_EQ(attr, open_attr);
  }

  // read handler, no write handler
  {
    auto file = fbl::MakeRefCounted<fs::UnbufferedPseudoFile>(reader);
    fs::VnodeAttributes attr;
    EXPECT_EQ(ZX_OK, file->GetAttributes(&attr));
    EXPECT_EQ(V_TYPE_FILE | V_IRUSR, attr.mode);
    EXPECT_EQ(0, attr.content_size);
    EXPECT_EQ(1, attr.link_count);

    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::ReadOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    fs::VnodeAttributes open_attr;
    EXPECT_EQ(ZX_OK, redirect->GetAttributes(&open_attr));
    EXPECT_ATTR_EQ(attr, open_attr);
  }

  // no read handler, write handler
  {
    auto file = fbl::MakeRefCounted<fs::UnbufferedPseudoFile>(nullptr, &DummyWriter);
    fs::VnodeAttributes attr;
    EXPECT_EQ(ZX_OK, file->GetAttributes(&attr));
    EXPECT_EQ(V_TYPE_FILE | V_IWUSR, attr.mode);
    EXPECT_EQ(0, attr.content_size);
    EXPECT_EQ(1, attr.link_count);

    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    fs::VnodeAttributes open_attr;
    EXPECT_EQ(ZX_OK, redirect->GetAttributes(&open_attr));
    EXPECT_ATTR_EQ(attr, open_attr);
  }

  // read handler, write handler
  {
    auto file = fbl::MakeRefCounted<fs::UnbufferedPseudoFile>(reader, &DummyWriter);
    fs::VnodeAttributes attr;
    EXPECT_EQ(ZX_OK, file->GetAttributes(&attr));
    EXPECT_EQ(V_TYPE_FILE | V_IRUSR | V_IWUSR, attr.mode);
    EXPECT_EQ(0, attr.content_size);
    EXPECT_EQ(1, attr.link_count);

    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::ReadWrite());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    fs::VnodeAttributes open_attr;
    EXPECT_EQ(ZX_OK, redirect->GetAttributes(&open_attr));
    EXPECT_ATTR_EQ(attr, open_attr);
  }
}

TEST(PseudoFile, ReadBuffered) {
  VectorReader reader{"first", "second", "", fbl::String(std::string_view("null\0null", 9u))};
  auto file = fbl::MakeRefCounted<fs::BufferedPseudoFile>(reader.GetHandler());

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::ReadOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    CheckRead(redirect, ZX_OK, 0u, 0u, "");
    CheckRead(redirect, ZX_OK, 4u, 0u, "firs");
    CheckRead(redirect, ZX_OK, 4u, 2u, "rst");
    CheckRead(redirect, ZX_OK, 5u, 0u, "first");
    CheckRead(redirect, ZX_OK, 8u, 0u, "first");
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::ReadOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    CheckRead(redirect, ZX_OK, 4u, 2u, "cond");
    CheckRead(redirect, ZX_OK, 6u, 0u, "second");
    CheckRead(redirect, ZX_OK, 8u, 0u, "second");
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::ReadOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    CheckRead(redirect, ZX_OK, 4u, 0u, "");
    CheckRead(redirect, ZX_OK, 4u, 2u, "");
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::ReadOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    CheckRead(redirect, ZX_OK, 0u, 0u, "");
    CheckRead(redirect, ZX_OK, 4u, 0u, "null");
    CheckRead(redirect, ZX_OK, 4u, 2u, std::string_view("ll\0n", 4u));
    CheckRead(redirect, ZX_OK, 9u, 0u, std::string_view("null\0null", 9u));
    CheckRead(redirect, ZX_OK, 12u, 0u, std::string_view("null\0null", 9u));
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::ReadOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_ERR_IO, file->Open(result.value(), &redirect));
  }
}

TEST(PseudoFile, ReadUnbuffered) {
  VectorReader reader{"first",
                      "second",
                      "third",
                      "fourth",
                      "fifth",
                      "",
                      fbl::String(std::string_view("null\0null", 9u))};
  auto file = fbl::MakeRefCounted<fs::UnbufferedPseudoFile>(reader.GetHandler());

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::ReadOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    CheckRead(redirect, ZX_OK, 0u, 0u, "");
    CheckRead(redirect, ZX_OK, 4u, 0u, "seco");
    CheckRead(redirect, ZX_OK, 4u, 2u, "");
    CheckRead(redirect, ZX_OK, 3u, 0u, "thi");
    CheckRead(redirect, ZX_OK, 6u, 0u, "fourth");
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::ReadOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    CheckRead(redirect, ZX_OK, 8u, 0u, "fifth");
    CheckRead(redirect, ZX_OK, 4u, 0u, "");
    CheckRead(redirect, ZX_OK, 12u, 0u, std::string_view("null\0null", 9u));
    CheckRead(redirect, ZX_ERR_IO, 0u, 0u, "");
    EXPECT_EQ(ZX_OK, redirect->Close());
  }
}

TEST(PseudoFile, WriteBuffered) {
  VectorWriter writer(6u);
  auto file = fbl::MakeRefCounted<fs::BufferedPseudoFile>(nullptr, writer.GetHandler(), 10u);

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    CheckWrite(redirect, ZX_OK, 0u, "fixx", 4u);
    CheckWrite(redirect, ZX_OK, 0u, "", 0u);
    CheckWrite(redirect, ZX_OK, 2u, "rst", 3u);
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    CheckWrite(redirect, ZX_OK, 0u, "second", 6u);
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    CheckAppend(redirect, ZX_OK, "thxrxxx", 7u, 7u);
    CheckWrite(redirect, ZX_OK, 2u, "i", 1u);
    EXPECT_EQ(ZX_OK, redirect->Truncate(4u));
    CheckAppend(redirect, ZX_OK, "d", 5u, 1u);
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    CheckWrite(redirect, ZX_OK, 0u, "null", 4u);
    EXPECT_EQ(ZX_OK, redirect->Truncate(5u));
    CheckAppend(redirect, ZX_OK, "null", 9u, 4u);
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    EXPECT_EQ(ZX_ERR_NO_SPACE, redirect->Truncate(11u));
    CheckAppend(redirect, ZX_OK, "too-long", 8u, 8u);
    CheckAppend(redirect, ZX_OK, "-off-the-end", 10u, 2u);
    CheckAppend(redirect, ZX_ERR_NO_SPACE, "-overflow", 0u, 0u);
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    EXPECT_EQ(ZX_ERR_IO, redirect->Close());
  }

  EXPECT_EQ(6u, writer.strings().size());
  EXPECT_FSTREQ(writer.strings()[0], fbl::String("first"));
  EXPECT_FSTREQ(writer.strings()[1], fbl::String("second"));
  EXPECT_FSTREQ(writer.strings()[2], fbl::String(""));
  EXPECT_FSTREQ(writer.strings()[3], fbl::String("third"));
  EXPECT_FSTREQ(writer.strings()[4], fbl::String("null\0null", 9u));
  EXPECT_FSTREQ(writer.strings()[5], fbl::String("too-long-o"));
}

TEST(PseudoFile, WriteUnbuffered) {
  VectorWriter writer(12u);
  auto file = fbl::MakeRefCounted<fs::UnbufferedPseudoFile>(nullptr, writer.GetHandler());

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    CheckWrite(redirect, ZX_OK, 0u, "first", 5u);
    CheckWrite(redirect, ZX_ERR_NO_SPACE, 2u, "xxx", 0u);
    CheckWrite(redirect, ZX_OK, 0u, "second", 6u);
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    CheckWrite(redirect, ZX_OK, 0u, "", 0u);
    CheckAppend(redirect, ZX_OK, "third", 5u, 5u);
    CheckAppend(redirect, ZX_OK, std::string_view("null\0null", 9u), 9u, 9u);
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly().set_truncate());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly().set_create());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    EXPECT_EQ(ZX_OK, redirect->Truncate(0u));
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    CheckAppend(redirect, ZX_OK, "fourth", 6u, 6u);
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    CheckAppend(redirect, ZX_OK, "fifth", 5u, 5u);
    EXPECT_EQ(ZX_ERR_INVALID_ARGS, redirect->Truncate(10u));
    EXPECT_EQ(ZX_OK, redirect->Truncate(0u));
    EXPECT_EQ(ZX_OK, redirect->Close());
  }

  {
    fbl::RefPtr<fs::Vnode> redirect;
    auto result = file->ValidateOptions(VnodeOptions::WriteOnly());
    EXPECT_RESULT_OK(result);
    EXPECT_EQ(ZX_OK, file->Open(result.value(), &redirect));
    CheckWrite(redirect, ZX_OK, 0u, "a long string", 13u);
    EXPECT_EQ(ZX_OK, redirect->Truncate(0u));
    EXPECT_EQ(ZX_ERR_IO, redirect->Close());
  }

  EXPECT_EQ(12u, writer.strings().size());
  EXPECT_FSTREQ(writer.strings()[0], fbl::String("first"));
  EXPECT_FSTREQ(writer.strings()[1], fbl::String("second"));
  EXPECT_FSTREQ(writer.strings()[2], fbl::String(""));
  EXPECT_FSTREQ(writer.strings()[3], fbl::String("third"));
  EXPECT_FSTREQ(writer.strings()[4], fbl::String("null\0null", 9u));
  EXPECT_FSTREQ(writer.strings()[5], fbl::String(""));
  EXPECT_FSTREQ(writer.strings()[6], fbl::String(""));
  EXPECT_FSTREQ(writer.strings()[7], fbl::String(""));
  EXPECT_FSTREQ(writer.strings()[8], fbl::String("fourth"));
  EXPECT_FSTREQ(writer.strings()[9], fbl::String("fifth"));
  EXPECT_FSTREQ(writer.strings()[10], fbl::String(""));
  EXPECT_FSTREQ(writer.strings()[11], fbl::String("a long string"));
}

}  // namespace
