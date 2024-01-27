// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fuchsia_zircon as zx,
    packet_encoding::{decodable_enum, Decodable, Encodable},
    std::{
        convert::TryFrom,
        fmt,
        io::{Cursor, Write},
        result,
    },
    thiserror::Error,
};

/// Result type for AVDTP, using avdtp::Error
pub type Result<T> = result::Result<T, Error>;

/// The error type of the AVDTP library.
#[derive(Error, Debug)]
#[non_exhaustive]
pub enum Error {
    /// The value that was sent on the wire was out of range.
    #[error("Value was out of range")]
    OutOfRange,

    /// The signal identifier was invalid when parsing a message.
    #[error("Invalid signal id for {:?}: {:X?}", _0, _1)]
    InvalidSignalId(TxLabel, u8),

    /// The header was invalid when parsing a message from the peer.
    #[error("Invalid Header for a AVDTP message")]
    InvalidHeader,

    /// The body format was invalid when parsing a message from the peer.
    #[error("Failed to parse AVDTP message contents")]
    InvalidMessage,

    /// The remote end failed to respond to this command in time.
    #[error("Command timed out")]
    Timeout,

    /// The Remote end rejected a command we sent (with this error code)
    #[error("Remote end rejected the command ({:})", _0)]
    RemoteRejected(#[from] RemoteReject),

    /// When a message hasn't been implemented yet, the parser will return this.
    #[error("Message has not been implemented yet")]
    UnimplementedMessage,

    /// The distant peer has disconnected.
    #[error("Peer has disconnected")]
    PeerDisconnected,

    /// Sent if a Command Future is polled after it's already completed
    #[error("Command Response has already been received")]
    AlreadyReceived,

    /// Encountered an IO error setting up the channel
    #[error("Encountered an IO error reading from the peer: {}", _0)]
    ChannelSetup(zx::Status),

    /// Encountered an IO error reading from the peer.
    #[error("Encountered an IO error reading from the peer: {}", _0)]
    PeerRead(zx::Status),

    /// Encountered an IO error reading from the peer.
    #[error("Encountered an IO error writing to the peer: {}", _0)]
    PeerWrite(zx::Status),

    /// A message couldn't be encoded.
    #[error("Encountered an error encoding a message")]
    Encoding,

    /// An error has been detected, and the request that is being handled
    /// should be rejected with the error code given.
    #[error("Invalid request detected: {:?}", _0)]
    RequestInvalid(ErrorCode),

    /// Same as RequestInvalid, but an extra byte is included, which is used
    /// in Stream and Configure responses
    #[error("Invalid request detected: {:?} (extra: {:?})", _0, _1)]
    RequestInvalidExtra(ErrorCode, u8),

    /// An operation was attempted in an Invalid State
    #[error("Invalid State")]
    InvalidState,

    /// An error from another source
    #[error(transparent)]
    Other(#[from] anyhow::Error),
}

/// Errors that can be returned by the remote peer in response to a message
#[derive(Error, Debug)]
#[cfg_attr(test, derive(PartialEq))]
pub struct RemoteReject {
    /// The signal identifier in this rejection.
    /// This is the only field populated for a General Rejection, which only occurs when the remote
    /// end does not understand the SignalIdentifier (but we do, or we would return a
    /// `Error::InvalidHeader`)
    signal_id: SignalIdentifier,
    /// Error code reported for this error.
    error_code: Option<u8>,
    /// The service category reported that applies to this error.  Only set when `signal_id` is
    /// SetConfiguration or Reconfigure.
    service_category: Option<u8>,
    /// The StreamEndpointId reported that applies for this rejection.
    stream_endpoint_id: Option<StreamEndpointId>,
}

impl RemoteReject {
    pub(crate) fn from_params(signal_id: SignalIdentifier, params: &[u8]) -> Self {
        if params.len() == 0 {
            // General Reject
            return Self {
                signal_id,
                error_code: None,
                service_category: None,
                stream_endpoint_id: None,
            };
        }
        let (error_code, service_category, stream_endpoint_id) = match signal_id {
            SignalIdentifier::SetConfiguration | SignalIdentifier::Reconfigure => {
                (params.get(1).copied(), params.get(0).copied(), None)
            }
            SignalIdentifier::Start | SignalIdentifier::Suspend => {
                // The Stream ID here is in the top 6 bits, with the bottom 2 bits RFA.
                (params.get(1).copied(), None, params.get(0).map(StreamEndpointId::from_msg))
            }
            _ => (params.get(0).copied(), None, None),
        };
        Self { signal_id, error_code, service_category, stream_endpoint_id }
    }

    /// Retrieve the error code returned by the remote end.
    /// Returns Some(Err(u8)) if the code isn't recognized, or None if it wasn't included
    pub fn error_code(&self) -> Option<std::result::Result<ErrorCode, u8>> {
        self.error_code.map(|e| ErrorCode::try_from(e).map_err(|_| e))
    }

    /// Retrieve the ServiceCategory returned by the remote end.
    /// Returns Err(u8) if the category wasn't recognized, or None if it wasn't included
    pub fn service_category(&self) -> Option<std::result::Result<ServiceCategory, u8>> {
        self.service_category.map(|e| ServiceCategory::try_from(e).map_err(|_| e))
    }

    /// Retrieve the stream identifier returned from the remote end.
    /// Returns None if it wasn't included.
    pub fn stream_id(&self) -> Option<StreamEndpointId> {
        self.stream_endpoint_id.clone()
    }

    #[cfg(test)]
    pub fn general(signal_id: SignalIdentifier) -> Self {
        Self { signal_id, error_code: None, service_category: None, stream_endpoint_id: None }
    }

    #[cfg(test)]
    pub fn rejected(signal_id: SignalIdentifier, code: u8) -> Self {
        Self { signal_id, error_code: Some(code), service_category: None, stream_endpoint_id: None }
    }

    #[cfg(test)]
    pub fn config(signal_id: SignalIdentifier, cat: u8, code: u8) -> Self {
        Self {
            signal_id,
            error_code: Some(code),
            service_category: Some(cat),
            stream_endpoint_id: None,
        }
    }

    #[cfg(test)]
    pub fn stream(signal_id: SignalIdentifier, stream: u8, code: u8) -> Self {
        Self {
            signal_id,
            error_code: Some(code),
            service_category: None,
            stream_endpoint_id: stream.try_into().ok(),
        }
    }
}

impl std::fmt::Display for RemoteReject {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::result::Result<(), std::fmt::Error> {
        let Some(error_code) = self.error_code else {
           return write!(f, "did not recognize {:?}", self.signal_id);
       };

        write!(f, "{:?}, ", self.signal_id)?;
        match self.signal_id {
            SignalIdentifier::SetConfiguration | SignalIdentifier::Reconfigure => {
                write!(f, "category {:?}, ", self.service_category)?;
            }
            SignalIdentifier::Start | SignalIdentifier::Suspend => {
                write!(f, "stream id {:?}, ", self.stream_endpoint_id)?;
            }
            _ => {}
        };
        write!(f, "{:x}", error_code)
    }
}

decodable_enum! {
    /// Error Codes that can be returned as part of a reject message.
    /// See Section 8.20.6
    pub enum ErrorCode<u8, Error, OutOfRange> {
        // Header Error Codes
        BadHeaderFormat = 0x01,

        // Payload Format Error Codes
        BadLength = 0x11,
        BadAcpSeid = 0x12,
        SepInUse = 0x13,
        SepNotInUse = 0x14,
        BadServiceCategory = 0x17,
        BadPayloadFormat = 0x18,
        NotSupportedCommand = 0x19,
        InvalidCapabilities = 0x1A,

        // Transport Service Capabilities Error Codes
        BadRecoveryType = 0x22,
        BadMediaTransportFormat = 0x23,
        BadRecoveryFormat = 0x25,
        BadRohcFormat = 0x26,
        BadCpFormat = 0x27,
        BadMultiplexingFormat = 0x28,
        UnsupportedConfiguration = 0x29,

        // Procedure Error Codes
        BadState = 0x31,
    }
}

/// An AVDTP Transaction Label
/// Not used outside the library.  Public as part of some internal Error variants.
/// See Section 8.4.1
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TxLabel(u8);

// Transaction labels are only 4 bits.
const MAX_TX_LABEL: u8 = 0xF;

impl TryFrom<u8> for TxLabel {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        if value > MAX_TX_LABEL {
            Err(Error::OutOfRange)
        } else {
            Ok(TxLabel(value))
        }
    }
}

impl From<&TxLabel> for u8 {
    fn from(v: &TxLabel) -> u8 {
        v.0
    }
}

impl From<&TxLabel> for usize {
    fn from(v: &TxLabel) -> usize {
        v.0 as usize
    }
}

decodable_enum! {
    /// Type of media
    /// USed to specify the type of media on a stream endpoint.
    /// Part of the StreamInformation in Discovery Response.
    /// Defined in the Bluetooth Assigned Numbers
    /// https://www.bluetooth.com/specifications/assigned-numbers/audio-video
    pub enum MediaType<u8, Error, OutOfRange> {
        Audio = 0x00,
        Video = 0x01,
        Multimedia = 0x02,
    }
}

decodable_enum! {
    /// Type of endpoint (source or sync)
    /// Part of the StreamInformation in Discovery Response.
    /// See Section 8.20.3
    pub enum EndpointType<u8, Error, OutOfRange> {
        Source = 0x00,
        Sink = 0x01,
    }
}

impl EndpointType {
    pub fn opposite(&self) -> Self {
        match self {
            Self::Source => Self::Sink,
            Self::Sink => Self::Source,
        }
    }
}

decodable_enum! {
    /// Indicated whether this packet is part of a fragmented packet set.
    /// See Section 8.4.2
    pub(crate) enum SignalingPacketType<u8, Error, OutOfRange> {
        Single = 0x00,
        Start = 0x01,
        Continue = 0x02,
        End = 0x03,
    }
}

decodable_enum! {
    /// Specifies the command type of each signaling command or the response
    /// type of each response packet.
    /// See Section 8.4.3
    pub(crate) enum SignalingMessageType<u8, Error, OutOfRange> {
        Command = 0x00,
        GeneralReject = 0x01,
        ResponseAccept = 0x02,
        ResponseReject = 0x03,
    }
}

decodable_enum! {
    /// Indicates the signaling command on a command packet.  The same identifier is used on the
    /// response to that command packet.
    /// See Section 8.4.4
    pub enum SignalIdentifier<u8, Error, OutOfRange> {
        Discover = 0x01,
        GetCapabilities = 0x02,
        SetConfiguration = 0x03,
        GetConfiguration = 0x04,
        Reconfigure = 0x05,
        Open = 0x06,
        Start = 0x07,
        Close = 0x08,
        Suspend = 0x09,
        Abort = 0x0A,
        SecurityControl = 0x0B,
        GetAllCapabilities = 0x0C,
        DelayReport = 0x0D,
    }
}

#[derive(Debug)]
pub(crate) struct SignalingHeader {
    pub label: TxLabel,
    packet_type: SignalingPacketType,
    pub(crate) message_type: SignalingMessageType,
    num_packets: u8,
    pub signal: SignalIdentifier,
}

impl SignalingHeader {
    pub fn new(
        label: TxLabel,
        signal: SignalIdentifier,
        message_type: SignalingMessageType,
    ) -> SignalingHeader {
        SignalingHeader {
            label,
            signal,
            message_type,
            packet_type: SignalingPacketType::Single,
            num_packets: 1,
        }
    }

