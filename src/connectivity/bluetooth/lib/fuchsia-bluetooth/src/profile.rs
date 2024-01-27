// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_bluetooth as fidl_bt;
use fidl_fuchsia_bluetooth_bredr::{
    self as fidl_bredr, ProfileDescriptor, ATTR_BLUETOOTH_PROFILE_DESCRIPTOR_LIST,
    ATTR_SERVICE_CLASS_ID_LIST,
};
use fidl_table_validation::ValidFidlTable;
use fuchsia_inspect as inspect;
use fuchsia_inspect_derive::{AttachError, Inspect, Unit};
use std::cmp::min;
use std::collections::HashSet;
use std::convert::{TryFrom, TryInto};

use crate::assigned_numbers::{constants::SERVICE_CLASS_UUIDS, AssignedNumber};
use crate::error::Error;
use crate::types::Uuid;

/// The Protocol and Service Multiplexer (PSM) for L2cap connections.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct Psm(u16);

impl Psm {
    /// PSMs commonly used in the codebase.
    pub const RFCOMM: Self = Self(fidl_bredr::PSM_RFCOMM);
    pub const HID_CONTROL: Self = Self(fidl_bredr::PSM_HID_CONTROL);
    pub const HID_INTERRUPT: Self = Self(fidl_bredr::PSM_HID_INTERRUPT);
    pub const AVDTP: Self = Self(fidl_bredr::PSM_AVDTP);
    pub const AVCTP: Self = Self(fidl_bredr::PSM_AVCTP);
    pub const AVCTP_BROWSE: Self = Self(fidl_bredr::PSM_AVCTP_BROWSE);

    pub fn new(value: u16) -> Self {
        Self(value)
    }
}

impl From<Psm> for u16 {
    fn from(src: Psm) -> u16 {
        src.0
    }
}

/// Try to interpret a DataElement as a ProfileDesciptor.
/// Returns None if the DataElement is not in the correct format to represent a ProfileDescriptor.
pub fn elem_to_profile_descriptor(elem: &fidl_bredr::DataElement) -> Option<ProfileDescriptor> {
    if let fidl_bredr::DataElement::Sequence(seq) = elem {
        if seq.len() != 2 {
            return None;
        }

        if seq[0].is_none() {
            return None;
        }
        let profile_id = match **seq[0].as_ref().expect("not none") {
            fidl_bredr::DataElement::Uuid(uuid) => {
                let uuid: Uuid = uuid.into();
                match uuid.try_into() {
                    Err(_) => return None,
                    Ok(profile_id) => profile_id,
                }
            }
            _ => return None,
        };

        if seq[1].is_none() {
            return None;
        }
        let [major_version, minor_version] = match **seq[1].as_ref().expect("not none") {
            fidl_bredr::DataElement::Uint16(val) => val.to_be_bytes(),
            _ => return None,
        };
        return Some(ProfileDescriptor { profile_id, major_version, minor_version });
    }
    None
}

/// Find an element representing the Bluetooth Profile Descriptor List in `attributes`, and
/// convert the elements in the list into ProfileDescriptors.
/// Returns an Error if no matching element was found, or if any element of the list couldn't be converted
/// into a ProfileDescriptor.
pub fn find_profile_descriptors(
    attributes: &[fidl_bredr::Attribute],
) -> Result<Vec<ProfileDescriptor>, Error> {
    let attr = attributes
        .iter()
        .find(|a| a.id == ATTR_BLUETOOTH_PROFILE_DESCRIPTOR_LIST)
        .ok_or(Error::profile("missing profile descriptor"))?;

    let fidl_bredr::DataElement::Sequence(profiles) = &attr.element else {
        return Err(Error::profile("attribute element is invalidly formatted"));
    };
    let mut result = Vec::new();
    for elem in profiles {
        let elem = elem.as_ref().ok_or(Error::profile("null DataElement in sequence"))?;
        result.push(
            elem_to_profile_descriptor(&*elem)
                .ok_or(Error::profile("couldn't convert to a ProfileDescriptor"))?,
        );
    }
    if result.is_empty() {
        Err(Error::profile("no profile descriptor found"))
    } else {
        Ok(result)
    }
}

pub fn profile_descriptor_to_assigned(profile_desc: &ProfileDescriptor) -> Option<AssignedNumber> {
    SERVICE_CLASS_UUIDS.iter().find(|scn| profile_desc.profile_id as u16 == scn.number).cloned()
}

/// Returns the PSM from the provided `protocol`. Returns None if the protocol
/// is not L2CAP or does not contain a PSM.
pub fn psm_from_protocol(protocol: &Vec<ProtocolDescriptor>) -> Option<Psm> {
    for descriptor in protocol {
        if descriptor.protocol == fidl_bredr::ProtocolIdentifier::L2Cap {
            if descriptor.params.len() != 1 {
                return None;
            }

            if let DataElement::Uint16(psm) = descriptor.params[0] {
                return Some(Psm::new(psm));
            }
            return None;
        }
    }
    None
}

/// Search for a Service Class UUID from a list of attributes (such as returned via Service Search)
pub fn find_service_classes(
    attributes: &[fidl_fuchsia_bluetooth_bredr::Attribute],
) -> Vec<AssignedNumber> {
    let attr = match attributes.iter().find(|a| a.id == ATTR_SERVICE_CLASS_ID_LIST) {
        None => return vec![],
        Some(attr) => attr,
    };
    if let fidl_fuchsia_bluetooth_bredr::DataElement::Sequence(elems) = &attr.element {
        let uuids: Vec<Uuid> = elems
            .iter()
            .filter_map(|e| {
                e.as_ref().and_then(|e| {
                    if let fidl_fuchsia_bluetooth_bredr::DataElement::Uuid(uuid) = **e {
                        Some(uuid.into())
                    } else {
                        None
                    }
                })
            })
            .collect();
        SERVICE_CLASS_UUIDS
            .iter()
            .filter(|scn| uuids.contains(&Uuid::new16(scn.number)))
            .cloned()
            .collect()
    } else {
        return vec![];
    }
}

/// Given two SecurityRequirements, combines both into requirements as strict as either.
/// A stricter SecurityRequirements is defined as:
///   1) Authentication required is stricter than not.
///   2) Secure Connections required is stricter than not.
pub fn combine_security_requirements(
    reqs: &SecurityRequirements,
    other: &SecurityRequirements,
) -> SecurityRequirements {
    let authentication_required =
        match (reqs.authentication_required, other.authentication_required) {
            (Some(true), _) | (_, Some(true)) => Some(true),
            (Some(x), None) | (None, Some(x)) => Some(x),
            _ => None,
        };
    let secure_connections_required =
        match (reqs.secure_connections_required, other.secure_connections_required) {
            (Some(true), _) | (_, Some(true)) => Some(true),
            (Some(x), None) | (None, Some(x)) => Some(x),
            _ => None,
        };
    SecurityRequirements { authentication_required, secure_connections_required }
}

