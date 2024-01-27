// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Context as _,
    serde::{Deserialize, Serialize},
    std::io::{Seek, SeekFrom},
    std::{fs, io, path},
};

const INTERFACE_PREFIX_WLAN: &str = "wlan";
const INTERFACE_PREFIX_ETHERNET: &str = "eth";

#[derive(PartialEq, Eq, Serialize, Deserialize, Debug, Clone)]
enum PersistentIdentifier {
    MacAddress(fidl_fuchsia_net_ext::MacAddress),
    TopologicalPath(String),
}

// TODO(https://fxbug.dev/118197): Remove this once we soak for some time to ensure no
// devices using the iwlwifi driver are using the legacy config format.
#[derive(Serialize, Deserialize, Debug)]
struct LegacyConfig {
    names: Vec<(PersistentIdentifier, String)>,
}

#[derive(Serialize, Deserialize, Debug, PartialEq)]
struct InterfaceConfig {
    id: PersistentIdentifier,
    name: String,
    device_class: crate::InterfaceType,
}

#[derive(Serialize, Deserialize, Debug, PartialEq)]
struct Config {
    interfaces: Vec<InterfaceConfig>,
}

impl Config {
    fn load<R: io::Read>(reader: R) -> Result<Self, anyhow::Error> {
        serde_json::from_reader(reader).map_err(Into::into)
    }

    fn generate_identifier(
        &self,
        topological_path: std::borrow::Cow<'_, str>,
        mac_address: fidl_fuchsia_net_ext::MacAddress,
    ) -> PersistentIdentifier {
        if topological_path.contains("/pci-") {
            if topological_path.contains("/usb/") {
                PersistentIdentifier::MacAddress(mac_address)
            } else {
                PersistentIdentifier::TopologicalPath(topological_path.into_owned())
            }
        } else if topological_path.contains("/platform/") {
            PersistentIdentifier::TopologicalPath(topological_path.into_owned())
        } else {
            PersistentIdentifier::MacAddress(mac_address)
        }
    }

    fn lookup_by_identifier(&self, persistent_id: &PersistentIdentifier) -> Option<usize> {
        self.interfaces.iter().enumerate().find_map(|(i, interface)| {
            if &interface.id == persistent_id {
                Some(i)
            } else {
                None
            }
        })
    }

    // We use MAC addresses to identify USB devices; USB devices are those devices whose
    // topological path contains "/usb/". We use topological paths to identify on-board
    // devices; on-board devices are those devices whose topological path does not
    // contain "/usb". Topological paths of
    // both device types are expected to
    // contain "/pci-"; devices whose topological path does not contain "/pci-" are
    // identified by their MAC address.
    //
    // At the time of writing, typical topological paths appear similar to:
    //
    // PCI:
    // "/dev/pci-02:00.0-fidl/e1000/ethernet"
    //
    // USB:
    // "/dev/pci-00:14.0-fidl/xhci/usb/007/ifc-000/<snip>/wlan/wlan-ethernet/ethernet"
    // 00:14:0 following "/pci-" represents BDF (Bus Device Function)
    //
    // SDIO
    // "/dev/sys/platform/05:00:6/aml-sd-emmc/sdio/broadcom-wlanphy/wlanphy"
    // 05:00:6 following "platform" represents
    // vid(vendor id):pid(product id):did(device id) and are defined in each board file
    //
    // Ethernet Jack for VIM2
    // "/dev/sys/platform/04:02:7/aml-ethernet/Designware-MAC/ethernet"
    // Though it is not a sdio device, it has the vid:pid:did info following "/platform/",
    // it's handled the same way as a sdio device.
    fn generate_name_from_mac(
        &self,
        octets: [u8; 6],
        interface_type: crate::InterfaceType,
    ) -> Result<String, anyhow::Error> {
        let prefix = format!(
            "{}{}",
            match interface_type {
                crate::InterfaceType::Wlan => INTERFACE_PREFIX_WLAN,
                crate::InterfaceType::Ethernet => INTERFACE_PREFIX_ETHERNET,
            },
            "x"
        );

        let last_byte = octets[octets.len() - 1];
        for i in 0u8..255u8 {
            let candidate = ((last_byte as u16 + i as u16) % 256 as u16) as u8;
            if self.interfaces.iter().any(|interface| {
                interface.name.starts_with(&prefix)
                    && u8::from_str_radix(&interface.name[prefix.len()..], 16) == Ok(candidate)
            }) {
                continue; // if the candidate is used, try next one
            } else {
                return Ok(format!("{}{:x}", prefix, candidate));
            }
        }
        Err(anyhow::format_err!(
            "could not find unique name for mac={}, interface_type={:?}",
            fidl_fuchsia_net_ext::MacAddress { octets: octets },
            interface_type
        ))
    }

