// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/services/mixer/fidl/consumer_node.h"

#include <lib/syslog/cpp/macros.h>

#include "src/media/audio/services/common/logging.h"

namespace media_audio {

// static
std::shared_ptr<ConsumerNode> ConsumerNode::Create(Args args) {
  struct WithPublicCtor : public ConsumerNode {
   public:
    explicit WithPublicCtor(std::string_view name, std::shared_ptr<Clock> reference_clock,
                            PipelineDirection pipeline_direction, ConsumerStagePtr pipeline_stage,
                            const Format& format,
                            std::shared_ptr<PendingStartStopCommand> pending_start_stop_command,
                            std::shared_ptr<GraphMixThread> mix_thread)
        : ConsumerNode(name, std::move(reference_clock), pipeline_direction,
                       std::move(pipeline_stage), format, std::move(pending_start_stop_command),
                       std::move(mix_thread)) {}
  };

  auto pending_start_stop_command = std::make_shared<PendingStartStopCommand>();
  auto pipeline_stage = std::make_shared<ConsumerStage>(ConsumerStage::Args{
      .name = args.name,
      .pipeline_direction = args.pipeline_direction,
      // TODO(fxbug.dev/87651): also presentation_delay
      .format = args.format,
      .reference_clock = UnreadableClock(args.reference_clock),
      .media_ticks_per_ns = args.media_ticks_per_ns,
      .pending_start_stop_command = pending_start_stop_command,
      .writer = std::move(args.writer),
  });
  pipeline_stage->set_thread(args.thread->pipeline_thread());
  args.thread->AddConsumer(pipeline_stage);

  auto node = std::make_shared<WithPublicCtor>(args.name, std::move(args.reference_clock),
                                               args.pipeline_direction, std::move(pipeline_stage),
                                               args.format, std::move(pending_start_stop_command),
                                               std::move(args.thread));
  return node;
}

ConsumerNode::ConsumerNode(std::string_view name, std::shared_ptr<Clock> reference_clock,
                           PipelineDirection pipeline_direction, ConsumerStagePtr pipeline_stage,
                           const Format& format,
                           std::shared_ptr<PendingStartStopCommand> pending_start_stop_command,
                           std::shared_ptr<GraphMixThread> mix_thread)
    : Node(Type::kConsumer, name, std::move(reference_clock), pipeline_direction, pipeline_stage,
           /*parent=*/nullptr),
      format_(format),
      pending_start_stop_command_(std::move(pending_start_stop_command)),
      mix_thread_(std::move(mix_thread)),
      consumer_stage_(std::move(pipeline_stage)) {
  set_thread(mix_thread_);
}

void ConsumerNode::Start(ConsumerStage::StartCommand cmd) const {
  if (auto old = pending_start_stop_command_->swap(std::move(cmd)); old) {
    StartStopControl::CancelCommand(*old);
  } else {
    mix_thread_->NotifyConsumerStarting(consumer_stage_);
  }
}

void ConsumerNode::Stop(ConsumerStage::StopCommand cmd) const {
  if (auto old = pending_start_stop_command_->swap(std::move(cmd)); old) {
    StartStopControl::CancelCommand(*old);
  } else {
    mix_thread_->NotifyConsumerStarting(consumer_stage_);
  }
}

zx::duration ConsumerNode::PresentationDelayForSourceEdge(const Node* source) const {
  // TODO(fxbug.dev/87651): Implement this.
  return zx::duration(0);
}

void ConsumerNode::DestroySelf() {
  // Deregister from the thread.
  mix_thread_->RemoveConsumer(consumer_stage_);
}

bool ConsumerNode::CanAcceptSourceFormat(const Format& format) const { return format == format_; }
std::optional<size_t> ConsumerNode::MaxSources() const { return 1; }
bool ConsumerNode::AllowsDest() const { return false; }

}  // namespace media_audio
