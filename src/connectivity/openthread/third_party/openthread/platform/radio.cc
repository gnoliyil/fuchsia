// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @file
 * This file implements the spinel based radio transceiver.
 *
 */

#include "radio.h"

#include <openthread/platform/radio.h>

static ot::Spinel::RadioSpinel<ot::Fuchsia::SpinelFidlInterface, otRadioSpinelContext> sRadioSpinel;

extern "C" void otPlatRadioGetIeeeEui64(otInstance *a_instance, uint8_t *a_ieee_eui64) {
  OT_UNUSED_VARIABLE(a_instance);
  SuccessOrDie(sRadioSpinel.GetIeeeEui64(a_ieee_eui64));
}

extern "C" void otPlatRadioSetPanId(otInstance *a_instance, uint16_t panid) {
  OT_UNUSED_VARIABLE(a_instance);
  SuccessOrDie(sRadioSpinel.SetPanId(panid));
}

extern "C" void otPlatRadioSetExtendedAddress(otInstance *a_instance,
                                              const otExtAddress *a_address) {
  OT_UNUSED_VARIABLE(a_instance);
  otExtAddress addr;

  for (size_t i = 0; i < sizeof(addr); i++) {
    addr.m8[i] = a_address->m8[sizeof(addr) - 1 - i];
  }

  SuccessOrDie(sRadioSpinel.SetExtendedAddress(addr));
}

extern "C" void otPlatRadioSetShortAddress(otInstance *a_instance, uint16_t a_address) {
  OT_UNUSED_VARIABLE(a_instance);
  SuccessOrDie(sRadioSpinel.SetShortAddress(a_address));
}

extern "C" void otPlatRadioSetPromiscuous(otInstance *a_instance, bool a_enable) {
  OT_UNUSED_VARIABLE(a_instance);
  SuccessOrDie(sRadioSpinel.SetPromiscuous(a_enable));
}

extern "C" void platformRadioInit(const otPlatformConfig *a_platform_config) {
  SuccessOrDie(sRadioSpinel.GetSpinelInterface().Init());
  sRadioSpinel.Init(a_platform_config->reset_rcp,
                    /* aRestoreDatasetFromNcp */ false,
                    /* aSkipRcpCompatibilityCheck */ false);
}

extern "C" otError otPlatRadioEnable(otInstance *a_instance) {
  otError ret_val = sRadioSpinel.Enable(a_instance);
  return ret_val;
}

extern "C" otError otPlatRadioDisable(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.Disable();
}

extern "C" otError otPlatRadioSleep(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.Sleep();
}

extern "C" otError otPlatRadioReceive(otInstance *a_instance, uint8_t a_channel) {
  OT_UNUSED_VARIABLE(a_instance);

  otError error;
  SuccessOrExit(error = sRadioSpinel.Receive(a_channel));

exit:
  return error;
}

extern "C" otError otPlatRadioTransmit(otInstance *a_instance, otRadioFrame *a_frame) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.Transmit(*a_frame);
}

extern "C" otRadioFrame *otPlatRadioGetTransmitBuffer(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  return &sRadioSpinel.GetTransmitFrame();
}

extern "C" int8_t otPlatRadioGetRssi(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.GetRssi();
}

extern "C" otRadioCaps otPlatRadioGetCaps(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.GetRadioCaps();
}

extern "C" bool otPlatRadioGetPromiscuous(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.IsPromiscuous();
}

extern "C" void otPlatRadioEnableSrcMatch(otInstance *a_instance, bool a_enable) {
  OT_UNUSED_VARIABLE(a_instance);
  SuccessOrDie(sRadioSpinel.EnableSrcMatch(a_enable));
}

extern "C" otError otPlatRadioAddSrcMatchShortEntry(otInstance *a_instance,
                                                    uint16_t a_short_address) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.AddSrcMatchShortEntry(a_short_address);
}

extern "C" otError otPlatRadioAddSrcMatchExtEntry(otInstance *a_instance,
                                                  const otExtAddress *a_ext_address) {
  OT_UNUSED_VARIABLE(a_instance);
  otExtAddress addr;

  for (size_t i = 0; i < sizeof(addr); i++) {
    addr.m8[i] = a_ext_address->m8[sizeof(addr) - 1 - i];
  }

  return sRadioSpinel.AddSrcMatchExtEntry(addr);
}
extern "C" otError otPlatRadioClearSrcMatchShortEntry(otInstance *a_instance,
                                                      uint16_t a_short_address) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.ClearSrcMatchShortEntry(a_short_address);
}

extern "C" otError otPlatRadioClearSrcMatchExtEntry(otInstance *a_instance,
                                                    const otExtAddress *a_ext_address) {
  OT_UNUSED_VARIABLE(a_instance);
  otExtAddress addr;

  for (size_t i = 0; i < sizeof(addr); i++) {
    addr.m8[i] = a_ext_address->m8[sizeof(addr) - 1 - i];
  }

  return sRadioSpinel.ClearSrcMatchExtEntry(addr);
}

