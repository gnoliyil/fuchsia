// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fpromise/promise.h>
#include <lib/fpromise/result.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/inspect/cpp/vmo/heap.h>
#include <lib/inspect/cpp/vmo/state.h>
#include <lib/inspect/cpp/vmo/types.h>

using inspect::internal::Heap;
using inspect::internal::State;

namespace inspect {

namespace {
const InspectSettings kDefaultInspectSettings = {.maximum_size = 256 * 1024};
}  // namespace

Inspector::Inspector() : Inspector(kDefaultInspectSettings) {}

Inspector::Inspector(const InspectSettings& settings)
    : root_(std::make_shared<Node>()),
      value_list_(std::make_shared<ValueList>()),
      value_mutex_(std::make_shared<std::mutex>()) {
  if (settings.maximum_size == 0) {
    return;
  }

  state_ = State::CreateWithSize(settings.maximum_size);
  if (!state_) {
    return;
  }

  *root_ = state_->CreateRootNode();
}

Inspector::Inspector(zx::vmo vmo)
    : root_(std::make_shared<Node>()), value_list_(std::make_shared<ValueList>()) {
  size_t size;

  zx_status_t status;
  if (ZX_OK != (status = vmo.get_size(&size))) {
    return;
  }

  if (size == 0) {
    // VMO cannot be zero size.
    return;
  }

  // Decommit all pages, reducing memory usage of the VMO and zeroing it.
  if (ZX_OK != (status = vmo.op_range(ZX_VMO_OP_DECOMMIT, 0, size, nullptr, 0))) {
    return;
  }

  state_ = State::Create(std::make_unique<Heap>(std::move(vmo)));
  if (!state_) {
    return;
  }

  *root_ = state_->CreateRootNode();
}

cpp17::optional<zx::vmo> Inspector::FrozenVmoCopy() const {
  if (!state_) {
    return {};
  }

  return state_->FrozenVmoCopy();
}

zx::vmo Inspector::DuplicateVmo() const {
  zx::vmo ret;

  if (state_) {
    state_->DuplicateVmo(&ret);
  }

  return ret;
}

cpp17::optional<zx::vmo> Inspector::CopyVmo() const {
  zx::vmo ret;

  if (!state_->Copy(&ret)) {
    return {};
  }

  return {std::move(ret)};
}

cpp17::optional<std::vector<uint8_t>> Inspector::CopyBytes() const {
  std::vector<uint8_t> ret;
  if (!state_->CopyBytes(&ret)) {
    return {};
  }

  return {std::move(ret)};
}

InspectStats Inspector::GetStats() const {
  if (!state_) {
    return InspectStats{};
  }
  return state_->GetStats();
}

Node& Inspector::GetRoot() const { return *root_; }

std::vector<std::string> Inspector::GetChildNames() const { return state_->GetLinkNames(); }

fpromise::promise<Inspector> Inspector::OpenChild(const std::string& child_name) const {
  return state_->CallLinkCallback(child_name);
}

void Inspector::AtomicUpdate(AtomicUpdateCallbackFn callback) {
  GetRoot().AtomicUpdate(std::move(callback));
}

namespace {
// The metric node name, as exposed by the stats node.
const char* FUCHSIA_INSPECT_STATS = "fuchsia.inspect.Stats";
const char* CURRENT_SIZE_KEY = "current_size";
const char* MAXIMUM_SIZE_KEY = "maximum_size";
const char* TOTAL_DYNAMIC_CHILDREN_KEY = "total_dynamic_children";
const char* ALLOCATED_BLOCKS_KEY = "allocated_blocks";
const char* DEALLOCATED_BLOCKS_KEY = "deallocated_blocks";
const char* FAILED_ALLOCATIONS_KEY = "failed_allocations";
}  // namespace

void Inspector::CreateStatsNode() {
  GetRoot().CreateLazyNode(
      FUCHSIA_INSPECT_STATS,
      [&] {
        auto stats = this->GetStats();
        Inspector insp;
        insp.GetRoot().CreateUint(CURRENT_SIZE_KEY, stats.size, &insp);
        insp.GetRoot().CreateUint(MAXIMUM_SIZE_KEY, stats.maximum_size, &insp);
        insp.GetRoot().CreateUint(TOTAL_DYNAMIC_CHILDREN_KEY, stats.dynamic_child_count, &insp);
        insp.GetRoot().CreateUint(ALLOCATED_BLOCKS_KEY, stats.allocated_blocks, &insp);
        insp.GetRoot().CreateUint(DEALLOCATED_BLOCKS_KEY, stats.deallocated_blocks, &insp);
        insp.GetRoot().CreateUint(FAILED_ALLOCATIONS_KEY, stats.failed_allocations, &insp);
        return fpromise::make_ok_promise(insp);
      },
      this);
}

namespace internal {
std::shared_ptr<State> GetState(const Inspector* inspector) { return inspector->state_; }
}  // namespace internal

}  // namespace inspect
