// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ADDING A NEW PROTOCOL
// When adding a new protocol, add a macro call at the end of this file after
// the last protocol definition with a tag, value, name, and flags in the form:
//
// DDK_PROTOCOL_DEF(tag, value, protocol_name)
//
// The value must be a unique identifier that is just the previous protocol
// value plus 1.

// clang-format off

#ifndef DDK_FIDL_PROTOCOL_DEF
#error Internal use only. Do not include.
#else
DDK_FIDL_PROTOCOL_DEF(RPMB,            1, "fuchsia.hardware.rpmb.Service")
DDK_FIDL_PROTOCOL_DEF(CHROMEOS_EC,     2, "fuchsia.hardware.google.ec.Service")
DDK_FIDL_PROTOCOL_DEF(I2C,             3, "fuchsia.hardware.i2c.Service")
DDK_FIDL_PROTOCOL_DEF(PCI,             4, "fuchsia.hardware.pci.Service")
DDK_FIDL_PROTOCOL_DEF(GOLDFISH_PIPE,   5, "fuchsia.hardware.goldfish.pipe.Service")
DDK_FIDL_PROTOCOL_DEF(ADDRESS_SPACE,   6, "fuchsia.hardware.goldfish.AddressSpaceService")
DDK_FIDL_PROTOCOL_DEF(GOLDFISH_SYNC,   7, "fuchsia.hardware.goldfish.SyncService")
DDK_FIDL_PROTOCOL_DEF(SPI,             8, "fuchsia.hardware.spi.Service")
DDK_FIDL_PROTOCOL_DEF(SYSMEM,          9, "fuchsia.hardware.sysmem.Service")
DDK_FIDL_PROTOCOL_DEF(MAILBOX,         10, "fuchsia.hardware.mailbox.Service")
DDK_FIDL_PROTOCOL_DEF(PLATFORM_BUS,    11, "fuchsia.hardware.platform.bus.PlatformBus")
DDK_FIDL_PROTOCOL_DEF(INTERRUPT,       12, "fuchsia.hardware.interrupt.Provider")
DDK_FIDL_PROTOCOL_DEF(PLATFORM_DEVICE, 13, "fuchsia.hardware.platform.device.Service")
DDK_FIDL_PROTOCOL_DEF(DSP,             14, "fuchsia.hardware.dsp.Service")
// 15 was "fuchsia.hardware.hdmi.Service"
DDK_FIDL_PROTOCOL_DEF(POWER_SENSOR,    16, "fuchsia.hardware.power.sensor.Service")
DDK_FIDL_PROTOCOL_DEF(VREG,            17, "fuchsia.hardware.vreg.Service")
DDK_FIDL_PROTOCOL_DEF(REGISTERS,       18, "fuchsia.hardware.registers.Service")
DDK_FIDL_PROTOCOL_DEF(TEE,             19, "fuchsia.hardware.tee.Service")
DDK_FIDL_PROTOCOL_DEF(AMLOGIC_CANVAS,  20, "fuchsia.hardware.amlogiccanvas.Service")
DDK_FIDL_PROTOCOL_DEF(POWER,           21, "fuchsia.hardware.power.Service")
DDK_FIDL_PROTOCOL_DEF(CLOCK,           22, "fuchsia.hardware.clock.Service")
DDK_FIDL_PROTOCOL_DEF(CODEC,           23, "fuchsia.hardware.audio.CodecService")
DDK_FIDL_PROTOCOL_DEF(PWM,             24, "fuchsia.hardware.pwm.Service")
DDK_FIDL_PROTOCOL_DEF(ETH_BOARD,       25, "fuchsia.hardware.ethernet.board.Service")
DDK_FIDL_PROTOCOL_DEF(GPIO,            26, "fuchsia.hardware.gpio.Service")
DDK_FIDL_PROTOCOL_DEF(I2C_IMPL,        27, "fuchsia.hardware.i2cimpl.Service")
#undef DDK_FIDL_PROTOCOL_DEF
#endif