/// Given two ChannelParameters, combines both into a set of ChannelParameters
/// with the least requesting of resources.
/// This is defined as:
///   1) Basic requires fewer resources than ERTM.
///   2) A smaller SDU size is more restrictive.
pub fn combine_channel_parameters(
    params: &ChannelParameters,
    other: &ChannelParameters,
) -> ChannelParameters {
    let channel_mode = match (params.channel_mode, other.channel_mode) {
        (Some(fidl_bredr::ChannelMode::Basic), _) | (_, Some(fidl_bredr::ChannelMode::Basic)) => {
            Some(fidl_bredr::ChannelMode::Basic)
        }
        (Some(x), None) | (None, Some(x)) => Some(x),
        _ => None,
    };
    let max_rx_sdu_size = match (params.max_rx_sdu_size, other.max_rx_sdu_size) {
        (Some(rx1), Some(rx2)) => Some(min(rx1, rx2)),
        (Some(x), None) | (None, Some(x)) => Some(x),
        _ => None,
    };
    let security_requirements = match (&params.security_requirements, &other.security_requirements)
    {
        (Some(reqs1), Some(reqs2)) => Some(combine_security_requirements(reqs1, reqs2)),
        (Some(reqs), _) | (_, Some(reqs)) => Some(reqs.clone()),
        _ => None,
    };
    ChannelParameters { channel_mode, max_rx_sdu_size, security_requirements }
}

/// The basic building block for elements in a SDP record.
/// Corresponds directly to the FIDL `DataElement` definition - with the extra
/// properties of Clone and PartialEq.
/// See [fuchsia.bluetooth.bredr.DataElement] for more documentation.
#[derive(Clone, Debug, PartialEq)]
pub enum DataElement {
    Int8(i8),
    Int16(i16),
    Int32(i32),
    Int64(i64),
    Uint8(u8),
    Uint16(u16),
    Uint32(u32),
    Uint64(u64),
    Str(Vec<u8>),
    Url(String),
    Uuid(fidl_bt::Uuid),
    Bool(bool),
    Sequence(Vec<Box<DataElement>>),
    Alternatives(Vec<Box<DataElement>>),
}

impl From<&fidl_bredr::DataElement> for DataElement {
    fn from(src: &fidl_bredr::DataElement) -> DataElement {
        use fidl_bredr::DataElement as fDataElement;
        match src {
            fDataElement::Int8(x) => DataElement::Int8(*x),
            fDataElement::Int16(x) => DataElement::Int16(*x),
            fDataElement::Int32(x) => DataElement::Int32(*x),
            fDataElement::Int64(x) => DataElement::Int64(*x),
            fDataElement::Uint8(x) => DataElement::Uint8(*x),
            fDataElement::Uint16(x) => DataElement::Uint16(*x),
            fDataElement::Uint32(x) => DataElement::Uint32(*x),
            fDataElement::Uint64(x) => DataElement::Uint64(*x),
            // TODO(fxb/107513) Replace clones with moves where possible.
            fDataElement::Str(v) => DataElement::Str(v.clone()),
            fDataElement::Url(s) => DataElement::Url(s.to_string()),
            fDataElement::Uuid(uuid) => DataElement::Uuid(uuid.clone()),
            fDataElement::B(b) => DataElement::Bool(*b),
            fDataElement::Sequence(x) => {
                let mapped = x
                    .into_iter()
                    .filter_map(|opt| opt.as_ref().map(|t| Box::new(DataElement::from(&**t))))
                    .collect::<Vec<_>>();
                DataElement::Sequence(mapped)
            }
            fDataElement::Alternatives(x) => {
                let mapped = x
                    .into_iter()
                    .filter_map(|opt| opt.as_ref().map(|t| Box::new(DataElement::from(&**t))))
                    .collect::<Vec<_>>();
                DataElement::Alternatives(mapped)
            }
        }
    }
}

impl From<&DataElement> for fidl_bredr::DataElement {
    fn from(src: &DataElement) -> fidl_bredr::DataElement {
        use fidl_bredr::DataElement as fDataElement;
        match src {
            DataElement::Int8(x) => fDataElement::Int8(*x),
            DataElement::Int16(x) => fDataElement::Int16(*x),
            DataElement::Int32(x) => fDataElement::Int32(*x),
            DataElement::Int64(x) => fDataElement::Int64(*x),
            DataElement::Uint8(x) => fDataElement::Uint8(*x),
            DataElement::Uint16(x) => fDataElement::Uint16(*x),
            DataElement::Uint32(x) => fDataElement::Uint32(*x),
            DataElement::Uint64(x) => fDataElement::Uint64(*x),
            DataElement::Str(v) => fDataElement::Str(v.clone()),
            DataElement::Url(s) => fDataElement::Url(s.to_string()),
            DataElement::Uuid(uuid) => fDataElement::Uuid(uuid.clone()),
            DataElement::Bool(b) => fDataElement::B(*b),
            DataElement::Sequence(x) => {
                let mapped = x
                    .into_iter()
                    .map(|t| Some(Box::new(fDataElement::from(&**t))))
                    .collect::<Vec<_>>();
                fDataElement::Sequence(mapped)
            }
            DataElement::Alternatives(x) => {
                let mapped = x
                    .into_iter()
                    .map(|t| Some(Box::new(fDataElement::from(&**t))))
                    .collect::<Vec<_>>();
                fDataElement::Alternatives(mapped)
            }
        }
    }
}

#[derive(Debug, PartialEq)]
pub struct DataElementConversionError {
    pub data_element: DataElement,
}

// Macro for generating impls for converting between rust types and their DataElement wrappers.
macro_rules! generate_data_element_conversion {
    ($variant: ident, $type: ty) => {
        impl TryFrom<DataElement> for $type {
            type Error = DataElementConversionError;

            fn try_from(data_element: DataElement) -> Result<$type, DataElementConversionError> {
                match data_element {
                    DataElement::$variant(x) => Ok(x),
                    _ => Err(DataElementConversionError { data_element }),
                }
            }
        }

        impl From<$type> for DataElement {
            fn from(x: $type) -> DataElement {
                DataElement::$variant(x)
            }
        }
    };
}

// Generate the impls for converting between rust types and their DataElement wrappers.
generate_data_element_conversion!(Int8, i8);
generate_data_element_conversion!(Int16, i16);
generate_data_element_conversion!(Int32, i32);
generate_data_element_conversion!(Int64, i64);
generate_data_element_conversion!(Uint8, u8);
generate_data_element_conversion!(Uint16, u16);
generate_data_element_conversion!(Uint32, u32);
generate_data_element_conversion!(Uint64, u64);
generate_data_element_conversion!(Str, Vec<u8>);
generate_data_element_conversion!(Uuid, fidl_bt::Uuid);
generate_data_element_conversion!(Url, String);
generate_data_element_conversion!(Bool, bool);

/// Information about a communications protocol.
/// Corresponds directly to the FIDL `ProtocolDescriptor` definition - with the extra
/// properties of Clone and PartialEq.
/// See [fuchsia.bluetooth.bredr.ProtocolDescriptor] for more documentation.
#[derive(Clone, Debug, PartialEq)]
pub struct ProtocolDescriptor {
    pub protocol: fidl_bredr::ProtocolIdentifier,
    pub params: Vec<DataElement>,
}

impl From<&fidl_bredr::ProtocolDescriptor> for ProtocolDescriptor {
    fn from(src: &fidl_bredr::ProtocolDescriptor) -> ProtocolDescriptor {
        let params = src.params.iter().map(|elem| DataElement::from(elem)).collect();
        ProtocolDescriptor { protocol: src.protocol, params }
    }
}

impl From<&ProtocolDescriptor> for fidl_bredr::ProtocolDescriptor {
    fn from(src: &ProtocolDescriptor) -> fidl_bredr::ProtocolDescriptor {
        let params = src.params.iter().map(|elem| fidl_bredr::DataElement::from(elem)).collect();
        fidl_bredr::ProtocolDescriptor { protocol: src.protocol, params }
    }
}

