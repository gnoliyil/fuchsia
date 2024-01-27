// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_SYS_COMPONENT_CPP_TESTING_REALM_BUILDER_H_
#define LIB_SYS_COMPONENT_CPP_TESTING_REALM_BUILDER_H_

#include <fuchsia/component/config/cpp/fidl.h>
#include <fuchsia/component/cpp/fidl.h>
#include <fuchsia/component/decl/cpp/fidl.h>
#include <fuchsia/component/runner/cpp/fidl.h>
#include <fuchsia/component/test/cpp/fidl.h>
#include <fuchsia/io/cpp/fidl.h>
#include <lib/async/dispatcher.h>
#include <lib/fidl/cpp/interface_handle.h>
#include <lib/sys/component/cpp/testing/internal/local_component_runner.h>
#include <lib/sys/component/cpp/testing/realm_builder_types.h>
#include <lib/sys/component/cpp/testing/scoped_child.h>
#include <lib/sys/cpp/service_directory.h>
#include <zircon/errors.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace component_testing {

// Default child options provided to all components.
const ChildOptions kDefaultChildOptions{.startup_mode = StartupMode::LAZY, .environment = ""};

// Default child collection name for constructed root.
constexpr char kDefaultCollection[] = "realm_builder";

// Root of a constructed Realm. This object can not be instantiated directly.
// Instead, it can only be constructed with the Realm::Builder/Build().
//
// TODO(https://fxbug.dev/120115): Remove all deprecated methods below, e.g. |Connect|.
class RealmRoot final {
 public:
  RealmRoot(RealmRoot&& other) = default;
  RealmRoot& operator=(RealmRoot&& other) = default;

  RealmRoot(const RealmRoot& other) = delete;
  RealmRoot& operator=(const RealmRoot& other) = delete;

  virtual ~RealmRoot();

  // Connect to an interface in the exposed directory of the root component.
  //
  // The discovery name of the interface is inferred from the C++ type of the
  // interface. Callers can supply an interface name explicitly to override
  // the default name.
  //
  // This overload for |Connect| panics if the connection operation
  // doesn't return ZX_OK. Callers that wish to receive that status should use
  // one of the other overloads that returns a |zx_status_t|.
  //
  // # Example
  //
  // ```
  // auto echo = realm.Connect<test::placeholders::Echo>();
  // ```
  template <typename Interface>
  fidl::InterfacePtr<Interface> Connect(const std::string& interface_name = Interface::Name_) const
      ZX_DEPRECATED_SINCE(1, 11, "Use component() instead") {
    ZX_ASSERT_MSG(root_, "RealmRoot already torn down.");
    return root_->Connect<Interface>(interface_name);
  }

  // SynchronousInterfacePtr method overload of |Connect|. See
  // method above for more details.
  template <typename Interface>
  fidl::SynchronousInterfacePtr<Interface> ConnectSync(
      const std::string& interface_name = Interface::Name_) const
      ZX_DEPRECATED_SINCE(1, 11, "Use component() instead") {
    ZX_ASSERT_MSG(root_, "RealmRoot already torn down.");
    return root_->ConnectSync<Interface>(interface_name);
  }

  // Connect to exposed directory of the root component.
  template <typename Interface>
  zx_status_t Connect(fidl::InterfaceRequest<Interface> request) const
      ZX_DEPRECATED_SINCE(1, 11, "Use component() instead") {
    ZX_ASSERT_MSG(root_, "RealmRoot already torn down.");
    return root_->Connect<Interface>(std::move(request));
  }

  // Connect to an interface in the exposed directory using the supplied
  // channel.
  zx_status_t Connect(const std::string& interface_name, zx::channel request) const
      ZX_DEPRECATED_SINCE(1, 11, "Use component() instead");

  // Return a handle to the exposed directory of the root component.
  fidl::InterfaceHandle<fuchsia::io::Directory> CloneRoot() const
      ZX_DEPRECATED_SINCE(1, 11, "Use root() instead") {
    ZX_ASSERT_MSG(root_, "RealmRoot already torn down.");
    return root_->CloneExposedDir();
  }

