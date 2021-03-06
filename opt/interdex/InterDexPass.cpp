/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InterDexPass.h"

#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "PassManager.h"

namespace {

std::unordered_set<interdex::DexStatus, std::hash<int>>
get_mixed_mode_dex_statuses(
    const std::vector<std::string>& mixed_mode_dex_statuses) {
  std::unordered_set<interdex::DexStatus, std::hash<int>> res;

  static std::unordered_map<std::string, interdex::DexStatus> string_to_status =
      {{"first_coldstart_dex", interdex::FIRST_COLDSTART_DEX},
       {"first_extended_dex", interdex::FIRST_EXTENDED_DEX},
       {"scroll_dex", interdex::SCROLL_DEX}};

  for (const std::string& mixed_mode_dex : mixed_mode_dex_statuses) {
    always_assert_log(string_to_status.count(mixed_mode_dex),
                      "Dex Status %s not found. Please check the list "
                      "of accepted statuses.\n",
                      mixed_mode_dex.c_str());
    res.emplace(string_to_status.at(mixed_mode_dex));
  }

  return res;
}

std::unordered_set<DexClass*> get_mixed_mode_classes(
    const std::string& mixed_mode_classes_file) {
  std::ifstream input(mixed_mode_classes_file.c_str(), std::ifstream::in);
  std::unordered_set<DexClass*> mixed_mode_classes;

  if (!input) {
    TRACE(IDEX, 2, "Mixed mode class file: %s : not found\n",
          mixed_mode_classes_file.c_str());
    return mixed_mode_classes;
  }

  std::string class_name;
  while (input >> class_name) {
    auto type = DexType::get_type(class_name.c_str());
    if (!type) {
      TRACE(IDEX, 4, "Couldn't find DexType for mixed mode class: %s\n",
            class_name.c_str());
      continue;
    }
    auto cls = type_class(type);
    if (!cls) {
      TRACE(IDEX, 4, "Couldn't find DexClass for mixed mode class: %s\n",
            class_name.c_str());
      continue;
    }
    if (mixed_mode_classes.count(cls)) {
      TRACE(IDEX, 2, "Duplicate classes found in mixed mode list\n");
      exit(1);
    }
    TRACE(IDEX, 4, "Adding %s in mixed mode list\n", SHOW(cls));
    mixed_mode_classes.emplace(cls);
  }
  input.close();

  return mixed_mode_classes;
}

std::unordered_set<DexClass*> get_mixed_mode_classes(
    const DexClassesVector& dexen, const std::string& mixed_mode_classes_file) {
  // If we have the list of the classes defined, use it.
  if (!mixed_mode_classes_file.empty()) {
    return get_mixed_mode_classes(mixed_mode_classes_file);
  }

  // Otherwise, check for classes that have the mix mode flag set.
  std::unordered_set<DexClass*> mixed_mode_classes;
  for (const auto& dex : dexen) {
    for (const auto& cls : dex) {
      if (cls->rstate.has_mix_mode()) {
        TRACE(IDEX, 4, "Adding class %s to the scroll list\n", SHOW(cls));
        mixed_mode_classes.emplace(cls);
      }
    }
  }
  return mixed_mode_classes;
}

} // namespace

namespace interdex {

void InterDexPass::configure_pass(const JsonWrapper& jw) {
  jw.get("static_prune", false, m_static_prune);
  jw.get("emit_canaries", true, m_emit_canaries);
  jw.get("normal_primary_dex", false, m_normal_primary_dex);
  jw.get("linear_alloc_limit", 11600 * 1024, m_linear_alloc_limit);
  jw.get("scroll_classes_file", "", m_mixed_mode_classes_file);

  jw.get("can_touch_coldstart_cls", false, m_can_touch_coldstart_cls);
  jw.get("can_touch_coldstart_extended_cls", false,
         m_can_touch_coldstart_extended_cls);
  always_assert_log(
      !m_can_touch_coldstart_cls || m_can_touch_coldstart_extended_cls,
      "can_touch_coldstart_extended_cls needs to be true, when we can touch "
      "coldstart classes. Please set can_touch_coldstart_extended_cls "
      "to true\n");

  std::vector<std::string> mixed_mode_dexes;
  jw.get("mixed_mode_dexes", {}, mixed_mode_dexes);
  m_mixed_mode_dex_statuses = get_mixed_mode_dex_statuses(mixed_mode_dexes);
}

void InterDexPass::run_pass(DexClassesVector& dexen,
                            Scope& original_scope,
                            ConfigFiles& cfg,
                            PassManager& mgr) {
  // Setup all external plugins.
  InterDexRegistry* registry = static_cast<InterDexRegistry*>(
      PluginRegistry::get().pass_registry(INTERDEX_PASS_NAME));

  auto plugins = registry->create_plugins();
  for (const auto& plugin : plugins) {
    plugin->configure(original_scope, cfg);
  }

  InterDex interdex(dexen, mgr.apk_manager(), cfg, plugins,
                    m_linear_alloc_limit, m_static_prune, m_normal_primary_dex,
                    m_emit_scroll_set_marker, m_emit_canaries);

  // If we have a list of pre-defined dexes for mixed mode, that has priority.
  // Otherwise, we check if we have a list of pre-defined classes.
  if (m_mixed_mode_dex_statuses.size()) {
    TRACE(IDEX, 3, "Will compile pre-defined dex(es)\n");
    interdex.set_mixed_mode_dex_statuses(std::move(m_mixed_mode_dex_statuses));
  } else {
    auto mixed_mode_classes =
        get_mixed_mode_classes(dexen, m_mixed_mode_classes_file);
    if (mixed_mode_classes.size() > 0) {
      TRACE(IDEX, 3, "[mixed mode]: %d pre-computed mixed mode classes\n",
            mixed_mode_classes.size());
      interdex.set_mixed_mode_classes(std::move(mixed_mode_classes),
                                      m_can_touch_coldstart_cls,
                                      m_can_touch_coldstart_extended_cls);
    }
  }

  dexen = interdex.run();

  for (const auto& plugin : plugins) {
    plugin->cleanup(original_scope);
  }
  mgr.set_metric(METRIC_COLD_START_SET_DEX_COUNT,
                 interdex.get_num_cold_start_set_dexes());
  mgr.set_metric(METRIC_SCROLL_SET_DEX_COUNT,
                 interdex.get_num_scroll_dexes());

  plugins.clear();
}

void InterDexPass::run_pass(DexStoresVector& stores,
                            ConfigFiles& cfg,
                            PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(
        IDEX, 1,
        "InterDexPass not run because no ProGuard configuration was provided.");
    return;
  }

  auto original_scope = build_class_scope(stores);
  for (auto& store : stores) {
    if (store.is_root_store()) {
      run_pass(store.get_dexen(), original_scope, cfg, mgr);
    }
  }
}

static InterDexPass s_pass;

} // namespace interdex
