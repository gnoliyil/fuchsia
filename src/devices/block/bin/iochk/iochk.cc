// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <fidl/fuchsia.hardware.block/cpp/wire.h>
#include <fidl/fuchsia.hardware.skipblock/cpp/wire.h>
#include <fuchsia/hardware/block/driver/c/banjo.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fzl/owned-vmo-mapper.h>
#include <lib/zircon-internal/xorshiftrand.h>
#include <lib/zx/fifo.h>
#include <lib/zx/thread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zircon/assert.h>
#include <zircon/process.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/threads.h>
#include <zircon/types.h>

#include <atomic>
#include <memory>
#include <utility>

#include <fbl/auto_lock.h>
#include <fbl/macros.h>
#include <fbl/mutex.h>
#include <fbl/unique_fd.h>

#include "src/lib/storage/block_client/cpp/client.h"

namespace {

constexpr char kUsageMessage[] = R"""(
usage: iochk [OPTIONS] <device>

    -bs block_size - number of bytes to treat as a unit (default=device block size)
    -t thread# - the number of threads to run (default=1)
    -c block_count - number of blocks to read (default=the whole device)
    -o offset - block-size offset to start reading from (default=0)
    -s seed - the seed to use for pseudorandom testing
    --live-dangerously - skip confirmation prompt
    --skip - verify skip-block interface instead of block interface
)""";

constexpr uint64_t kBlockHeader = 0xdeadbeef;

// Flags.
bool skip = false;
uint32_t start_block = 0;
size_t block_size = 0;
uint32_t block_count = 0;

// Constant after init.
uint64_t base_seed;

// Not thread safe.
class ProgressBar {
 public:
  explicit ProgressBar() : total_work_(0) {}
  explicit ProgressBar(uint32_t block_count, size_t num_threads)
      : total_work_(
            static_cast<uint32_t>(static_cast<int>(block_count * log(block_count)) * num_threads)) {
  }

  ProgressBar(const ProgressBar& other) = default;
  ProgressBar& operator=(const ProgressBar& other) = default;

  void Update(uint32_t was_read) {
    int old_progress = static_cast<int>(100 * blocks_read_ / total_work_);
    blocks_read_ += was_read;
    int progress = static_cast<int>(100 * blocks_read_ / total_work_);

    if (old_progress != progress) {
      int ticks = 40;
      char str[ticks + 1];
      memset(str, ' ', ticks);
      memset(str, '=', ticks * progress / 100);
      str[ticks] = '\0';
      printf("\r[%s] %02d%%", str, progress);
      fflush(stdout);
    }
    if (progress == 100) {
      printf("\n");
    }
  }

 private:
  uint32_t total_work_;
  uint32_t blocks_read_ = 0;
};

// Context for thread workers.
class WorkContext {
 public:
  explicit WorkContext(ProgressBar progress, bool okay) : progress(progress), okay_(okay) {}
  ~WorkContext() = default;

  DISALLOW_COPY_ASSIGN_AND_MOVE(WorkContext);

  // Implementation specific information.
  struct {
    std::unique_ptr<block_client::Client> client;
    fuchsia_hardware_block::wire::BlockInfo info = {};
  } block;
  struct {
    fuchsia_hardware_skipblock::wire::PartitionInfo info = {};
  } skip;
  // Connection to device being tested.
  fdio_cpp::FdioCaller caller;
  // Protects |iochk_failure| and |progress|
  fbl::Mutex lock;
  bool iochk_failure = false;
  ProgressBar progress;
  bool okay_;
};

// Interface to abstract over block/skip-block device interface differences.
class Checker {
 public:
  // Fills the device with data based on location in the block.
  virtual zx_status_t Fill(uint32_t start, uint32_t count) { return ZX_ERR_NOT_SUPPORTED; }

  // Validates that data in specified was region on device is what was written
  // by Fill.
  virtual zx_status_t Check(uint32_t start, uint32_t count) { return ZX_ERR_NOT_SUPPORTED; }

  virtual ~Checker() = default;
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Checker);

 protected:
  explicit Checker(void* buffer) : buffer_(buffer) {}