    pub fn label(&self) -> TxLabel {
        self.label
    }

    pub fn signal(&self) -> SignalIdentifier {
        self.signal
    }

    pub fn is_type(&self, other: SignalingMessageType) -> bool {
        self.message_type == other
    }

    pub fn is_command(&self) -> bool {
        self.is_type(SignalingMessageType::Command)
    }
}

impl Decodable for SignalingHeader {
    type Error = Error;

    fn decode(bytes: &[u8]) -> Result<SignalingHeader> {
        if bytes.len() < 2 {
            return Err(Error::OutOfRange);
        }
        let label = TxLabel::try_from(bytes[0] >> 4)?;
        let packet_type = SignalingPacketType::try_from((bytes[0] >> 2) & 0x3)?;
        let (id_offset, num_packets) = match packet_type {
            SignalingPacketType::Start => {
                if bytes.len() < 3 {
                    return Err(Error::OutOfRange);
                }
                (2, bytes[1])
            }
            _ => (1, 1),
        };
        let signal_id_val = bytes[id_offset] & 0x3F;
        let id = SignalIdentifier::try_from(signal_id_val)
            .map_err(|_| Error::InvalidSignalId(label, signal_id_val))?;
        let header = SignalingHeader {
            label,
            packet_type,
            message_type: SignalingMessageType::try_from(bytes[0] & 0x3)?,
            signal: id,
            num_packets,
        };
        Ok(header)
    }
}

impl Encodable for SignalingHeader {
    type Error = Error;

