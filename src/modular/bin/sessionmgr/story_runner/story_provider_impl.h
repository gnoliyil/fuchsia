// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MODULAR_BIN_SESSIONMGR_STORY_RUNNER_STORY_PROVIDER_IMPL_H_
#define SRC_MODULAR_BIN_SESSIONMGR_STORY_RUNNER_STORY_PROVIDER_IMPL_H_

#include <fuchsia/element/cpp/fidl.h>
#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/modular/internal/cpp/fidl.h>
#include <fuchsia/ui/policy/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <fuchsia/ui/views/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_ptr.h>
#include <lib/fidl/cpp/interface_ptr_set.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fidl/cpp/string.h>
#include <lib/fit/function.h>
#include <lib/sys/inspect/cpp/component.h>

#include <map>
#include <memory>
#include <set>
#include <variant>

#include "src/lib/fxl/macros.h"
#include "src/modular/bin/sessionmgr/agent_services_factory.h"
#include "src/modular/bin/sessionmgr/component_context_impl.h"
#include "src/modular/bin/sessionmgr/storage/session_storage.h"
#include "src/modular/bin/sessionmgr/storage/story_storage.h"
#include "src/modular/bin/sessionmgr/story_runner/annotation_controller_impl.h"
#include "src/modular/lib/async/cpp/operation.h"
#include "src/modular/lib/common/viewmode.h"
#include "src/modular/lib/deprecated_service_provider/service_provider_impl.h"
#include "src/modular/lib/fidl/app_client.h"
#include "src/modular/lib/fidl/environment.h"

namespace modular {

using PresentationProtocolPtr = std::variant<std::monostate, fuchsia::modular::SessionShellPtr,
                                             fuchsia::element::GraphicalPresenterPtr>;

struct AttachOrPresentViewParams {
  std::string story_id;
  std::optional<fuchsia::ui::views::ViewportCreationToken> viewport_creation_token;
  std::optional<fuchsia::ui::views::ViewHolderToken> view_holder_token;
  std::optional<fuchsia::ui::views::ViewRef> view_ref;
};

// StoryControllerImpl has a circular dependency on StoryProviderImpl.
class StoryControllerImpl;

class StoryProviderImpl : fuchsia::modular::StoryProvider {
 public:
  StoryProviderImpl(Environment* session_environment, SessionStorage* session_storage,
                    std::optional<fuchsia::modular::session::AppConfig> story_shell_config,
                    fuchsia::modular::StoryShellFactoryPtr story_shell_factory,
                    PresentationProtocolPtr presentation_protocol, bool present_mods_as_stories,
                    ViewMode view_mode, ComponentContextInfo component_context_info,
                    AgentServicesFactory* agent_services_factory, inspect::Node* root_node);

  ~StoryProviderImpl() override;

  void Connect(fidl::InterfaceRequest<fuchsia::modular::StoryProvider> request);

  // Used when the session shell is swapped.
  void StopAllStories(fit::function<void()> callback);

  // Stops serving the fuchsia::modular::StoryProvider interface and stops all
  // stories.
  void Teardown(fit::function<void()> callback);

  // Called by StoryControllerImpl.
  Environment* session_environment() const { return session_environment_; }

  // Called by StoryControllerImpl.
  const ComponentContextInfo& component_context_info() { return component_context_info_; }

  // Called by SessionmgrImpl.
  //
  // Returns a StoryControllerImpl ptr for |story_id| or nullptr if that story
  // is not running. The returned pointer is safe to use for the stack frame of
  // the calling function.
  StoryControllerImpl* GetStoryControllerImpl(const std::string& story_id);

  // Called by StoryControllerImpl.
  std::unique_ptr<AsyncHolderBase> StartStoryShell(
      std::string story_id,
      fidl::InterfaceRequest<fuchsia::modular::StoryShell> story_shell_request);

  // Called by StoryControllerImpl.
  //
  // Returns nullptr if the StoryInfo for |story_id| is not cached.
  fuchsia::modular::StoryInfo2Ptr GetCachedStoryInfo(const std::string& story_id);

  // |fuchsia::modular::StoryProvider|.
  void GetStoryInfo(std::string story_id, GetStoryInfoCallback callback) override;

  // |fuchsia::modular::StoryProvider|.
  void GetStoryInfo2(std::string story_id, GetStoryInfo2Callback callback) override;

  // Called by StoryControllerImpl. Sends a token for the view of the story identified by
  // |story_id| to the current session shell or graphical presenter
  void AttachOrPresentView(AttachOrPresentViewParams params);

  // Called by StoryControllerImpl. Notifies the current session shell or graphical presenter
  // that the view of the story identified by |story_id| is about to close.
  void DetachOrDismissView(const std::string& story_id, fit::function<void()> done);

  // Converts a StoryInfo2 to StoryInfo.
  static fuchsia::modular::StoryInfo StoryInfo2ToStoryInfo(
      const fuchsia::modular::StoryInfo2& story_info_2);

  // Called by StoryProviderImpl when the StoryState changes.
  void NotifyStoryStateChange(const std::string& story_id);

  bool is_session_shell_presentation() const {
    return std::holds_alternative<fuchsia::modular::SessionShellPtr>(presentation_protocol_);
  }

  bool is_graphical_presenter_presentation() const {
    return std::holds_alternative<fuchsia::element::GraphicalPresenterPtr>(presentation_protocol_);
  }

  ViewMode view_mode() const { return view_mode_; }

 private:
  // |fuchsia::modular::StoryProvider|
  void GetController(std::string story_id,
                     fidl::InterfaceRequest<fuchsia::modular::StoryController> request) override;

  // |fuchsia::modular::StoryProvider|
  void GetStories(fidl::InterfaceHandle<fuchsia::modular::StoryProviderWatcher> watcher,
                  GetStoriesCallback callback) override;

