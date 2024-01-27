// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "usage_pixel_format_cost.h"

#include <fidl/fuchsia.sysmem2/cpp/fidl.h>
#include <fidl/fuchsia.sysmem2/cpp/wire.h>
#include <lib/ddk/platform-defs.h>
#include <lib/fidl/cpp/wire/arena.h>
#include <lib/image-format/image_format.h>
#include <zircon/assert.h>

#include <list>
#include <map>

#include "macros.h"
#include "utils.h"

// TODO(fxbug.dev/68491): This platform/board/etc-specific allocation/creation
// policy code belongs in a platform/board/etc-specific binary.

namespace sysmem_driver {

namespace {

// The local Platform definition is different than the PID(s) in platform-defs.h
// in that this local enum includes values that can be used as catch-all for any
// PID in a set of PIDs, typically named with a _GENERIC suffix.
//
// Membership of a PID in a more _GENERIC category is via the next_platform
// field.
//
// Some values of this enum are 1:1 with specific PID values, while others are
// essentially more generic categories (groupings) of PID values.  This allows
// an entry for a more-specific Platform value to effectively share entries of
// a more-generic Platform value.
enum Platform {
  kPlatform_None,
  kPlatform_Generic,
  kPlatform_Arm_Mali,
  kPlatform_Amlogic_Generic,
  kPlatform_Amlogic_S912,
  kPlatform_Amlogic_S905D2,
  kPlatform_Amlogic_T931,
  kPlatform_Amlogic_A311D,
};

constexpr uint64_t MakeVidPidKey(uint32_t vid, uint32_t pid) {
  return (safe_cast<uint64_t>(vid) << 32) | pid;
}

// Map from PID (platform id) to Platform value.
const std::map<uint64_t, Platform> kPlatformTranslation = {
    {MakeVidPidKey(PDEV_VID_AMLOGIC, PDEV_PID_AMLOGIC_S912), kPlatform_Amlogic_S912},
    {MakeVidPidKey(PDEV_VID_AMLOGIC, PDEV_PID_AMLOGIC_S905D2), kPlatform_Amlogic_S905D2},
    {MakeVidPidKey(PDEV_VID_AMLOGIC, PDEV_PID_AMLOGIC_T931), kPlatform_Amlogic_T931},
    {MakeVidPidKey(PDEV_VID_AMLOGIC, PDEV_PID_AMLOGIC_A311D), kPlatform_Amlogic_A311D},
};

// A UsagePixelFormatCostEntry with more query usage bits in
// required_buffer_usage_bits is considered a closer match.
//
// If two UsagePixelFormatCostEntry(s) have equal number of corresponding usage
// bits in required_buffer_usage_bits, the entry occurring first in the overall
// effective list of CostEntry(s) for the PID is preferred.  This causes any
// later-listed otherwise-equally-close-match to be ignored.
struct UsagePixelFormatCostEntry {
  // The query's pixel_format must match for this entry to be considered.
  PixelFormatAndModifier pixel_format_and_modifier;
  // A query's usage bits must contain all these usage bits for this entry to
  // be considered.
  fuchsia_sysmem2::BufferUsage required_buffer_usage_bits;
  // First the entry that's the best match for the GetCost() query is
  // determined.  If this entry is selected as the best match for the query,
  // this is the cost returned by GetCost().
  const double cost;
};

struct PlatformCostsEntry {
  // platform
  const Platform platform;
  // The next_platform can be kPlatform_None in which case the effective
  // overall list is terminated, or next_pid can chain into another Platform
  // enum value which will be considered part of this platform's list.  In
  // this way, more specific Platform values can chain into less-specific
  // platform values.
  const Platform next_platform;