  // Get the child name of the root component.
  std::string GetChildName() const ZX_DEPRECATED_SINCE(1, 11, "Use component() instead");

  // Returns a callback that returns |true| when realm teardown has completed
  // successfully. If realm teardown fails, it will trigger a panic.
  fit::function<bool()> TeardownCallback() ZX_AVAILABLE_SINCE(9);

  // Destructs the root component and sends Component Manager a request to
  // destroy its realm, which will stop all child components. Each
  // |LocalComponentImpl| should receive an |OnStop()| callback, and after
  // returning, the |LocalComponentImpl| will be destructed. If an
  // |on_teardown_complete| callback is provided, the callback will be invoked
  // when Component Manager has completed the realm teardown. If
  // |TeardownCallback()| was used to get a |bool| callback function, the
  // function will return |false| until the realm is torn down, and then it
  // will return |true|.
  void Teardown(ScopedChild::TeardownCallback on_teardown_complete = nullptr)
      ZX_AVAILABLE_SINCE(10);

  // Returns reference to underlying |ScopedChild| object. Note that this object
  // will be destroyed if |Teardown| is invoked. In that scenario, using this
  // value will yield undefined behavior. Invoking this method after |Teardown| is
  // invoked will cause this process to panic.
  ScopedChild& component() ZX_AVAILABLE_SINCE(11);

 private:
  // Friend classes are needed because the constructor is private.
  friend class Realm;
  friend class RealmBuilder;

  // True if complete, or Error if teardown failed.
  using TeardownStatus = cpp17::variant<bool, fuchsia::component::Error>;
  RealmRoot(std::unique_ptr<internal::LocalComponentRunner> local_component_runner,
            ScopedChild root, async_dispatcher_t* dispatcher);

  std::unique_ptr<internal::LocalComponentRunner> local_component_runner_;

  // root_ is allocated on the heap so it can be destructed independently of
  // the RealmRoot (via RealmRoot::Teardown()). The RealmRoot should not be
  // destructed until the TeardownCallback (passed to Teardown()) is called,
  // indicating Realm::DestroyChild successfully destroyed the realm (the
  // component managed by the ScopedChild).
  std::unique_ptr<ScopedChild> root_;
  async_dispatcher_t* dispatcher_;

  // Holds the shared |TeardownStatus| value if |Teardown()| is called before
  // calling |TeardownStatus()|. When |TeardownStatus()| is called, the
  // shared_ptr is moved to the callback.
  std::shared_ptr<RealmRoot::TeardownStatus> teardown_status_;

  // If not empty, this member is a weak_ptr to the shared_ptr that was or
  // may later be moved to the |TeardownStatus()| callback.
  std::weak_ptr<RealmRoot::TeardownStatus> weak_teardown_status_;
};

// A `Realm` describes a component instance together with its children.
// Clients can use this class to build a realm from scratch,
// programmatically adding children and routes.
//
// Clients may also use this class to recursively build sub-realms by calling
// `AddChildRealm`.
// For more information about RealmBuilder, see the following link.
// https://fuchsia.dev/fuchsia-src/development/testing/components/realm_builder
// For examples on how to use this library, see the integration tests
// found at //sdk/cpp/tests/realm_builder_test.cc
class Realm final {
 public:
  Realm(Realm&&) = default;
  Realm& operator=(Realm&&) = default;

  Realm(const Realm&) = delete;
  Realm operator=(const Realm&) = delete;

  // Add a v2 component (.cm) to this Realm.
  // Names must be unique. Duplicate names will result in a panic.
  Realm& AddChild(const std::string& child_name, const std::string& url,
                  ChildOptions options = kDefaultChildOptions);

  // Add a v1 component (.cmx) to this Realm.
  // Names must be unique. Duplicate names will result in a panic.
#if __Fuchsia_API_level__ <= 11
  Realm& AddLegacyChild(const std::string& child_name, const std::string& url,
                        ChildOptions options = kDefaultChildOptions);
#endif