  // |fuchsia::modular::StoryProvider|
  void GetStories2(fidl::InterfaceHandle<fuchsia::modular::StoryProviderWatcher> watcher,
                   GetStories2Callback callback) override;

  // |fuchsia::modular::StoryProvider|
  void Watch(fidl::InterfaceHandle<fuchsia::modular::StoryProviderWatcher> watcher) override;

  // Callbacks invoked through subscriptions on |session_storage_|.
  void OnStoryStorageDeleted(const std::string& story_id);
  void OnStoryStorageUpdated(std::string story_id,
                             const fuchsia::modular::internal::StoryData& story_data);
  void OnAnnotationsUpdated(std::string story_id,
                            const std::vector<fuchsia::modular::Annotation>& annotations,
                            const std::set<std::string>& annotation_keys_updated,
                            const std::set<std::string>& annotation_keys_deleted);

  void NotifyStoryWatchers(const fuchsia::modular::internal::StoryData* story_data,
                           fuchsia::modular::StoryState story_state);

  void MaybeLoadStoryShell();
  void AttachOrPresentStoryShellView(std::string story_id);

  // Called through AttachOrPresentView and send a token for the
  // view of the story identified by |story_id| to the current session shell.
  void AttachView(AttachOrPresentViewParams params);

  // Called through AttachOrPresentView and notifies the current
  // session shell that the view of the story identified by |story_id| is about
  // to close.
  void DetachView(std::string story_id, fit::function<void()> done);

  void PresentView(AttachOrPresentViewParams params);
  void DismissView(const std::string& story_id, fit::function<void()> done);

  Environment* const session_environment_;  // Not owned.
  SessionStorage* session_storage_;         // Not owned.

  // The bindings for this instance.
  fidl::BindingSet<fuchsia::modular::StoryProvider> bindings_;
  fidl::InterfacePtrSet<fuchsia::modular::StoryProviderWatcher> watchers_;

  // Component URL and arguments used to launch story shells.
  std::optional<fuchsia::modular::session::AppConfig> story_shell_config_;

  // Services that story shells can connect to from their environment.
  component::ServiceProviderImpl story_shell_services_;

  // Used to preload story shell before it is requested.
  std::unique_ptr<AppClient<fuchsia::modular::Lifecycle>> preloaded_story_shell_app_;

  // Used to manufacture new StoryShells if not launching a new component for
  // every requested StoryShell instance.
  fuchsia::modular::StoryShellFactoryPtr story_shell_factory_;

  PresentationProtocolPtr presentation_protocol_;

  // When set, mod views are presented as story views.
  bool present_mods_as_stories_;

  // The view API to use for story views, if any.
  ViewMode view_mode_;

  // The story controllers of the currently active stories, indexed by their
  // story IDs.
  //
  // Only user logout or delete story calls ever remove story controllers from
  // this collection, but controllers for stopped stories stay in it.
  //
  // Also keeps a cached version of the StoryData for every story so it does
  // not have to be loaded from disk when querying about this story.
  struct StoryRuntimeContainer {
    // The executor on which asynchronous tasks are scheduled for this story.
    //
    // TODO(thatguy): Migrate all operations under |controller_impl| to use
    // fpromise::promise and |executor|. fxbug.dev/16062
    // TODO(thatguy): Once fpromise::scope is complete, share one executor for the
    // whole process and take advantage of fpromise::scope to auto-cancel tasks when
    // |this| dies.
    std::unique_ptr<fpromise::executor> executor;

    std::unique_ptr<StoryControllerImpl> controller_impl;
    std::shared_ptr<StoryStorage> storage;
    fuchsia::modular::internal::StoryDataPtr current_data;

    std::unique_ptr<inspect::Node> story_node;
    std::map<const std::string, inspect::StringProperty> annotation_inspect_properties;

    void InitializeInspect(std::string story_id, inspect::Node* session_inspect_node);
    void ResetInspect();
  };
  std::map<std::string, StoryRuntimeContainer> story_runtime_containers_;

  const ComponentContextInfo component_context_info_;

  AgentServicesFactory* const agent_services_factory_;  // Not owned.

  inspect::Node* session_inspect_node_;

  // This is a container of all operations that are currently enqueued to run in
  // a FIFO manner. All operations exposed via |fuchsia::modular::StoryProvider|
  // interface are queued here.
  //
  // The advantage of doing this is that if an operation consists of multiple
  // asynchronous calls then no state needs to be maintained for incomplete /
  // pending operations.
  //
  // TODO(mesch): If a story provider operation invokes a story operation that
  // causes the story updating its story info state, that update operation gets
  // scheduled on this queue again, after the current operation. It would be
  // better to be able to schedule such an operation on the story queue because
  // it's a per story operation even if it affects the per story key in the root
  // page, and then the update of story info is bounded by the outer operation.
  OperationQueue operation_queue_;

  fxl::WeakPtrFactory<StoryProviderImpl> weak_factory_;

  // The key for this map is the story id
  std::unordered_map<std::string, std::vector<fuchsia::element::ViewControllerPtr>>
      view_controllers_;
  std::unordered_map<std::string, std::unique_ptr<AnnotationControllerImpl>>
      annotation_controllers_;
  std::unordered_map<std::string, std::vector<fit::function<void()>>> dismiss_callbacks_;

  // Operations implemented here.
  class LoadStoryRuntimeCall;
  class StopStoryCall;
  class StopAllStoriesCall;
  class StopStoryShellCall;

  FXL_DISALLOW_COPY_AND_ASSIGN(StoryProviderImpl);
};

}  // namespace modular

#endif  // SRC_MODULAR_BIN_SESSIONMGR_STORY_RUNNER_STORY_PROVIDER_IMPL_H_