    fn encoded_len(&self) -> usize {
        if self.num_packets > 1 {
            3
        } else {
            2
        }
    }

    fn encode(&self, buf: &mut [u8]) -> Result<()> {
        if buf.len() < self.encoded_len() {
            return Err(Error::Encoding);
        }
        buf[0] = u8::from(&self.label) << 4
            | u8::from(&self.packet_type) << 2
            | u8::from(&self.message_type);
        buf[1] = u8::from(&self.signal);
        Ok(())
    }
}

/// A Stream Endpoint Identifier, aka SEID, INT SEID, ACP SEID - Sec 8.20.1
/// Valid values are 0x01 - 0x3E
#[derive(Debug, PartialEq, Eq, Clone, Hash)]
pub struct StreamEndpointId(pub(crate) u8);

impl StreamEndpointId {
    /// Interpret a StreamEndpointId from the upper six bits of a byte, which
    /// is often how it's transmitted in a message.
    pub(crate) fn from_msg(byte: &u8) -> Self {
        StreamEndpointId(byte >> 2)
    }

    /// Produce a byte where the SEID value is placed in the upper six bits,
    /// which is often how it is placed in a message.
    pub(crate) fn to_msg(&self) -> u8 {
        self.0 << 2
    }
}

impl TryFrom<u8> for StreamEndpointId {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        if value == 0 || value > 0x3E {
            Err(Error::OutOfRange)
        } else {
            Ok(StreamEndpointId(value))
        }
    }
}