  void GenerateBlockData(int block_idx, size_t length) const {
    // Block size should be a multiple of sizeof(uint64_t), but assert just to be safe
    ZX_ASSERT(length % sizeof(uint64_t) == 0);

    rand64_t seed_gen = RAND63SEED(base_seed + block_idx);
    for (int i = 0; i < 10; i++) {
      rand64(&seed_gen);
    }
    rand64_t data_gen = RAND63SEED(rand64(&seed_gen));

    auto* buf = static_cast<uint64_t*>(buffer_);
    size_t idx = 0;
    uint64_t data = kBlockHeader | (static_cast<uint64_t>(block_idx) << 32);

    while (idx < length / sizeof(uint64_t)) {
      buf[idx] = data;
      data = rand64(&data_gen);
      idx++;
    }
  }

  int CheckBlockData(int block_idx, size_t length) const {
    rand64_t seed_gen = RAND63SEED(base_seed + block_idx);
    for (int i = 0; i < 10; i++) {
      rand64(&seed_gen);
    }
    rand64_t data_gen = RAND63SEED(rand64(&seed_gen));

    auto* buf = static_cast<uint64_t*>(buffer_);
    uint64_t expected = kBlockHeader | (static_cast<uint64_t>(block_idx) << 32);
    size_t idx = 0;

    while (idx < length / sizeof(uint64_t)) {
      if (buf[idx] != expected) {
        printf(
            "initial read verification failed: "
            "block_idx=%d offset=%zu expected=0x%016lx val=0x%016lx\n",
            block_idx, idx, expected, buf[idx]);
        return ZX_ERR_INTERNAL;
      }
      idx++;
      expected = rand64(&data_gen);
    }
    return 0;
  }

  void* buffer_;
};

class BlockChecker : public Checker {
 public:
  static zx_status_t Initialize(fdio_cpp::FdioCaller& caller,
                                fuchsia_hardware_block::wire::BlockInfo info,
                                block_client::Client& client, std::unique_ptr<Checker>* checker) {
    fzl::OwnedVmoMapper mapping;
    if (zx_status_t status = mapping.CreateAndMap(block_size, ""); status != ZX_OK) {
      printf("Failed to CreateAndMap Vmo: %s\n", zx_status_get_string(status));
      return status;
    }

    zx::result vmoid = client.RegisterVmo(mapping.vmo());
    if (vmoid.is_error()) {
      printf("Failed to RegisterVmo: %s\n", vmoid.status_string());
      return vmoid.error_value();
    }

    groupid_t group = next_txid_.fetch_add(1);
    ZX_ASSERT(group < MAX_TXN_GROUP_COUNT);

    checker->reset(
        new BlockChecker(std::move(mapping), info, client, std::move(vmoid.value()), group));
    return ZX_OK;
  }

  static void ResetAtomic() { next_txid_.store(0); }

  zx_status_t Fill(uint32_t start, uint32_t count) override {
    for (uint32_t block_idx = start; block_idx < count; block_idx++) {
      uint64_t length = (info_.block_size * info_.block_count) - (block_idx * block_size);
      if (length > block_size) {
        length = block_size;
      }

      GenerateBlockData(block_idx, block_size);
      block_fifo_request_t request = {
          .opcode = BLOCK_OP_WRITE,
          .reqid = 0,
          .group = group_,
          .vmoid = vmoid_,
          .length = static_cast<uint32_t>(length / info_.block_size),
          .vmo_offset = 0,
          .dev_offset = (block_idx * block_size) / info_.block_size,
      };
      if (zx_status_t status = client_.Transaction(&request, 1); status != ZX_OK) {
        printf("write block_fifo_txn error %s\n", zx_status_get_string(status));
        return status;
      }
    }
    return ZX_OK;
  }

  zx_status_t Check(uint32_t start, uint32_t count) override {
    for (uint32_t block_idx = start; block_idx < count; block_idx++) {
      uint64_t length = (info_.block_size * info_.block_count) - (block_idx * block_size);
      if (length > block_size) {
        length = block_size;
      }

      block_fifo_request_t request = {
          .opcode = BLOCK_OP_READ,
          .reqid = 0,
          .group = group_,
          .vmoid = vmoid_,
          .length = static_cast<uint32_t>(length / info_.block_size),
          .vmo_offset = 0,
          .dev_offset = (block_idx * block_size) / info_.block_size,
      };
      if (zx_status_t status = client_.Transaction(&request, 1); status != ZX_OK) {
        printf("read block_fifo_txn error %s\n", zx_status_get_string(status));
        return status;
      }
      if (zx_status_t status = CheckBlockData(block_idx, length); status != ZX_OK) {
        return status;
      }
    }
    return ZX_OK;
  }

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(BlockChecker);