    fn generate_name_from_topological_path(
        &self,
        topological_path: &str,
        interface_type: crate::InterfaceType,
    ) -> Result<String, anyhow::Error> {
        let prefix = match interface_type {
            crate::InterfaceType::Wlan => INTERFACE_PREFIX_WLAN,
            crate::InterfaceType::Ethernet => INTERFACE_PREFIX_ETHERNET,
        };
        let (suffix, pat) =
            if topological_path.contains("/pci-") { ("p", "/pci-") } else { ("s", "/platform/") };

        let index = topological_path.find(pat).ok_or_else(|| {
            anyhow::format_err!(
                "unexpected topological path {}: {} is not found",
                topological_path,
                pat
            )
        })?;
        let topological_path = &topological_path[index + pat.len()..];
        let index = topological_path.find('/').ok_or_else(|| {
            anyhow::format_err!(
                "unexpected topological path suffix {}: '/' is not found after {}",
                topological_path,
                pat
            )
        })?;

        let mut name = format!("{}{}", prefix, suffix);
        for digit in topological_path[..index]
            .trim_end_matches(|c: char| !c.is_digit(16) || c == '0')
            .chars()
            .filter(|c| c.is_digit(16))
        {
            name.push(digit);
        }
        Ok(name)
    }

    fn generate_name(
        &self,
        persistent_id: &PersistentIdentifier,
        interface_type: crate::InterfaceType,
    ) -> Result<String, anyhow::Error> {
        match persistent_id {
            PersistentIdentifier::MacAddress(fidl_fuchsia_net_ext::MacAddress { octets }) => {
                self.generate_name_from_mac(*octets, interface_type)
            }
            PersistentIdentifier::TopologicalPath(ref topological_path) => {
                self.generate_name_from_topological_path(&topological_path, interface_type)
            }
        }
    }
}

/// Checks the interface and device class to check that they match.
// TODO(https://fxbug.dev/118197): Remove this function once we save the device class.
fn name_matches_interface_type(name: &str, interface_type: &crate::InterfaceType) -> bool {
    match interface_type {
        crate::InterfaceType::Wlan => name.starts_with(INTERFACE_PREFIX_WLAN),
        crate::InterfaceType::Ethernet => name.starts_with(INTERFACE_PREFIX_ETHERNET),
    }
}

#[derive(Debug)]
pub struct FileBackedConfig<'a> {
    path: &'a path::Path,
    config: Config,
    temp_id: u64,
}

impl<'a> FileBackedConfig<'a> {
    /// Loads the persistent/stable interface names from the backing file.
    pub fn load<P: AsRef<path::Path>>(path: &'a P) -> Result<Self, anyhow::Error> {
        let path = path.as_ref();
        let config = match fs::File::open(path) {
            Ok(mut file) => Config::load(&file).or_else(|_| {
                // Since deserialization as Config failed, try loading the file
                // as the legacy config format.
                let _seek = file.seek(SeekFrom::Start(0))?;
                let legacy_config: LegacyConfig = serde_json::from_reader(&file)?;
                // Transfer the values from the old format to the new format.
                let new_config = Config {
                    interfaces: legacy_config
                        .names
                        .into_iter()
                        .map(|(id, name)| InterfaceConfig {
                            id,
                            device_class: if name.starts_with(INTERFACE_PREFIX_WLAN) {
                                crate::InterfaceType::Wlan
                            } else {
                                crate::InterfaceType::Ethernet
                            },
                            name,
                        })
                        .collect::<Vec<_>>(),
                };
                Ok(new_config)
            }),
            Err(error) => {
                if error.kind() == io::ErrorKind::NotFound {
                    Ok(Config { interfaces: vec![] })
                } else {
                    Err(error)
                        .with_context(|| format!("could not open config file {}", path.display()))
                }
            }
        }?;
        Ok(Self { path, config, temp_id: 0 })
    }

    /// Stores the persistent/stable interface names to the backing file.
    pub fn store(&self) -> Result<(), anyhow::Error> {
        let Self { path, config, temp_id: _ } = self;
        let temp_file_path = match path.file_name() {
            None => Err(anyhow::format_err!("unexpected non-file path {}", path.display())),
            Some(file_name) => {
                let mut file_name = file_name.to_os_string();
                file_name.push(".tmp");
                Ok(path.with_file_name(file_name))
            }
        }?;
        {
            let temp_file = fs::File::create(&temp_file_path).with_context(|| {
                format!("could not create temporary file {}", temp_file_path.display())
            })?;
            serde_json::to_writer_pretty(temp_file, &config).with_context(|| {
                format!(
                    "could not serialize config into temporary file {}",
                    temp_file_path.display()
                )
            })?;
        }

        fs::rename(&temp_file_path, path).with_context(|| {
            format!(
                "could not rename temporary file {} to {}",
                temp_file_path.display(),
                path.display()
            )
        })?;
        Ok(())
    }