pub fn l2cap_connect_parameters(
    psm: Psm,
    mode: fidl_bredr::ChannelMode,
) -> fidl_bredr::ConnectParameters {
    fidl_bredr::ConnectParameters::L2cap(fidl_bredr::L2capParameters {
        psm: Some(psm.into()),
        parameters: Some(fidl_bredr::ChannelParameters {
            channel_mode: Some(mode),
            ..fidl_bredr::ChannelParameters::default()
        }),
        ..fidl_bredr::L2capParameters::default()
    })
}

/// A generic attribute used for protocol information.
/// Corresponds directly to the FIDL `Attribute` definition - with the extra
/// properties of Clone and PartialEq.
/// See [fuchsia.bluetooth.bredr.Attribute] for more documentation.
#[derive(Clone, Debug, PartialEq)]
pub struct Attribute {
    pub id: u16,
    pub element: DataElement,
}

impl From<&fidl_bredr::Attribute> for Attribute {
    fn from(src: &fidl_bredr::Attribute) -> Attribute {
        Attribute { id: src.id, element: DataElement::from(&src.element) }
    }
}

impl From<&Attribute> for fidl_bredr::Attribute {
    fn from(src: &Attribute) -> fidl_bredr::Attribute {
        fidl_bredr::Attribute { id: src.id, element: fidl_bredr::DataElement::from(&src.element) }
    }
}

/// Human-readable information about a service.
/// Corresponds directly to the FIDL `Information` definition - with the extra
/// properties of Clone and PartialEq.
/// See [fuchsia.bluetooth.bredr.Information] for more documentation.
#[derive(Clone, Debug, PartialEq)]
pub struct Information {
    pub language: String,
    pub name: Option<String>,
    pub description: Option<String>,
    pub provider: Option<String>,
}

impl TryFrom<&fidl_bredr::Information> for Information {
    type Error = Error;

    fn try_from(src: &fidl_bredr::Information) -> Result<Information, Self::Error> {
        let language = match src.language.as_ref().map(String::as_str) {
            None | Some("") => return Err(Error::missing("bredr.Information.language")),
            Some(l) => l.to_string().clone(),
        };

        Ok(Information {
            language,
            name: src.name.clone(),
            description: src.description.clone(),
            provider: src.provider.clone(),
        })
    }
}

impl TryFrom<&Information> for fidl_bredr::Information {
    type Error = Error;

    fn try_from(src: &Information) -> Result<fidl_bredr::Information, Self::Error> {
        if src.language.is_empty() {
            return Err(Error::missing("Information.language"));
        }

        Ok(fidl_bredr::Information {
            language: Some(src.language.clone()),
            name: src.name.clone(),
            description: src.description.clone(),
            provider: src.provider.clone(),
            ..Default::default()
        })
    }
}

/// Definition of a service that is to be advertised via Bluetooth BR/EDR.
/// Corresponds directly to the FIDL `ServiceDefinition` definition - with the extra
/// properties of Clone and PartialEq.
/// See [fuchsia.bluetooth.bredr.ServiceDefinition] for more documentation.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ServiceDefinition {
    pub service_class_uuids: Vec<Uuid>,
    pub protocol_descriptor_list: Vec<ProtocolDescriptor>,
    pub additional_protocol_descriptor_lists: Vec<Vec<ProtocolDescriptor>>,
    pub profile_descriptors: Vec<fidl_bredr::ProfileDescriptor>,
    pub information: Vec<Information>,
    pub additional_attributes: Vec<Attribute>,
}

impl ServiceDefinition {
    /// Returns the primary PSM associated with this ServiceDefinition.
    pub fn primary_psm(&self) -> Option<Psm> {
        psm_from_protocol(&self.protocol_descriptor_list)
    }

    /// Returns the additional PSMs associated with this ServiceDefinition.
    pub fn additional_psms(&self) -> HashSet<Psm> {
        self.additional_protocol_descriptor_lists
            .iter()
            .filter_map(|protocol| psm_from_protocol(protocol))
            .collect()
    }

    /// Returns all the PSMs associated with this ServiceDefinition.
    ///
    /// It's possible that the definition doesn't provide any PSMs, in which
    /// case the returned set will be empty.
    pub fn psm_set(&self) -> HashSet<Psm> {
        let mut psms = self.additional_psms();
        if let Some(psm) = self.primary_psm() {
            let _ = psms.insert(psm);
        }
        psms
    }
}

impl TryFrom<&fidl_bredr::ServiceDefinition> for ServiceDefinition {
    type Error = Error;

    fn try_from(src: &fidl_bredr::ServiceDefinition) -> Result<ServiceDefinition, Self::Error> {
        let service_class_uuids = match &src.service_class_uuids {
            Some(uuids) if !uuids.is_empty() => uuids.iter().map(Uuid::from).collect(),
            _ => {
                return Err(Error::conversion(
                    "bredr.ServiceDefinition.service_class_uuids is empty",
                ))
            }
        };

        let protocol_descriptor_list: Vec<ProtocolDescriptor> = src
            .protocol_descriptor_list
            .as_ref()
            .map_or(vec![], |p| p.into_iter().map(|d| ProtocolDescriptor::from(d)).collect());
        let additional_protocol_descriptor_lists: Vec<Vec<ProtocolDescriptor>> =
            src.additional_protocol_descriptor_lists.as_ref().map_or(vec![], |desc_lists| {
                desc_lists
                    .into_iter()
                    .map(|desc_list| {
                        desc_list.into_iter().map(|d| ProtocolDescriptor::from(d)).collect()
                    })
                    .collect()
            });
        let profile_descriptors: Vec<fidl_bredr::ProfileDescriptor> =
            src.profile_descriptors.clone().unwrap_or(vec![]);
        let information: Result<Vec<Information>, Error> =
            src.information.as_ref().map_or(Ok(vec![]), |infos| {
                infos.into_iter().map(|i| Information::try_from(i)).collect()
            });
        let additional_attributes: Vec<Attribute> = src
            .additional_attributes
            .as_ref()
            .map_or(vec![], |attrs| attrs.into_iter().map(|a| Attribute::from(a)).collect());

        Ok(ServiceDefinition {
            service_class_uuids,
            protocol_descriptor_list,
            additional_protocol_descriptor_lists,
            profile_descriptors,
            information: information?,
            additional_attributes,
        })
    }
}

impl TryFrom<&ServiceDefinition> for fidl_bredr::ServiceDefinition {
    type Error = Error;

