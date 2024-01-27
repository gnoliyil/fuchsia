// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/bin/driver_manager/v2/node.h"

#include <fidl/fuchsia.driver.framework/cpp/fidl.h>
#include <lib/driver/component/cpp/node_add_args.h>
#include <lib/driver/component/cpp/start_args.h>

#include <deque>
#include <unordered_set>

#include "src/devices/bin/driver_manager/v2/node_removal_tracker.h"
#include "src/devices/lib/log/log.h"
#include "src/lib/fxl/strings/join_strings.h"

const std::string kUnboundUrl = "unbound";

namespace fdf {
using namespace fuchsia_driver_framework;
}  // namespace fdf
namespace fdecl = fuchsia_component_decl;

namespace dfv2 {

namespace {

const char* State2String(NodeState state) {
  switch (state) {
    case NodeState::kRunning:
      return "kRunning";
    case NodeState::kPrestop:
      return "kPrestop";
    case NodeState::kWaitingOnChildren:
      return "kWaitingOnChildren";
    case NodeState::kWaitingOnDriver:
      return "kWaitingOnDriver";
    case NodeState::kStopping:
      return "kStopping";
  }
}

template <typename R, typename F>
std::optional<R> VisitOffer(fdecl::Offer& offer, F apply) {
  // Note, we access each field of the union as mutable, so that `apply` can
  // modify the field if necessary.
  switch (offer.Which()) {
    case fdecl::Offer::Tag::kService:
      return apply(offer.service());
    case fdecl::Offer::Tag::kProtocol:
      return apply(offer.protocol());
    case fdecl::Offer::Tag::kDirectory:
      return apply(offer.directory());
    case fdecl::Offer::Tag::kStorage:
      return apply(offer.storage());
    case fdecl::Offer::Tag::kRunner:
      return apply(offer.runner());
    case fdecl::Offer::Tag::kResolver:
      return apply(offer.resolver());
    case fdecl::Offer::Tag::kEvent:
      return apply(offer.event());
    case fdecl::Offer::Tag::kEventStream:
      return apply(offer.event_stream());
    default:
      return {};
  }
}

const char* CollectionName(Collection collection) {
  switch (collection) {
    case Collection::kNone:
      return "";
    case Collection::kHost:
      return "driver-hosts";
    case Collection::kBoot:
      return "boot-drivers";
    case Collection::kPackage:
      return "pkg-drivers";
    case Collection::kUniversePackage:
      return "universe-pkg-drivers";
  }
}

fit::result<fdf::wire::NodeError, fdecl::Offer> AddOfferToNodeOffer(fdecl::Offer add_offer,
                                                                    fdecl::Ref source) {
  auto has_source_name =
      VisitOffer<bool>(add_offer, [](const auto& decl) { return decl->source_name().has_value(); });
  if (!has_source_name.value_or(false)) {
    return fit::as_error(fdf::wire::NodeError::kOfferSourceNameMissing);
  }
  auto has_ref = VisitOffer<bool>(add_offer, [](const auto& decl) {
    return decl->source().has_value() || decl->target().has_value();
  });
  if (has_ref.value_or(false)) {
    return fit::as_error(fdf::wire::NodeError::kOfferRefExists);
  }

  // Assign the source of the offer.
  VisitOffer<bool>(add_offer, [source = std::move(source)](auto decl) mutable {
    decl->source(std::move(source));
    return true;
  });

  return fit::ok(std::move(add_offer));
}

bool IsDefaultOffer(std::string_view target_name) {
  return std::string_view("default").compare(target_name) == 0;
}

template <typename T>
void UnbindAndReset(std::optional<fidl::ServerBindingRef<T>>& ref) {
  if (ref) {
    ref->Unbind();
    ref.reset();
  }
}

fit::result<fdf::wire::NodeError> ValidateSymbols(fidl::VectorView<fdf::wire::NodeSymbol> symbols) {
  std::unordered_set<std::string_view> names;
  for (auto& symbol : symbols) {
    if (!symbol.has_name()) {
      LOGF(ERROR, "SymbolError: a symbol is missing a name");
      return fit::error(fdf::wire::NodeError::kSymbolNameMissing);
    }
    if (!symbol.has_address()) {
      LOGF(ERROR, "SymbolError: symbol '%.*s' is missing an address",
           static_cast<int>(symbol.name().size()), symbol.name().data());
      return fit::error(fdf::wire::NodeError::kSymbolAddressMissing);
    }
    auto [_, inserted] = names.emplace(symbol.name().get());
    if (!inserted) {
      LOGF(ERROR, "SymbolError: symbol '%.*s' already exists",
           static_cast<int>(symbol.name().size()), symbol.name().data());
      return fit::error(fdf::wire::NodeError::kSymbolAlreadyExists);
    }
  }
  return fit::ok();
}

}  // namespace

std::optional<fdecl::wire::Offer> CreateCompositeServiceOffer(fidl::AnyArena& arena,
                                                              fdecl::wire::Offer& offer,
                                                              std::string_view parents_name,
                                                              bool primary_parent) {
  if (!offer.is_service() || !offer.service().has_source_instance_filter() ||
      !offer.service().has_renamed_instances()) {
    return std::nullopt;
  }

  size_t new_instance_count = offer.service().renamed_instances().count();
  if (primary_parent) {
    for (auto& instance : offer.service().renamed_instances()) {
      if (IsDefaultOffer(instance.target_name.get())) {
        new_instance_count++;
      }
    }
  }

  size_t new_filter_count = offer.service().source_instance_filter().count();
  if (primary_parent) {
    for (auto& filter : offer.service().source_instance_filter()) {
      if (IsDefaultOffer(filter.get())) {
        new_filter_count++;
      }
    }
  }

  // We have to create a new offer so we aren't manipulating our parent's offer.
  auto service = fdecl::wire::OfferService::Builder(arena);
  if (offer.service().has_source_name()) {
    service.source_name(offer.service().source_name());
  }
  if (offer.service().has_target_name()) {
    service.target_name(offer.service().target_name());
  }
  if (offer.service().has_source()) {
    service.source(offer.service().source());
  }
  if (offer.service().has_target()) {
    service.target(offer.service().target());
  }

  size_t index = 0;
  fidl::VectorView<fdecl::wire::NameMapping> mappings(arena, new_instance_count);
  for (auto instance : offer.service().renamed_instances()) {
    // The instance is not "default", so copy it over.
    if (!IsDefaultOffer(instance.target_name.get())) {
      mappings[index].source_name = fidl::StringView(arena, instance.source_name.get());
      mappings[index].target_name = fidl::StringView(arena, instance.target_name.get());
      index++;
      continue;
    }

    // We are the primary parent, so add the "default" offer.
    if (primary_parent) {
      mappings[index].source_name = fidl::StringView(arena, instance.source_name.get());
      mappings[index].target_name = fidl::StringView(arena, instance.target_name.get());
      index++;
    }

    // Rename the instance to match the parent's name.
    mappings[index].source_name = fidl::StringView(arena, instance.source_name.get());
    mappings[index].target_name = fidl::StringView(arena, parents_name);
    index++;
  }
  ZX_ASSERT(index == new_instance_count);

  index = 0;
  fidl::VectorView<fidl::StringView> filters(arena, new_instance_count);
  for (auto filter : offer.service().source_instance_filter()) {
    // The filter is not "default", so copy it over.
    if (!IsDefaultOffer(filter.get())) {
      filters[index] = fidl::StringView(arena, filter.get());
      index++;
      continue;
    }

    // We are the primary parent, so add the "default" filter.
    if (primary_parent) {
      filters[index] = fidl::StringView(arena, "default");
      index++;
    }

    // Rename the filter to match the parent's name.
    filters[index] = fidl::StringView(arena, parents_name);
    index++;
  }
  ZX_ASSERT(index == new_filter_count);

  service.renamed_instances(mappings);
  service.source_instance_filter(filters);

  return fdecl::wire::Offer::WithService(arena, service.Build());
}

std::optional<fdecl::wire::Offer> CreateCompositeOffer(fidl::AnyArena& arena,
                                                       fdecl::wire::Offer& offer,
                                                       std::string_view parents_name,
                                                       bool primary_parent) {
  // We route 'service' capabilities based on the parent's name.
  if (offer.is_service()) {
    return CreateCompositeServiceOffer(arena, offer, parents_name, primary_parent);
  }

  // Other capabilities we can simply forward unchanged, but allocated on the new arena.
  return fidl::ToWire(arena, fidl::ToNatural(offer));
}

BindResultTracker::BindResultTracker(size_t expected_result_count,
                                     NodeBindingInfoResultCallback result_callback)
    : expected_result_count_(expected_result_count),
      currently_reported_(0),
      result_callback_(std::move(result_callback)) {}

void BindResultTracker::ReportNoBind() {
  size_t current;
  {
    std::scoped_lock guard(lock_);
    currently_reported_++;
    current = currently_reported_;
  }

  Complete(current);
}

void BindResultTracker::ReportSuccessfulBind(const std::string_view& node_name,
                                             const std::string_view& driver) {
  size_t current;
  {
    std::scoped_lock guard(lock_);
    currently_reported_++;
    auto node_binding_info = fuchsia_driver_development::wire::NodeBindingInfo::Builder(arena_)
                                 .node_name(node_name)
                                 .driver_url(driver)
                                 .Build();
    results_.emplace_back(node_binding_info);
    current = currently_reported_;
  }

  Complete(current);
}

void BindResultTracker::Complete(size_t current) {
  if (current == expected_result_count_) {
    result_callback_(
        fidl::VectorView<fuchsia_driver_development::wire::NodeBindingInfo>(arena_, results_));
  }
}

Node::Node(std::string_view name, std::vector<Node*> parents, NodeManager* node_manager,
           async_dispatcher_t* dispatcher, uint32_t primary_index)
    : name_(name),
      parents_(std::move(parents)),
      primary_index_(primary_index),
      node_manager_(node_manager),
      dispatcher_(dispatcher) {
  ZX_ASSERT(primary_index_ == 0 || primary_index_ < parents_.size());
  if (auto primary_parent = GetPrimaryParent()) {
    // By default, we set `driver_host_` to match the primary parent's
    // `driver_host_`. If the node is then subsequently bound to a driver in a
    // different driver host, this value will be updated to match.
    driver_host_ = primary_parent->driver_host_;
  }
}

Node::Node(std::string_view name, std::vector<Node*> parents, NodeManager* node_manager,
           async_dispatcher_t* dispatcher, DriverHost* driver_host)
    : name_(name),
      parents_(std::move(parents)),
      node_manager_(node_manager),
      dispatcher_(dispatcher),
      driver_host_(driver_host) {}

zx::result<std::shared_ptr<Node>> Node::CreateCompositeNode(
    std::string_view node_name, std::vector<Node*> parents, std::vector<std::string> parents_names,
    std::vector<fuchsia_driver_framework::wire::NodeProperty> properties,
    NodeManager* driver_binder, async_dispatcher_t* dispatcher, uint32_t primary_index) {
  ZX_ASSERT(!parents.empty());
  if (primary_index >= parents.size()) {
    LOGF(ERROR, "Primary node index is out of bounds");
    return zx::error(ZX_ERR_INVALID_ARGS);
  }

  auto composite = std::make_shared<Node>(node_name, std::move(parents), driver_binder, dispatcher,
                                          primary_index);
  composite->parents_names_ = std::move(parents_names);

  for (auto& prop : properties) {
    auto natural = fidl::ToNatural(prop);
    auto new_prop = fidl::ToWire(composite->arena_, std::move(natural));
    composite->properties_.push_back(new_prop);
  }

  auto primary = composite->GetPrimaryParent();
  // We know that our device has a parent because we're creating it.
  ZX_ASSERT(primary);

  // Copy the symbols from the primary parent.
  composite->symbols_.reserve(primary->symbols_.size());
  for (auto& symbol : primary->symbols_) {
    composite->symbols_.emplace_back(fdf::wire::NodeSymbol::Builder(composite->arena_)
                                         .name(composite->arena_, symbol.name().get())
                                         .address(symbol.address())
                                         .Build());
  }

  // Copy the offers from each parent.
  std::vector<fdecl::wire::Offer> node_offers;
  size_t parent_index = 0;
  for (const Node* parent : composite->parents_) {
    auto parent_offers = parent->offers();
    node_offers.reserve(node_offers.size() + parent_offers.count());

    for (auto& parent_offer : parent_offers) {
      auto offer = CreateCompositeOffer(composite->arena_, parent_offer,
                                        composite->parents_names_[parent_index], parent_index == 0);
      if (offer) {
        node_offers.push_back(*offer);
      }
    }
    parent_index++;
  }
  composite->offers_ = std::move(node_offers);

  composite->AddToParents();
  return zx::ok(std::move(composite));
}

Node::~Node() { UnbindAndReset(controller_ref_); }

const std::string& Node::driver_url() const {
  if (driver_component_) {
    return driver_component_->driver_url;
  }
  return kUnboundUrl;
}
const std::vector<Node*>& Node::parents() const { return parents_; }

const std::list<std::shared_ptr<Node>>& Node::children() const { return children_; }

fidl::VectorView<fuchsia_component_decl::wire::Offer> Node::offers() const {
  // TODO(fxbug.dev/66150): Once FIDL wire types support a Clone() method,
  // remove the const_cast.
  return fidl::VectorView<fdecl::wire::Offer>::FromExternal(
      const_cast<decltype(offers_)&>(offers_));
}

fidl::VectorView<fdf::wire::NodeSymbol> Node::symbols() const {
  // TODO(fxbug.dev/7999): Remove const_cast once VectorView supports const.
  return fidl::VectorView<fdf::wire::NodeSymbol>::FromExternal(
      const_cast<decltype(symbols_)&>(symbols_));
}

const std::vector<fdf::wire::NodeProperty>& Node::properties() const { return properties_; }

void Node::set_collection(Collection collection) { collection_ = collection; }

std::string Node::MakeTopologicalPath() const {
  std::deque<std::string_view> names;
  for (auto node = this; node != nullptr; node = node->GetPrimaryParent()) {
    names.push_front(node->name());
  }
  return fxl::JoinStrings(names, "/");
}

std::string Node::MakeComponentMoniker() const {
  std::string topo_path = MakeTopologicalPath();

  // The driver's component name is based on the node name, which means that the
  // node name cam only have [a-z0-9-_.] characters. DFv1 composites contain ':'
  // which is not allowed, so replace those characters.
  // TODO(fxbug.dev/111156): Migrate driver names to only use CF valid characters.
  std::replace(topo_path.begin(), topo_path.end(), ':', '_');
  std::replace(topo_path.begin(), topo_path.end(), '/', '.');
  return topo_path;
}

fuchsia_driver_index::wire::MatchDriverArgs Node::CreateMatchArgs(fidl::AnyArena& arena) {
  return fuchsia_driver_index::wire::MatchDriverArgs::Builder(arena)
      .name(arena, name())
      .properties(
          fidl::VectorView<fuchsia_driver_framework::wire::NodeProperty>::FromExternal(properties_))
      .Build();
}

void Node::OnBind() const {
  if (controller_ref_) {
    fidl::Status result = fidl::WireSendEvent(*controller_ref_)->OnBind();
    if (!result.ok()) {
      LOGF(ERROR, "Failed to send OnBind event: %s", result.FormatDescription().data());
    }
  }
}

void Node::Stop(StopCompleter::Sync& completer) {
  LOGF(DEBUG, "Calling Remove on %s because of Stop() from component framework.", name().c_str());
  Remove(RemovalSet::kAll, nullptr);
}

void Node::Kill(KillCompleter::Sync& completer) {
  LOGF(DEBUG, "Calling Remove on %s because of Kill() from component framework.", name().c_str());
  Remove(RemovalSet::kAll, nullptr);
}

Node* Node::GetPrimaryParent() const {
  return parents_.empty() ? nullptr : parents_[primary_index_];
}

void Node::AddToParents() {
  auto this_node = shared_from_this();
  for (auto parent : parents_) {
    parent->children_.push_back(this_node);
  }
}

void Node::RemoveChild(std::shared_ptr<Node> child) {
  LOGF(DEBUG, "RemoveChild %s from parent %s", child->name().c_str(), name().c_str());
  children_.erase(std::find(children_.begin(), children_.end(), child));
  // If we are waiting for children, see if that is done:
  if (node_state_ != NodeState::kPrestop && node_state_ != NodeState::kRunning) {
    CheckForRemoval();
  }
}

void Node::CheckForRemoval() {
  if (node_state_ != NodeState::kWaitingOnChildren) {
    LOGF(DEBUG, "Node: %s CheckForRemoval: not waiting on children.", name().c_str());
    return;
  }

  LOGF(DEBUG, "Node: %s Checking for removal", name().c_str());
  if (!children_.empty()) {
    return;
  }
  if (removal_tracker_) {
    removal_tracker_->NotifyNoChildren(this);
  }
  LOGF(DEBUG, "Node::Remove(): %s children are empty", name().c_str());
  node_state_ = NodeState::kWaitingOnDriver;
  if (driver_component_ && driver_component_->driver && driver_component_->driver->is_valid()) {
    auto result = (*driver_component_->driver)->Stop();
    if (result.ok()) {
      return;  // We'll now wait for the channel to close
    }
    LOGF(ERROR, "Node: %s failed to stop driver: %s", name().c_str(),
         result.FormatDescription().data());
    // We'd better continue to close, since we can't talk to the driver.
  }
  // No driver, go straight to full shutdown
  FinishRemoval();
}

void Node::FinishRemoval() {
  LOGF(DEBUG, "Node: %s Finishing removal", name().c_str());
  // Get an extra shared_ptr to ourselves so we are not freed halfway through this function.
  auto this_node = shared_from_this();
  ZX_ASSERT(node_state_ == NodeState::kWaitingOnDriver);
  node_state_ = NodeState::kStopping;
  StopComponent();
  driver_component_.reset();
  for (auto& parent : parents()) {
    parent->RemoveChild(shared_from_this());
  }
  parents_.clear();

  LOGF(DEBUG, "Node: %s unbinding and resetting", name().c_str());
  UnbindAndReset(controller_ref_);
  UnbindAndReset(node_ref_);
  if (removal_tracker_) {
    removal_tracker_->NotifyRemovalComplete(this);
  }
}
// State table for package driver:
//                                   Initial States
//                 Running | Prestop|  WoC   | WoDriver | Stopping
// Remove(kPkg)      WoC   |  WoC   | Ignore |  Error!  |  Error!
// Remove(kAll)      WoC   |  WoC   |  WoC   |  Error!  |  Error!
// children empty    N/A   |  N/A   |WoDriver|  Error!  |  Error!
// Driver exit       WoC   |  WoC   |  WoC   | Stopping |  Error!
//
// State table for boot driver:
//                                   Initial States
//                  Running | Prestop |  WoC   | WoDriver | Stopping
// Remove(kPkg)     Prestop | Ignore  | Ignore |  Ignore  |  Ignore
// Remove(kAll)      WoC    |   WoC   | Ignore |  Ignore  |  Ignore
// children empty    N/A    |   N/A   |WoDriver|  Ignore  |  Ignore
// Driver exit       WoC    |   WoC   |  WoC   | Stopping |  Ignore
// Boot drivers go into the Prestop state when Remove(kPackage) is set, to signify that
// a removal is taking place, but this node will not be removed yet, even if all its children
// are removed.
void Node::Remove(RemovalSet removal_set, NodeRemovalTracker* removal_tracker) {
  bool should_register = false;
  if (removal_tracker) {
    if (!removal_tracker_) {
      // First time we are seeing the removal tracker, register with it:
      removal_tracker_ = removal_tracker;
      should_register = true;
    } else {
      // We should never have two competing trackers
      ZX_ASSERT(removal_tracker_ == removal_tracker);
    }
  } else {
    if (removal_tracker_) {
      // TODO(fxbug.dev/115171): Change this to an error when we track shutdown steps better.
      LOGF(WARNING, "Untracked Node::Remove() called on %s, indicating an error during shutdown",
           name().c_str());
    }
  }

  LOGF(DEBUG, "Remove called on Node: %s", name().c_str());
  // Two cases where we will transition state and take action:
  // Removing kAll, and state is Running or Prestop
  // Removing kPkg, and state is Running
  if ((node_state_ != NodeState::kPrestop && node_state_ != NodeState::kRunning) ||
      (node_state_ == NodeState::kPrestop && removal_set == RemovalSet::kPackage)) {
    LOGF(WARNING, "Node::Remove() %s called late, already in state %s",
         MakeComponentMoniker().c_str(), State2String(node_state_));
    if (should_register)
      removal_tracker_->RegisterNode(this, collection_, MakeComponentMoniker(), node_state_);
    return;
  }

  // Now, the cases where we do something:
  // Set the new state
  if (removal_set == RemovalSet::kPackage &&
      (collection_ == Collection::kBoot || collection_ == Collection::kNone)) {
    node_state_ = NodeState::kPrestop;
  } else {
    // Either removing kAll, or is package driver and removing kPackage.
    node_state_ = NodeState::kWaitingOnChildren;
    if (!should_register && removal_tracker_) {
      removal_tracker_->NotifyWaitingOnChildren(this);
    }
    // All children should be removed regardless as they block removal of this node.
    removal_set = RemovalSet::kAll;
  }
  // Either way, propagate removal message to children
  if (should_register) {
    removal_tracker_->RegisterNode(this, collection_, MakeComponentMoniker(), node_state_);
  }

  // Ask each of our children to remove themselves.
  for (auto it = children_.begin(); it != children_.end();) {
    // We have to be careful here - Remove() could invalidate the iterator, so we increment the
    // iterator before we call Remove().
    LOGF(DEBUG, "Node: %s calling remove on child: %s", name().c_str(), (*it)->name().c_str());
    auto child = it->get();
    ++it;
    child->Remove(removal_set, removal_tracker);
  }

  // In case we had no children, or they removed themselves synchronously:
  CheckForRemoval();
}

fit::result<fuchsia_driver_framework::wire::NodeError, std::shared_ptr<Node>> Node::AddChild(
    fuchsia_driver_framework::wire::NodeAddArgs args,
    fidl::ServerEnd<fuchsia_driver_framework::NodeController> controller,
    fidl::ServerEnd<fuchsia_driver_framework::Node> node) {
  if (node_manager_ == nullptr) {
    LOGF(WARNING, "Failed to add Node, as this Node '%s' was removed", name().data());
    return fit::as_error(fdf::wire::NodeError::kNodeRemoved);
  }
  if (!args.has_name()) {
    LOGF(ERROR, "Failed to add Node, a name must be provided");
    return fit::as_error(fdf::wire::NodeError::kNameMissing);
  }
  std::string_view name = args.name().get();
  for (auto& child : children_) {
    if (child->name() == name) {
      LOGF(ERROR, "Failed to add Node '%.*s', name already exists among siblings",
           static_cast<int>(name.size()), name.data());
      return fit::as_error(fdf::wire::NodeError::kNameAlreadyExists);
    }
  };
  auto child = std::make_shared<Node>(name, std::vector<Node*>{this}, *node_manager_, dispatcher_);

  if (args.has_offers()) {
    child->offers_.reserve(args.offers().count());

    // Find a parent node with a collection. This indicates that a driver has
    // been bound to the node, and the driver is running within the collection.
    Node* source_node = this;
    while (source_node && source_node->collection_ == Collection::kNone) {
      source_node = source_node->GetPrimaryParent();
    }
    fdecl::Ref source_ref =
        fdecl::Ref::WithChild(fdecl::ChildRef()
                                  .name(source_node->MakeComponentMoniker())
                                  .collection(CollectionName(source_node->collection_)));

    for (auto& offer : args.offers()) {
      fit::result new_offer = AddOfferToNodeOffer(fidl::ToNatural(offer), source_ref);
      if (new_offer.is_error()) {
        LOGF(ERROR, "Failed to add Node '%s': Bad add offer: %d",
             child->MakeTopologicalPath().c_str(), new_offer.error_value());
        return new_offer.take_error();
      }
      child->offers_.push_back(fidl::ToWire(child->arena_, new_offer.value()));
    }
  }

  if (args.has_properties()) {
    child->properties_.reserve(args.properties().count() + 1);  // + 1 for DFv2 prop.
    for (auto& property : args.properties()) {
      child->properties_.emplace_back(fidl::ToWire(child->arena_, fidl::ToNatural(property)));
    }
  }

  // We set a property for DFv2 devices.
  child->properties_.emplace_back(
      fdf::MakeProperty(child->arena_, "fuchsia.driver.framework.dfv2", true));

  if (args.has_symbols()) {
    auto is_valid = ValidateSymbols(args.symbols());
    if (is_valid.is_error()) {
      LOGF(ERROR, "Failed to add Node '%.*s', bad symbols", static_cast<int>(name.size()),
           name.data());
      return fit::as_error(is_valid.error_value());
    }

    child->symbols_.reserve(args.symbols().count());
    for (auto& symbol : args.symbols()) {
      child->symbols_.emplace_back(fdf::wire::NodeSymbol::Builder(child->arena_)
                                       .name(child->arena_, symbol.name().get())
                                       .address(symbol.address())
                                       .Build());
    }
  }

  if (controller.is_valid()) {
    child->controller_ref_ = fidl::BindServer(dispatcher_, std::move(controller), child.get());
  }
  if (node.is_valid()) {
    child->node_ref_ =
        fidl::BindServer(dispatcher_, std::move(node), child, [](Node* node, auto, auto) {
          LOGF(WARNING, "Removing node %s because of binding closed", node->name().c_str());
          node->Remove(RemovalSet::kAll, nullptr);
        });
  } else {
    // We don't care about tracking binds here, sending nullptr is fine.
    (*node_manager_)->Bind(*child, nullptr);
  }
  child->AddToParents();
  return fit::ok(child);
}

bool Node::IsComposite() const { return parents_.size() > 1; }

void Node::Remove(RemoveCompleter::Sync& completer) {
  LOGF(DEBUG, "Remove() Fidl call for %s", name().c_str());
  Remove(RemovalSet::kAll, nullptr);
}

void Node::AddChild(AddChildRequestView request, AddChildCompleter::Sync& completer) {
  auto node = AddChild(request->args, std::move(request->controller), std::move(request->node));
  if (node.is_error()) {
    completer.Reply(node.take_error());
    return;
  }
  completer.ReplySuccess();
}

zx::result<> Node::StartDriver(
    fuchsia_component_runner::wire::ComponentStartInfo start_info,
    fidl::ServerEnd<fuchsia_component_runner::ComponentController> controller) {
  auto url = start_info.resolved_url().get();
  bool colocate = fdf::ProgramValue(start_info.program(), "colocate").value_or("") == "true";

  if (colocate && !driver_host_) {
    LOGF(ERROR,
         "Failed to start driver '%.*s', driver is colocated but does not have a prent with a "
         "driver host",
         static_cast<int>(url.size()), url.data());
    return zx::error(ZX_ERR_INVALID_ARGS);
  }

  auto symbols = fidl::VectorView<fdf::wire::NodeSymbol>();
  if (colocate) {
    symbols = this->symbols();
  }

  // Launch a driver host if we are not colocated.
  if (!colocate) {
    auto result = (*node_manager_)->CreateDriverHost();
    if (result.is_error()) {
      return result.take_error();
    }
    driver_host_ = result.value();
  }

  // Bind the Node associated with the driver.
  auto endpoints = fidl::CreateEndpoints<fdf::Node>();
  if (endpoints.is_error()) {
    return zx::error(endpoints.error_value());
  }
  node_ref_ =
      fidl::BindServer(dispatcher_, std::move(endpoints->server), shared_from_this(),
                       [](Node* node, fidl::UnbindInfo info, auto) {
                         LOGF(WARNING, "Removing node %s because of fdf::Node binding closed: %s",
                              node->name().c_str(), info.FormatDescription().c_str());
                         node->Remove(RemovalSet::kAll, nullptr);
                       });

  LOGF(INFO, "Binding %.*s to  %s", static_cast<int>(url.size()), url.data(), name().c_str());
  // Start the driver within the driver host.
  auto start =
      (*driver_host_)
          ->Start(std::move(endpoints->client), name_, std::move(symbols), std::move(start_info));
  if (start.is_error()) {
    return zx::error(start.error_value());
  }

  driver_component_ = DriverComponent{
      .component_controller_ref = fidl::BindServer(
          dispatcher_, std::move(controller), shared_from_this(),
          [](Node* node, fidl::UnbindInfo info, auto) {
            LOGF(WARNING, "Removing node %s because of ComponentController binding closed: %s",
                 node->name().c_str(), info.FormatDescription().c_str());
            node->Remove(RemovalSet::kAll, nullptr);
          }),

      .driver =
          fidl::WireSharedClient<fuchsia_driver_host::Driver>(std::move(*start), dispatcher_, this),
      .driver_url = std::string(url),
  };

  return zx::ok();
}

void Node::StopComponent() {
  if (!driver_component_) {
    return;
  }
  // Send an epitaph to the component manager and close the connection. The
  // server of a `ComponentController` protocol is expected to send an epitaph
  // before closing the associated connection.
  driver_component_->component_controller_ref.Close(ZX_OK);
}

void Node::on_fidl_error(fidl::UnbindInfo info) {
  if (driver_component_) {
    driver_component_->driver.reset();
  }
  // The only valid way a driver host should shut down the Driver channel
  // is with the ZX_OK epitaph.
  if (info.reason() != fidl::Reason::kPeerClosedWhileReading || info.status() != ZX_OK) {
    LOGF(ERROR, "Node: %s: driver channel shutdown with: %s", name().c_str(),
         info.FormatDescription().data());
  }
  if (node_state_ == NodeState::kWaitingOnDriver) {
    LOGF(DEBUG, "Node: %s: driver channel had expected shutdown.", name().c_str());
    FinishRemoval();
  } else {
    LOGF(WARNING, "Removing node %s because of unexpected driver channel shutdown.",
         name().c_str());
    Remove(RemovalSet::kAll, nullptr);
  }
}

}  // namespace dfv2