impl fmt::Display for StreamEndpointId {
    fn fmt(&self, fmt: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(fmt, "{}", self.0)
    }
}

/// The type of the codec in the MediaCodec Service Capability
/// Valid values are defined in the Bluetooth Assigned Numbers and are
/// interpreted differently for different Media Types, so we do not interpret
/// them here.
/// Associated constants are provided that specify the value of `MediaCodecType`
/// for different codecs given the `MediaType::Audio`.
/// See https://www.bluetooth.com/specifications/assigned-numbers/audio-video
#[derive(PartialEq, Eq, Clone)]
pub struct MediaCodecType(u8);

impl MediaCodecType {
    pub const AUDIO_SBC: Self = MediaCodecType(0b0);
    pub const AUDIO_MPEG12: Self = MediaCodecType(0b1);
    pub const AUDIO_AAC: Self = MediaCodecType(0b10);
    pub const AUDIO_ATRAC: Self = MediaCodecType(0b100);
    pub const AUDIO_NON_A2DP: Self = MediaCodecType(0b1111_1111);

    pub fn new(num: u8) -> MediaCodecType {
        MediaCodecType(num)
    }
}

impl fmt::Debug for MediaCodecType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            &Self::AUDIO_SBC => write!(f, "MediaCodecType::AUDIO_SBC"),
            &Self::AUDIO_MPEG12 => write!(f, "MediaCodecType::AUDIO_MPEG12"),
            &Self::AUDIO_AAC => write!(f, "MediaCodecType::AUDIO_AAC"),
            &Self::AUDIO_ATRAC => write!(f, "MediaCodecType::AUDIO_ATRAC"),
            &Self::AUDIO_NON_A2DP => write!(f, "MediaCodecType::AUDIO_NON_A2DP"),
            _ => f.debug_tuple("MediaCodecType").field(&self.0).finish(),
        }
    }
}

/// The type of content protection used in the Content Protection Service Capability.
/// Defined in the Bluetooth Assigned Numbers
/// https://www.bluetooth.com/specifications/assigned-numbers/audio-video
#[derive(Debug, PartialEq, Clone)]
pub enum ContentProtectionType {
    DigitalTransmissionContentProtection, // DTCP, 0x0001
    SerialCopyManagementSystem,           // SCMS-T, 0x0002
}