extern "C" void otPlatRadioClearSrcMatchShortEntries(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  SuccessOrDie(sRadioSpinel.ClearSrcMatchShortEntries());
}

extern "C" void otPlatRadioClearSrcMatchExtEntries(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  SuccessOrDie(sRadioSpinel.ClearSrcMatchExtEntries());
}

extern "C" otError otPlatRadioEnergyScan(otInstance *a_instance, uint8_t a_scan_channel,
                                         uint16_t a_scan_duration) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.EnergyScan(a_scan_channel, a_scan_duration);
}

extern "C" int8_t otPlatRadioGetReceiveSensitivity(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.GetReceiveSensitivity();
}

void platformRadioProcess(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  otRadioSpinelContext ctx;
  sRadioSpinel.Process(ctx);
}

otError otPlatRadioGetCcaEnergyDetectThreshold(otInstance *a_instance, int8_t *a_threshold) {
  OT_UNUSED_VARIABLE(a_instance);
  assert(a_threshold != NULL);
  return sRadioSpinel.GetCcaEnergyDetectThreshold(*a_threshold);
}

otError otPlatRadioSetCcaEnergyDetectThreshold(otInstance *a_instance, int8_t a_threshold) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.SetCcaEnergyDetectThreshold(a_threshold);
}

otError otPlatRadioGetTransmitPower(otInstance *a_instance, int8_t *a_power) {
  OT_UNUSED_VARIABLE(a_instance);
  assert(a_power != NULL);
  return sRadioSpinel.GetTransmitPower(*a_power);
}

otError otPlatRadioSetTransmitPower(otInstance *a_instance, int8_t a_power) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.SetTransmitPower(a_power);
}

extern "C" void platformRadioDeinit(void) { sRadioSpinel.Deinit(); }

#if OPENTHREAD_CONFIG_DIAG_ENABLE

extern "C" otError otPlatDiagProcess(otInstance *a_instance, uint8_t a_args_length, char *a_args[],
                                     char *a_output, size_t a_output_max_len) {
  OT_UNUSED_VARIABLE(a_instance);
  char cmd[OPENTHREAD_CONFIG_DIAG_CMD_LINE_BUFFER_SIZE] = {'\0'};
  char *cur = cmd;
  char *end = cmd + sizeof(cmd);

  for (uint8_t index = 0; (index < a_args_length) && (cur < end); index++) {
    cur += snprintf(cur, static_cast<size_t>(end - cur), "%s ", a_args[index]);
  }

  return sRadioSpinel.PlatDiagProcess(cmd, a_output, a_output_max_len);
}

extern "C" void otPlatDiagModeSet(bool a_mode) {
  SuccessOrExit(sRadioSpinel.PlatDiagProcess(a_mode ? "start" : "stop", nullptr, 0));
  sRadioSpinel.SetDiagEnabled(a_mode);

exit:
  return;
}

bool otPlatDiagModeGet(void) { return sRadioSpinel.IsDiagEnabled(); }

extern "C" void otPlatDiagTxPowerSet(int8_t a_tx_power) {
  char cmd[OPENTHREAD_CONFIG_DIAG_CMD_LINE_BUFFER_SIZE];

  snprintf(cmd, sizeof(cmd), "power %d", a_tx_power);
  SuccessOrExit(sRadioSpinel.PlatDiagProcess(cmd, nullptr, 0));

exit:
  return;
}

extern "C" void otPlatDiagChannelSet(uint8_t a_channel) {
  char cmd[OPENTHREAD_CONFIG_DIAG_CMD_LINE_BUFFER_SIZE];

  snprintf(cmd, sizeof(cmd), "channel %d", a_channel);
  SuccessOrExit(sRadioSpinel.PlatDiagProcess(cmd, nullptr, 0));

exit:
  return;
}

extern "C" void otPlatDiagRadioReceived(otInstance *a_instance, otRadioFrame *a_frame,
                                        otError a_error) {
  OT_UNUSED_VARIABLE(a_instance);
  OT_UNUSED_VARIABLE(a_frame);
  OT_UNUSED_VARIABLE(a_error);
}

extern "C" void otPlatDiagAlarmCallback(otInstance *a_instance) { OT_UNUSED_VARIABLE(a_instance); }

#endif  // OPENTHREAD_CONFIG_DIAG_ENABLE

otError otPlatRadioSetRegion(otInstance *a_instance, uint16_t a_region_code) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.SetRadioRegion(a_region_code);
}

otError otPlatRadioGetRegion(otInstance *a_instance, uint16_t *a_region_code) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.GetRadioRegion(a_region_code);
}

#if OPENTHREAD_CONFIG_MLE_LINK_METRICS_SUBJECT_ENABLE
otError otPlatRadioConfigureEnhAckProbing(otInstance *aInstance, otLinkMetrics aLinkMetrics,
                                          const otShortAddress aShortAddress,
                                          const otExtAddress *aExtAddress) {
  OT_UNUSED_VARIABLE(aInstance);

  return sRadioSpinel.ConfigureEnhAckProbing(aLinkMetrics, aShortAddress, *aExtAddress);
}
#endif

