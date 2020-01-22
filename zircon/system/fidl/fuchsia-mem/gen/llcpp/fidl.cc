// WARNING: This file is machine generated by fidlgen.

#include <fuchsia/mem/llcpp/fidl.h>
#include <memory>

namespace llcpp {

namespace fuchsia {
namespace mem {
auto ::llcpp::fuchsia::mem::Data::which() const -> Tag {
  ZX_ASSERT(!has_invalid_tag());
  switch (ordinal()) {
  case Ordinal::kBytes:
  case Ordinal::kBuffer:
    return static_cast<Tag>(ordinal());
  default:
    return Tag::kUnknown;
  }
}

void ::llcpp::fuchsia::mem::Data::SizeAndOffsetAssertionHelper() {
  static_assert(sizeof(Data) == sizeof(fidl_xunion_t));
  static_assert(offsetof(Data, ordinal_) == offsetof(fidl_xunion_t, tag));
  static_assert(offsetof(Data, envelope_) == offsetof(fidl_xunion_t, envelope));
}

}  // namespace mem
}  // namespace fuchsia
}  // namespace llcpp
