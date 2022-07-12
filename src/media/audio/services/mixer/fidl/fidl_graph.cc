// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/services/mixer/fidl/fidl_graph.h"

#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>

namespace media_audio {

// static
std::shared_ptr<FidlGraph> FidlGraph::Create(std::shared_ptr<const FidlThread> main_fidl_thread,
                                             fidl::ServerEnd<fuchsia_audio_mixer::Graph> server_end,
                                             Args args) {
  return BaseFidlServer::Create(std::move(main_fidl_thread), std::move(server_end),
                                std::move(args));
}

void FidlGraph::CreateProducer(CreateProducerRequestView request,
                               CreateProducerCompleter::Sync& completer) {
  TRACE_DURATION("audio", "Graph:::CreateProducer");
  ScopedThreadChecker checker(thread().checker());
  FX_CHECK(false) << "not implemented";
}

void FidlGraph::CreateConsumer(CreateConsumerRequestView request,
                               CreateConsumerCompleter::Sync& completer) {
  TRACE_DURATION("audio", "Graph:::CreateConsumer");
  ScopedThreadChecker checker(thread().checker());
  FX_CHECK(false) << "not implemented";
}

void FidlGraph::CreateMixer(CreateMixerRequestView request, CreateMixerCompleter::Sync& completer) {
  TRACE_DURATION("audio", "Graph:::CreateMixer");
  ScopedThreadChecker checker(thread().checker());
  FX_CHECK(false) << "not implemented";
}

void FidlGraph::CreateSplitter(CreateSplitterRequestView request,
                               CreateSplitterCompleter::Sync& completer) {
  TRACE_DURATION("audio", "Graph:::CreateSplitter");
  ScopedThreadChecker checker(thread().checker());
  FX_CHECK(false) << "not implemented";
}

void FidlGraph::CreateCustom(CreateCustomRequestView request,
                             CreateCustomCompleter::Sync& completer) {
  TRACE_DURATION("audio", "Graph:::CreateCustom");
  ScopedThreadChecker checker(thread().checker());
  FX_CHECK(false) << "not implemented";
}

void FidlGraph::DeleteNode(DeleteNodeRequestView request, DeleteNodeCompleter::Sync& completer) {
  TRACE_DURATION("audio", "Graph:::DeleteNode");
  ScopedThreadChecker checker(thread().checker());
  FX_CHECK(false) << "not implemented";
}

void FidlGraph::CreateEdge(CreateEdgeRequestView request, CreateEdgeCompleter::Sync& completer) {
  TRACE_DURATION("audio", "Graph:::CreateEdge");
  ScopedThreadChecker checker(thread().checker());
  FX_CHECK(false) << "not implemented";
}

void FidlGraph::DeleteEdge(DeleteEdgeRequestView request, DeleteEdgeCompleter::Sync& completer) {
  TRACE_DURATION("audio", "Graph:::DeleteEdge");
  ScopedThreadChecker checker(thread().checker());
  FX_CHECK(false) << "not implemented";
}

void FidlGraph::CreateThread(CreateThreadRequestView request,
                             CreateThreadCompleter::Sync& completer) {
  TRACE_DURATION("audio", "Graph:::CreateThread");
  ScopedThreadChecker checker(thread().checker());
  FX_CHECK(false) << "not implemented";
}

void FidlGraph::DeleteThread(DeleteThreadRequestView request,
                             DeleteThreadCompleter::Sync& completer) {
  TRACE_DURATION("audio", "Graph:::DeleteThread");
  ScopedThreadChecker checker(thread().checker());
  FX_CHECK(false) << "not implemented";
}

void FidlGraph::CreateGainControl(CreateGainControlRequestView request,
                                  CreateGainControlCompleter::Sync& completer) {
  TRACE_DURATION("audio", "Graph:::CreateGainControl");
  ScopedThreadChecker checker(thread().checker());
  FX_CHECK(false) << "not implemented";
}

void FidlGraph::DeleteGainControl(DeleteGainControlRequestView request,
                                  DeleteGainControlCompleter::Sync& completer) {
  TRACE_DURATION("audio", "Graph:::DeleteGainControl");
  ScopedThreadChecker checker(thread().checker());
  FX_CHECK(false) << "not implemented";
}

void FidlGraph::CreateGraphControlledReferenceClock(
    CreateGraphControlledReferenceClockRequestView request,
    CreateGraphControlledReferenceClockCompleter::Sync& completer) {
  TRACE_DURATION("audio", "Graph:::CreateGraphControlledReferenceClock");
  ScopedThreadChecker checker(thread().checker());

  auto result = clock_registry_->CreateGraphControlledClock();
  if (!result.is_ok()) {
    completer.ReplyError(result.status_value());
    return;
  }

  auto handle = std::move(result.value().second);
  fidl::Arena arena;
  completer.ReplySuccess(
      fuchsia_audio_mixer::wire::GraphCreateGraphControlledReferenceClockResponse::Builder(arena)
          .reference_clock(std::move(handle))
          .Build());
}

void FidlGraph::ForgetGraphControlledReferenceClock(
    ForgetGraphControlledReferenceClockRequestView request,
    ForgetGraphControlledReferenceClockCompleter::Sync& completer) {
  TRACE_DURATION("audio", "Graph:::ForgetGraphControlledReferenceClock");
  ScopedThreadChecker checker(thread().checker());
  FX_CHECK(false) << "not implemented";
}

}  // namespace media_audio