    /// Returns a stable interface name for the specified interface.
    pub(crate) fn generate_stable_name(
        &mut self,
        topological_path: std::borrow::Cow<'_, str>,
        mac_address: fidl_fuchsia_net_ext::MacAddress,
        interface_type: crate::InterfaceType,
    ) -> Result<&str, NameGenerationError<'_>> {
        let persistent_id = self.config.generate_identifier(topological_path, mac_address);

        if let Some(index) = self.config.lookup_by_identifier(&persistent_id) {
            let InterfaceConfig { name, .. } = &self.config.interfaces[index];
            if name_matches_interface_type(&name, &interface_type) {
                // Need to take a new reference to appease the borrow checker.
                let InterfaceConfig { name, .. } = &self.config.interfaces[index];
                return Ok(&name);
            }
            // Identifier was found in the vector, but device class does not match interface
            // name. Remove and generate a new name.
            let _interface_config = self.config.interfaces.remove(index);
        }
        let generated_name = self
            .config
            .generate_name(&persistent_id, interface_type)
            .map_err(NameGenerationError::GenerationError)?;
        let () = self.config.interfaces.push(InterfaceConfig {
            id: persistent_id,
            name: generated_name,
            device_class: interface_type,
        });
        let interface_config = &self.config.interfaces[self.config.interfaces.len() - 1];
        let () = self.store().map_err(|err| NameGenerationError::FileUpdateError {
            name: &interface_config.name,
            err,
        })?;
        Ok(&interface_config.name)
    }

    /// Returns a temporary name for an interface.
    pub(crate) fn generate_temporary_name(
        &mut self,
        interface_type: crate::InterfaceType,
    ) -> String {
        let id = self.temp_id;
        self.temp_id += 1;

        let prefix = match interface_type {
            crate::InterfaceType::Wlan => "wlant",
            crate::InterfaceType::Ethernet => "etht",
        };
        format!("{}{}", prefix, id)
    }
}