impl ContentProtectionType {
    fn to_le_bytes(&self) -> [u8; 2] {
        match self {
            ContentProtectionType::DigitalTransmissionContentProtection => [0x01, 0x00],
            ContentProtectionType::SerialCopyManagementSystem => [0x02, 0x00],
        }
    }
}

impl TryFrom<u16> for ContentProtectionType {
    type Error = Error;

    fn try_from(val: u16) -> Result<Self> {
        match val {
            1 => Ok(ContentProtectionType::DigitalTransmissionContentProtection),
            2 => Ok(ContentProtectionType::SerialCopyManagementSystem),
            _ => Err(Error::OutOfRange),
        }
    }
}

decodable_enum! {
    /// Indicates the signaling command on a command packet.  The same identifier is used on the
    /// response to that command packet.
    /// See Section 8.4.4
    pub enum ServiceCategory<u8, Error, OutOfRange> {
        None = 0x00,
        MediaTransport = 0x01,
        Reporting = 0x02,
        Recovery = 0x03,
        ContentProtection = 0x04,
        HeaderCompression = 0x05,
        Multiplexing = 0x06,
        MediaCodec = 0x07,
        DelayReporting = 0x08,
    }
}

/// Service Capabilities indicate possible services that can be provided by
/// each stream endpoint.  See AVDTP Spec section 8.21.
#[derive(Debug, PartialEq, Clone)]
pub enum ServiceCapability {
    /// Indicates that the end point can provide at least basic media transport
    /// service as defined by RFC 3550 and outlined in section 7.2.
    /// Defined in section 8.21.2
    MediaTransport,
    /// Indicates that the end point can provide reporting service as outlined in section 7.3
    /// Defined in section 8.21.3
    Reporting,
    /// Indicates the end point can provide recovery service as outlined in section 7.4
    /// Defined in section 8.21.4
    Recovery { recovery_type: u8, max_recovery_window_size: u8, max_number_media_packets: u8 },
    /// Indicates the codec which is supported by this end point. |codec_extra| is defined within
    /// the relevant profiles (A2DP for Audio, etc).
    /// Defined in section 8.21.5
    MediaCodec {
        media_type: MediaType,
        codec_type: MediaCodecType,
        codec_extra: Vec<u8>, // TODO: Media codec specific information elements
    },
    /// Present when the device has content protection capability.
    /// |extra| is defined elsewhere.
    /// Defined in section 8.21.6
    ContentProtection {
        protection_type: ContentProtectionType,
        extra: Vec<u8>, // Protection specific parameters
    },
    /// Indicates that header compression capabilities is offered  by this end point.
    /// Defined in section 8.21.7
    /// TODO(fxbug.dev/38280): Implement header compression specific fields to use the payload.
    HeaderCompression { payload_len: u8 },
    /// Indicates that multiplexing service is offered by this end point.
    /// Defined in section 8.21.8
    /// TODO(fxbug.dev/38282): Implement multiplexing specific fields to use the payload.
    Multiplexing { payload_len: u8 },
    /// Indicates that delay reporting is offered by this end point.
    /// Defined in section 8.21.9
    DelayReporting,
}

impl ServiceCapability {
    pub fn category(&self) -> ServiceCategory {
        match self {
            ServiceCapability::MediaTransport => ServiceCategory::MediaTransport,
            ServiceCapability::Reporting => ServiceCategory::Reporting,
            ServiceCapability::Recovery { .. } => ServiceCategory::Recovery,
            ServiceCapability::ContentProtection { .. } => ServiceCategory::ContentProtection,
            ServiceCapability::MediaCodec { .. } => ServiceCategory::MediaCodec,
            ServiceCapability::DelayReporting => ServiceCategory::DelayReporting,
            ServiceCapability::HeaderCompression { .. } => ServiceCategory::HeaderCompression,
            ServiceCapability::Multiplexing { .. } => ServiceCategory::Multiplexing,
        }
    }

    pub(crate) fn length_of_service_capabilities(&self) -> u8 {
        match self {
            ServiceCapability::MediaTransport => 0,
            ServiceCapability::Reporting => 0,
            ServiceCapability::Recovery { .. } => 3,
            ServiceCapability::MediaCodec { codec_extra, .. } => 2 + codec_extra.len() as u8,
            ServiceCapability::ContentProtection { extra, .. } => 2 + extra.len() as u8,
            ServiceCapability::DelayReporting => 0,
            ServiceCapability::HeaderCompression { payload_len } => *payload_len,
            ServiceCapability::Multiplexing { payload_len } => *payload_len,
        }
    }