    fn try_from(src: &ServiceDefinition) -> Result<fidl_bredr::ServiceDefinition, Self::Error> {
        if src.service_class_uuids.is_empty() {
            return Err(Error::conversion("ServiceDefinitions.service_class_uuids is empty"));
        }
        let service_class_uuids = src.service_class_uuids.iter().map(fidl_bt::Uuid::from).collect();

        let protocol_descriptor_list: Vec<fidl_bredr::ProtocolDescriptor> = src
            .protocol_descriptor_list
            .iter()
            .map(|d| fidl_bredr::ProtocolDescriptor::from(d))
            .collect();
        let additional_protocol_descriptor_lists: Vec<Vec<fidl_bredr::ProtocolDescriptor>> = src
            .additional_protocol_descriptor_lists
            .iter()
            .map(|desc_list| {
                desc_list.into_iter().map(|d| fidl_bredr::ProtocolDescriptor::from(d)).collect()
            })
            .collect();
        let profile_descriptors: Vec<fidl_bredr::ProfileDescriptor> =
            src.profile_descriptors.clone();
        let information: Result<Vec<fidl_bredr::Information>, Error> =
            src.information.iter().map(|i| fidl_bredr::Information::try_from(i)).collect();
        let additional_attributes: Vec<fidl_bredr::Attribute> =
            src.additional_attributes.iter().map(|a| fidl_bredr::Attribute::from(a)).collect();

        Ok(fidl_bredr::ServiceDefinition {
            service_class_uuids: Some(service_class_uuids),
            protocol_descriptor_list: Some(protocol_descriptor_list),
            additional_protocol_descriptor_lists: Some(additional_protocol_descriptor_lists),
            profile_descriptors: Some(profile_descriptors),
            information: Some(information?),
            additional_attributes: Some(additional_attributes),
            ..Default::default()
        })
    }
}

/// Authentication and permission requirements for an advertised service.
/// Corresponds directly to the FIDL `SecurityRequirements` definition - with the extra properties
/// of Clone and PartialEq.
/// See [fuchsia.bluetooth.bredr.SecurityRequirements] for more documentation.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SecurityRequirements {
    pub authentication_required: Option<bool>,
    pub secure_connections_required: Option<bool>,
}

impl From<&fidl_bredr::SecurityRequirements> for SecurityRequirements {
    fn from(src: &fidl_bredr::SecurityRequirements) -> SecurityRequirements {
        SecurityRequirements {
            authentication_required: src.authentication_required,
            secure_connections_required: src.secure_connections_required,
        }
    }
}

impl From<&SecurityRequirements> for fidl_bredr::SecurityRequirements {
    fn from(src: &SecurityRequirements) -> fidl_bredr::SecurityRequirements {
        fidl_bredr::SecurityRequirements {
            authentication_required: src.authentication_required,
            secure_connections_required: src.secure_connections_required,
            ..Default::default()
        }
    }
}

/// Minimum SDU size the service is capable of accepting.
/// See [fuchsia.bluetooth.bredr.ChannelParameters] for more documentation.
const MIN_RX_SDU_SIZE: u16 = 48;

/// Preferred L2CAP channel parameters for an advertised service.
/// Corresponds directly to the FIDL `ChannelParameters` definition - with the extra properties
/// of Clone and PartialEq.
/// The invariants of the FIDL definition are enforced - the max SDU size must be >= 48.
/// See [fuchsia.bluetooth.bredr.ChannelParameters] for more documentation.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ChannelParameters {
    pub channel_mode: Option<fidl_bredr::ChannelMode>,
    pub max_rx_sdu_size: Option<u16>,
    pub security_requirements: Option<SecurityRequirements>,
}

impl TryFrom<&fidl_bredr::ChannelParameters> for ChannelParameters {
    type Error = Error;

    fn try_from(src: &fidl_bredr::ChannelParameters) -> Result<ChannelParameters, Self::Error> {
        if let Some(size) = src.max_rx_sdu_size {
            if size < MIN_RX_SDU_SIZE {
                return Err(Error::conversion(format!(
                    "bredr.ChannelParameters.max_rx_sdu_size is too small: {size}"
                )));
            }
        }

        Ok(ChannelParameters {
            channel_mode: src.channel_mode,
            max_rx_sdu_size: src.max_rx_sdu_size,
            security_requirements: src
                .security_requirements
                .as_ref()
                .map(SecurityRequirements::from),
        })
    }
}

impl TryFrom<&ChannelParameters> for fidl_bredr::ChannelParameters {
    type Error = Error;

    fn try_from(src: &ChannelParameters) -> Result<fidl_bredr::ChannelParameters, Self::Error> {
        if let Some(size) = src.max_rx_sdu_size {
            if size < MIN_RX_SDU_SIZE {
                return Err(Error::conversion(format!(
                    "ChannelParameters.max_rx_sdu_size is too small: {size}"
                )));
            }
        }

        Ok(fidl_bredr::ChannelParameters {
            channel_mode: src.channel_mode,
            max_rx_sdu_size: src.max_rx_sdu_size,
            security_requirements: src
                .security_requirements
                .as_ref()
                .map(fidl_bredr::SecurityRequirements::from),
            ..Default::default()
        })
    }
}

#[derive(Debug, Clone, ValidFidlTable, PartialEq)]
#[fidl_table_src(fidl_bredr::ScoConnectionParameters)]
pub struct ValidScoConnectionParameters {
    pub parameter_set: fidl_bredr::HfpParameterSet,
    pub air_coding_format: fidl_bredr::CodingFormat,
    pub air_frame_size: u16,
    pub io_bandwidth: u32,
    pub io_coding_format: fidl_bredr::CodingFormat,
    pub io_frame_size: u16,
    #[fidl_field_type(optional)]
    pub io_pcm_data_format: Option<fidl_fuchsia_hardware_audio::SampleFormat>,
    #[fidl_field_type(optional)]
    pub io_pcm_sample_payload_msb_position: Option<u8>,
    pub path: fidl_bredr::DataPath,
}

impl Unit for ValidScoConnectionParameters {
    type Data = inspect::Node;
    fn inspect_create(&self, parent: &inspect::Node, name: impl AsRef<str>) -> Self::Data {
        let mut node = parent.create_child(name.as_ref());
        self.inspect_update(&mut node);
        node
    }

    fn inspect_update(&self, data: &mut Self::Data) {
        data.record_string("parameter_set", &format!("{:?}", self.parameter_set));
        data.record_string("air_coding_format", &format!("{:?}", self.air_coding_format));
        data.record_uint("air_frame_size", self.air_frame_size.into());
        data.record_uint("io_bandwidth", self.io_bandwidth.into());
        data.record_string("io_coding_format", &format!("{:?}", self.io_coding_format));
        data.record_uint("io_frame_size", self.io_frame_size.into());
        if let Some(io_pcm_data_format) = &self.io_pcm_data_format {
            data.record_string("io_pcm_data_format", &format!("{:?}", io_pcm_data_format));
        }
        if let Some(io_pcm_sample_payload_msb_position) = &self.io_pcm_sample_payload_msb_position {
            data.record_uint(
                "io_pcm_sample_payload_msb_position",
                (*io_pcm_sample_payload_msb_position).into(),
            );
        }
        data.record_string("path", &format!("{:?}", self.path));
    }
}

