// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::assigned_number;
use super::AssignedNumber;

pub(super) const SERVICE_UUIDS: [AssignedNumber; 40] = [
    assigned_number!("1800", "GAP", "Generic Access"),
    assigned_number!("1801", "GATT", "Generic Attribute"),
    assigned_number!("1802", "IAS", "Immediate Alert Service"),
    assigned_number!("1803", "LLS", "Link Loss Service"),
    assigned_number!("1804", "TPS", "Tx Power Service"),
    assigned_number!("1805", "CTS", "Current Time Service"),
    assigned_number!("1806", "RTUS", "Reference Time Update Service"),
    assigned_number!("1807", "NDCS", "Next DST Change Service"),
    assigned_number!("1808", "GLS", "Glucose Service"),
    assigned_number!("1809", "HTS", "Health Thermometer Service"),
    assigned_number!("180A", "DIS", "Device Information Service"),
    assigned_number!("180D", "HRS", "Heart Rate Service"),
    assigned_number!("180E", "PASS", "Phone Alert Status Service"),
    assigned_number!("180F", "BAS", "Battery Service"),
    assigned_number!("1810", "BLS", "Blood Pressure Service"),
    assigned_number!("1811", "ANS", "Alert Notification Service"),
    assigned_number!("1812", "HIDS", "Human Interface Device Service"),
    assigned_number!("1813", "SCPS", "Scan Parameters Service"),
    assigned_number!("1814", "RSCS", "Running Speed and Cadence Service"),
    assigned_number!("1815", "AIOS", "Automation IO Service"),
    assigned_number!("1816", "CSCS", "Cycling Speed and Cadence Service"),
    assigned_number!("1818", "CPS", "Cycling Power Service"),
    assigned_number!("1819", "LNS", "Location and Navigation Service"),
    assigned_number!("181A", "ESS", "Environmental Sensing Service"),
    assigned_number!("181B", "BCS", "Body Composition Service"),
    assigned_number!("181C", "UDS", "User Data Service"),
    assigned_number!("181D", "WSS", "Weight Scale Service"),
    assigned_number!("181E", "BMS", "Bond Management Service"),
    assigned_number!("181F", "CGMS", "Continuous Glucose Monitoring Service"),
    assigned_number!("1820", "IPSS", "Internet Protocol Support Service"),
    assigned_number!("1821", "IPS", "Indoor Positioning Service"),
    assigned_number!("1822", "PLXS", "Pulse Oximeter Service"),
    assigned_number!("1823", "HPS", "Http Proxy Service"),
    assigned_number!("1824", "TDS", "Transport Discovery Service"),
    assigned_number!("1825", "OTS", "Object Transfer Service"),
    assigned_number!("1826", "FTMS", "Fitness Machine Service"),
    assigned_number!("1827", "MPVS", "Mesh Provisioning Service"),
    assigned_number!("1828", "MPS", "Mesh Proxy Service"),
    assigned_number!("1829", "RCS", "Reconnection Configuration Service"),
    assigned_number!("183A", "IDS", "Insulin Delivery Service"),
];