  const std::list<const UsagePixelFormatCostEntry>& costs;
};

static void AddRgbaPixelFormat(fidl::AnyArena& allocator, uint64_t format_modifier, double cost,
                               std::list<const UsagePixelFormatCostEntry>& result) {
  // Both RGBA and BGRA versions have similar cost, if they're supported.
  for (auto format :
       {fuchsia_images2::PixelFormat::kBgra32, fuchsia_images2::PixelFormat::kR8G8B8A8}) {
    fuchsia_images2::PixelFormat pixel_format = format;
    uint64_t pixel_format_modifier = format_modifier;
    fuchsia_sysmem2::BufferUsage buffer_usage;
    buffer_usage.none().emplace(0u);
    buffer_usage.cpu().emplace(0u);
    buffer_usage.vulkan().emplace(0u);
    buffer_usage.display().emplace(0u);
    buffer_usage.video().emplace(0u);
    result.emplace_back(UsagePixelFormatCostEntry{
        .pixel_format_and_modifier = PixelFormatAndModifier(pixel_format, pixel_format_modifier),
        .required_buffer_usage_bits = std::move(buffer_usage),
        .cost = cost,
    });
  }
}

// Since we know exactly how much space we need to avoid using heap, and because this buffer is
// exactly full of stuff that has trivial dtor, we can (marginally) justify avoiding the heap for
// this stuff, since I happen to already know the appropriate size this time.  However, it's a good
// idea to avoid spending the time to update this number each time more entries are added, since the
// opportunity cost of that time will almost certainly be more than any real savings from updating
// this number.
constexpr size_t kAllocatorSize = 3792;
fidl::Arena<kAllocatorSize> allocator;

const std::list<const UsagePixelFormatCostEntry> kArm_Mali_Cost_Entries = [] {
  std::list<const UsagePixelFormatCostEntry> result;
  // Split block is slightly worse than non-split-block for GPU<->GPU, but better for GPU->display.
  constexpr double kSplitCost = 10.0;
  constexpr double kNonYuvCost = 100.0;
  // Tiled headers enable more optimizations and are more efficient, but alignment requirements make
  // them take up more RAM. They're still worthwhile for our usecases.
  constexpr double kNonTiledHeaderCost = 500.0;
  // Formats without sparse set are substantially worse for the GPU than sparse formats.
  constexpr double kNonSparseCost = 1000.0;
  constexpr double kNonTeCost = 2000.0;
  // Non-16X16 can have large advantages for the display, but it's much worse for the GPU.
  constexpr double kNon16X16Cost = 4000.0;
  uint64_t modifiers[] = {
      fuchsia_images2::kFormatModifierArmAfbc16X16SplitBlockSparseYuvTeTiledHeader,
      fuchsia_images2::kFormatModifierArmAfbc16X16Te,
      fuchsia_images2::kFormatModifierArmAfbc32X8Te,
      fuchsia_images2::kFormatModifierArmAfbc16X16SplitBlockSparseYuvTe,
      fuchsia_images2::kFormatModifierArmAfbc16X16SplitBlockSparseYuvTiledHeader,
      fuchsia_images2::kFormatModifierArmAfbc16X16SplitBlockSparseYuv,
      fuchsia_images2::kFormatModifierArmAfbc16X16YuvTiledHeader,
      fuchsia_images2::kFormatModifierArmAfbc16X16,
      fuchsia_images2::kFormatModifierArmAfbc32X8};
  for (auto modifier : modifiers) {
    double cost = 0.0;
    if (!(modifier & fuchsia_images2::kFormatModifierArmYuvBit))
      cost += kNonYuvCost;
    if (!(modifier & fuchsia_images2::kFormatModifierArmTiledHeaderBit))
      cost += kNonTiledHeaderCost;
    if (modifier & fuchsia_images2::kFormatModifierArmTiledHeaderBit)
      cost += kSplitCost;
    if (!(modifier & fuchsia_images2::kFormatModifierArmSparseBit))
      cost += kNonSparseCost;
    if (!(modifier & fuchsia_images2::kFormatModifierArmTeBit))
      cost += kNonTeCost;

    constexpr uint64_t kAfbcTypeMask = 0xf;
    if ((modifier & kAfbcTypeMask) !=
        (fuchsia_images2::kFormatModifierArmAfbc16X16 & kAfbcTypeMask))
      cost += kNon16X16Cost;
    AddRgbaPixelFormat(allocator, modifier, cost, result);
  }
  // Should be higher cost than all AFBC formats.
  AddRgbaPixelFormat(allocator, fuchsia_images2::kFormatModifierArmLinearTe, 30000.0, result);
  return result;
}();

const PlatformCostsEntry kArm_Mali_Costs = {
    .platform = kPlatform_Arm_Mali,
    .next_platform = kPlatform_Generic,
    .costs = kArm_Mali_Cost_Entries,
};

const std::list<const UsagePixelFormatCostEntry> kAmlogic_Generic_Cost_Entries = [] {
  std::list<const UsagePixelFormatCostEntry> result;
  // NV12 weakly preferred for VIDEO_USAGE_HW_DECODER.
  fuchsia_images2::PixelFormat pixel_format;
  pixel_format = fuchsia_images2::PixelFormat::kNv12;
  fuchsia_sysmem2::BufferUsage buffer_usage;
  buffer_usage.none().emplace(0u);
  buffer_usage.cpu().emplace(0u);
  buffer_usage.vulkan().emplace(0u);
  buffer_usage.display().emplace(0u);
  buffer_usage.video().emplace(fuchsia_sysmem2::kVideoUsageHwDecoder);
  result.emplace_back(UsagePixelFormatCostEntry{
      .pixel_format_and_modifier =
          PixelFormatAndModifier(pixel_format, fuchsia_images2::kFormatModifierNone),
      .required_buffer_usage_bits = std::move(buffer_usage),
      .cost = 100.0L,
  });
  return result;
}();

// These costs are expected to be true on every platform.
const std::list<const UsagePixelFormatCostEntry> kGeneric_Cost_Entries = [] {
  std::list<const UsagePixelFormatCostEntry> result;
  AddRgbaPixelFormat(allocator, fuchsia_images2::kFormatModifierIntelI915YfTiledCcs, 500.0, result);
  AddRgbaPixelFormat(allocator, fuchsia_images2::kFormatModifierIntelI915YTiledCcs, 600.0, result);
  AddRgbaPixelFormat(allocator, fuchsia_images2::kFormatModifierIntelI915YfTiled, 1000.0, result);
  AddRgbaPixelFormat(allocator, fuchsia_images2::kFormatModifierIntelI915YTiled, 2000.0, result);
  AddRgbaPixelFormat(allocator, fuchsia_images2::kFormatModifierIntelI915XTiled, 3000.0, result);
  // LOG(INFO, "usage_pixel_format_cost.cc - allocator.debug_needed_buffer_size(): %zu",
  //    allocator.inner_allocator().debug_needed_buffer_size());
  return result;
}();

const PlatformCostsEntry kAmlogic_Generic_Costs = {
    .platform = kPlatform_Amlogic_Generic,
    .next_platform = kPlatform_Arm_Mali,
    .costs = kAmlogic_Generic_Cost_Entries,
};

// kAmlogic_S912_Cost_Entries will go here.

const PlatformCostsEntry kAmlogic_S912_Costs = {
    .platform = kPlatform_Amlogic_S912,
    .next_platform = kPlatform_Amlogic_Generic,
    .costs = std::list<const UsagePixelFormatCostEntry>(),
};

// kAmlogic_S905D2_Cost_Entries will go here.

const PlatformCostsEntry kAmlogic_S905D2_Costs = {
    .platform = kPlatform_Amlogic_S905D2,
    .next_platform = kPlatform_Amlogic_Generic,
    .costs = std::list<const UsagePixelFormatCostEntry>(),
};

// kAmlogic_T931_Cost_Entries will go here.

const PlatformCostsEntry kAmlogic_T931_Costs = {
    .platform = kPlatform_Amlogic_T931,
    .next_platform = kPlatform_Amlogic_Generic,
    .costs = std::list<const UsagePixelFormatCostEntry>(),
};

// kAmlogic_A311D_Cost_Entries will go here.

const PlatformCostsEntry kAmlogic_A311D_Costs = {
    .platform = kPlatform_Amlogic_A311D,
    .next_platform = kPlatform_Amlogic_Generic,
    .costs = std::list<const UsagePixelFormatCostEntry>(),
};

const PlatformCostsEntry kGeneric_Costs = {
    .platform = kPlatform_Generic,
    .next_platform = kPlatform_None,
    .costs = kGeneric_Cost_Entries,
};

const std::map<Platform, const PlatformCostsEntry*> kPlatformCosts = {
    {kPlatform_Generic, &kGeneric_Costs},
    {kPlatform_Arm_Mali, &kArm_Mali_Costs},
    {kPlatform_Amlogic_Generic, &kAmlogic_Generic_Costs},
    {kPlatform_Amlogic_S912, &kAmlogic_S912_Costs},
    {kPlatform_Amlogic_S905D2, &kAmlogic_S905D2_Costs},
    {kPlatform_Amlogic_T931, &kAmlogic_T931_Costs},
    {kPlatform_Amlogic_A311D, &kAmlogic_A311D_Costs},
};

const double kDefaultCost = std::numeric_limits<double>::max();

Platform FindPlatform(uint32_t pdev_device_info_vid, uint32_t pdev_device_info_pid) {
  auto iter = kPlatformTranslation.find(MakeVidPidKey(pdev_device_info_vid, pdev_device_info_pid));
  if (iter == kPlatformTranslation.end()) {
    return kPlatform_Generic;
  }
  return iter->second;
}

const PlatformCostsEntry* FindPlatformCosts(Platform platform) {
  if (platform == kPlatform_None) {
    return nullptr;
  }
  auto iter = kPlatformCosts.find(platform);
  if (iter == kPlatformCosts.end()) {
    return nullptr;
  }
  return iter->second;
}

// |a| to check
// |r| required bits
bool HasAllRequiredBits(uint32_t a, uint32_t r) { return (r & a) == r; }

// |a| to check
// |r| required bits
bool HasAllRequiredUsageBits(const fuchsia_sysmem2::BufferUsage& a,
                             const fuchsia_sysmem2::BufferUsage& r) {
  const uint32_t a_cpu = a.cpu().has_value() ? a.cpu().value() : 0;
  const uint32_t a_vulkan = a.vulkan().has_value() ? a.vulkan().value() : 0;
  const uint32_t a_display = a.display().has_value() ? a.display().value() : 0;
  const uint32_t a_video = a.video().has_value() ? a.video().value() : 0;
  const uint32_t r_cpu = r.cpu().has_value() ? r.cpu().value() : 0;
  const uint32_t r_vulkan = r.vulkan().has_value() ? r.vulkan().value() : 0;
  const uint32_t r_display = r.display().has_value() ? r.display().value() : 0;
  const uint32_t r_video = r.video().has_value() ? r.video().value() : 0;
  return HasAllRequiredBits(a_cpu, r_cpu) && HasAllRequiredBits(a_vulkan, r_vulkan) &&
         HasAllRequiredBits(a_display, r_display) && HasAllRequiredBits(a_video, r_video);
}

uint32_t SharedBitsCount(uint32_t a, uint32_t b) {
  uint32_t set_in_both = a & b;

  // TODO(dustingreen): Consider using popcount intrinsic (or equivalent).
  uint32_t count = 0;
  for (uint32_t i = 0; i < sizeof(uint32_t) * 8; ++i) {
    if (set_in_both & (1 << i)) {
      ++count;
    }
  }

  return count;
}

uint32_t SharedUsageBitsCount(const fuchsia_sysmem2::BufferUsage& a,
                              const fuchsia_sysmem2::BufferUsage& b) {
  const uint32_t a_cpu = a.cpu().has_value() ? a.cpu().value() : 0;
  const uint32_t a_vulkan = a.vulkan().has_value() ? a.vulkan().value() : 0;
  const uint32_t a_display = a.display().has_value() ? a.display().value() : 0;
  const uint32_t a_video = a.video().has_value() ? a.video().value() : 0;
  const uint32_t b_cpu = b.cpu().has_value() ? b.cpu().value() : 0;
  const uint32_t b_vulkan = b.vulkan().has_value() ? b.vulkan().value() : 0;
  const uint32_t b_display = b.display().has_value() ? b.display().value() : 0;
  const uint32_t b_video = b.video().has_value() ? b.video().value() : 0;
  return SharedBitsCount(a_cpu, b_cpu) + SharedBitsCount(a_vulkan, b_vulkan) +
         SharedBitsCount(a_display, b_display) + SharedBitsCount(a_video, b_video);
}

// This comparison has nothing to do with the cost of a or cost of b.  This is
// only about finding the best-match UsagePixelFormatCostEntry for the given
// query.
//
// |constraints| the query's constraints
//
// |image_format_constraints_index| the query's image_format_constraints_index
//
// |a| the new UsagePixelFormatCostEntry to consider
//
// |b| the existing UsagePixelFormatCostEntry that a is being compared against
bool IsBetterMatch(const fuchsia_sysmem2::BufferCollectionConstraints& constraints,
                   uint32_t image_format_constraints_index, const UsagePixelFormatCostEntry* a,
                   const UsagePixelFormatCostEntry* b) {
  ZX_DEBUG_ASSERT(a);
  ZX_DEBUG_ASSERT(image_format_constraints_index < constraints.image_format_constraints()->size());
  // We intentionally allow b to be nullptr.

  if (!ImageFormatIsPixelFormatEqual(
          a->pixel_format_and_modifier,
          PixelFormatAndModifierFromConstraints(
              constraints.image_format_constraints()->at(image_format_constraints_index)))) {
    return false;
  }

  fuchsia_sysmem2::BufferUsage default_usage;
  const fuchsia_sysmem2::BufferUsage* usage_ptr;
  if (constraints.usage().has_value()) {
    usage_ptr = &constraints.usage().value();
  } else {
    usage_ptr = &default_usage;
  }
  const fuchsia_sysmem2::BufferUsage& usage = *usage_ptr;
  if (!HasAllRequiredUsageBits(usage, a->required_buffer_usage_bits)) {
    return false;
  }
  ZX_DEBUG_ASSERT(HasAllRequiredUsageBits(usage, a->required_buffer_usage_bits));
  // We intentionally allow b to be nullptr.
  if (b == nullptr) {
    return true;
  }
  ZX_DEBUG_ASSERT(HasAllRequiredUsageBits(usage, b->required_buffer_usage_bits));
  uint32_t a_shared_bits = SharedUsageBitsCount(usage, a->required_buffer_usage_bits);
  uint32_t b_shared_bits = SharedUsageBitsCount(usage, b->required_buffer_usage_bits);
  return a_shared_bits > b_shared_bits;
}

double GetCostInternal(const fuchsia_sysmem2::BufferCollectionConstraints& constraints,
                       uint32_t image_format_constraints_index, Platform platform) {
  const PlatformCostsEntry* platform_costs = FindPlatformCosts(platform);
  if (!platform_costs) {
    return kDefaultCost;
  }
  const UsagePixelFormatCostEntry* best_match = nullptr;
  while (platform_costs) {
    for (const UsagePixelFormatCostEntry& cost : platform_costs->costs) {
      if (IsBetterMatch(constraints, image_format_constraints_index, &cost, best_match)) {
        best_match = &cost;
      }
    }
    platform_costs = FindPlatformCosts(platform_costs->next_platform);
  }
  if (!best_match) {
    return kDefaultCost;
  }
  ZX_DEBUG_ASSERT(best_match);
  return best_match->cost;
}

double GetCost(uint32_t pdev_device_info_vid, uint32_t pdev_device_info_pid,
               const fuchsia_sysmem2::BufferCollectionConstraints& constraints,
               uint32_t image_format_constraints_index) {
  Platform platform = FindPlatform(pdev_device_info_vid, pdev_device_info_pid);
  if (platform == kPlatform_None) {
    return kDefaultCost;
  }
  return GetCostInternal(constraints, image_format_constraints_index, platform);
}

}  // namespace

int32_t UsagePixelFormatCost::Compare(
    uint32_t pdev_device_info_vid, uint32_t pdev_device_info_pid,
    const fuchsia_sysmem2::BufferCollectionConstraints& constraints,
    uint32_t image_format_constraints_index_a, uint32_t image_format_constraints_index_b) {
  double cost_a = GetCost(pdev_device_info_vid, pdev_device_info_pid, constraints,
                          image_format_constraints_index_a);
  double cost_b = GetCost(pdev_device_info_vid, pdev_device_info_pid, constraints,
                          image_format_constraints_index_b);

  if (cost_a < cost_b) {
    return -1;
  } else if (cost_a > cost_b) {
    return 1;
  } else {
    return 0;
  }
}

}  // namespace sysmem_driver