/// An error observed when generating a new name.
#[derive(Debug)]
pub enum NameGenerationError<'a> {
    GenerationError(anyhow::Error),
    FileUpdateError { name: &'a str, err: anyhow::Error },
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[derive(Clone)]
    struct TestCase {
        topological_path: &'static str,
        mac: [u8; 6],
        interface_type: crate::InterfaceType,
        want_name: &'static str,
    }

    #[test]
    fn test_generate_name() {
        let test_cases = vec![
            // usb interfaces
            TestCase {
                topological_path: "/dev/pci-00:14.0-fidl/xhci/usb/004/004/ifc-000/ax88179/ethernet",
                mac: [0x01, 0x01, 0x01, 0x01, 0x01, 0x01],
                interface_type: crate::InterfaceType::Wlan,
                want_name: "wlanx1",
            },
            TestCase {
                topological_path: "/dev/pci-00:15.0-fidl/xhci/usb/004/004/ifc-000/ax88179/ethernet",
                mac: [0x02, 0x02, 0x02, 0x02, 0x02, 0x02],
                interface_type: crate::InterfaceType::Ethernet,
                want_name: "ethx2",
            },
            // pci intefaces
            TestCase {
                topological_path: "/dev/pci-00:14.0/ethernet",
                mac: [0x03, 0x03, 0x03, 0x03, 0x03, 0x03],
                interface_type: crate::InterfaceType::Wlan,
                want_name: "wlanp0014",
            },
            TestCase {
                topological_path: "/dev/pci-00:15.0/ethernet",
                mac: [0x04, 0x04, 0x04, 0x04, 0x04, 0x04],
                interface_type: crate::InterfaceType::Ethernet,
                want_name: "ethp0015",
            },
            // platform interfaces (ethernet jack and sdio devices)
            TestCase {
                topological_path:
                    "/dev/sys/platform/05:00:6/aml-sd-emmc/sdio/broadcom-wlanphy/wlanphy",
                mac: [0x05, 0x05, 0x05, 0x05, 0x05, 0x05],
                interface_type: crate::InterfaceType::Wlan,
                want_name: "wlans05006",
            },
            TestCase {
                topological_path: "/dev/sys/platform/04:02:7/aml-ethernet/Designware-MAC/ethernet",
                mac: [0x07, 0x07, 0x07, 0x07, 0x07, 0x07],
                interface_type: crate::InterfaceType::Ethernet,
                want_name: "eths04027",
            },
            // unknown interfaces
            TestCase {
                topological_path: "/dev/sys/unknown",
                mac: [0x08, 0x08, 0x08, 0x08, 0x08, 0x08],
                interface_type: crate::InterfaceType::Wlan,
                want_name: "wlanx8",
            },
            TestCase {
                topological_path: "unknown",
                mac: [0x09, 0x09, 0x09, 0x09, 0x09, 0x09],
                interface_type: crate::InterfaceType::Wlan,
                want_name: "wlanx9",
            },
        ];
        let config = Config { interfaces: vec![] };
        for TestCase { topological_path, mac, interface_type, want_name } in test_cases.into_iter()
        {
            let persistent_id = config.generate_identifier(
                topological_path.into(),
                fidl_fuchsia_net_ext::MacAddress { octets: mac },
            );
            let name = config
                .generate_name(&persistent_id, interface_type)
                .expect("failed to generate the name");
            assert_eq!(name, want_name);
        }
    }

    #[test]
    fn test_generate_stable_name() {
        #[derive(Clone)]
        struct FileBackedConfigTestCase {
            topological_path: &'static str,
            mac: [u8; 6],
            interface_type: crate::InterfaceType,
            want_name: &'static str,
            expected_size: usize,
        }

        let test_cases = vec![
            // Base case.
            FileBackedConfigTestCase {
                topological_path: "/dev/pci-00:14.0/ethernet",
                mac: [0x01, 0x01, 0x01, 0x01, 0x01, 0x01],
                interface_type: crate::InterfaceType::Wlan,
                want_name: "wlanp0014",
                expected_size: 1,
            },
            // Same topological path as the base case, different MAC address.
            FileBackedConfigTestCase {
                topological_path: "/dev/pci-00:14.0/ethernet",
                mac: [0xFE, 0x01, 0x01, 0x01, 0x01, 0x01],
                interface_type: crate::InterfaceType::Wlan,
                want_name: "wlanp0014",
                expected_size: 1,
            },
            // Test case that labels iwilwifi as ethernet.
            FileBackedConfigTestCase {
                topological_path: "/dev/sys/platform/platform-passthrough/PCI0/bus/01:00.0_\
/pci-01:00.0-fidl/iwlwifi-wlan-softmac/wlan-ethernet/ethernet",
                mac: [0x01, 0x01, 0x01, 0x01, 0x01, 0x01],
                interface_type: crate::InterfaceType::Ethernet,
                want_name: "ethp01000fd",
                expected_size: 2,
            },
            // Test case that changes the previous test case's device class to wlan.
            // The test should detect that the device class doesn't match the interface
            // name, and overwrite with the new interface name that does match.
            FileBackedConfigTestCase {
                topological_path: "/dev/sys/platform/platform-passthrough/PCI0/bus/01:00.0_\
/pci-01:00.0-fidl/iwlwifi-wlan-softmac/wlan-ethernet/ethernet",
                mac: [0x01, 0x01, 0x01, 0x01, 0x01, 0x01],
                interface_type: crate::InterfaceType::Wlan,
                want_name: "wlanp01000fd",
                expected_size: 2,
            },
        ];

        let temp_dir = tempfile::tempdir_in("/tmp").expect("failed to create the temp dir");
        let path = temp_dir.path().join("net.config.json");

        // query an existing interface with the same topo path and a different mac address
        for (
            _i,
            FileBackedConfigTestCase {
                topological_path,
                mac,
                interface_type,
                want_name,
                expected_size,
            },
        ) in test_cases.into_iter().enumerate()
        {
            let mut interface_config =
                FileBackedConfig::load(&path).expect("failed to load the interface config");

            let name = interface_config
                .generate_stable_name(
                    topological_path.into(),
                    fidl_fuchsia_net_ext::MacAddress { octets: mac },
                    interface_type,
                )
                .expect("failed to get the interface name");
            assert_eq!(name, want_name);
            // Load the file again to ensure that generate_stable_name saved correctly.
            interface_config =
                FileBackedConfig::load(&path).expect("failed to load the interface config");
            assert_eq!(interface_config.config.interfaces.len(), expected_size);
        }
    }

    #[test]
    fn test_generate_temporary_name() {
        let temp_dir = tempfile::tempdir_in("/tmp").expect("failed to create the temp dir");
        let path = temp_dir.path().join("net.config.json");
        let mut interface_config =
            FileBackedConfig::load(&path).expect("failed to load the interface config");
        assert_eq!(
            &interface_config.generate_temporary_name(crate::InterfaceType::Ethernet),
            "etht0"
        );
        assert_eq!(&interface_config.generate_temporary_name(crate::InterfaceType::Wlan), "wlant1");
    }

    #[test]
    fn test_get_usb_255() {
        let topo_usb = "/dev/pci-00:14.0-fidl/xhci/usb/004/004/ifc-000/ax88179/ethernet";

        // test cases for 256 usb interfaces
        let mut config = Config { interfaces: vec![] };
        for n in 0u8..255u8 {
            let octets = [n, 0x01, 0x01, 0x01, 0x01, 00];

            let persistent_id = config
                .generate_identifier(topo_usb.into(), fidl_fuchsia_net_ext::MacAddress { octets });

            if let Some(index) = config.lookup_by_identifier(&persistent_id) {
                assert_eq!(config.interfaces[index].name, format!("{}{:x}", "wlanx", n));
            } else {
                let name = config
                    .generate_name(&persistent_id, crate::InterfaceType::Wlan)
                    .expect("failed to generate the name");
                assert_eq!(name, format!("{}{:x}", "wlanx", n));
                config.interfaces.push(InterfaceConfig {
                    id: persistent_id,
                    name: name,
                    device_class: crate::InterfaceType::Ethernet,
                });
            }
        }
        let octets = [0x00, 0x00, 0x01, 0x01, 0x01, 00];
        let persistent_id = config
            .generate_identifier(topo_usb.into(), fidl_fuchsia_net_ext::MacAddress { octets });
        assert!(config.generate_name(&persistent_id, crate::InterfaceType::Wlan).is_err());
    }

    #[test]
    fn test_load_malformed_file() {
        let temp_dir = tempfile::tempdir_in("/tmp").expect("failed to create the temp dir");
        let path = temp_dir.path().join("net.config.json");
        {
            let mut file = fs::File::create(&path).expect("failed to open file for writing");
            // Write invalid JSON and close the file
            let () = file.write_all(b"{").expect("failed to write broken json into file");
        }
        assert_eq!(
            FileBackedConfig::load(&path)
                .unwrap_err()
                .downcast_ref::<serde_json::error::Error>()
                .unwrap()
                .classify(),
            serde_json::error::Category::Eof
        );
    }

    #[test]
    fn test_store_nonexistent_path() {
        let interface_config = FileBackedConfig::load(&"not/a/real/path")
            .expect("failed to load the interface config");
        assert_eq!(
            interface_config.store().unwrap_err().downcast_ref::<io::Error>().unwrap().kind(),
            io::ErrorKind::NotFound
        );
    }

    #[test]
    fn test_load_legacy_config_file() {
        const ETHERNET_TOPO_PATH: &str =
            "/dev/pci-00:15.0-fidl/xhci/usb/004/004/ifc-000/ax88179/ethernet";
        const ETHERNET_NAME: &str = "ethx2";
        const WLAN_TOPO_PATH: &str = "/dev/pci-00:14.0/ethernet";
        const WLAN_NAME: &str = "wlanp0014";
        let test_config = LegacyConfig {
            names: vec![
                (
                    PersistentIdentifier::TopologicalPath(ETHERNET_TOPO_PATH.to_string()),
                    ETHERNET_NAME.to_string(),
                ),
                (
                    PersistentIdentifier::TopologicalPath(WLAN_TOPO_PATH.to_string()),
                    WLAN_NAME.to_string(),
                ),
            ],
        };

        let temp_dir = tempfile::tempdir_in("/tmp").expect("failed to create the temp dir");
        let path = temp_dir.path().join("net.config.json");
        {
            let file = fs::File::create(&path).expect("failed to open file for writing");

            serde_json::to_writer_pretty(file, &test_config)
                .expect("could not serialize config into temporary file");

            let new_config =
                FileBackedConfig::load(&path).expect("failed to load the interface config");
            assert_eq!(
                new_config.config,
                Config {
                    interfaces: vec![
                        InterfaceConfig {
                            id: PersistentIdentifier::TopologicalPath(
                                ETHERNET_TOPO_PATH.to_string()
                            ),
                            name: ETHERNET_NAME.to_string(),
                            device_class: crate::InterfaceType::Ethernet
                        },
                        InterfaceConfig {
                            id: PersistentIdentifier::TopologicalPath(WLAN_TOPO_PATH.to_string()),
                            name: WLAN_NAME.to_string(),
                            device_class: crate::InterfaceType::Wlan
                        },
                    ]
                }
            )
        }
    }
}
