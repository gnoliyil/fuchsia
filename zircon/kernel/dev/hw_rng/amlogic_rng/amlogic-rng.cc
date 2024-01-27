// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/zbi-format/driver-config.h>
#include <reg.h>

#include <arch/arm64/periphmap.h>
#include <dev/hw_rng.h>
#include <dev/hw_rng/amlogic_rng/init.h>
#include <explicit-memory/bytes.h>
#include <fbl/algorithm.h>
#include <kernel/thread.h>
#include <ktl/algorithm.h>

#include <ktl/enforce.h>

// Mask for the bit indicating RNG status.
#define AML_RNG_READY 1

namespace {

// Register for RNG data
static vaddr_t rng_data = 0;
// Register whose 1st bit indicates RNG status: 1->ready, 0->not ready.
static vaddr_t rng_status = 0;
// Hardware RNG refresh time in microsecond.
static uint64_t rng_refresh_interval_usec = 0;
// Hardware RNG Version.
static ZbiAmlogicRng::Version rng_version = ZbiAmlogicRng::Version::kV1;

// Size of each RNG draw.
constexpr size_t kRngDrawSize = 4;
// Max number of retry
constexpr size_t kMaxRetry = 10000;

bool rng_v1_is_ready() { return (readl(rng_status) & AML_RNG_READY) == 1; }

bool rng_v2_is_ready() {
  constexpr size_t kTimeoutCount = 100;

  // 1. Send a request.
  uint32_t status = readl(rng_status);
  writel(status | (1 << 31), rng_status);

  // 2. Check whether the request has responded
  do {
    size_t count = 0;
    status = readl(rng_status) & (1 << 31);
    if (count++ >= kTimeoutCount) {
      return false;
    }
  } while (status != 0);

  // 3. check whether the random seed has returned.
  do {
    size_t count = 0;
    status = readl(rng_status) & (1 << 0);
    if (count++ >= kTimeoutCount) {
      return false;
    }
  } while (status != 0);

  return true;
}

bool rng_is_ready() {
  switch (rng_version) {
    case ZbiAmlogicRng::Version::kV1:
      return rng_v1_is_ready();
      break;
    case ZbiAmlogicRng::Version::kV2:
      return rng_v2_is_ready();
      break;
  }

  return false;
}

size_t amlogic_hw_rng_get_entropy(void* buf, size_t len) {
  if (buf == nullptr) {
    return 0;
  }

  char* dest = static_cast<char*>(buf);
  size_t total_read = 0;
  size_t retry = 0;

  while (len > 0) {
    // Retry until RNG is ready.
    while (!rng_is_ready()) {
      if (retry > kMaxRetry) {
        mandatory_memset(&buf, 0, len);
        return 0;
      }

      Thread::Current::SleepRelative(ZX_USEC(1));
      retry++;
    }

    uint32_t read_buf = readl(rng_data);
    static_assert(sizeof(read_buf) == kRngDrawSize);

    size_t read_size = ktl::min(len, kRngDrawSize);
    memcpy(dest + total_read, &read_buf, read_size);
    mandatory_memset(&read_buf, 0, sizeof(read_buf));

    total_read += read_size;
    len -= read_size;

    // Hardware RNG expected to be ready after an interval.
    Thread::Current::SleepRelative(ZX_USEC(rng_refresh_interval_usec));
  }
  return total_read;
}

static struct hw_rng_ops ops = {
    .hw_rng_get_entropy = amlogic_hw_rng_get_entropy,
};

}  // namespace

void AmlogicRngInit(const ZbiAmlogicRng& info) {
  ASSERT(info.config.rng_data_phys);
  ASSERT(info.config.rng_status_phys);

  rng_data = periph_paddr_to_vaddr(info.config.rng_data_phys);
  rng_status = periph_paddr_to_vaddr(info.config.rng_status_phys);
  rng_refresh_interval_usec = info.config.rng_refresh_interval_usec;
  rng_version = info.version;

  ASSERT(rng_data);
  ASSERT(rng_status);
  ASSERT(rng_refresh_interval_usec > 0);

  hw_rng_register(&ops);
}