/// Custom service uuids are SIG allocated 16-bit Universally Unique Identifier (UUID)
/// for use with a custom GATT-based service defined and registered by members.
/// Member names are used here in the `name` field of the `AssignedNumber`.
///
/// Source: https://www.bluetooth.com/specifications/assigned-numbers/16-bit-uuids-for-members
pub(super) const CUSTOM_SERVICE_UUIDS: [AssignedNumber; 304] = [
    assigned_number!("FDCF", "Nalu Medical, Inc"),
    assigned_number!("FDD0", "Huawei Technologies Co., Ltd"),
    assigned_number!("FDD1", "Huawei Technologies Co., Ltd"),
    assigned_number!("FDD2", "Bose Corporation"),
    assigned_number!("FDD3", "FUBA Automotive Electronics GmbH"),
    assigned_number!("FDD4", "LX Solutions Pty Limited"),
    assigned_number!("FDD5", "Brompton Bicycle Ltd"),
    assigned_number!("FDD6", "Ministry of Supply"),
    assigned_number!("FDD7", "Emerson"),
    assigned_number!("FDD8", "Jiangsu Teranovo Tech Co., Ltd."),
    assigned_number!("FDD9", "Jiangsu Teranovo Tech Co., Ltd."),
    assigned_number!("FDDA", "MHCS"),
    assigned_number!("FDDB", "Samsung Electronics Co., Ltd."),
    assigned_number!("FDDC", "4iiii Innovations Inc."),
    assigned_number!("FDDD", "Arch Systems Inc"),
    assigned_number!("FDDE", "Noodle Technology Inc."),
    assigned_number!("FDDF", "Harman International"),
    assigned_number!("FDE0", "John Deere"),
    assigned_number!("FDE1", "Fortin Electronic Systems"),
    assigned_number!("FDE2", "Google Inc."),
    assigned_number!("FDE3", "Abbott Diabetes Care"),
    assigned_number!("FDE4", "JUUL Labs, Inc."),
    assigned_number!("FDE5", "SMK Corporation"),
    assigned_number!("FDE6", "Intelletto Technologies Inc"),
    assigned_number!("FDE7", "SECOM Co., LTD"),
    assigned_number!("FDE8", "Robert Bosch GmbH"),
    assigned_number!("FDE9", "Spacesaver Corporation"),
    assigned_number!("FDEA", "SeeScan, Inc"),
    assigned_number!("FDEB", "Syntronix Corporation"),
    assigned_number!("FDEC", "Mannkind Corporation"),
    assigned_number!("FDED", "Pole Star"),
    assigned_number!("FDEE", "Huawei Technologies Co., Ltd."),
    assigned_number!("FDEF", "ART AND PROGRAM, INC."),
    assigned_number!("FDF0", "Google Inc."),
    assigned_number!("FDF1", "LAMPLIGHT Co.,Ltd"),
    assigned_number!("FDF2", "AMICCOM Electronics Corporation"),
    assigned_number!("FDF3", "Amersports"),
    assigned_number!("FDF4", "O. E. M. Controls, Inc."),
    assigned_number!("FDF5", "Milwaukee Electric Tools"),
    assigned_number!("FDF6", "AIAIAI ApS"),
    assigned_number!("FDF7", "HP Inc."),
    assigned_number!("FDF8", "Onvocal"),
    assigned_number!("FDF9", "INIA"),
    assigned_number!("FDFA", "Tandem Diabetes Care"),
    assigned_number!("FDFB", "Tandem Diabetes Care"),
    assigned_number!("FDFC", "Optrel AG"),
    assigned_number!("FDFD", "RecursiveSoft Inc."),
    assigned_number!("FDFE", "ADHERIUM(NZ) LIMITED"),
    assigned_number!("FDFF", "OSRAM GmbH"),
    assigned_number!("FE00", "Amazon.com Services, Inc."),
    assigned_number!("FE01", "Duracell U.S. Operations Inc."),
    assigned_number!("FE02", "Robert Bosch GmbH"),
    assigned_number!("FE03", "Amazon.com Services, Inc."),
    assigned_number!("FE04", "OpenPath Security Inc"),
    assigned_number!("FE05", "CORE Transport Technologies NZ Limited"),
    assigned_number!("FE06", "Qualcomm Technologies, Inc."),
    assigned_number!("FE08", "Microsoft"),
    assigned_number!("FE09", "Pillsy, Inc."),
    assigned_number!("FE0A", "ruwido austria gmbh"),
    assigned_number!("FE0B", "ruwido austria gmbh"),
    assigned_number!("FE0C", "Procter & Gamble"),
    assigned_number!("FE0D", "Procter & Gamble"),
    assigned_number!("FE0E", "Setec Pty Ltd"),
    assigned_number!("FE0F", "Philips Lighting B.V."),
    assigned_number!("FE10", "Lapis Semiconductor Co., Ltd."),
    assigned_number!("FE11", "GMC-I Messtechnik GmbH"),
    assigned_number!("FE12", "M-Way Solutions GmbH"),
    assigned_number!("FE13", "Apple Inc."),
    assigned_number!("FE14", "Flextronics International USA Inc."),
    assigned_number!("FE15", "Amazon.com Services, Inc."),
    assigned_number!("FE16", "Footmarks, Inc."),
    assigned_number!("FE17", "Telit Wireless Solutions GmbH"),
    assigned_number!("FE18", "Runtime, Inc."),
    assigned_number!("FE19", "Google Inc."),
    assigned_number!("FE1A", "Tyto Life LLC"),
    assigned_number!("FE1B", "Tyto Life LLC"),
    assigned_number!("FE1C", "NetMedia, Inc."),
    assigned_number!("FE1D", "Illuminati Instrument Corporation"),
    assigned_number!("FE1E", "Smart Innovations Co., Ltd"),
    assigned_number!("FE1F", "Garmin International, Inc."),
    assigned_number!("FE20", "Emerson"),
    assigned_number!("FE21", "Bose Corporation"),
    assigned_number!("FE22", "Zoll Medical Corporation"),
    assigned_number!("FE23", "Zoll Medical Corporation"),
    assigned_number!("FE24", "August Home Inc"),
    assigned_number!("FE25", "Apple, Inc."),
    assigned_number!("FE26", "Google Inc."),
    assigned_number!("FE27", "Google Inc."),
    assigned_number!("FE28", "Ayla Networks"),
    assigned_number!("FE29", "Gibson Innovations"),
    assigned_number!("FE2A", "DaisyWorks, Inc."),
    assigned_number!("FE2B", "ITT Industries"),
    assigned_number!("FE2C", "Google Inc."),
    assigned_number!("FE2D", "SMART INNOVATION Co.,Ltd"),
    assigned_number!("FE2E", "ERi,Inc."),
    assigned_number!("FE2F", "CRESCO Wireless, Inc"),
    assigned_number!("FE30", "Volkswagen AG"),
    assigned_number!("FE31", "Volkswagen AG"),
    assigned_number!("FE32", "Pro-Mark, Inc."),
    assigned_number!("FE33", "CHIPOLO d.o.o."),
    assigned_number!("FE34", "SmallLoop LLC"),
    assigned_number!("FE35", "HUAWEI Technologies Co., Ltd"),
    assigned_number!("FE36", "HUAWEI Technologies Co., Ltd"),
    assigned_number!("FE37", "Spaceek LTD"),
    assigned_number!("FE38", "Spaceek LTD"),
    assigned_number!("FE39", "TTS Tooltechnic Systems AG & Co. KG"),
    assigned_number!("FE3A", "TTS Tooltechnic Systems AG & Co. KG"),
    assigned_number!("FE3B", "Dolby Laboratories"),
    assigned_number!("FE3C", "Alibaba"),
    assigned_number!("FE3D", "BD Medical"),
    assigned_number!("FE3E", "BD Medical"),
    assigned_number!("FE3F", "Friday Labs Limited"),
    assigned_number!("FE40", "Inugo Systems Limited"),
    assigned_number!("FE41", "Inugo Systems Limited"),
    assigned_number!("FE42", "Nets A/S"),
    assigned_number!("FE43", "Andreas Stihl AG & Co. KG"),
    assigned_number!("FE44", "SK Telecom"),
    assigned_number!("FE45", "Snapchat Inc"),
    assigned_number!("FE46", "B&O Play A/S"),
    assigned_number!("FE47", "General Motors"),
    assigned_number!("FE48", "General Motors"),
    assigned_number!("FE49", "SenionLab AB"),
    assigned_number!("FE4A", "OMRON HEALTHCARE Co., Ltd."),
    assigned_number!("FE4B", "Philips Lighting B.V."),
    assigned_number!("FE4C", "Volkswagen AG"),
    assigned_number!("FE4D", "Casambi Technologies Oy"),
    assigned_number!("FE4E", "NTT docomo"),
    assigned_number!("FE4F", "Molekule, Inc."),
    assigned_number!("FE50", "Google Inc."),
    assigned_number!("FE51", "SRAM"),
    assigned_number!("FE52", "SetPoint Medical"),
    assigned_number!("FE53", "3M"),
    assigned_number!("FE54", "Motiv, Inc."),
    assigned_number!("FE55", "Google Inc."),
    assigned_number!("FE56", "Google Inc."),
    assigned_number!("FE57", "Dotted Labs"),
    assigned_number!("FE58", "Nordic Semiconductor ASA"),
    assigned_number!("FE59", "Nordic Semiconductor ASA"),
    assigned_number!("FE5A", "Chronologics Corporation"),
    assigned_number!("FE5B", "GT-tronics HK Ltd"),
    assigned_number!("FE5C", "million hunters GmbH"),
    assigned_number!("FE5D", "Grundfos A/S"),
    assigned_number!("FE5E", "Plastc Corporation"),
    assigned_number!("FE5F", "Eyefi, Inc."),
    assigned_number!("FE60", "Lierda Science & Technology Group Co., Ltd."),
    assigned_number!("FE61", "Logitech International SA"),
    assigned_number!("FE62", "Indagem Tech LLC"),
    assigned_number!("FE63", "Connected Yard, Inc."),
    assigned_number!("FE64", "Siemens AG"),
    assigned_number!("FE65", "CHIPOLO d.o.o."),
    assigned_number!("FE66", "Intel Corporation"),
    assigned_number!("FE67", "Lab Sensor Solutions"),
    assigned_number!("FE68", "Qualcomm Life Inc"),
    assigned_number!("FE69", "Qualcomm Life Inc"),
    assigned_number!("FE6A", "Kontakt Micro-Location Sp. z o.o."),
    assigned_number!("FE6B", "TASER International, Inc."),
    assigned_number!("FE6C", "TASER International, Inc."),
    assigned_number!("FE6D", "The University of Tokyo"),
    assigned_number!("FE6E", "The University of Tokyo"),
    assigned_number!("FE6F", "LINE Corporation"),
    assigned_number!("FE70", "Beijing Jingdong Century Trading Co., Ltd."),
    assigned_number!("FE71", "Plume Design Inc"),
    assigned_number!("FE72", "St. Jude Medical, Inc."),
    assigned_number!("FE73", "St. Jude Medical, Inc."),
    assigned_number!("FE74", "unwire"),
    assigned_number!("FE75", "TangoMe"),
    assigned_number!("FE76", "TangoMe"),
    assigned_number!("FE77", "Hewlett-Packard Company"),
    assigned_number!("FE78", "Hewlett-Packard Company"),
    assigned_number!("FE79", "Zebra Technologies"),
    assigned_number!("FE7A", "Bragi GmbH"),
    assigned_number!("FE7B", "Orion Labs, Inc."),
    assigned_number!("FE7C", "Telit Wireless Solutions (Formerly Stollmann E+V GmbH)"),
    assigned_number!("FE7D", "Aterica Health Inc."),
    assigned_number!("FE7E", "Awear Solutions Ltd"),
    assigned_number!("FE7F", "Doppler Lab"),
    assigned_number!("FE80", "Doppler Lab"),
    assigned_number!("FE81", "Medtronic Inc."),
    assigned_number!("FE82", "Medtronic Inc."),
    assigned_number!("FE83", "Blue Bite"),
    assigned_number!("FE84", "RF Digital Corp"),
    assigned_number!("FE85", "RF Digital Corp"),
    assigned_number!("FE86", "HUAWEI Technologies Co., Ltd. ( )"),
    assigned_number!("FE87", "Qingdao Yeelink Information Technology Co., Ltd. ( )"),
    assigned_number!("FE88", "SALTO SYSTEMS S.L."),
    assigned_number!("FE89", "B&O Play A/S"),
    assigned_number!("FE8A", "Apple, Inc."),
    assigned_number!("FE8B", "Apple, Inc."),
    assigned_number!("FE8C", "TRON Forum"),
    assigned_number!("FE8D", "Interaxon Inc."),
    assigned_number!("FE8E", "ARM Ltd"),
    assigned_number!("FE8F", "CSR"),
    assigned_number!("FE90", "JUMA"),
    assigned_number!("FE91", "Shanghai Imilab Technology Co.,Ltd"),
    assigned_number!("FE92", "Jarden Safety & Security"),
    assigned_number!("FE93", "OttoQ Inc."),
    assigned_number!("FE94", "OttoQ Inc."),
    assigned_number!("FE95", "Xiaomi Inc."),
    assigned_number!("FE96", "Tesla Motor Inc."),
    assigned_number!("FE97", "Tesla Motor Inc."),
    assigned_number!("FE98", "Currant, Inc."),
    assigned_number!("FE99", "Currant, Inc."),
    assigned_number!("FE9A", "Estimote"),
    assigned_number!("FE9B", "Samsara Networks, Inc"),
    assigned_number!("FE9C", "GSI Laboratories, Inc."),
    assigned_number!("FE9D", "Mobiquity Networks Inc"),
    assigned_number!("FE9E", "Dialog Semiconductor B.V."),
    assigned_number!("FE9F", "Google Inc."),
    assigned_number!("FEA0", "Google Inc."),
    assigned_number!("FEA1", "Intrepid Control Systems, Inc."),
    assigned_number!("FEA2", "Intrepid Control Systems, Inc."),
    assigned_number!("FEA3", "ITT Industries"),
    assigned_number!("FEA4", "Paxton Access Ltd"),
    assigned_number!("FEA5", "GoPro, Inc."),
    assigned_number!("FEA6", "GoPro, Inc."),
    assigned_number!("FEA7", "UTC Fire and Security"),
    assigned_number!("FEA8", "Savant Systems LLC"),
    assigned_number!("FEA9", "Savant Systems LLC"),
    assigned_number!("FEAA", "Google Inc."),
    assigned_number!("FEAB", "Nokia Corporation"),
    assigned_number!("FEAC", "Nokia Corporation"),
    assigned_number!("FEAD", "Nokia Corporation"),
    assigned_number!("FEAE", "Nokia Corporation"),
    assigned_number!("FEAF", "Nest Labs Inc."),
    assigned_number!("FEB0", "Nest Labs Inc."),
    assigned_number!("FEB1", "Electronics Tomorrow Limited"),
    assigned_number!("FEB2", "Microsoft Corporation"),
    assigned_number!("FEB3", "Taobao"),
    assigned_number!("FEB4", "WiSilica Inc."),
    assigned_number!("FEB5", "WiSilica Inc."),
    assigned_number!("FEB6", "Vencer Co, Ltd"),
    assigned_number!("FEB7", "Facebook, Inc."),
    assigned_number!("FEB8", "Facebook, Inc."),
    assigned_number!("FEB9", "LG Electronics"),
    assigned_number!("FEBA", "Tencent Holdings Limited"),
    assigned_number!("FEBB", "adafruit industries"),
    assigned_number!("FEBC", "Dexcom, Inc."),
    assigned_number!("FEBD", "Clover Network, Inc."),
    assigned_number!("FEBE", "Bose Corporation"),
    assigned_number!("FEBF", "Nod, Inc."),
    assigned_number!("FEC0", "KDDI Corporation"),
    assigned_number!("FEC1", "KDDI Corporation"),
    assigned_number!("FEC2", "Blue Spark Technologies, Inc."),
    assigned_number!("FEC3", "360fly, Inc."),
    assigned_number!("FEC4", "PLUS Location Systems"),
    assigned_number!("FEC5", "Realtek Semiconductor Corp."),
    assigned_number!("FEC6", "Kocomojo, LLC"),
    assigned_number!("FEC7", "Apple, Inc."),
    assigned_number!("FEC8", "Apple, Inc."),
    assigned_number!("FEC9", "Apple, Inc."),
    assigned_number!("FECA", "Apple, Inc."),
    assigned_number!("FECB", "Apple, Inc."),
    assigned_number!("FECC", "Apple, Inc."),
    assigned_number!("FECD", "Apple, Inc."),
    assigned_number!("FECE", "Apple, Inc."),
    assigned_number!("FECF", "Apple, Inc."),
    assigned_number!("FED0", "Apple, Inc."),
    assigned_number!("FED1", "Apple, Inc."),
    assigned_number!("FED2", "Apple, Inc."),
    assigned_number!("FED3", "Apple, Inc."),
    assigned_number!("FED4", "Apple, Inc."),
    assigned_number!("FED5", "Plantronics Inc."),
    assigned_number!("FED6", "Broadcom Corporation"),
    assigned_number!("FED7", "Broadcom Corporation"),
    assigned_number!("FED8", "Google Inc."),
    assigned_number!("FED9", "Pebble Technology Corporation"),
    assigned_number!("FEDA", "ISSC Technologies Corporation"),
    assigned_number!("FEDB", "Perka, Inc."),
    assigned_number!("FEDC", "Jawbone"),
    assigned_number!("FEDD", "Jawbone"),
    assigned_number!("FEDE", "Coin, Inc."),
    assigned_number!("FEDF", "Design SHIFT"),
    assigned_number!("FEE0", "Anhui Huami Information Technology Co."),
    assigned_number!("FEE1", "Anhui Huami Information Technology Co."),
    assigned_number!("FEE2", "Anki, Inc."),
    assigned_number!("FEE3", "Anki, Inc."),
    assigned_number!("FEE4", "Nordic Semiconductor ASA"),
    assigned_number!("FEE5", "Nordic Semiconductor ASA"),
    assigned_number!("FEE6", "Silvair, Inc."),
    assigned_number!("FEE7", "Tencent Holdings Limited"),
    assigned_number!("FEE8", "Quintic Corp."),
    assigned_number!("FEE9", "Quintic Corp."),
    assigned_number!("FEEA", "Swirl Networks, Inc."),
    assigned_number!("FEEB", "Swirl Networks, Inc."),
    assigned_number!("FEEC", "Tile, Inc."),
    assigned_number!("FEED", "Tile, Inc."),
    assigned_number!("FEEE", "Polar Electro Oy"),
    assigned_number!("FEEF", "Polar Electro Oy"),
    assigned_number!("FEF0", "Intel"),
    assigned_number!("FEF1", "CSR"),
    assigned_number!("FEF2", "CSR"),
    assigned_number!("FEF3", "Google Inc."),
    assigned_number!("FEF4", "Google Inc."),
    assigned_number!("FEF5", "Dialog Semiconductor GmbH"),
    assigned_number!("FEF6", "Wicentric, Inc."),
    assigned_number!("FEF7", "Aplix Corporation"),
    assigned_number!("FEF8", "Aplix Corporation"),
    assigned_number!("FEF9", "PayPal, Inc."),
    assigned_number!("FEFA", "PayPal, Inc."),
    assigned_number!("FEFB", "Telit Wireless Solutions (Formerly Stollmann E+V GmbH)"),
    assigned_number!("FEFC", "Gimbal, Inc."),
    assigned_number!("FEFD", "Gimbal, Inc."),
    assigned_number!("FEFE", "GN ReSound A/S"),
    assigned_number!("FEFF", "GN Netcom"),
];

