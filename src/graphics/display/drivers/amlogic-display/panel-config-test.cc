// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/display/drivers/amlogic-display/panel-config.h"

#include <lib/device-protocol/display-panel.h>
#include <lib/zx/result.h>
#include <zircon/errors.h>

#include <gtest/gtest.h>

namespace amlogic_display {

namespace {

TEST(PanelConfig, Tv070wsmFt) {
  const PanelConfig* config_tv070wsm_ft = GetPanelConfig(PANEL_TV070WSM_FT);
  ASSERT_NE(config_tv070wsm_ft, nullptr);
  EXPECT_STREQ(config_tv070wsm_ft->name, "TV070WSM_FT");
}

TEST(PanelConfig, P070acbFt) {
  const PanelConfig* config_p070acb_ft = GetPanelConfig(PANEL_P070ACB_FT);
  ASSERT_NE(config_p070acb_ft, nullptr);
  EXPECT_STREQ(config_p070acb_ft->name, "P070ACB_FT");
}

TEST(PanelConfig, Tv101wxmFt) {
  const PanelConfig* config_tv101wxm_ft = GetPanelConfig(PANEL_TV101WXM_FT);
  ASSERT_NE(config_tv101wxm_ft, nullptr);
  EXPECT_STREQ(config_tv101wxm_ft->name, "TV101WXM_FT");
}

TEST(PanelConfig, P101dezFt) {
  const PanelConfig* config_p101dez_ft = GetPanelConfig(PANEL_P101DEZ_FT);
  ASSERT_NE(config_p101dez_ft, nullptr);
  EXPECT_STREQ(config_p101dez_ft->name, "P101DEZ_FT");
}

TEST(PanelConfig, Tv101wxmFt9365) {
  const PanelConfig* config_tv101wxm_ft_9365 = GetPanelConfig(PANEL_TV101WXM_FT_9365);
  ASSERT_NE(config_tv101wxm_ft_9365, nullptr);
  EXPECT_STREQ(config_tv101wxm_ft_9365->name, "TV101WXM_FT_9365");
}

TEST(PanelConfig, Tv070wsmFt9365) {
  const PanelConfig* config_tv070wsm_ft_9365 = GetPanelConfig(PANEL_TV070WSM_FT_9365);
  ASSERT_NE(config_tv070wsm_ft_9365, nullptr);
  EXPECT_STREQ(config_tv070wsm_ft_9365->name, "TV070WSM_FT_9365");
}

TEST(PanelConfig, Kd070d82Ft) {
  const PanelConfig* config_kd070d82_ft = GetPanelConfig(PANEL_KD070D82_FT);
  ASSERT_NE(config_kd070d82_ft, nullptr);
  EXPECT_STREQ(config_kd070d82_ft->name, "KD070D82_FT");
}

TEST(PanelConfig, Kd070d82Ft9365) {
  const PanelConfig* config_kd070d82_ft_9365 = GetPanelConfig(PANEL_KD070D82_FT_9365);
  ASSERT_NE(config_kd070d82_ft_9365, nullptr);
  EXPECT_STREQ(config_kd070d82_ft_9365->name, "KD070D82_FT_9365");
}

TEST(PanelConfig, Tv070wsmSt7703i) {
  const PanelConfig* config_tv070wsm_st7703i = GetPanelConfig(PANEL_TV070WSM_ST7703I);
  ASSERT_NE(config_tv070wsm_st7703i, nullptr);
  EXPECT_STREQ(config_tv070wsm_st7703i->name, "TV070WSM_ST7703I");
}

TEST(PanelConfig, Mtf050fhdi03) {
  const PanelConfig* config_mtf050fhdi_03 = GetPanelConfig(PANEL_MTF050FHDI_03);
  ASSERT_NE(config_mtf050fhdi_03, nullptr);
  EXPECT_STREQ(config_mtf050fhdi_03->name, "MTF050FHDI_03");
}

TEST(PanelConfig, InvalidPanels) {
  const PanelConfig* config_0x04 = GetPanelConfig(0x04);
  EXPECT_EQ(config_0x04, nullptr);

  const PanelConfig* config_0x05 = GetPanelConfig(0x05);
  EXPECT_EQ(config_0x05, nullptr);

  const PanelConfig* config_0x06 = GetPanelConfig(0x06);
  EXPECT_EQ(config_0x06, nullptr);

  const PanelConfig* config_overly_large = GetPanelConfig(0x0d);
  EXPECT_EQ(config_overly_large, nullptr);

  const PanelConfig* config_unknown = GetPanelConfig(PANEL_UNKNOWN);
  EXPECT_EQ(config_unknown, nullptr);
}

TEST(PanelDisplaySetting, P070acbFt) {
  const display_setting_t* timing_p070acb_ft = GetPanelDisplaySetting(PANEL_P070ACB_FT);
  ASSERT_NE(timing_p070acb_ft, nullptr);
  EXPECT_EQ(timing_p070acb_ft->h_active, 600u);
  EXPECT_EQ(timing_p070acb_ft->v_active, 1024u);
}

TEST(PanelDisplaySetting, Tv070wsmFt) {
  const display_setting_t* timing_tv070wsm_ft = GetPanelDisplaySetting(PANEL_TV070WSM_FT);
  ASSERT_NE(timing_tv070wsm_ft, nullptr);
  EXPECT_EQ(timing_tv070wsm_ft->h_active, 600u);
  EXPECT_EQ(timing_tv070wsm_ft->v_active, 1024u);
}

TEST(PanelDisplaySetting, Kd070d82Ft) {
  const display_setting_t* timing_kd070d82_ft = GetPanelDisplaySetting(PANEL_KD070D82_FT);
  ASSERT_NE(timing_kd070d82_ft, nullptr);
  EXPECT_EQ(timing_kd070d82_ft->h_active, 600u);
  EXPECT_EQ(timing_kd070d82_ft->v_active, 1024u);
}

TEST(PanelDisplaySetting, Kd070d82Ft9365) {
  const display_setting_t* timing_kd070d82_ft_9365 = GetPanelDisplaySetting(PANEL_KD070D82_FT_9365);
  ASSERT_NE(timing_kd070d82_ft_9365, nullptr);
  EXPECT_EQ(timing_kd070d82_ft_9365->h_active, 600u);
  EXPECT_EQ(timing_kd070d82_ft_9365->v_active, 1024u);
}

TEST(PanelDisplaySetting, Tv070wsmFt9365) {
  const display_setting_t* timing_tv070wsm_ft_9365 = GetPanelDisplaySetting(PANEL_TV070WSM_FT_9365);
  ASSERT_NE(timing_tv070wsm_ft_9365, nullptr);
  EXPECT_EQ(timing_tv070wsm_ft_9365->h_active, 600u);
  EXPECT_EQ(timing_tv070wsm_ft_9365->v_active, 1024u);
}

TEST(PanelDisplaySetting, Tv070wsmSt7703i) {
  const display_setting_t* timing_tv070wsm_st7703i = GetPanelDisplaySetting(PANEL_TV070WSM_ST7703I);
  ASSERT_NE(timing_tv070wsm_st7703i, nullptr);
  EXPECT_EQ(timing_tv070wsm_st7703i->h_active, 600u);
  EXPECT_EQ(timing_tv070wsm_st7703i->v_active, 1024u);
}

TEST(PanelDisplaySetting, P101dezFt) {
  const display_setting_t* timing_p101dez_ft = GetPanelDisplaySetting(PANEL_P101DEZ_FT);
  ASSERT_NE(timing_p101dez_ft, nullptr);
  EXPECT_EQ(timing_p101dez_ft->h_active, 800u);
  EXPECT_EQ(timing_p101dez_ft->v_active, 1280u);
}

TEST(PanelDisplaySetting, Tv101wxmFt) {
  const display_setting_t* timing_tv101wxm_ft = GetPanelDisplaySetting(PANEL_TV101WXM_FT);
  ASSERT_NE(timing_tv101wxm_ft, nullptr);
  EXPECT_EQ(timing_tv101wxm_ft->h_active, 800u);
  EXPECT_EQ(timing_tv101wxm_ft->v_active, 1280u);
}

TEST(PanelDisplaySetting, Tv101wxmFt9365) {
  const display_setting_t* timing_tv101wxm_ft_9365 = GetPanelDisplaySetting(PANEL_TV101WXM_FT_9365);
  ASSERT_NE(timing_tv101wxm_ft_9365, nullptr);
  EXPECT_EQ(timing_tv101wxm_ft_9365->h_active, 800u);
  EXPECT_EQ(timing_tv101wxm_ft_9365->v_active, 1280u);
}

TEST(PanelDisplaySetting, Mtf050fhdi03) {
  const display_setting_t* timing_mtf050fhdi_03 = GetPanelDisplaySetting(PANEL_MTF050FHDI_03);
  ASSERT_NE(timing_mtf050fhdi_03, nullptr);
  EXPECT_EQ(timing_mtf050fhdi_03->h_active, 1080u);
  EXPECT_EQ(timing_mtf050fhdi_03->v_active, 1920u);
}

TEST(PanelDisplaySetting, InvalidPanels) {
  const display_setting_t* timing_0x04 = GetPanelDisplaySetting(0x04);
  EXPECT_EQ(timing_0x04, nullptr);

  const display_setting_t* timing_0x05 = GetPanelDisplaySetting(0x05);
  EXPECT_EQ(timing_0x05, nullptr);

  const display_setting_t* timing_0x06 = GetPanelDisplaySetting(0x06);
  EXPECT_EQ(timing_0x06, nullptr);

  const display_setting_t* timing_overly_large = GetPanelDisplaySetting(0x0d);
  EXPECT_EQ(timing_overly_large, nullptr);

  const display_setting_t* timing_unknown = GetPanelDisplaySetting(PANEL_UNKNOWN);
  EXPECT_EQ(timing_unknown, nullptr);
}

}  // namespace

}  // namespace amlogic_display