impl Inspect for &mut ValidScoConnectionParameters {
    fn iattach(self, parent: &inspect::Node, name: impl AsRef<str>) -> Result<(), AttachError> {
        // The created node is owned by the provided `parent`.
        parent.record(self.inspect_create(parent, name));
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fuchsia_inspect::assert_data_tree;

    #[test]
    fn test_find_descriptors_fails_with_no_descriptors() {
        assert!(find_profile_descriptors(&[]).is_err());

        let mut attributes = vec![fidl_bredr::Attribute {
            id: 0x3001,
            element: fidl_bredr::DataElement::Uint32(0xF00FC0DE),
        }];

        assert!(find_profile_descriptors(&attributes).is_err());

        // Wrong element type
        attributes.push(fidl_bredr::Attribute {
            id: fidl_bredr::ATTR_BLUETOOTH_PROFILE_DESCRIPTOR_LIST,
            element: fidl_bredr::DataElement::Uint32(0xABADC0DE),
        });

        assert!(find_profile_descriptors(&attributes).is_err());

        // Empty sequence
        attributes[1].element = fidl_bredr::DataElement::Sequence(vec![]);

        assert!(find_profile_descriptors(&attributes).is_err());
    }

    #[test]
    fn test_find_descriptors_returns_descriptors() {
        let attributes = vec![fidl_bredr::Attribute {
            id: fidl_bredr::ATTR_BLUETOOTH_PROFILE_DESCRIPTOR_LIST,
            element: fidl_bredr::DataElement::Sequence(vec![
                Some(Box::new(fidl_bredr::DataElement::Sequence(vec![
                    Some(Box::new(fidl_bredr::DataElement::Uuid(Uuid::new16(0x1101).into()))),
                    Some(Box::new(fidl_bredr::DataElement::Uint16(0x0103))),
                ]))),
                Some(Box::new(fidl_bredr::DataElement::Sequence(vec![
                    Some(Box::new(fidl_bredr::DataElement::Uuid(Uuid::new16(0x113A).into()))),
                    Some(Box::new(fidl_bredr::DataElement::Uint16(0x0302))),
                ]))),
            ]),
        }];

        let result = find_profile_descriptors(&attributes);
        assert!(result.is_ok());
        let result = result.expect("result");
        assert_eq!(2, result.len());

        assert_eq!(fidl_bredr::ServiceClassProfileIdentifier::SerialPort, result[0].profile_id);
        assert_eq!(1, result[0].major_version);
        assert_eq!(3, result[0].minor_version);
    }

    #[test]
    fn test_find_service_classes_attribute_missing() {
        assert_eq!(find_service_classes(&[]), Vec::new());
        let attributes = vec![fidl_bredr::Attribute {
            id: fidl_bredr::ATTR_BLUETOOTH_PROFILE_DESCRIPTOR_LIST,
            element: fidl_bredr::DataElement::Sequence(vec![
                Some(Box::new(fidl_bredr::DataElement::Sequence(vec![
                    Some(Box::new(fidl_bredr::DataElement::Uuid(Uuid::new16(0x1101).into()))),
                    Some(Box::new(fidl_bredr::DataElement::Uint16(0x0103))),
                ]))),
                Some(Box::new(fidl_bredr::DataElement::Sequence(vec![
                    Some(Box::new(fidl_bredr::DataElement::Uuid(Uuid::new16(0x113A).into()))),
                    Some(Box::new(fidl_bredr::DataElement::Uint16(0x0302))),
                ]))),
            ]),
        }];
        assert_eq!(find_service_classes(&attributes), Vec::new());
    }

    #[test]
    fn test_find_service_classes_wrong_type() {
        let attributes = vec![fidl_bredr::Attribute {
            id: fidl_bredr::ATTR_SERVICE_CLASS_ID_LIST,
            element: fidl_bredr::DataElement::Uint32(0xc0defae5u32),
        }];
        assert_eq!(find_service_classes(&attributes), Vec::new());
    }

    #[test]
    fn test_find_service_classes_returns_known_classes() {
        let attribute = fidl_bredr::Attribute {
            id: fidl_bredr::ATTR_SERVICE_CLASS_ID_LIST,
            element: fidl_bredr::DataElement::Sequence(vec![Some(Box::new(
                fidl_bredr::DataElement::Uuid(Uuid::new16(0x1101).into()),
            ))]),
        };

        let result = find_service_classes(&[attribute]);
        assert_eq!(1, result.len());
        let assigned_num = result.first().unwrap();
        assert_eq!(0x1101, assigned_num.number); // 0x1101 is the 16-bit UUID of SerialPort
        assert_eq!("SerialPort", assigned_num.name);

        let unknown_uuids = fidl_bredr::Attribute {
            id: fidl_bredr::ATTR_SERVICE_CLASS_ID_LIST,
            element: fidl_bredr::DataElement::Sequence(vec![
                Some(Box::new(fidl_bredr::DataElement::Uuid(Uuid::new16(0x1101).into()))),
                Some(Box::new(fidl_bredr::DataElement::Uuid(Uuid::new16(0xc0de).into()))),
            ]),
        };

        // Discards unknown UUIDs
        let result = find_service_classes(&[unknown_uuids]);
        assert_eq!(1, result.len());
        let assigned_num = result.first().unwrap();
        assert_eq!(0x1101, assigned_num.number); // 0x1101 is the 16-bit UUID of SerialPort
        assert_eq!("SerialPort", assigned_num.name);
    }

    #[test]
    fn test_psm_from_protocol() {
        let empty = vec![];
        assert_eq!(None, psm_from_protocol(&empty));

        let no_psm = vec![ProtocolDescriptor {
            protocol: fidl_bredr::ProtocolIdentifier::L2Cap,
            params: vec![],
        }];
        assert_eq!(None, psm_from_protocol(&no_psm));

        let psm = Psm::new(10);
        let valid_psm = vec![ProtocolDescriptor {
            protocol: fidl_bredr::ProtocolIdentifier::L2Cap,
            params: vec![DataElement::Uint16(psm.into())],
        }];
        assert_eq!(Some(psm), psm_from_protocol(&valid_psm));

        let rfcomm = vec![
            ProtocolDescriptor {
                protocol: fidl_bredr::ProtocolIdentifier::L2Cap,
                params: vec![], // PSM omitted for RFCOMM.
            },
            ProtocolDescriptor {
                protocol: fidl_bredr::ProtocolIdentifier::Rfcomm,
                params: vec![DataElement::Uint8(10)], // Server channel
            },
        ];
        assert_eq!(None, psm_from_protocol(&rfcomm));
    }

    #[test]
    fn test_elem_to_profile_descriptor_works() {
        let element = fidl_bredr::DataElement::Sequence(vec![
            Some(Box::new(fidl_bredr::DataElement::Uuid(Uuid::new16(0x1101).into()))),
            Some(Box::new(fidl_bredr::DataElement::Uint16(0x0103))),
        ]);

        let descriptor =
            elem_to_profile_descriptor(&element).expect("descriptor should be returned");

        assert_eq!(fidl_bredr::ServiceClassProfileIdentifier::SerialPort, descriptor.profile_id);
        assert_eq!(1, descriptor.major_version);
        assert_eq!(3, descriptor.minor_version);
    }

    #[test]
    fn test_elem_to_profile_descriptor_wrong_element_types() {
        let element = fidl_bredr::DataElement::Sequence(vec![
            Some(Box::new(fidl_bredr::DataElement::Uint16(0x1101))),
            Some(Box::new(fidl_bredr::DataElement::Uint16(0x0103))),
        ]);
        assert!(elem_to_profile_descriptor(&element).is_none());

        let element = fidl_bredr::DataElement::Sequence(vec![
            Some(Box::new(fidl_bredr::DataElement::Uuid(Uuid::new16(0x1101).into()))),
            Some(Box::new(fidl_bredr::DataElement::Uint32(0x0103))),
        ]);
        assert!(elem_to_profile_descriptor(&element).is_none());

        let element = fidl_bredr::DataElement::Sequence(vec![Some(Box::new(
            fidl_bredr::DataElement::Uint32(0x0103),
        ))]);
        assert!(elem_to_profile_descriptor(&element).is_none());

        let element = fidl_bredr::DataElement::Sequence(vec![None]);
        assert!(elem_to_profile_descriptor(&element).is_none());

        let element = fidl_bredr::DataElement::Uint32(0xDEADC0DE);
        assert!(elem_to_profile_descriptor(&element).is_none());
    }

    #[test]
    fn test_invalid_information_fails_gracefully() {
        let empty_language = "".to_string();

        let invalid_local = Information {
            language: empty_language.clone(),
            name: None,
            description: None,
            provider: None,
        };
        let fidl = fidl_bredr::Information::try_from(&invalid_local);
        assert!(fidl.is_err());

        // No language.
        let local = Information::try_from(&fidl_bredr::Information::default());
        assert!(local.is_err());

        let empty_lang_fidl =
            fidl_bredr::Information { language: Some(empty_language), ..Default::default() };
        let local = Information::try_from(&empty_lang_fidl);
        assert!(local.is_err());
    }

    #[test]
    fn test_get_psm_from_service_definition() {
        let uuid = Uuid::new32(1234);
        let psm1 = Psm(10);
        let psm2 = Psm(12);
        let mut def = ServiceDefinition {
            service_class_uuids: vec![uuid],
            protocol_descriptor_list: vec![],
            additional_protocol_descriptor_lists: vec![],
            profile_descriptors: vec![],
            information: vec![],
            additional_attributes: vec![],
        };

        assert_eq!(def.primary_psm(), None);
        assert_eq!(def.additional_psms(), HashSet::new());
        assert_eq!(def.psm_set(), HashSet::new());

        def.protocol_descriptor_list = vec![ProtocolDescriptor {
            protocol: fidl_bredr::ProtocolIdentifier::L2Cap,
            params: vec![DataElement::Uint16(psm1.into())],
        }];

        let mut expected_psms = HashSet::new();
        let _ = expected_psms.insert(psm1);
        assert_eq!(def.primary_psm(), Some(psm1));
        assert_eq!(def.additional_psms(), HashSet::new());
        assert_eq!(def.psm_set(), expected_psms);

        def.additional_protocol_descriptor_lists = vec![
            vec![ProtocolDescriptor {
                protocol: fidl_bredr::ProtocolIdentifier::L2Cap,
                params: vec![DataElement::Uint16(psm2.into())],
            }],
            vec![ProtocolDescriptor {
                protocol: fidl_bredr::ProtocolIdentifier::Avdtp,
                params: vec![DataElement::Uint16(0x0103)],
            }],
        ];

        let mut expected_psms = HashSet::new();
        let _ = expected_psms.insert(psm2);
        assert_eq!(def.primary_psm(), Some(psm1));
        assert_eq!(def.additional_psms(), expected_psms);
        let _ = expected_psms.insert(psm1);
        assert_eq!(def.psm_set(), expected_psms);
    }

    #[test]
    fn test_service_definition_conversions() {
        let uuid = fidl_bt::Uuid { value: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15] };
        let prof_descs = vec![ProfileDescriptor {
            profile_id: fidl_bredr::ServiceClassProfileIdentifier::AvRemoteControl,
            major_version: 1,
            minor_version: 6,
        }];
        let language = "en".to_string();
        let name = "foobar".to_string();
        let description = "fake".to_string();
        let provider = "random".to_string();
        let attribute_id = 0x3001;
        let attribute_value = 0xF00FC0DE;

        let local = ServiceDefinition {
            service_class_uuids: vec![uuid.into()],
            protocol_descriptor_list: vec![ProtocolDescriptor {
                protocol: fidl_bredr::ProtocolIdentifier::L2Cap,
                params: vec![DataElement::Uint16(10)],
            }],
            additional_protocol_descriptor_lists: vec![
                vec![ProtocolDescriptor {
                    protocol: fidl_bredr::ProtocolIdentifier::L2Cap,
                    params: vec![DataElement::Uint16(12)],
                }],
                vec![ProtocolDescriptor {
                    protocol: fidl_bredr::ProtocolIdentifier::Avdtp,
                    params: vec![DataElement::Uint16(3)],
                }],
            ],
            profile_descriptors: prof_descs.clone(),
            information: vec![Information {
                language: language.clone(),
                name: Some(name.clone()),
                description: Some(description.clone()),
                provider: Some(provider.clone()),
            }],
            additional_attributes: vec![Attribute {
                id: attribute_id,
                element: DataElement::Sequence(vec![Box::new(DataElement::Uint32(
                    attribute_value,
                ))]),
            }],
        };

        let fidl = fidl_bredr::ServiceDefinition {
            service_class_uuids: Some(vec![uuid]),
            protocol_descriptor_list: Some(vec![fidl_bredr::ProtocolDescriptor {
                protocol: fidl_bredr::ProtocolIdentifier::L2Cap,
                params: vec![fidl_bredr::DataElement::Uint16(10)],
            }]),
            additional_protocol_descriptor_lists: Some(vec![
                vec![fidl_bredr::ProtocolDescriptor {
                    protocol: fidl_bredr::ProtocolIdentifier::L2Cap,
                    params: vec![fidl_bredr::DataElement::Uint16(12)],
                }],
                vec![fidl_bredr::ProtocolDescriptor {
                    protocol: fidl_bredr::ProtocolIdentifier::Avdtp,
                    params: vec![fidl_bredr::DataElement::Uint16(3)],
                }],
            ]),
            profile_descriptors: Some(prof_descs.clone()),
            information: Some(vec![fidl_bredr::Information {
                language: Some(language.clone()),
                name: Some(name.clone()),
                description: Some(description.clone()),
                provider: Some(provider.clone()),
                ..Default::default()
            }]),
            additional_attributes: Some(vec![fidl_bredr::Attribute {
                id: attribute_id,
                element: fidl_bredr::DataElement::Sequence(vec![Some(Box::new(
                    fidl_bredr::DataElement::Uint32(attribute_value),
                ))]),
            }]),
            ..Default::default()
        };

        // Converting from local ServiceDefinition to the FIDL ServiceDefinition should work.
        let local_to_fidl: fidl_bredr::ServiceDefinition =
            fidl_bredr::ServiceDefinition::try_from(&local).expect("should work");
        assert_eq!(local_to_fidl, fidl);

        // Converting from FIDL ServiceDefinition to the local ServiceDefinition should work.
        let fidl_to_local: ServiceDefinition =
            ServiceDefinition::try_from(&fidl).expect("should work");
        assert_eq!(fidl_to_local, local);
    }