pub(super) const CHARACTERISTIC_NUMBERS: [AssignedNumber; 226] = [
    assigned_number!("2A00", "Device Name"),
    assigned_number!("2A01", "Appearance"),
    assigned_number!("2A02", "Peripheral Privacy Flag"),
    assigned_number!("2A03", "Reconnection Address"),
    assigned_number!("2A04", "Peripheral Preferred Connection Parameters"),
    assigned_number!("2A05", "Service Changed"),
    assigned_number!("2A06", "Alert Level"),
    assigned_number!("2A07", "Tx Power Level"),
    assigned_number!("2A08", "Date Time"),
    assigned_number!("2A09", "Day of Week"),
    assigned_number!("2A0A", "Day Date Time"),
    assigned_number!("2A0B", "Exact Time 100"),
    assigned_number!("2A0C", "Exact Time 256"),
    assigned_number!("2A0D", "DST Offset"),
    assigned_number!("2A0E", "Time Zone"),
    assigned_number!("2A0F", "Local Time Information"),
    assigned_number!("2A10", "Secondary Time Zone"),
    assigned_number!("2A11", "Time with DST"),
    assigned_number!("2A12", "Time Accuracy"),
    assigned_number!("2A13", "Time Source"),
    assigned_number!("2A14", "Reference Time Information"),
    assigned_number!("2A15", "Time Broadcast"),
    assigned_number!("2A16", "Time Update Control Point"),
    assigned_number!("2A17", "Time Update State"),
    assigned_number!("2A18", "Glucose Measurement"),
    assigned_number!("2A19", "Battery Level"),
    assigned_number!("2A1A", "Battery Power State"),
    assigned_number!("2A1B", "Battery Level State"),
    assigned_number!("2A1C", "Temperature Measurement"),
    assigned_number!("2A1D", "Temperature Type"),
    assigned_number!("2A1E", "Intermediate Temperature"),
    assigned_number!("2A1F", "Temperature Celsius"),
    assigned_number!("2A20", "Temperature Fahrenheit"),
    assigned_number!("2A21", "Measurement Interval"),
    assigned_number!("2A22", "Boot Keyboard Input Report"),
    assigned_number!("2A23", "System ID"),
    assigned_number!("2A24", "Model Number String"),
    assigned_number!("2A25", "Serial Number String"),
    assigned_number!("2A26", "Firmware Revision String"),
    assigned_number!("2A27", "Hardware Revision String"),
    assigned_number!("2A28", "Software Revision String"),
    assigned_number!("2A29", "Manufacturer Name String"),
    assigned_number!("2A2A", "IEEE 11073-20601 Regulatory Certification Data List"),
    assigned_number!("2A2B", "Current Time"),
    assigned_number!("2A2C", "Magnetic Declination"),
    assigned_number!("2A2F", "Position 2D"),
    assigned_number!("2A30", "Position 3D"),
    assigned_number!("2A31", "Scan Refresh"),
    assigned_number!("2A32", "Boot Keyboard Output Report"),
    assigned_number!("2A33", "Boot Mouse Input Report"),
    assigned_number!("2A34", "Glucose Measurement Context"),
    assigned_number!("2A35", "Blood Pressure Measurement"),
    assigned_number!("2A36", "Intermediate Cuff Pressure"),
    assigned_number!("2A37", "Heart Rate Measurement"),
    assigned_number!("2A38", "Body Sensor Location"),
    assigned_number!("2A39", "Heart Rate Control Point"),
    assigned_number!("2A3A", "Removable"),
    assigned_number!("2A3B", "Service Required"),
    assigned_number!("2A3C", "Scientific Temperature Celsius"),
    assigned_number!("2A3D", "String"),
    assigned_number!("2A3E", "Network Availability"),
    assigned_number!("2A3F", "Alert Status"),
    assigned_number!("2A40", "Ringer Control point"),
    assigned_number!("2A41", "Ringer Setting"),
    assigned_number!("2A42", "Alert Category ID Bit Mask"),
    assigned_number!("2A43", "Alert Category ID"),
    assigned_number!("2A44", "Alert Notification Control Point"),
    assigned_number!("2A45", "Unread Alert Status"),
    assigned_number!("2A46", "New Alert"),
    assigned_number!("2A47", "Supported New Alert Category"),
    assigned_number!("2A48", "Supported Unread Alert Category"),
    assigned_number!("2A49", "Blood Pressure Feature"),
    assigned_number!("2A4A", "HID Information"),
    assigned_number!("2A4B", "Report Map"),
    assigned_number!("2A4C", "HID Control Point"),
    assigned_number!("2A4D", "Report"),
    assigned_number!("2A4E", "Protocol Mode"),
    assigned_number!("2A4F", "Scan Interval Window"),
    assigned_number!("2A50", "PnP ID"),
    assigned_number!("2A51", "Glucose Feature"),
    assigned_number!("2A52", "Record Access Control Point"),
    assigned_number!("2A53", "RSC Measurement"),
    assigned_number!("2A54", "RSC Feature"),
    assigned_number!("2A55", "SC Control Point"),
    assigned_number!("2A56", "Digital"),
    assigned_number!("2A57", "Digital Output"),
    assigned_number!("2A58", "Analog"),
    assigned_number!("2A59", "Analog Output"),
    assigned_number!("2A5A", "Aggregate"),
    assigned_number!("2A5B", "CSC Measurement"),
    assigned_number!("2A5C", "CSC Feature"),
    assigned_number!("2A5D", "Sensor Location"),
    assigned_number!("2A5E", "PLX Spot-Check Measurement"),
    assigned_number!("2A5F", "PLX Continuous Measurement Characteristic"),
    assigned_number!("2A60", "PLX Features"),
    assigned_number!("2A62", "Pulse Oximetry Control Point"),
    assigned_number!("2A63", "Cycling Power Measurement"),
    assigned_number!("2A64", "Cycling Power Vector"),
    assigned_number!("2A65", "Cycling Power Feature"),
    assigned_number!("2A66", "Cycling Power Control Point"),
    assigned_number!("2A67", "Location and Speed Characteristic"),
    assigned_number!("2A68", "Navigation"),
    assigned_number!("2A69", "Position Quality"),
    assigned_number!("2A6A", "LN Feature"),
    assigned_number!("2A6B", "LN Control Point"),
    assigned_number!("2A6C", "Elevation"),
    assigned_number!("2A6D", "Pressure"),
    assigned_number!("2A6E", "Temperature"),
    assigned_number!("2A6F", "Humidity"),
    assigned_number!("2A70", "True Wind Speed"),
    assigned_number!("2A71", "True Wind Direction"),
    assigned_number!("2A72", "Apparent Wind Speed"),
    assigned_number!("2A73", "Apparent Wind Direction"),
    assigned_number!("2A74", "Gust Factor"),
    assigned_number!("2A75", "Pollen Concentration"),
    assigned_number!("2A76", "UV Index"),
    assigned_number!("2A77", "Irradiance"),
    assigned_number!("2A78", "Rainfall"),
    assigned_number!("2A79", "Wind Chill"),
    assigned_number!("2A7A", "Heat Index"),
    assigned_number!("2A7B", "Dew Point"),
    assigned_number!("2A7D", "Descriptor Value Changed"),
    assigned_number!("2A7E", "Aerobic Heart Rate Lower Limit"),
    assigned_number!("2A7F", "Aerobic Threshold"),
    assigned_number!("2A80", "Age"),
    assigned_number!("2A81", "Anaerobic Heart Rate Lower Limit"),
    assigned_number!("2A82", "Anaerobic Heart Rate Upper Limit"),
    assigned_number!("2A83", "Anaerobic Threshold"),
    assigned_number!("2A84", "Aerobic Heart Rate Upper Limit"),
    assigned_number!("2A85", "Date of Birth"),
    assigned_number!("2A86", "Date of Threshold Assessment"),
    assigned_number!("2A87", "Email Address"),
    assigned_number!("2A88", "Fat Burn Heart Rate Lower Limit"),
    assigned_number!("2A89", "Fat Burn Heart Rate Upper Limit"),
    assigned_number!("2A8A", "First Name"),
    assigned_number!("2A8B", "Five Zone Heart Rate Limits"),
    assigned_number!("2A8C", "Gender"),
    assigned_number!("2A8D", "Heart Rate Max"),
    assigned_number!("2A8E", "Height"),
    assigned_number!("2A8F", "Hip Circumference"),
    assigned_number!("2A90", "Last Name"),
    assigned_number!("2A91", "Maximum Recommended Heart Rate"),
    assigned_number!("2A92", "Resting Heart Rate"),
    assigned_number!("2A93", "Sport Type for Aerobic and Anaerobic Thresholds"),
    assigned_number!("2A94", "Three Zone Heart Rate Limits"),
    assigned_number!("2A95", "Two Zone Heart Rate Limit"),
    assigned_number!("2A96", "VO2 Max"),
    assigned_number!("2A97", "Waist Circumference"),
    assigned_number!("2A98", "Weight"),
    assigned_number!("2A99", "Database Change Increment"),
    assigned_number!("2A9A", "User Index"),
    assigned_number!("2A9B", "Body Composition Feature"),
    assigned_number!("2A9C", "Body Composition Measurement"),
    assigned_number!("2A9D", "Weight Measurement"),
    assigned_number!("2A9E", "Weight Scale Feature"),
    assigned_number!("2A9F", "User Control Point"),
    assigned_number!("2AA0", "Magnetic Flux Density - 2D"),
    assigned_number!("2AA1", "Magnetic Flux Density - 3D"),
    assigned_number!("2AA2", "Language"),
    assigned_number!("2AA3", "Barometric Pressure Trend"),
    assigned_number!("2AA4", "Bond Management Control Point"),
    assigned_number!("2AA5", "Bond Management Features"),
    assigned_number!("2AA6", "Central Address Resolution"),
    assigned_number!("2AA7", "CGM Measurement"),
    assigned_number!("2AA8", "CGM Feature"),
    assigned_number!("2AA9", "CGM Status"),
    assigned_number!("2AAA", "CGM Session Start Time"),
    assigned_number!("2AAB", "CGM Session Run Time"),
    assigned_number!("2AAC", "CGM Specific Ops Control Point"),
    assigned_number!("2AAD", "Indoor Positioning Configuration"),
    assigned_number!("2AAE", "Latitude"),
    assigned_number!("2AAF", "Longitude"),
    assigned_number!("2AB0", "Local North Coordinate"),
    assigned_number!("2AB1", "Local East Coordinate"),
    assigned_number!("2AB2", "Floor Number"),
    assigned_number!("2AB3", "Altitude"),
    assigned_number!("2AB4", "Uncertainty"),
    assigned_number!("2AB5", "Location Name"),
    assigned_number!("2AB6", "URI"),
    assigned_number!("2AB7", "HTTP Headers"),
    assigned_number!("2AB8", "HTTP Status Code"),
    assigned_number!("2AB9", "HTTP Entity Body"),
    assigned_number!("2ABA", "HTTP Control Point"),
    assigned_number!("2ABB", "HTTPS Security"),
    assigned_number!("2ABC", "TDS Control Point"),
    assigned_number!("2ABD", "OTS Feature"),
    assigned_number!("2ABE", "Object Name"),
    assigned_number!("2ABF", "Object Type"),
    assigned_number!("2AC0", "Object Size"),
    assigned_number!("2AC1", "Object First-Created"),
    assigned_number!("2AC2", "Object Last-Modified"),
    assigned_number!("2AC3", "Object ID"),
    assigned_number!("2AC4", "Object Properties"),
    assigned_number!("2AC5", "Object Action Control Point"),
    assigned_number!("2AC6", "Object List Control Point"),
    assigned_number!("2AC7", "Object List Filter"),
    assigned_number!("2AC8", "Object Changed"),
    assigned_number!("2AC9", "Resolvable Private Address Only"),
    assigned_number!("2ACC", "Fitness Machine Feature"),
    assigned_number!("2ACD", "Treadmill Data"),
    assigned_number!("2ACE", "Cross Trainer Data"),
    assigned_number!("2ACF", "Step Climber Data"),
    assigned_number!("2AD0", "Stair Climber Data"),
    assigned_number!("2AD1", "Rower Data"),
    assigned_number!("2AD2", "Indoor Bike Data"),
    assigned_number!("2AD3", "Training Status"),
    assigned_number!("2AD4", "Supported Speed Range"),
    assigned_number!("2AD5", "Supported Inclination Range"),
    assigned_number!("2AD6", "Supported Resistance Level Range"),
    assigned_number!("2AD7", "Supported Heart Rate Range"),
    assigned_number!("2AD8", "Supported Power Range"),
    assigned_number!("2AD9", "Fitness Machine Control Point"),
    assigned_number!("2ADA", "Fitness Machine Status"),
    assigned_number!("2AED", "Date UTC"),
    assigned_number!("2B1D", "RC Feature"),
    assigned_number!("2B1E", "RC Settings"),
    assigned_number!("2B1F", "Reconnection Configuration Control Point"),
    assigned_number!("2B20", "IDD Status Changed"),
    assigned_number!("2B21", "IDD Status"),
    assigned_number!("2B22", "IDD Annunciation Status"),
    assigned_number!("2B23", "IDD Features"),
    assigned_number!("2B24", "IDD Status Reader Control Point"),
    assigned_number!("2B25", "IDD Command Control Point"),
    assigned_number!("2B26", "IDD Command Data"),
    assigned_number!("2B27", "IDD Record Access Control Point"),
    assigned_number!("2B28", "IDD History Data"),
];

pub(super) const DESCRIPTOR_NUMBERS: [AssignedNumber; 15] = [
    assigned_number!("2900", "Characteristic Extended Properties"),
    assigned_number!("2901", "Characteristic User Description"),
    assigned_number!("2902", "Client Characteristic Configuration"),
    assigned_number!("2903", "Server Characteristic Configuration"),
    assigned_number!("2904", "Characteristic Presentation Format"),
    assigned_number!("2905", "Characteristic Aggregate Format"),
    assigned_number!("2906", "Valid Range"),
    assigned_number!("2907", "External Report Reference"),
    assigned_number!("2908", "Report Reference"),
    assigned_number!("2909", "Number of Digitals"),
    assigned_number!("290A", "Value Trigger Setting"),
    assigned_number!("290B", "Environmental Sensing Configuration"),
    assigned_number!("290C", "Environmental Sensing Measurement"),
    assigned_number!("290D", "Environmental Sensing Trigger Setting"),
    assigned_number!("290E", "Time Trigger Setting"),
];
