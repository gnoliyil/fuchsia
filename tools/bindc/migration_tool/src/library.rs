// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::collections::HashSet;

#[derive(Debug, PartialEq, Eq, Hash)]
pub enum Library {
    Acpi,
    Amlogic,
    Bluetooth,
    Clock,
    Dsi,
    Gpio,
    I2c,
    Pci,
    Platform,
    Pwm,
    Serial,
    Sysmem,
    Tee,
    Test,
    Usb,
    Wlan,
}

impl Library {
    pub fn name(&self) -> &str {
        match self {
            Library::Acpi => "fuchsia.acpi",
            Library::Amlogic => "fuchsia.amlogic.platform",
            Library::Bluetooth => "fuchsia.bluetooth",
            Library::Dsi => "fuchsia.hardware.dsi",
            Library::I2c => "fuchsia.i2c",
            Library::Clock => "fuchsia.hardware.clock",
            Library::Gpio => "fuchsia.hardware.gpio",
            Library::Pci => "fuchsia.pci",
            Library::Platform => "fuchsia.platform",
            Library::Pwm => "fuchsia.pwm",
            Library::Serial => "fuchsia.serial",
            Library::Sysmem => "fuchsia.sysmem",
            Library::Tee => "fuchsia.tee",
            Library::Test => "fuchsia.test",
            Library::Usb => "fuchsia.usb",
            Library::Wlan => "fuchsia.wlan",
        }
    }

    pub fn build_target(&self) -> &str {
        match self {
            Library::Acpi => "//src/devices/bind/fuchsia.acpi",
            Library::Amlogic => "//src/devices/bind/fuchsia.amlogic.platform",
            Library::Bluetooth => "//src/devices/bind/fuchsia.bluetooth",
            Library::Clock => "//src/devices/bind/fuchsia.hardware.clock",
            Library::Dsi => "//src/devices/bind/fuchsia.hardware.dsi",
            Library::Gpio => "//src/devices/bind/fuchsia.hardware.gpio",
            Library::I2c => "//src/devices/bind/fuchsia.i2c",
            Library::Pci => "//src/devices/bind/fuchsia.pci",
            Library::Platform => "//src/devices/bind/fuchsia.platform",
            Library::Pwm => "//src/devices/bind/fuchsia.pwm",
            Library::Serial => "//src/devices/bind/fuchsia.serial",
            Library::Sysmem => "//src/devices/bind/fuchsia.sysmem",
            Library::Tee => "//src/devices/bind/fuchsia.tee",
            Library::Test => "//src/devices/bind/fuchsia.test",
            Library::Usb => "//src/devices/bind/fuchsia.usb",
            Library::Wlan => "//src/devices/bind/fuchsia.wlan",
        }
    }
}