    #[test]
    fn test_invalid_service_definition_fails_gracefully() {
        let no_uuids_fidl = fidl_bredr::ServiceDefinition::default();
        let fidl_to_local = ServiceDefinition::try_from(&no_uuids_fidl);
        assert!(fidl_to_local.is_err());

        let empty_uuids_fidl = fidl_bredr::ServiceDefinition {
            service_class_uuids: Some(vec![]),
            ..Default::default()
        };
        let fidl_to_local = ServiceDefinition::try_from(&empty_uuids_fidl);
        assert!(fidl_to_local.is_err());
    }

    #[test]
    fn test_channel_parameters_conversions() {
        let channel_mode = Some(fidl_bredr::ChannelMode::EnhancedRetransmission);
        let max_rx_sdu_size = Some(MIN_RX_SDU_SIZE);

        let local =
            ChannelParameters { channel_mode, max_rx_sdu_size, security_requirements: None };
        let fidl =
            fidl_bredr::ChannelParameters { channel_mode, max_rx_sdu_size, ..Default::default() };

        let local_to_fidl =
            fidl_bredr::ChannelParameters::try_from(&local).expect("conversion should work");
        assert_eq!(local_to_fidl, fidl);

        let fidl_to_local = ChannelParameters::try_from(&fidl).expect("conversion should work");
        assert_eq!(fidl_to_local, local);

        // Empty FIDL parameters is OK.
        let fidl = fidl_bredr::ChannelParameters::default();
        let expected = ChannelParameters {
            channel_mode: None,
            max_rx_sdu_size: None,
            security_requirements: None,
        };

        let fidl_to_local = ChannelParameters::try_from(&fidl).expect("conversion should work");
        assert_eq!(fidl_to_local, expected);
    }