 private:
  BlockChecker(fzl::OwnedVmoMapper mapper, fuchsia_hardware_block::wire::BlockInfo info,
               block_client::Client& client, storage::Vmoid vmoid, groupid_t group)
      : Checker(mapper.start()),
        mapper_(std::move(mapper)),
        info_(info),
        client_(client),
        vmoid_(vmoid.TakeId()),
        group_(group) {}
  ~BlockChecker() override = default;

  static std::atomic<uint16_t> next_txid_;

  fzl::OwnedVmoMapper mapper_;
  fuchsia_hardware_block::wire::BlockInfo info_;
  block_client::Client& client_;
  vmoid_t vmoid_;
  groupid_t group_;
};

std::atomic<uint16_t> BlockChecker::next_txid_;

class SkipBlockChecker : public Checker {
 public:
  static zx_status_t Initialize(fdio_cpp::FdioCaller& caller,
                                fuchsia_hardware_skipblock::wire::PartitionInfo info,
                                std::unique_ptr<Checker>* checker) {
    fzl::VmoMapper mapping;
    zx::vmo vmo;
    if (zx_status_t status =
            mapping.CreateAndMap(block_size, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, nullptr, &vmo);
        status != ZX_OK) {
      printf("Failled to CreateAndMap Vmo\n");
      return status;
    }

    checker->reset(new SkipBlockChecker(std::move(mapping), std::move(vmo), caller, info));
    return ZX_OK;
  }

  zx_status_t Fill(uint32_t start, uint32_t count) override {
    for (uint32_t block_idx = start; block_idx < count; block_idx++) {
      uint64_t length =
          (info_.block_size_bytes * info_.partition_block_count) - (block_idx * block_size);
      if (length > block_size) {
        length = block_size;
      }

      zx::vmo dup;
      if (zx_status_t status = vmo_.duplicate(ZX_RIGHT_SAME_RIGHTS, &dup); status != ZX_OK) {
        printf("cannot duplicate handle: %s\n", zx_status_get_string(status));
        return status;
      }

      GenerateBlockData(block_idx, block_size);
      const fidl::WireResult result =
          fidl::WireCall(caller_.borrow_as<fuchsia_hardware_skipblock::SkipBlock>())
              ->Write({
                  .vmo = std::move(dup),
                  .vmo_offset = 0,
                  .block = static_cast<uint32_t>((block_idx * block_size) / info_.block_size_bytes),
                  .block_count = static_cast<uint32_t>(length / info_.block_size_bytes),
              });
      if (!result.ok()) {
        printf("SkipBlockWrite error %s\n", result.FormatDescription().c_str());
        return result.status();
      }
      const fidl::WireResponse response = result.value();
      if (zx_status_t status = response.status; status != ZX_OK) {
        printf("SkipBlockWrite error %s\n", zx_status_get_string(status));
        return status;
      }
    }
    return ZX_OK;
  }

  zx_status_t Check(uint32_t start, uint32_t count) override {
    for (uint32_t block_idx = start; block_idx < count; block_idx++) {
      uint64_t length =
          (info_.block_size_bytes * info_.partition_block_count) - (block_idx * block_size);
      if (length > block_size) {
        length = block_size;
      }

      zx::vmo dup;
      if (zx_status_t status = vmo_.duplicate(ZX_RIGHT_SAME_RIGHTS, &dup); status != ZX_OK) {
        printf("cannot duplicate handle: %s\n", zx_status_get_string(status));
        return status;
      }

      const fidl::WireResult result =
          fidl::WireCall(caller_.borrow_as<fuchsia_hardware_skipblock::SkipBlock>())
              ->Read({
                  .vmo = std::move(dup),
                  .vmo_offset = 0,
                  .block = static_cast<uint32_t>((block_idx * block_size) / info_.block_size_bytes),
                  .block_count = static_cast<uint32_t>(length / info_.block_size_bytes),
              });
      if (!result.ok()) {
        printf("SkipBlockRead error %s\n", result.FormatDescription().c_str());
        return result.status();
      }
      const fidl::WireResponse response = result.value();
      if (zx_status_t status = response.status; status != ZX_OK) {
        printf("SkipBlockRead error %s\n", zx_status_get_string(status));
        return status;
      }

      if (zx_status_t status = CheckBlockData(block_idx, length); status != ZX_OK) {
        return status;
      }
    }
    return ZX_OK;
  }

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(SkipBlockChecker);