    /// True when this ServiceCapability is a "basic" capability.
    /// See Table 8.47 in Section 8.21.1
    pub(crate) fn is_basic(&self) -> bool {
        match self {
            ServiceCapability::DelayReporting => false,
            _ => true,
        }
    }

    /// True when this capability should be included in the response to a |sig| command.
    pub(crate) fn in_response(&self, sig: SignalIdentifier) -> bool {
        sig != SignalIdentifier::GetCapabilities || self.is_basic()
    }

    /// True when this capability is classified as an "Application Service Capability"
    pub(crate) fn is_application(&self) -> bool {
        match self {
            ServiceCapability::MediaCodec { .. } | ServiceCapability::ContentProtection { .. } => {
                true
            }
            _ => false,
        }
    }

    pub fn is_codec(&self) -> bool {
        match self {
            ServiceCapability::MediaCodec { .. } => true,
            _ => false,
        }
    }

    pub fn codec_type(&self) -> Option<&MediaCodecType> {
        match self {
            ServiceCapability::MediaCodec { codec_type, .. } => Some(codec_type),
            _ => None,
        }
    }
}

impl Decodable for ServiceCapability {
    type Error = Error;

    fn decode(from: &[u8]) -> Result<Self> {
        if from.len() < 2 {
            return Err(Error::Encoding);
        }
        let length_of_capability = from[1] as usize;
        let d = match ServiceCategory::try_from(from[0]) {
            Ok(ServiceCategory::MediaTransport) => match length_of_capability {
                0 => ServiceCapability::MediaTransport,
                _ => return Err(Error::RequestInvalid(ErrorCode::BadMediaTransportFormat)),
            },
            Ok(ServiceCategory::Reporting) => match length_of_capability {
                0 => ServiceCapability::Reporting,
                _ => return Err(Error::RequestInvalid(ErrorCode::BadPayloadFormat)),
            },
            Ok(ServiceCategory::Recovery) => {
                if from.len() < 5 || length_of_capability != 3 {
                    return Err(Error::RequestInvalid(ErrorCode::BadRecoveryFormat));
                }
                let recovery_type = from[2];
                let mrws = from[3];
                let mnmp = from[4];
                // Check format of parameters. See Section 8.21.4, Table 8.51
                // The only recovery type is RFC2733 (0x01)
                if recovery_type != 0x01 {
                    return Err(Error::RequestInvalid(ErrorCode::BadRecoveryType));
                }
                // The MRWS and MNMP must be 0x01 - 0x18
                if mrws < 0x01 || mrws > 0x18 || mnmp < 0x01 || mnmp > 0x18 {
                    return Err(Error::RequestInvalid(ErrorCode::BadRecoveryFormat));
                }
                ServiceCapability::Recovery {
                    recovery_type,
                    max_recovery_window_size: mrws,
                    max_number_media_packets: mnmp,
                }
            }
            Ok(ServiceCategory::ContentProtection) => {
                let cp_err = Err(Error::RequestInvalid(ErrorCode::BadCpFormat));
                if from.len() < 4
                    || length_of_capability < 2
                    || from.len() < length_of_capability + 2
                {
                    return cp_err;
                }
                let cp_type: u16 = ((from[3] as u16) << 8) + from[2] as u16;
                let protection_type = match ContentProtectionType::try_from(cp_type) {
                    Ok(val) => val,
                    Err(_) => return cp_err,
                };
                let extra_len = length_of_capability - 2;
                let mut extra = vec![0; extra_len];
                extra.copy_from_slice(&from[4..4 + extra_len]);
                ServiceCapability::ContentProtection { protection_type, extra }
            }
            Ok(ServiceCategory::MediaCodec) => {
                let err = Err(Error::RequestInvalid(ErrorCode::BadPayloadFormat));
                if from.len() < 4
                    || length_of_capability < 2
                    || from.len() < length_of_capability + 2
                {
                    return err;
                }
                let media_type = match MediaType::try_from(from[2] >> 4) {
                    Ok(media) => media,
                    Err(_) => return err,
                };
                let codec_type = MediaCodecType::new(from[3]);
                let extra_len = length_of_capability - 2;
                let mut codec_extra = vec![0; extra_len];
                codec_extra.copy_from_slice(&from[4..4 + extra_len]);
                ServiceCapability::MediaCodec { media_type, codec_type, codec_extra }
            }
            Ok(ServiceCategory::DelayReporting) => match length_of_capability {
                0 => ServiceCapability::DelayReporting,
                _ => return Err(Error::RequestInvalid(ErrorCode::BadPayloadFormat)),
            },
            Ok(ServiceCategory::Multiplexing) => {
                ServiceCapability::Multiplexing { payload_len: length_of_capability as u8 }
            }
            Ok(ServiceCategory::HeaderCompression) => {
                ServiceCapability::HeaderCompression { payload_len: length_of_capability as u8 }
            }
            _ => {
                return Err(Error::RequestInvalid(ErrorCode::BadServiceCategory));
            }
        };
        Ok(d)
    }
}