    #[test]
    fn test_invalid_channel_parameters_fails_gracefully() {
        let too_small_sdu = Some(MIN_RX_SDU_SIZE - 1);
        let local = ChannelParameters {
            channel_mode: None,
            max_rx_sdu_size: too_small_sdu,
            security_requirements: None,
        };
        let fidl =
            fidl_bredr::ChannelParameters { max_rx_sdu_size: too_small_sdu, ..Default::default() };

        let local_to_fidl = fidl_bredr::ChannelParameters::try_from(&local);
        assert!(local_to_fidl.is_err());

        let fidl_to_local = ChannelParameters::try_from(&fidl);
        assert!(fidl_to_local.is_err());
    }

    #[test]
    fn test_security_requirements_conversions() {
        let authentication_required = Some(false);
        let secure_connections_required = Some(true);

        let local = SecurityRequirements { authentication_required, secure_connections_required };
        let fidl = fidl_bredr::SecurityRequirements {
            authentication_required,
            secure_connections_required,
            ..Default::default()
        };

        let local_to_fidl = fidl_bredr::SecurityRequirements::from(&local);
        assert_eq!(local_to_fidl, fidl);

        let fidl_to_local = SecurityRequirements::from(&fidl);
        assert_eq!(fidl_to_local, local);
    }

    #[test]
    fn test_combine_security_requirements() {
        let req1 = SecurityRequirements {
            authentication_required: None,
            secure_connections_required: None,
        };
        let req2 = SecurityRequirements {
            authentication_required: None,
            secure_connections_required: None,
        };
        let expected = SecurityRequirements {
            authentication_required: None,
            secure_connections_required: None,
        };
        assert_eq!(combine_security_requirements(&req1, &req2), expected);

        let req1 = SecurityRequirements {
            authentication_required: Some(true),
            secure_connections_required: None,
        };
        let req2 = SecurityRequirements {
            authentication_required: None,
            secure_connections_required: Some(true),
        };
        let expected = SecurityRequirements {
            authentication_required: Some(true),
            secure_connections_required: Some(true),
        };
        assert_eq!(combine_security_requirements(&req1, &req2), expected);

        let req1 = SecurityRequirements {
            authentication_required: Some(false),
            secure_connections_required: Some(true),
        };
        let req2 = SecurityRequirements {
            authentication_required: None,
            secure_connections_required: Some(true),
        };
        let expected = SecurityRequirements {
            authentication_required: Some(false),
            secure_connections_required: Some(true),
        };
        assert_eq!(combine_security_requirements(&req1, &req2), expected);

        let req1 = SecurityRequirements {
            authentication_required: Some(true),
            secure_connections_required: Some(false),
        };
        let req2 = SecurityRequirements {
            authentication_required: Some(false),
            secure_connections_required: Some(true),
        };
        let expected = SecurityRequirements {
            authentication_required: Some(true),
            secure_connections_required: Some(true),
        };
        assert_eq!(combine_security_requirements(&req1, &req2), expected);
    }

    #[test]
    fn test_combine_channel_parameters() {
        let p1 = ChannelParameters::default();
        let p2 = ChannelParameters::default();
        let expected = ChannelParameters::default();
        assert_eq!(combine_channel_parameters(&p1, &p2), expected);

        let p1 = ChannelParameters {
            channel_mode: Some(fidl_bredr::ChannelMode::EnhancedRetransmission),
            max_rx_sdu_size: None,
            security_requirements: None,
        };
        let p2 = ChannelParameters {
            channel_mode: Some(fidl_bredr::ChannelMode::Basic),
            max_rx_sdu_size: Some(70),
            security_requirements: None,
        };
        let expected = ChannelParameters {
            channel_mode: Some(fidl_bredr::ChannelMode::Basic),
            max_rx_sdu_size: Some(70),
            security_requirements: None,
        };
        assert_eq!(combine_channel_parameters(&p1, &p2), expected);

        let empty_seq_reqs = SecurityRequirements::default();
        let p1 = ChannelParameters {
            channel_mode: None,
            max_rx_sdu_size: Some(75),
            security_requirements: Some(empty_seq_reqs.clone()),
        };
        let p2 = ChannelParameters {
            channel_mode: Some(fidl_bredr::ChannelMode::EnhancedRetransmission),
            max_rx_sdu_size: None,
            security_requirements: None,
        };
        let expected = ChannelParameters {
            channel_mode: Some(fidl_bredr::ChannelMode::EnhancedRetransmission),
            max_rx_sdu_size: Some(75),
            security_requirements: Some(empty_seq_reqs),
        };
        assert_eq!(combine_channel_parameters(&p1, &p2), expected);

        let reqs1 = SecurityRequirements {
            authentication_required: Some(true),
            secure_connections_required: None,
        };
        let reqs2 = SecurityRequirements {
            authentication_required: Some(false),
            secure_connections_required: Some(false),
        };
        let combined_reqs = combine_security_requirements(&reqs1, &reqs2);
        let p1 = ChannelParameters {
            channel_mode: None,
            max_rx_sdu_size: Some(90),
            security_requirements: Some(reqs1),
        };
        let p2 = ChannelParameters {
            channel_mode: Some(fidl_bredr::ChannelMode::Basic),
            max_rx_sdu_size: Some(70),
            security_requirements: Some(reqs2),
        };
        let expected = ChannelParameters {
            channel_mode: Some(fidl_bredr::ChannelMode::Basic),
            max_rx_sdu_size: Some(70),
            security_requirements: Some(combined_reqs),
        };
        assert_eq!(combine_channel_parameters(&p1, &p2), expected);
    }

    #[test]
    fn local_sco_parameters_inspect_tree() {
        let inspect = inspect::Inspector::default();
        assert_data_tree!(inspect, root: {});

        let params = fidl_bredr::ScoConnectionParameters {
            parameter_set: Some(fidl_bredr::HfpParameterSet::D1),
            air_coding_format: Some(fidl_bredr::CodingFormat::Cvsd),
            air_frame_size: Some(60),
            io_bandwidth: Some(16000),
            io_coding_format: Some(fidl_bredr::CodingFormat::LinearPcm),
            io_frame_size: Some(16),
            io_pcm_data_format: Some(fidl_fuchsia_hardware_audio::SampleFormat::PcmSigned),
            io_pcm_sample_payload_msb_position: Some(1),
            path: Some(fidl_bredr::DataPath::Offload),
            ..Default::default()
        };

        let mut local: ValidScoConnectionParameters = params.try_into().expect("can convert");
        assert_data_tree!(inspect, root: {});

        let _ = local.iattach(&inspect.root(), "state").expect("can attach inspect");
        assert_data_tree!(inspect, root: {
            state: {
                parameter_set: "D1",
                air_coding_format: "Cvsd",
                air_frame_size: 60u64,
                io_bandwidth: 16000u64,
                io_coding_format: "LinearPcm",
                io_frame_size: 16u64,
                io_pcm_data_format: "PcmSigned",
                io_pcm_sample_payload_msb_position: 1u64,
                path: "Offload",
            }
        });
    }

