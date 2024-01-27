// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(deprecated)] // Necessary for AsciiExt usage from clap args_enum macro

use clap::arg_enum;
use fidl_fuchsia_wlan_common as wlan_common;
use fidl_fuchsia_wlan_common::PowerSaveType;
use structopt::StructOpt;

arg_enum! {
    #[derive(PartialEq, Copy, Clone, Debug)]
    pub enum RoleArg {
        Client,
        Ap
    }
}

arg_enum! {
    #[derive(PartialEq, Copy, Clone, Debug)]
    pub enum PhyArg {
        Erp,
        Ht,
        Vht,
    }
}

arg_enum! {
    #[derive(PartialEq, Copy, Clone, Debug)]
    pub enum CbwArg {
        Cbw20,
        Cbw40,
        Cbw80,
    }
}

arg_enum! {
    #[derive(PartialEq, Copy, Clone, Debug)]
    pub enum ScanTypeArg {
        Active,
        Passive,
    }
}

arg_enum! {
    #[derive(PartialEq, Copy, Clone, Debug)]
    pub enum PsModeArg {
        PsModeUltraLowPower,
        PsModeLowPower,
        PsModeBalanced,
        PsModePerformance,
    }
}

impl ::std::convert::From<RoleArg> for wlan_common::WlanMacRole {
    fn from(arg: RoleArg) -> Self {
        match arg {
            RoleArg::Client => wlan_common::WlanMacRole::Client,
            RoleArg::Ap => wlan_common::WlanMacRole::Ap,
        }
    }
}

impl ::std::convert::From<PhyArg> for wlan_common::WlanPhyType {
    fn from(arg: PhyArg) -> Self {
        match arg {
            PhyArg::Erp => wlan_common::WlanPhyType::Erp,
            PhyArg::Ht => wlan_common::WlanPhyType::Ht,
            PhyArg::Vht => wlan_common::WlanPhyType::Vht,
        }
    }
}

impl ::std::convert::From<CbwArg> for wlan_common::ChannelBandwidth {
    fn from(arg: CbwArg) -> Self {
        match arg {
            CbwArg::Cbw20 => wlan_common::ChannelBandwidth::Cbw20,
            CbwArg::Cbw40 => wlan_common::ChannelBandwidth::Cbw40,
            CbwArg::Cbw80 => wlan_common::ChannelBandwidth::Cbw80,
        }
    }
}

impl ::std::convert::From<ScanTypeArg> for wlan_common::ScanType {
    fn from(arg: ScanTypeArg) -> Self {
        match arg {
            ScanTypeArg::Active => wlan_common::ScanType::Active,
            ScanTypeArg::Passive => wlan_common::ScanType::Passive,
        }
    }
}

impl ::std::convert::From<PsModeArg> for PowerSaveType {
    fn from(arg: PsModeArg) -> Self {
        match arg {
            PsModeArg::PsModePerformance => PowerSaveType::PsModePerformance,
            PsModeArg::PsModeBalanced => PowerSaveType::PsModeBalanced,
            PsModeArg::PsModeLowPower => PowerSaveType::PsModeLowPower,
            PsModeArg::PsModeUltraLowPower => PowerSaveType::PsModeUltraLowPower,
        }
    }
}

#[derive(StructOpt, Debug, PartialEq)]
pub enum Opt {
    #[structopt(name = "phy")]
    /// commands for wlan phy devices
    Phy(PhyCmd),

    #[structopt(name = "iface")]
    /// commands for wlan iface devices
    Iface(IfaceCmd),

    /// commands for client stations
    #[structopt(name = "client")]
    Client(ClientCmd),
    #[structopt(name = "connect")]
    Connect(ClientConnectCmd),
    #[structopt(name = "disconnect")]
    Disconnect(ClientDisconnectCmd),
    #[structopt(name = "scan")]
    Scan(ClientScanCmd),
    #[structopt(name = "status")]
    Status(IfaceStatusCmd),
    #[structopt(name = "wmm_status")]
    WmmStatus(ClientWmmStatusCmd),

    #[structopt(name = "ap")]
    /// commands for AP stations
    Ap(ApCmd),

    #[structopt(name = "rsn")]
    #[cfg(target_os = "fuchsia")]
    /// commands for verifying RSN behavior
    Rsn(RsnCmd),
}

#[derive(StructOpt, Clone, Debug, PartialEq)]
pub enum PhyCmd {
    #[structopt(name = "list")]
    /// lists phy devices
    List,
    #[structopt(name = "query")]
    /// queries a phy device
    Query {
        #[structopt(raw(required = "true"))]
        /// id of the phy to query
        phy_id: u16,
    },
    #[structopt(name = "get-country")]
    /// gets the phy's country used for WLAN regulatory purposes
    GetCountry {
        #[structopt(raw(required = "true"))]
        /// id of the phy to query
        phy_id: u16,
    },
    #[structopt(name = "set-country")]
    /// sets the phy's country for WLAN regulatory purpose
    SetCountry {
        #[structopt(raw(required = "true"))]
        /// id of the phy to query
        phy_id: u16,
        #[structopt(raw(required = "true"))]
        country: String,
    },
    #[structopt(name = "clear-country")]
    /// sets the phy's country code to world-safe value
    ClearCountry {
        #[structopt(raw(required = "true"))]
        /// id of the phy to query
        phy_id: u16,
    },
    #[structopt(name = "set-ps-mode")]
    /// sets the phy's power save mode
    SetPowerSaveMode {
        #[structopt(raw(required = "true"))]
        /// id of the phy to query
        phy_id: u16,
        #[structopt(raw(required = "true"))]
        #[structopt(
            raw(possible_values = "&PsModeArg::variants()"),
            raw(case_insensitive = "true"),
            help = "Specify PS Mode"
        )]
        mode: PsModeArg,
    },

    #[structopt(name = "get-ps-mode")]
    /// gets the phy's power save mode
    GetPowerSaveMode {
        #[structopt(raw(required = "true"))]
        /// id of the phy to query
        phy_id: u16,
    },
}