 private:
  SkipBlockChecker(fzl::VmoMapper mapper, zx::vmo vmo, fdio_cpp::FdioCaller& caller,
                   fuchsia_hardware_skipblock::wire::PartitionInfo info)
      : Checker(mapper.start()),
        mapper_(std::move(mapper)),
        vmo_(std::move(vmo)),
        caller_(caller),
        info_(info) {}
  ~SkipBlockChecker() override = default;

  fzl::VmoMapper mapper_;
  zx::vmo vmo_;
  fdio_cpp::FdioCaller& caller_;
  fuchsia_hardware_skipblock::wire::PartitionInfo info_;
};

zx_status_t InitializeChecker(WorkContext& ctx, std::unique_ptr<Checker>* checker) {
  return skip ? SkipBlockChecker::Initialize(ctx.caller, ctx.skip.info, checker)
              : BlockChecker::Initialize(ctx.caller, ctx.block.info, *ctx.block.client, checker);
}

zx_status_t InitializeDevice(WorkContext& ctx) {
  std::unique_ptr<Checker> checker;
  if (zx_status_t status = InitializeChecker(ctx, &checker); status != ZX_OK) {
    printf("Failed to alloc resources to init device: %s\n", zx_status_get_string(status));
    return status;
  }

  printf("writing test data to device...\n");
  fflush(stdout);
  if (zx_status_t status = checker->Fill(start_block, block_count); status != ZX_OK) {
    printf("failed to write test data: %s\n", zx_status_get_string(status));
    return status;
  }
  printf("done\n");

  printf("verifying test data...\n");
  fflush(stdout);
  if (zx_status_t status = checker->Check(start_block, block_count); status != ZX_OK) {
    printf("failed to verify test data: %s\n", zx_status_get_string(status));
    return status;
  }
  printf("done\n");

  return 0;
}

int DoWork(void* arg) {
  auto* ctx = static_cast<WorkContext*>(arg);

  std::unique_ptr<Checker> checker;
  if (zx_status_t status = InitializeChecker(*ctx, &checker); status != ZX_OK) {
    printf("Failed to alloc resources to init device: %s\n", zx_status_get_string(status));
    return status;
  }

  auto tid = static_cast<uintptr_t>(zx::thread::self()->get());
  rand32_t seed_gen = RAND32SEED(static_cast<uint32_t>(base_seed + tid));
  for (int i = 0; i < 20; i++) {
  }
  rand32_t work_gen = RAND32SEED(rand32(&seed_gen));
  // The expected number of random pages we need to hit all of them is
  // approx n*log(n) (the coupon collector problem)
  uint32_t blocks_left = static_cast<uint32_t>(block_count * log(block_count));

  while (blocks_left > 0 && !ctx->iochk_failure) {
    uint32_t to_read = (rand32(&work_gen) % blocks_left) + 1;
    uint32_t work_offset = rand32(&work_gen) % block_count;
    if (work_offset + to_read > block_count) {
      to_read = block_count - work_offset;
    }

    zx_status_t status;
    if (rand32(&work_gen) % 2) {
      status = checker->Check(start_block + work_offset, to_read);
    } else {
      status = checker->Fill(start_block + work_offset, to_read);
    }

    fbl::AutoLock al(&ctx->lock);
    if (status != ZX_OK) {
      ctx->iochk_failure = true;
    } else if (!ctx->iochk_failure) {
      ctx->progress.Update(to_read);
      blocks_left -= to_read;
    }
  }

  return 0;
}

uint64_t Number(const char* str) {
  char* end;
  uint64_t n = strtoull(str, &end, 10);

  uint64_t m = 1;
  switch (*end) {
    case 'G':
    case 'g':
      m = 1024 * 1024 * 1024;
      break;
    case 'M':
    case 'm':
      m = 1024 * 1024;
      break;
    case 'K':
    case 'k':
      m = 1024;
      break;
  }
  return m * n;
}

int Usage() {
  printf("%s\n", kUsageMessage);
  return -1;
}

}  // namespace

