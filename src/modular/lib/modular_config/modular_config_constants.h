// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MODULAR_LIB_MODULAR_CONFIG_MODULAR_CONFIG_CONSTANTS_H_
#define SRC_MODULAR_LIB_MODULAR_CONFIG_MODULAR_CONFIG_CONSTANTS_H_

namespace modular_config {

constexpr char kBasemgrConfigName[] = "basemgr";
constexpr char kSessionmgrConfigName[] = "sessionmgr";
constexpr char kSessionmgrUrl[] = "fuchsia-pkg://fuchsia.com/sessionmgr#meta/sessionmgr.cmx";

constexpr char kConfigDataDir[] = "/config/data";
constexpr char kPackageDataDir[] = "/pkg/data";
constexpr char kOverriddenConfigDir[] = "/config_override/data";

static constexpr auto kServicesForV1Sessionmgr = "svc_for_v1_sessionmgr";
static constexpr auto kServicesFromV1Sessionmgr = "svc_from_v1_sessionmgr";

// This file path is rooted at either:
//    |kOverriddenConfigDir|
//    |kPackageDataDir|
//    |kDefaultConfigDir|
constexpr char kStartupConfigFilePath[] = "startup.config";

constexpr char kTrue[] = "true";

// Used by sessionmgr component_args.
constexpr char kArgs[] = "args";

// Basemgr constants
constexpr char kEnableCobalt[] = "enable_cobalt";
constexpr char kUseSessionShellForStoryShellFactory[] = "use_session_shell_for_story_shell_factory";
constexpr char kSessionLauncher[] = "session_launcher";
constexpr char kPersistUserArg[] = "--persist_user";
constexpr char kHeadless[] = "headless";

// Sessionmgr constants
constexpr char kComponentArgs[] = "component_args";
constexpr char kAgentServiceIndex[] = "agent_service_index";
constexpr char kV2ModularAgents[] = "v2_modular_agents";
constexpr char kServiceName[] = "service_name";
constexpr char kAgentUrl[] = "agent_url";
constexpr char kExposeFrom[] = "expose_from";
constexpr char kUri[] = "uri";
constexpr char kStartupAgents[] = "startup_agents";
constexpr char kSessionAgents[] = "session_agents";
constexpr char kRestartSessionOnAgentCrash[] = "restart_session_on_agent_crash";
constexpr char kDisableAgentRestartOnCrash[] = "disable_agent_restart_on_crash";
constexpr char kPresentModsAsStories[] = "present_mods_as_stories";

// Inspect property constants
constexpr char kInspectModuleSource[] = "module_source";
constexpr char kInspectIsEmbedded[] = "is_embedded";
constexpr char kInspectIntentAction[] = "intent_action";
constexpr char kInspectIsDeleted[] = "is_deleted";
constexpr char kInspectSurfaceRelationArrangement[] = "surface_arrangement";
constexpr char kInspectSurfaceRelationDependency[] = "surface_dependency";
constexpr char kInspectSurfaceRelationEmphasis[] = "surface_emphasis";
constexpr char kInspectModulePath[] = "module_path";
constexpr char kInspectConfig[] = "config";

// Shell constants
constexpr char kUrl[] = "url";
constexpr char kSessionShells[] = "session_shells";
constexpr char kStoryShellUrl[] = "story_shell_url";

}  // namespace modular_config

#endif  // SRC_MODULAR_LIB_MODULAR_CONFIG_MODULAR_CONFIG_CONSTANTS_H_