impl Encodable for ServiceCapability {
    type Error = Error;

    fn encoded_len(&self) -> usize {
        2 + self.length_of_service_capabilities() as usize
    }

    fn encode(&self, buf: &mut [u8]) -> Result<()> {
        if buf.len() < self.encoded_len() {
            return Err(Error::Encoding);
        }
        let mut cursor = Cursor::new(buf);
        let _ = cursor
            .write(&[u8::from(&self.category()), self.length_of_service_capabilities()])
            .map_err(|_| Error::Encoding)?;
        match self {
            ServiceCapability::Recovery {
                recovery_type: t,
                max_recovery_window_size: max_size,
                max_number_media_packets: max_packets,
            } => {
                let _ =
                    cursor.write(&[*t, *max_size, *max_packets]).map_err(|_| Error::Encoding)?;
            }
            ServiceCapability::MediaCodec { media_type, codec_type, codec_extra } => {
                let _ = cursor
                    .write(&[u8::from(media_type) << 4, codec_type.0])
                    .map_err(|_| Error::Encoding)?;
                let _ = cursor.write(codec_extra.as_slice()).map_err(|_| Error::Encoding)?;
            }
            ServiceCapability::ContentProtection { protection_type, extra } => {
                let _ =
                    cursor.write(&protection_type.to_le_bytes()).map_err(|_| Error::Encoding)?;
                let _ = cursor.write(extra.as_slice()).map_err(|_| Error::Encoding)?;
            }
            _ => {}
        }
        Ok(())
    }
}

/// All information related to a stream. Part of the Discovery Response.
/// See Sec 8.6.2
#[derive(Debug, PartialEq)]
pub struct StreamInformation {
    id: StreamEndpointId,
    in_use: bool,
    media_type: MediaType,
    endpoint_type: EndpointType,
}

impl StreamInformation {
    /// Create a new StreamInformation from an ID.
    /// This will only fail if the ID given is out of the range of valid SEIDs (0x01 - 0x3E)
    pub fn new(
        id: StreamEndpointId,
        in_use: bool,
        media_type: MediaType,
        endpoint_type: EndpointType,
    ) -> StreamInformation {
        StreamInformation { id, in_use, media_type, endpoint_type }
    }

    pub fn id(&self) -> &StreamEndpointId {
        &self.id
    }

    pub fn media_type(&self) -> &MediaType {
        &self.media_type
    }

    pub fn endpoint_type(&self) -> &EndpointType {
        &self.endpoint_type
    }

    pub fn in_use(&self) -> &bool {
        &self.in_use
    }
}

impl Decodable for StreamInformation {
    type Error = Error;

    fn decode(from: &[u8]) -> Result<Self> {
        if from.len() < 2 {
            return Err(Error::InvalidMessage);
        }
        let id = StreamEndpointId::from_msg(&from[0]);
        let in_use: bool = from[0] & 0x02 != 0;
        let media_type = MediaType::try_from(from[1] >> 4)?;
        let endpoint_type = EndpointType::try_from((from[1] >> 3) & 0x1)?;
        Ok(StreamInformation { id, in_use, media_type, endpoint_type })
    }
}

impl Encodable for StreamInformation {
    type Error = Error;

    fn encoded_len(&self) -> usize {
        2
    }

