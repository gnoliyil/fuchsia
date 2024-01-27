// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @file
 *   This file include declarations of functions that will be called in fuchsia component.
 */

#ifndef SRC_CONNECTIVITY_OPENTHREAD_THIRD_PARTY_OPENTHREAD_PLATFORM_RADIO_H_
#define SRC_CONNECTIVITY_OPENTHREAD_THIRD_PARTY_OPENTHREAD_PLATFORM_RADIO_H_

#include "openthread-system.h"
#include "spinel_fidl_interface.h"

#define NEW_HPP_  // TODO(jiamingw) remove after OpenThread updated to May 17, 2023 version
#define OT_CORE_COMMON_NEW_HPP_  // Use the new operator defined in fuchsia instead
#include <spinel/radio_spinel.hpp>
#undef OT_CORE_COMMON_NEW_HPP_
#undef NEW_HPP_

extern "C" void platformRadioInit(const otPlatformConfig *a_platform_config);
extern "C" otError otPlatRadioEnable(otInstance *a_instance);
extern "C" otInstance *otPlatRadioCheckOtInstanceEnabled();
extern "C" void platformRadioProcess(otInstance *a_instance);

#endif  // SRC_CONNECTIVITY_OPENTHREAD_THIRD_PARTY_OPENTHREAD_PLATFORM_RADIO_H_
