/*
 *  Copyright (c) 2016, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   This file implements the OpenThread platform abstraction for non-volatile storage of settings.
 *
 */

#include <openthread/platform/misc.h>
#include <openthread/platform/settings.h>

#include "thread_config_manager.h"

static otError platformSettingsDelete(otInstance *instance, uint16_t key, int index);

std::unique_ptr<ThreadConfigManager> config_manager;

void otPlatSettingsInit(otInstance *instance, const uint16_t *aSensitiveKeys,
                        uint16_t aSensitiveKeysLength) {
  otPlatLog(OT_LOG_LEVEL_DEBG, OT_LOG_REGION_PLATFORM, "Inside otPlatSettingsInit");
  if (config_manager == nullptr) {
    config_manager = std::make_unique<ThreadConfigManager>(kThreadSettingsPath);
    otPlatLog(OT_LOG_LEVEL_DEBG, OT_LOG_REGION_PLATFORM, "Created new config_manager!");
  }
}

void otPlatSettingsDeinit(otInstance *instance) {
  otPlatLog(OT_LOG_LEVEL_DEBG, OT_LOG_REGION_PLATFORM, "Deleting config_manager during Deinit.");
  config_manager.reset();
}

static otError get_ot_error(ThreadConfigMgrError error) {
  switch (error) {
    case kThreadConfigMgrErrorConfigNotFound:
      return OT_ERROR_NOT_FOUND;
    case kThreadConfigMgrErrorBufferTooSmall:
      return OT_ERROR_NONE;
    case kThreadConfigMgrErrorPersistedStorageFail:
      otPlatLog(OT_LOG_LEVEL_CRIT, otLogRegion::OT_LOG_REGION_PLATFORM,
                "failed to write to config file: kThreadConfigMgrErrorPersistedStorageFail");
      abort();
      // return OT_ERROR_FAILED;
    case kThreadConfigMgrErrorConflictingTypes:
      // This happens when either during set or get it is found that key exists
      // but the value type is not array. There is no way to handle this
      // situation as it is totally unexpected unless there is some bug in
      // config manager itself
      otPlatLog(OT_LOG_LEVEL_CRIT, otLogRegion::OT_LOG_REGION_PLATFORM,
                "Matching keys with different types");
      abort();
    default:
      return OT_ERROR_NONE;
  }
}

otError otPlatSettingsGet(otInstance *instance, uint16_t key, int index, uint8_t *value,
                          uint16_t *value_length) {
  OT_UNUSED_VARIABLE(instance);
  std::string key_str(std::to_string(key));

  size_t buffer_length = (value_length == NULL ? 0 : *value_length);
  size_t actual_value_length;
  ThreadConfigMgrError error;
  if (config_manager == nullptr) {
    return OT_ERROR_NOT_FOUND;
  }
  error = config_manager->ReadConfigValueFromBinArray(key_str.c_str(), index, value, buffer_length,
                                                      &actual_value_length);
  if (value_length != NULL) {
    *value_length = actual_value_length;
  }

  return get_ot_error(error);
}

otError otPlatSettingsSet(otInstance *instance, uint16_t key, const uint8_t *value,
                          uint16_t value_length) {
  OT_UNUSED_VARIABLE(instance);

  // Delete existing entry
  // Index of -1 means all indices in the value for this key are deleted.
  // ie, key and all values are deleted
  otError err = platformSettingsDelete(instance, key, -1);
  // Return early for error
  // Ignore NOT_FOUND error, this simply means delete wasn't necessary
  if (err != OT_ERROR_NONE && err != OT_ERROR_NOT_FOUND) {
    return err;
  }

  return otPlatSettingsAdd(instance, key, value, value_length);
}

otError otPlatSettingsAdd(otInstance *instance, uint16_t key, const uint8_t *value,
                          uint16_t value_length) {
  OT_UNUSED_VARIABLE(instance);
  std::string key_str(std::to_string(key));
  ThreadConfigMgrError err;
  if (config_manager == nullptr) {
    return OT_ERROR_NOT_FOUND;
  }
  err = config_manager->AppendConfigValueBinArray(key_str.c_str(), value, value_length);
  return get_ot_error(err);
}

otError otPlatSettingsDelete(otInstance *instance, uint16_t key, int index) {
  return platformSettingsDelete(instance, key, index);
}

static otError platformSettingsDelete(otInstance *instance, uint16_t key, int index) {
  OT_UNUSED_VARIABLE(instance);
  otPlatLog(OT_LOG_LEVEL_DEBG, OT_LOG_REGION_PLATFORM, "Deleting settings for key: %d index: %d",
            key, index);
  std::string key_str(std::to_string(key));
  ThreadConfigMgrError err;
  if (index < -1) {
    return OT_ERROR_INVALID_ARGS;
  }
  if (config_manager == nullptr) {
    return OT_ERROR_NOT_FOUND;
  }
  if (index == -1) {
    // Special case: index == -1 means delete all values
    // corresponding to a key
    err = config_manager->ClearConfigValue(key_str.c_str());
  } else {
    err = config_manager->ClearConfigValueFromArray(key_str.c_str(), index);
  }
  return get_ot_error(err);
}

void otPlatSettingsWipe(otInstance *instance) {
  OT_UNUSED_VARIABLE(instance);
  otPlatLog(OT_LOG_LEVEL_DEBG, OT_LOG_REGION_PLATFORM,
            "Clearing all settings during otPlatSettingsWipe!");
  if (config_manager == nullptr) {
    return;
  }
  config_manager->FactoryResetConfig();
}