    fn encode(&self, into: &mut [u8]) -> Result<()> {
        if into.len() < self.encoded_len() {
            return Err(Error::Encoding);
        }
        into[0] = self.id.to_msg() | if self.in_use { 0x02 } else { 0x00 };
        into[1] = u8::from(&self.media_type) << 4 | u8::from(&self.endpoint_type) << 3;
        Ok(())
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use assert_matches::assert_matches;

    #[test]
    fn remote_reject_from_params_short() {
        let r = RemoteReject::from_params(SignalIdentifier::Discover, &[]);
        assert_eq!(None, r.error_code());
        assert_eq!(None, r.service_category());
        assert_eq!(None, r.stream_id());

        let r = RemoteReject::from_params(SignalIdentifier::Start, &[0x12]);
        assert_eq!(None, r.error_code());
        assert_eq!(None, r.service_category());
        assert_eq!(Some(StreamEndpointId(4)), r.stream_id());

        let r = RemoteReject::from_params(SignalIdentifier::SetConfiguration, &[0x02]);
        assert_eq!(None, r.error_code());
        assert_eq!(Some(Ok(ServiceCategory::Reporting)), r.service_category());
        assert_eq!(None, r.stream_id());
    }

    #[test]
    fn remote_reject_ignores_unexpected_params() {
        let r: RemoteReject =
            RemoteReject::from_params(SignalIdentifier::Open, &[0x01, 0x02, 0x03, 0x04]);
        assert_eq!(Some(Ok(ErrorCode::BadHeaderFormat)), r.error_code());
        assert_eq!(None, r.service_category());
        assert_eq!(None, r.stream_id());

        let r: RemoteReject = RemoteReject::from_params(
            SignalIdentifier::SetConfiguration,
            &[0x02, 0x12, 0x03, 0x04],
        );
        assert_eq!(Some(Ok(ErrorCode::BadAcpSeid)), r.error_code());
        assert_eq!(Some(Ok(ServiceCategory::Reporting)), r.service_category());
        assert_eq!(None, r.stream_id());

        let r: RemoteReject =
            RemoteReject::from_params(SignalIdentifier::Start, &[0x04, 0x01, 0x03, 0x04]);
        assert_eq!(Some(Ok(ErrorCode::BadHeaderFormat)), r.error_code());
        assert_eq!(None, r.service_category());
        assert_eq!(Some(StreamEndpointId(1)), r.stream_id());
    }

    #[test]
    fn remote_reject_unrecognized_category() {
        let r: RemoteReject =
            RemoteReject::from_params(SignalIdentifier::SetConfiguration, &[0xF0, 0x01]);
        assert_eq!(Some(Ok(ErrorCode::BadHeaderFormat)), r.error_code());
        assert_eq!(Some(Err(0xF0)), r.service_category());
        assert_eq!(None, r.stream_id());
    }

    #[test]
    fn remote_reject_unrecognized_errorcode() {
        let r: RemoteReject = RemoteReject::from_params(SignalIdentifier::Discover, &[0xF1]);
        assert_eq!(Some(Err(0xF1)), r.error_code());
        assert_eq!(None, r.service_category());
        assert_eq!(None, r.stream_id());
    }

    #[test]
    fn txlabel_tofrom_u8() {
        let mut label: Result<TxLabel> = TxLabel::try_from(15);
        assert!(label.is_ok());
        assert_eq!(15, u8::from(&label.unwrap()));
        label = TxLabel::try_from(16);
        assert_matches!(label, Err(Error::OutOfRange));
    }

    #[test]
    fn txlabel_to_usize() {
        let label = TxLabel::try_from(1).unwrap();
        assert_eq!(1, usize::from(&label));
    }

    #[test]
    fn test_capability_inspection() {
        let sbc_codec = ServiceCapability::MediaCodec {
            media_type: MediaType::Audio,
            codec_type: MediaCodecType::AUDIO_SBC.clone(),
            codec_extra: vec![0x01],
        };
        let transport = ServiceCapability::MediaTransport;

        assert!(sbc_codec.is_application());
        assert!(!transport.is_application());

        assert!(sbc_codec.is_codec());
        assert!(!transport.is_codec());

        assert_eq!(Some(&MediaCodecType::AUDIO_SBC), sbc_codec.codec_type());
        assert_eq!(None, transport.codec_type());
    }
}