  // This method signature is DEPRECATED.
  //
  // Add a component instance implementation by raw pointer to a
  // LocalComponent-derived instance. This component implementation can only be
  // started once.
  //
  // The caller is expected to keep the pointer valid for the lifetime of the
  // component instance (typically the lifetime of the constructed RealmRoot,
  // unless the component is intentionally stopped earlier). If not, calling
  // FIDL bindings handled by the LocalComponent would cause undefined behavior.
  //
  // |Start()| will be called (asynchronously) sometime after calling
  // |RealmBuilder::Build()|. Use |ChildOptions| |StartupMode::EAGER| to request
  // component manager start the component automatically.
  //
  // Names must be unique. Duplicate names will result in a panic.
  //
  // TODO(fxbug.dev/109804): Migrate clients to use |LocalComponentFactory|, and
  // remove this deprecated method.
  Realm& AddLocalChild(const std::string& child_name, LocalComponent* local_impl,
                       ChildOptions options = kDefaultChildOptions)
      ZX_DEPRECATED_SINCE(1, 9, "Use AddLocalChild(..., LocalComponentFactory, ...) instead.");

#if __Fuchsia_API_level__ >= 9
  // Add a component by implementing a factory function that creates and returns
  // a new instance of a |LocalComponentImpl|-derived class. The factory
  // function will be called whenever the local child is started.
  //
  // After returning the |LocalComponentImpl|, the RealmBuilder framework will
  // call |LocalComponentImpl::OnStart()|. Component handles (|ns()|, |svc()|,
  // and |outgoing()|) are not available during the |LocalComponentImpl|
  // construction, but are available when |OnStart()| is invoked.
  //
  // If the component's associated |ComponentController| receives a |Stop()|
  // request, the |LocalComponentImpl::OnStop()| method will be called. A
  // derived |LocalComponentImpl| class can override the |OnStop()| method if
  // the component wishes to take some action during component stop.
  //
  // A |LocalComponentImpl| can also self-terminate, by calling `Exit()`.
  //
  // Names must be unique. Duplicate names will result in a panic.
  Realm& AddLocalChild(const std::string& child_name, LocalComponentFactory local_impl,
                       ChildOptions options = kDefaultChildOptions);
#endif

  // Create a sub realm as child of this Realm instance. The constructed
  // Realm is returned.
  Realm AddChildRealm(const std::string& child_name, ChildOptions options = kDefaultChildOptions);

  // Route a capability from one child to another.
  Realm& AddRoute(Route route);

  // Offers a directory capability to a component in this realm. The
  // directory will be read-only (i.e. have `r*` rights), and will have the
  // contents described in `directory`.
  Realm& RouteReadOnlyDirectory(const std::string& name, std::vector<Ref> to,
                                DirectoryContents directory);

#if __Fuchsia_API_level__ >= 9
  // Load the packaged configuration of the component if available.
  Realm& InitMutableConfigFromPackage(const std::string& name);
#endif

#if __Fuchsia_API_level__ >= 9
  // Allow setting configuration values without loading packaged configuration.
  Realm& InitMutableConfigToEmpty(const std::string& name);
#endif

  // Replaces the value of a given configuration field
  Realm& SetConfigValue(const std::string& name, const std::string& key, ConfigValue value);

  // Fetches the Component decl of the given child. This operation is only
  // supported for:
  //
  // * A component with a local implementation
  // * A legacy component
  // * A component added with a fragment-only component URL (typically,
  //   components bundled in the same package as the realm builder client,
  //   sharing the same `/pkg` directory, for example,
  //   `#meta/other-component.cm`; see
  //   https://fuchsia.dev/fuchsia-src/reference/components/url#relative-fragment-only)
  // * An automatically generated realm (such as the root)
  fuchsia::component::decl::Component GetComponentDecl(const std::string& child_name);

  // Fetches the Component decl of this Realm.
  fuchsia::component::decl::Component GetRealmDecl();