#[derive(StructOpt, Clone, Debug, PartialEq)]
pub enum IfaceCmd {
    #[structopt(name = "new")]
    /// creates a new iface device
    New {
        #[structopt(short = "p", long = "phy", raw(required = "true"))]
        /// id of the phy that will host the iface
        phy_id: u16,

        #[structopt(
            short = "r",
            long = "role",
            raw(possible_values = "&RoleArg::variants()"),
            default_value = "Client",
            raw(case_insensitive = "true")
        )]
        /// role of the new iface
        role: RoleArg,

        #[structopt(
            short = "m",
            long = "sta_addr",
            help = "Optional sta addr when we create an iface"
        )]
        /// initial sta address for this iface
        sta_addr: Option<String>,
    },

    #[structopt(name = "del")]
    /// destroys an iface device
    Delete {
        #[structopt(raw(required = "true"))]
        /// iface id to destroy
        iface_id: u16,
    },

    #[structopt(name = "list")]
    List,
    #[structopt(name = "query")]
    Query {
        #[structopt(raw(required = "true"))]
        iface_id: u16,
    },
    #[structopt(name = "minstrel")]
    Minstrel(MinstrelCmd),
    #[structopt(name = "status")]
    Status(IfaceStatusCmd),
}

#[derive(StructOpt, Clone, Debug, PartialEq)]
pub enum MinstrelCmd {
    #[structopt(name = "list")]
    List { iface_id: Option<u16> },
    #[structopt(name = "show")]
    Show { iface_id: Option<u16>, peer_addr: Option<String> },
}

#[derive(StructOpt, Clone, Debug, PartialEq)]
pub struct ClientConnectCmd {
    #[structopt(short = "i", long = "iface", default_value = "0")]
    pub iface_id: u16,
    #[structopt(short = "p", long = "password", help = "WPA2 PSK")]
    pub password: Option<String>,
    #[structopt(short = "hash", long = "hash", help = "WPA2 PSK as hex string")]
    pub psk: Option<String>,
    #[structopt(
        short = "s",
        long = "scan-type",
        default_value = "passive",
        raw(possible_values = "&ScanTypeArg::variants()"),
        raw(case_insensitive = "true"),
        help = "Determines the type of scan performed on non-DFS channels when connecting."
    )]
    pub scan_type: ScanTypeArg,
    #[structopt(
        raw(required = "true"),
        help = "SSID of the target network. Connecting via only an SSID is deprecated and will be \
                removed; use the `donut` tool instead."
    )]
    pub ssid: String,
}

#[derive(StructOpt, Clone, Debug, PartialEq)]
pub struct ClientDisconnectCmd {
    #[structopt(short = "i", long = "iface", default_value = "0")]
    pub iface_id: u16,
}

#[derive(StructOpt, Clone, Debug, PartialEq)]
pub struct ClientScanCmd {
    #[structopt(short = "i", long = "iface", default_value = "0")]
    pub iface_id: u16,
    #[structopt(
        short = "s",
        long = "scan-type",
        default_value = "passive",
        raw(possible_values = "&ScanTypeArg::variants()"),
        raw(case_insensitive = "true"),
        help = "Experimental. Default scan type on each channel. \
                Behavior may differ on DFS channel"
    )]
    pub scan_type: ScanTypeArg,
}

#[derive(StructOpt, Clone, Debug, PartialEq)]
pub struct ClientWmmStatusCmd {
    #[structopt(short = "i", long = "iface", default_value = "0")]
    pub iface_id: u16,
}

#[derive(StructOpt, Clone, Debug, PartialEq)]
pub struct IfaceStatusCmd {
    #[structopt(short = "i", long = "iface")]
    pub iface_id: Option<u16>,
}

#[derive(StructOpt, Clone, Debug, PartialEq)]
pub enum ClientCmd {
    #[structopt(name = "scan")]
    Scan(ClientScanCmd),
    #[structopt(name = "connect")]
    Connect(ClientConnectCmd),
    #[structopt(name = "disconnect")]
    Disconnect(ClientDisconnectCmd),
    #[structopt(name = "wmm_status")]
    WmmStatus(ClientWmmStatusCmd),
}

#[derive(StructOpt, Clone, Debug, PartialEq)]
pub enum ApCmd {
    #[structopt(name = "start")]
    Start {
        #[structopt(short = "i", long = "iface", default_value = "0")]
        iface_id: u16,
        #[structopt(short = "s", long = "ssid")]
        ssid: String,
        #[structopt(short = "p", long = "password")]
        password: Option<String>,
        #[structopt(short = "c", long = "channel")]
        // TODO(porce): Expand to support PHY and CBW
        channel: u8,
    },
    #[structopt(name = "stop")]
    Stop {
        #[structopt(short = "i", long = "iface", default_value = "0")]
        iface_id: u16,
    },
}

#[derive(StructOpt, Clone, Debug, PartialEq)]
pub enum RsnCmd {
    #[structopt(name = "generate-psk")]
    GeneratePsk {
        #[structopt(short = "p", long = "passphrase")]
        passphrase: String,
        #[structopt(short = "s", long = "ssid")]
        ssid: String,
    },
}