int iochk(int argc, char** argv) {
  const char* device = argv[argc - 1];
  fbl::unique_fd fd;
  if (zx_status_t status = fdio_open_fd(device, 0, fd.reset_and_get_address()); status != ZX_OK) {
    printf("cannot open '%s': %s\n", device, zx_status_get_string(status));
    return -1;
  }

  bool seed_set = false;
  size_t num_threads = 1;
  bool confirmed = false;
  char** end = argv + argc - 1;
  argv++;
  while (argv < end) {
    if (strcmp(*argv, "-t") == 0) {
      num_threads = atoi(argv[1]);
      argv += 2;
    } else if (strcmp(*argv, "-c") == 0) {
      block_count = atoi(argv[1]);
      argv += 2;
    } else if (strcmp(*argv, "-o") == 0) {
      start_block = atoi(argv[1]);
      argv += 2;
    } else if (strcmp(*argv, "-bs") == 0) {
      block_size = Number(argv[1]);
      argv += 2;
    } else if (strcmp(*argv, "-s") == 0) {
      base_seed = atoll(argv[1]);
      seed_set = true;
      argv += 2;
    } else if (strcmp(*argv, "--live-dangerously") == 0) {
      confirmed = true;
      argv++;
    } else if (strcmp(*argv, "--skip") == 0) {
      skip = true;
      argv++;
    } else if (strcmp(*argv, "-h") == 0 || strcmp(*argv, "--help") == 0) {
      return Usage();
    } else {
      printf("Invalid arg %s\n", *argv);
      return Usage();
    }
  }

  if (!confirmed) {
    constexpr char kWarning[] = "\033[0;31mWARNING\033[0m";
    printf("%s: iochk is a destructive operation.\n", kWarning);
    printf("%s: All data on %s in the given range will be overwritten.\n", kWarning, device);
    printf("%s: Type 'y' to continue, 'n' or ESC to cancel:\n", kWarning);
    for (;;) {
      char c;
      ssize_t r = read(STDIN_FILENO, &c, 1);
      if (r < 0) {
        printf("Error reading from stdin\n");
        return -1;
      }
      if (c == 'y' || c == 'Y') {
        break;
      }
      if (c == 'n' || c == 'N' || c == 27) {
        return 0;
      }
    }
  }

  if (!seed_set) {
    base_seed = zx_clock_get_monotonic();
  }
  printf("seed is %ld\n", base_seed);

  WorkContext ctx(ProgressBar(), false);

  ctx.caller.reset(std::move(fd));
  if (skip) {
    // Skip Block Device Setup.
    const fidl::WireResult result =
        fidl::WireCall(ctx.caller.borrow_as<fuchsia_hardware_skipblock::SkipBlock>())
            ->GetPartitionInfo();
    if (!result.ok()) {
      printf("unable to get skip-block partition info: %s\n", result.FormatDescription().c_str());
      printf("fd: %d\n", ctx.caller.release().get());
      return -1;
    }
    const fidl::WireResponse response = result.value();
    if (zx_status_t status = response.status; status != ZX_OK) {
      printf("unable to get skip-block partition info: %s\n", zx_status_get_string(status));
      printf("fd: %d\n", ctx.caller.release().get());
      return -1;
    }
    const fuchsia_hardware_skipblock::wire::PartitionInfo& info = response.partition_info;
    printf("opened %s - block_size_bytes=%lu, partition_block_count=%u\n", device,
           info.block_size_bytes, info.partition_block_count);

    ctx.skip.info = info;

    if (block_size == 0) {
      block_size = info.block_size_bytes;
    } else if (block_size % info.block_size_bytes != 0) {
      printf("block-size is not a multiple of device block size\n");
      return -1;
    }
    uint32_t dev_blocks_per_block = static_cast<uint32_t>(block_size / info.block_size_bytes);

    if (dev_blocks_per_block * start_block >= info.partition_block_count) {
      printf("offset past end of device\n");
      return -1;
    }

    if (block_count == 0) {
      block_count = static_cast<uint32_t>((info.partition_block_count + dev_blocks_per_block - 1) /
                                          dev_blocks_per_block);
    } else if (dev_blocks_per_block * (block_count + start_block) >=
               dev_blocks_per_block + info.partition_block_count) {
      // Don't allow blocks to start past the end of the device
      printf("block_count+offset too large\n");
      return -1;
    }
  } else {
    // Block Device Setup.
    const fidl::WireResult result =
        fidl::WireCall(ctx.caller.borrow_as<fuchsia_hardware_block::Block>())->GetInfo();
    if (!result.ok()) {
      printf("unable to get block info: %s\n", result.FormatDescription().c_str());
      printf("fd: %d\n", ctx.caller.release().get());
      return -1;
    }
    const fit::result response = result.value();
    if (response.is_error()) {
      printf("unable to get block info: %s\n", zx_status_get_string(response.error_value()));
      printf("fd: %d\n", ctx.caller.release().get());
      return -1;
    }
    const fuchsia_hardware_block::wire::BlockInfo& info = response.value()->info;
    printf("opened %s - block_size=%u, block_count=%lu\n", device, info.block_size,
           info.block_count);

    ctx.block.info = info;

    if (block_size == 0) {
      block_size = static_cast<uint32_t>(info.block_size);
    } else if (block_size % info.block_size != 0) {
      printf("block-size is not a multiple of device block size\n");
      return -1;
    }
    uint32_t dev_blocks_per_block = static_cast<uint32_t>(block_size / info.block_size);

    if (dev_blocks_per_block * start_block >= info.block_count) {
      printf("offset past end of device\n");
      return -1;
    }

    if (block_count == 0) {
      block_count = static_cast<uint32_t>((info.block_count + dev_blocks_per_block - 1) /
                                          dev_blocks_per_block);
    } else if (dev_blocks_per_block * (block_count + start_block) >=
               dev_blocks_per_block + info.block_count) {
      // Don't allow blocks to start past the end of the device
      printf("block_count+offset too large\n");
      return -1;
    }

    if (info.max_transfer_size < block_size) {
      printf("block-size is larger than max transfer size (%d)\n", info.max_transfer_size);
      return -1;
    }

    zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_block::Session>();
    if (endpoints.is_error()) {
      fprintf(stderr, "error: cannot create endpoints for device: %s\n", endpoints.status_string());
      return endpoints.status_value();
    }
    auto& [client, server] = endpoints.value();
    if (fidl::Status result = fidl::WireCall(ctx.caller.borrow_as<fuchsia_hardware_block::Block>())
                                  ->OpenSession(std::move(server));
        !result.ok()) {
      fprintf(stderr, "error: cannot open session for device: %s\n",
              result.FormatDescription().c_str());
      return result.status();
    }

    {
      const fidl::WireResult result = fidl::WireCall(client)->GetFifo();
      if (!result.ok()) {
        printf("cannot get fifo for device: %s\n", result.FormatDescription().c_str());
        return -1;
      }
      const fit::result response = result.value();
      if (response.is_error()) {
        printf("cannot get fifo for device: %s\n", zx_status_get_string(response.error_value()));
        return -1;
      }
      ctx.block.client = std::make_unique<block_client::Client>(std::move(client),
                                                                std::move(response.value()->fifo));
    }

    BlockChecker::ResetAtomic();
  }

  ctx.progress = ProgressBar(block_count, num_threads);

  if (InitializeDevice(ctx)) {
    printf("device initialization failed\n");
    return -1;
  }

  // Reset before launching any worker threads.
  if (!skip) {
    BlockChecker::ResetAtomic();
  }

  printf("starting worker threads...\n");
  thrd_t threads[num_threads];

  if (num_threads > MAX_TXN_GROUP_COUNT) {
    printf("number of threads capped at %u\n", MAX_TXN_GROUP_COUNT);
    num_threads = MAX_TXN_GROUP_COUNT;
  }

  for (auto& thread : threads) {
    if (thrd_create(&thread, DoWork, &ctx) != thrd_success) {
      printf("thread creation failed\n");
      return -1;
    }
  }

  for (auto& thread : threads) {
    thrd_join(thread, nullptr);
  }

  // Reset after launching worker threads to avoid hitting the capacity.
  if (!skip) {
    BlockChecker::ResetAtomic();
  }

  if (!ctx.iochk_failure) {
    printf("re-verifying device...\n");
    fflush(stdout);
    std::unique_ptr<Checker> checker;
    if (zx_status_t status = InitializeChecker(ctx, &checker); status != ZX_OK) {
      printf("failed to initialize verification thread: %s\n", zx_status_get_string(status));
      return status;
    }
    if (checker->Check(start_block, block_count) != ZX_OK) {
      printf("failed to re-verify test data\n");
      ctx.iochk_failure = true;
    } else {
      printf("done\n");
    }
  }

  if (!ctx.iochk_failure) {
    printf("iochk completed successfully\n");
    return 0;
  }
  printf("iochk failed (seed was %ld)\n", base_seed);
  return -1;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    return Usage();
  }

  int res = iochk(argc, argv);
  return res;
}