#if OPENTHREAD_API_VERSION < 175
void otPlatRadioSetMacKey(otInstance *a_instance, uint8_t a_key_id_mode, uint8_t a_key_id,
                          const otMacKey *a_prev_key, const otMacKey *a_curr_key,
                          const otMacKey *a_next_key) {
  SuccessOrDie(
      sRadioSpinel.SetMacKey(a_key_id_mode, a_key_id, *a_prev_key, *a_curr_key, *a_next_key));
  OT_UNUSED_VARIABLE(a_instance);
}
#else
void otPlatRadioSetMacKey(otInstance *a_instance, uint8_t a_key_id_mode, uint8_t a_key_id,
                          const otMacKeyMaterial *a_prev_key, const otMacKeyMaterial *a_curr_key,
                          const otMacKeyMaterial *a_next_key, otRadioKeyType a_key_type) {
  SuccessOrDie(sRadioSpinel.SetMacKey(a_key_id_mode, a_key_id, a_prev_key, a_curr_key, a_next_key));
  OT_UNUSED_VARIABLE(a_instance);
  OT_UNUSED_VARIABLE(a_key_type);
}
#endif

extern "C" bool otPlatRadioIsEnabled(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.IsEnabled();
}

extern "C" const char *otPlatRadioGetVersionString(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.GetVersion();
}

extern "C" otError otPlatRadioGetFemLnaGain(otInstance *a_instance, int8_t *a_gain) {
  OT_UNUSED_VARIABLE(a_instance);
  assert(a_gain != nullptr);
  return sRadioSpinel.GetFemLnaGain(*a_gain);
}

extern "C" otError otPlatRadioSetFemLnaGain(otInstance *a_instance, int8_t a_gain) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.SetFemLnaGain(a_gain);
}

#if OPENTHREAD_CONFIG_PLATFORM_RADIO_COEX_ENABLE
extern "C" otError otPlatRadioSetCoexEnabled(otInstance *a_instance, bool a_enabled) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.SetCoexEnabled(a_enabled);
}

extern "C" bool otPlatRadioIsCoexEnabled(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.IsCoexEnabled();
}

extern "C" otError otPlatRadioGetCoexMetrics(otInstance *a_instance,
                                             otRadioCoexMetrics *a_coex_metrics) {
  OT_UNUSED_VARIABLE(a_instance);

  otError error = OT_ERROR_NONE;

  VerifyOrExit(a_coex_metrics != nullptr, error = OT_ERROR_INVALID_ARGS);

  error = sRadioSpinel.GetCoexMetrics(*a_coex_metrics);

exit:
  return error;
}
#endif

extern "C" uint32_t otPlatRadioGetSupportedChannelMask(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.GetRadioChannelMask(false);
}

extern "C" uint32_t otPlatRadioGetPreferredChannelMask(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.GetRadioChannelMask(true);
}

extern "C" otRadioState otPlatRadioGetState(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.GetState();
}

#ifdef OPENTHREAD_USE_OLD_SETTING_API
extern "C" void otPlatRadioSetMacFrameCounter(otInstance *a_instance,
                                              uint32_t a_mac_frame_counter) {
  SuccessOrDie(sRadioSpinel.SetMacFrameCounter(a_mac_frame_counter));
  OT_UNUSED_VARIABLE(a_instance);
}
#else
extern "C" void otPlatRadioSetMacFrameCounter(otInstance *a_instance,
                                              uint32_t a_mac_frame_counter) {
  SuccessOrDie(sRadioSpinel.SetMacFrameCounter(a_mac_frame_counter, /* aSetIfLarger */ false));
  OT_UNUSED_VARIABLE(a_instance);
}

extern "C" void otPlatRadioSetMacFrameCounterIfLarger(otInstance *aInstance,
                                                      uint32_t aMacFrameCounter) {
  SuccessOrDie(sRadioSpinel.SetMacFrameCounter(aMacFrameCounter, /* aSetIfLarger */ true));
  OT_UNUSED_VARIABLE(aInstance);
}
#endif

extern "C" uint64_t otPlatRadioGetNow(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.GetNow();
}

extern "C" uint8_t otPlatRadioGetCslAccuracy(otInstance *a_instance) {
  OT_UNUSED_VARIABLE(a_instance);

  return 0;
}

extern "C" otError otPlatRadioSetChannelMaxTransmitPower(otInstance *a_instance, uint8_t a_channel,
                                                         int8_t a_max_power) {
  OT_UNUSED_VARIABLE(a_instance);
  return sRadioSpinel.SetChannelMaxTransmitPower(a_channel, a_max_power);
}

extern "C" otError otPlatRadioReceiveAt(otInstance *a_instance, uint8_t a_channel, uint32_t a_start,
                                        uint32_t a_duration) {
  OT_UNUSED_VARIABLE(a_instance);
  OT_UNUSED_VARIABLE(a_channel);
  OT_UNUSED_VARIABLE(a_start);
  OT_UNUSED_VARIABLE(a_duration);
  return OT_ERROR_NOT_IMPLEMENTED;
}