pub fn rename_and_add<'a>(libraries: &mut HashSet<Library>, original: &'a str) -> &'a str {
    match original {
        "BIND_PROTOCOL" => "fuchsia.BIND_PROTOCOL",

        "ZX_PROTOCOL_ACPI" => {
            libraries.insert(Library::Acpi);
            "fuchsia.acpi.BIND_PROTOCOL.DEVICE"
        }
        "ZX_PROTOCOL_PCI" => {
            libraries.insert(Library::Pci);
            "fuchsia.pci.BIND_PROTOCOL.DEVICE"
        }
        "ZX_PROTOCOL_USB_INTERFACE" => {
            libraries.insert(Library::Usb);
            "fuchsia.usb.BIND_PROTOCOL.INTERFACE"
        }
        "ZX_PROTOCOL_GPIO" => {
            libraries.insert(Library::Gpio);
            "fuchsia.hardware.gpio.BIND_PROTOCOL.DEVICE"
        }
        "ZX_PROTOCOL_GPIO_IMPL" => {
            libraries.insert(Library::Gpio);
            "fuchsia.hardware.gpio.BIND_PROTOCOL.IMPL"
        }
        "ZX_PROTOCOL_USB_FUNCTION" => {
            libraries.insert(Library::Usb);
            "fuchsia.usb.BIND_PROTOCOL.FUNCTION"
        }
        "ZX_PROTOCOL_SERIAL" => {
            libraries.insert(Library::Serial);
            "fuchsia.serial.BIND_PROTOCOL.DEVICE"
        }
        "ZX_PROTOCOL_SERIAL_IMPL" => {
            libraries.insert(Library::Serial);
            "fuchsia.serial.BIND_PROTOCOL.IMPL"
        }
        "ZX_PROTOCOL_SERIAL_IMPL_ASYNC" => {
            libraries.insert(Library::Serial);
            "fuchsia.serial.BIND_PROTOCOL.IMPL_ASYNC"
        }
        "ZX_PROTOCOL_TEST" => {
            libraries.insert(Library::Test);
            "fuchsia.test.BIND_PROTOCOL.DEVICE"
        }
        "ZX_PROTOCOL_TEST_COMPAT_CHILD" => {
            libraries.insert(Library::Test);
            "fuchsia.test.BIND_PROTOCOL.COMPAT_CHILD"
        }
        "ZX_PROTOCOL_TEST_POWER_CHILD" => {
            libraries.insert(Library::Test);
            "fuchsia.test.BIND_PROTOCOL.POWER_CHILD"
        }
        "ZX_PROTOCOL_TEST_PARENT" => {
            libraries.insert(Library::Test);
            "fuchsia.test.BIND_PROTOCOL.PARENT"
        }
        "ZX_PROTOCOL_WLANPHY" => {
            libraries.insert(Library::Wlan);
            "fuchsia.wlan.BIND_PROTOCOL.PHY"
        }
        "ZX_PROTOCOL_WLANPHY_IMPL" => {
            libraries.insert(Library::Wlan);
            "fuchsia.wlan.BIND_PROTOCOL.PHY_IMPL"
        }
        "ZX_PROTOCOL_WLAN_FULLMAC" => {
            libraries.insert(Library::Wlan);
            "fuchsia.wlan.BIND_PROTOCOL.IF"
        }
        "ZX_PROTOCOL_WLAN_FULLMAC_IMPL" => {
            libraries.insert(Library::Wlan);
            "fuchsia.wlan.BIND_PROTOCOL.IF_IMPL"
        }
        "ZX_PROTOCOL_WLAN_SOFTMAC" => {
            libraries.insert(Library::Wlan);
            "fuchsia.wlan.BIND_PROTOCOL.MAC"
        }
        "ZX_PROTOCOL_BT_HCI" => {
            libraries.insert(Library::Bluetooth);
            "fuchsia.bluetooth.BIND_PROTOCOL.HCI"
        }
        "ZX_PROTOCOL_BT_EMULATOR" => {
            libraries.insert(Library::Bluetooth);
            "fuchsia.bluetooth.BIND_PROTOCOL.EMULATOR"
        }
        "ZX_PROTOCOL_BT_TRANSPORT" => {
            libraries.insert(Library::Bluetooth);
            "fuchsia.bluetooth.BIND_PROTOCOL.TRANSPORT"
        }
        "ZX_PROTOCOL_BT_HOST" => {
            libraries.insert(Library::Bluetooth);
            "fuchsia.bluetooth.BIND_PROTOCOL.HOST"
        }
        "ZX_PROTOCOL_BT_GATT_SVC" => {
            libraries.insert(Library::Bluetooth);
            "fuchsia.bluetooth.BIND_PROTOCOL.GATT_SVC"
        }
        "ZX_PROTOCOL_PDEV" => {
            libraries.insert(Library::Platform);
            "fuchsia.platform.BIND_PROTOCOL.DEVICE"
        }
        "ZX_PROTOCOL_PWM" => {
            libraries.insert(Library::Pwm);
            "fuchsia.pwm.BIND_PROTOCOL.PWM"
        }
        "ZX_PROTOCOL_PWM_IMPL" => {
            libraries.insert(Library::Pwm);
            "fuchsia.pwm.BIND_PROTOCOL.PWM_IMPL"
        }
        "PDEV_VID_AMLOGIC" => {
            libraries.insert(Library::Amlogic);
            "fuchsia.amlogic.platform.BIND_PLATFORM_DEV_VID.AMLOGIC"
        }
        "PDEV_PID_AMLOGIC_S905D2" => {
            libraries.insert(Library::Amlogic);
            "fuchsia.amlogic.platform.BIND_PLATFORM_DEV_PID.S905D2"
        }
        "PDEV_DID_AMLOGIC_VIDEO" => {
            libraries.insert(Library::Amlogic);
            "fuchsia.amlogic.platform.BIND_PLATFORM_DEV_DID.VIDEO"
        }
        "BIND_PCI_VID" => {
            libraries.insert(Library::Pci);
            "fuchsia.BIND_PCI_VID"
        }
        "BIND_PCI_DID" => {
            libraries.insert(Library::Pci);
            "fuchsia.BIND_PCI_DID"
        }

        "BIND_USB_DID" => {
            libraries.insert(Library::Usb);
            "fuchsia.BIND_USB_DID"
        }
        "BIND_USB_PID" => {
            libraries.insert(Library::Usb);
            "fuchsia.BIND_USB_PID"
        }
        "BIND_USB_CLASS" => {
            libraries.insert(Library::Usb);
            "fuchsia.BIND_USB_CLASS"
        }
        "BIND_USB_SUBCLASS" => {
            libraries.insert(Library::Usb);
            "fuchsia.BIND_USB_SUBCLASS"
        }

        "BIND_PLATFORM_DEV_VID" => {
            libraries.insert(Library::Platform);
            "fuchsia.BIND_PLATFORM_DEV_VID"
        }
        "PDEV_VID_GENERIC" => {
            libraries.insert(Library::Platform);
            "fuchsia.platform.BIND_PLATFORM_DEV_VID.GENERIC"
        }
        "PDEV_PID_GENERIC" => {
            libraries.insert(Library::Platform);
            "fuchsia.platform.BIND_PLATFORM_DEV_PID.GENERIC"
        }
        "BIND_I2C_BUS_ID" => {
            libraries.insert(Library::I2c);
            "fuchsia.BIND_I2C_BUS_ID"
        }
        "BIND_I2C_ADDRESS" => {
            libraries.insert(Library::I2c);
            "fuchsia.BIND_I2C_ADDRESS"
        }
        "ZX_PROTOCOL_I2C" => {
            libraries.insert(Library::I2c);
            "fuchsia.i2c.BIND_PROTOCOL.DEVICE"
        }
        "ZX_PROTOCOL_DSI_IMPL" => {
            libraries.insert(Library::Dsi);
            "fuchsia.hardware.dsi.BIND_PROTOCOL.IMPL"
        }
        "ZX_PROTOCOL_SYSMEM" => {
            libraries.insert(Library::Sysmem);
            "fuchsia.sysmem.BIND_PROTOCOL.DEVICE"
        }
        "ZX_PROTOCOL_TEE" => {
            libraries.insert(Library::Tee);
            "fuchsia.tee.BIND_PROTOCOL.DEVICE"
        }

        "BIND_CLOCK_ID" => "fuchsia.BIND_CLOCK_ID",
        "BIND_GPIO_PIN" => "fuchsia.BIND_GPIO_PIN",
        "BIND_PWM_ID" => "fuchsia.BIND_PWM_ID",
        "BIND_MAILBOX_ID" => "fuchsia.BIND_MAILBOX_ID",

        "BIND_PLATFORM_DEV_PID" => "fuchsia.BIND_PLATFORM_DEV_PID",
        "BIND_PLATFORM_DEV_DID" => "fuchsia.BIND_PLATFORM_DEV_DID",

        "ATHEROS_VID" => "fuchsia.pci.BIND_PCI_VID.ATHEROS",
        "INTEL_VID" => "fuchsia.pci.BIND_PCI_VID.INTEL",

        _ => original,
    }
}