  // Updates the Component decl of the given child. This operation is only
  // supported for:
  //
  // * A component with a local implementation
  // * A legacy component
  // * A component added with a fragment-only component URL (typically,
  //   components bundled in the same package as the realm builder client,
  //   sharing the same `/pkg` directory, for example,
  //   `#meta/other-component.cm`; see
  //   https://fuchsia.dev/fuchsia-src/reference/components/url#relative-fragment-only)
  // * An automatically generated realm (such as the root)
  void ReplaceComponentDecl(const std::string& child_name,
                            fuchsia::component::decl::Component decl);

  // Updates the Component decl of this Realm.
  void ReplaceRealmDecl(fuchsia::component::decl::Component decl);

  friend class RealmBuilder;

 private:
  explicit Realm(fuchsia::component::test::RealmSyncPtr realm_proxy,
                 std::shared_ptr<internal::LocalComponentRunner::Builder> runner_builder,
                 std::vector<std::string> scope = {});

  std::string GetResolvedName(const std::string& child_name);

  Realm& AddLocalChildImpl(const std::string& child_name, LocalComponentKind local_impl,
                           ChildOptions options = kDefaultChildOptions);

  fuchsia::component::test::RealmSyncPtr realm_proxy_;
  std::shared_ptr<internal::LocalComponentRunner::Builder> runner_builder_;
  std::vector<std::string> scope_;
};

// Use this Builder class to construct a Realm object.
class RealmBuilder final {
 public:
  // Factory method to create a new Realm::Builder object.
  // |svc| must outlive the RealmBuilder object and created Realm object.
  // If it's nullptr, then the current process' "/svc" namespace entry is used.
  static RealmBuilder Create(std::shared_ptr<sys::ServiceDirectory> svc = nullptr);

  // Same as above but the Realm will contain the contents of the manifest
  // located in the test package at the path indicated by the fragment-only URL
  // (for example, `#meta/other-component.cm`; see
  // https://fuchsia.dev/fuchsia-src/reference/components/url#relative-fragment-only).
  static RealmBuilder CreateFromRelativeUrl(std::string_view fragment_only_url,
                                            std::shared_ptr<sys::ServiceDirectory> svc = nullptr);

  RealmBuilder(RealmBuilder&&) = default;
  RealmBuilder& operator=(RealmBuilder&&) = default;

  RealmBuilder(const RealmBuilder&) = delete;
  RealmBuilder& operator=(const RealmBuilder&) = delete;

  // Add a v2 component (.cm) to the root realm being constructed.
  // See |Realm.AddChild| for more details.
  RealmBuilder& AddChild(const std::string& child_name, const std::string& url,
                         ChildOptions options = kDefaultChildOptions);

  // Add a v1 component (.cmx) to the root realm being constructed.
  // See |Realm.AddLegacyChild| for more details.
#if __Fuchsia_API_level__ <= 11
  RealmBuilder& AddLegacyChild(const std::string& child_name, const std::string& url,
                               ChildOptions options = kDefaultChildOptions);
#endif

  // This method signature is DEPRECATED. Use the LocalComponentFactory
  // implementation of AddLocalChild instead.
  //
  // Add a component by raw pointer to a LocalComponent-derived instance.
  // See |Realm.AddLocalChild| for more details.
  //
  // TODO(fxbug.dev/109804): Migrate clients to use LocalComponentFactory, and
  // remove this deprecated method.
  RealmBuilder& AddLocalChild(const std::string& child_name, LocalComponent* local_impl,
                              ChildOptions options = kDefaultChildOptions)
      ZX_DEPRECATED_SINCE(1, 9, "Use AddLocalChild(..., LocalComponentFactory, ...) instead.");

#if __Fuchsia_API_level__ >= 9
  // Add a component by LocalComponentFactory.
  //
  // See |Realm.AddLocalChild| for more details.

  RealmBuilder& AddLocalChild(const std::string& child_name, LocalComponentFactory local_impl,
                              ChildOptions options = kDefaultChildOptions);
#endif