    #[test]
    fn data_element_primitve_conversions() {
        type Result<T> = std::result::Result<T, DataElementConversionError>;

        let rust_u8 = 8u8;
        let data_element_uint8 = DataElement::Uint8(8u8);
        let data_element_uint8_into: DataElement = rust_u8.into();
        let rust_u8_ok: Result<u8> = data_element_uint8.clone().try_into();
        let rust_u8_err: Result<u16> = data_element_uint8.clone().try_into();
        assert_eq!(data_element_uint8_into, data_element_uint8);
        assert_eq!(rust_u8_ok, Ok(rust_u8));
        assert_eq!(
            rust_u8_err,
            Err(DataElementConversionError { data_element: data_element_uint8 })
        );

        let rust_i8 = 9i8;
        let data_element_int8 = DataElement::Int8(9i8);
        let data_element_int8_into: DataElement = rust_i8.into();
        let rust_i8_ok: Result<i8> = data_element_int8.clone().try_into();
        let rust_i8_err: Result<u16> = data_element_int8.clone().try_into();
        assert_eq!(data_element_int8_into, data_element_int8);
        assert_eq!(rust_i8_ok, Ok(rust_i8));
        assert_eq!(
            rust_i8_err,
            Err(DataElementConversionError { data_element: data_element_int8 })
        );

        let rust_u16 = 16u16;
        let data_element_uint16 = DataElement::Uint16(16u16);
        let data_element_uint16_into: DataElement = rust_u16.into();
        let rust_u16_ok: Result<u16> = data_element_uint16.clone().try_into();
        let rust_u16_err: Result<i16> = data_element_uint16.clone().try_into();
        assert_eq!(data_element_uint16_into, data_element_uint16);
        assert_eq!(rust_u16_ok, Ok(rust_u16));
        assert_eq!(
            rust_u16_err,
            Err(DataElementConversionError { data_element: data_element_uint16 })
        );

        let rust_i16 = 17i16;
        let data_element_int16 = DataElement::Int16(17i16);
        let data_element_int16_into: DataElement = rust_i16.into();
        let rust_i16_ok: Result<i16> = data_element_int16.clone().try_into();
        let rust_i16_err: Result<u16> = data_element_int16.clone().try_into();
        assert_eq!(data_element_int16_into, data_element_int16);
        assert_eq!(rust_i16_ok, Ok(rust_i16));
        assert_eq!(
            rust_i16_err,
            Err(DataElementConversionError { data_element: data_element_int16 })
        );

        let rust_u32 = 32u32;
        let data_element_uint32 = DataElement::Uint32(32u32);
        let data_element_uint32_into: DataElement = rust_u32.into();
        let rust_u32_ok: Result<u32> = data_element_uint32.clone().try_into();
        let rust_u32_err: Result<u16> = data_element_uint32.clone().try_into();
        assert_eq!(data_element_uint32_into, data_element_uint32);
        assert_eq!(rust_u32_ok, Ok(rust_u32));
        assert_eq!(
            rust_u32_err,
            Err(DataElementConversionError { data_element: data_element_uint32 })
        );

        let rust_i32 = 33i32;
        let data_element_int32 = DataElement::Int32(33i32);
        let data_element_int32_into: DataElement = rust_i32.into();
        let rust_i32_ok: Result<i32> = data_element_int32.clone().try_into();
        let rust_i32_err: Result<u16> = data_element_int32.clone().try_into();
        assert_eq!(data_element_int32_into, data_element_int32);
        assert_eq!(rust_i32_ok, Ok(rust_i32));
        assert_eq!(
            rust_i32_err,
            Err(DataElementConversionError { data_element: data_element_int32 })
        );

        let rust_u64 = 64u64;
        let data_element_uint64 = DataElement::Uint64(64u64);
        let data_element_uint64_into: DataElement = rust_u64.into();
        let rust_u64_ok: Result<u64> = data_element_uint64.clone().try_into();
        let rust_u64_err: Result<u16> = data_element_uint64.clone().try_into();
        assert_eq!(data_element_uint64_into, data_element_uint64);
        assert_eq!(rust_u64_ok, Ok(rust_u64));
        assert_eq!(
            rust_u64_err,
            Err(DataElementConversionError { data_element: data_element_uint64 })
        );

        let rust_i64 = 65i64;
        let data_element_int64 = DataElement::Int64(65i64);
        let data_element_int64_into: DataElement = rust_i64.into();
        let rust_i64_ok: Result<i64> = data_element_int64.clone().try_into();
        let rust_i64_err: Result<u16> = data_element_int64.clone().try_into();
        assert_eq!(data_element_int64_into, data_element_int64);
        assert_eq!(rust_i64_ok, Ok(rust_i64));
        assert_eq!(
            rust_i64_err,
            Err(DataElementConversionError { data_element: data_element_int64 })
        );

        let rust_vec = "ABC".as_bytes().to_vec();
        let data_element_str = DataElement::Str("ABC".as_bytes().to_vec());
        let data_element_str_into: DataElement = rust_vec.clone().into();
        let rust_vec_ok: Result<Vec<u8>> = data_element_str.clone().try_into();
        let rust_vec_err: Result<u16> = data_element_str.clone().try_into();
        assert_eq!(data_element_str_into, data_element_str);
        assert_eq!(rust_vec_ok, Ok(rust_vec));
        assert_eq!(
            rust_vec_err,
            Err(DataElementConversionError { data_element: data_element_str })
        );

        let rust_uuid: fidl_bt::Uuid = Uuid::new16(0x1101).into();
        let data_element_uuid = DataElement::Uuid(Uuid::new16(0x1101).into());
        let data_element_uuid_into: DataElement = rust_uuid.clone().into();
        let rust_uuid_ok: Result<fidl_bt::Uuid> = data_element_uuid.clone().try_into();
        let rust_uuid_err: Result<u16> = data_element_uuid.clone().try_into();
        assert_eq!(data_element_uuid_into, data_element_uuid);
        assert_eq!(rust_uuid_ok, Ok(rust_uuid));
        assert_eq!(
            rust_uuid_err,
            Err(DataElementConversionError { data_element: data_element_uuid })
        );

        let rust_string = String::from("ABC");
        let data_element_url = DataElement::Url(String::from("ABC"));
        let data_element_url_into: DataElement = rust_string.clone().into();
        let rust_string_ok: Result<String> = data_element_url.clone().try_into();
        let rust_string_err: Result<u16> = data_element_url.clone().try_into();
        assert_eq!(data_element_url_into, data_element_url);
        assert_eq!(rust_string_ok, Ok(rust_string));
        assert_eq!(
            rust_string_err,
            Err(DataElementConversionError { data_element: data_element_url })
        );

        let rust_bool = true;
        let data_element_bool = DataElement::Bool(true);
        let data_element_bool_into: DataElement = rust_bool.into();
        let rust_bool_ok: Result<bool> = data_element_bool.clone().try_into();
        let rust_bool_err: Result<u16> = data_element_bool.clone().try_into();
        assert_eq!(data_element_bool_into, data_element_bool);
        assert_eq!(rust_bool_ok, Ok(rust_bool));
        assert_eq!(
            rust_bool_err,
            Err(DataElementConversionError { data_element: data_element_bool })
        );
    }
}