  // Create a sub realm as child of the root realm. The constructed
  // Realm is returned.
  // See |Realm.AddChildRealm| for more details.
  Realm AddChildRealm(const std::string& child_name, ChildOptions options = kDefaultChildOptions);

  // Route a capability for the root realm being constructed.
  // See |Realm.AddRoute| for more details.
  RealmBuilder& AddRoute(Route route);

  // Offers a directory capability to a component for the root realm.
  // See |Realm.RouteReadOnlyDirectory| for more details.
  RealmBuilder& RouteReadOnlyDirectory(const std::string& name, std::vector<Ref> to,
                                       DirectoryContents directory);

#if __Fuchsia_API_level__ >= 9
  // Load the packaged configuration of the component if available.
  RealmBuilder& InitMutableConfigFromPackage(const std::string& name);
#endif

#if __Fuchsia_API_level__ >= 9
  // Allow setting configuration values without loading packaged configuration.
  RealmBuilder& InitMutableConfigToEmpty(const std::string& name);
#endif

  // Replaces the value of a given configuration field for the root realm.
  RealmBuilder& SetConfigValue(const std::string& name, const std::string& key, ConfigValue value);

  // Fetches the Component decl of the given child of the root realm.
  // See |Realm.GetComponentDecl| for more details.
  fuchsia::component::decl::Component GetComponentDecl(const std::string& child_name);

  // Fetches the Component decl of this root realm.
  fuchsia::component::decl::Component GetRealmDecl();

  // Updates the Component decl of the given child of the root realm.
  // See |Realm.GetRealmDecl| for more details.
  void ReplaceComponentDecl(const std::string& child_name,
                            fuchsia::component::decl::Component decl);

  // Updates the Component decl of this root realm.
  void ReplaceRealmDecl(fuchsia::component::decl::Component decl);

#if __Fuchsia_API_level__ >= 10
  // Set the name of the collection that the realm will be added to.
  // By default this is set to |kDefaultCollection|.
  //
  // Note that this collection name is referenced in the Realm Builder
  // shard (//sdk/lib/sys/component/realm_builder_base.shard.cml) under the
  // collection name |kDefaultCollection|. To retain the same routing, component
  // authors that override the collection name should make the appropriate
  // changes in the test component's manifest.
  RealmBuilder& SetRealmCollection(const std::string& collection);

  // Set the name for the constructed realm. By default, a randomly
  // generated string is used.
  RealmBuilder& SetRealmName(const std::string& name);
#endif

  // Build the realm root prepared by the associated builder methods, e.g. |AddComponent|.
  // |dispatcher| must be non-null, or |async_get_default_dispatcher| must be
  // configured to return a non-null value
  // This function can only be called once per Realm::Builder instance.
  // Multiple invocations will result in a panic.
  // |dispatcher| must outlive the lifetime of the constructed |RealmRoot|.
  RealmRoot Build(async_dispatcher* dispatcher = nullptr);

  // A reference to the root `Realm` object.
  Realm& root();

 private:
  RealmBuilder(std::shared_ptr<sys::ServiceDirectory> svc,
               fuchsia::component::test::BuilderSyncPtr builder_proxy,
               fuchsia::component::test::RealmSyncPtr test_realm_proxy);

  static RealmBuilder CreateImpl(
      cpp17::optional<std::string_view> fragment_only_url = cpp17::nullopt,
      std::shared_ptr<sys::ServiceDirectory> svc = nullptr);

  bool realm_commited_ = false;
  std::string realm_collection_ = kDefaultCollection;
  cpp17::optional<std::string> realm_name_ = cpp17::nullopt;
  std::shared_ptr<sys::ServiceDirectory> svc_;
  fuchsia::component::test::BuilderSyncPtr builder_proxy_;
  std::shared_ptr<internal::LocalComponentRunner::Builder> runner_builder_;
  Realm root_;
};

}  // namespace component_testing

#endif  // LIB_SYS_COMPONENT_CPP_TESTING_REALM_BUILDER_H_
