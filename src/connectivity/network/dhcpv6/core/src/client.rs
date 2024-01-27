// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Core DHCPv6 client state transitions.

use assert_matches::assert_matches;
use derivative::Derivative;
use net_types::ip::{Ipv6Addr, Subnet};
use num::{rational::Ratio, CheckedMul};
use packet::serialize::InnerPacketBuilder;
use packet_formats_dhcp::v6;
use rand::{thread_rng, Rng};
use std::{
    cmp::{Eq, Ord, PartialEq, PartialOrd},
    collections::{hash_map::Entry, BinaryHeap, HashMap, HashSet},
    convert::TryFrom,
    default::Default,
    fmt::Debug,
    hash::Hash,
    marker::PhantomData,
    time::Duration,
};
use tracing::{info, warn};
use zerocopy::ByteSlice;

use crate::{Instant, InstantExt as _};

/// Initial Information-request timeout `INF_TIMEOUT` from [RFC 8415, Section 7.6].
///
/// [RFC 8415, Section 7.6]: https://tools.ietf.org/html/rfc8415#section-7.6
const INITIAL_INFO_REQ_TIMEOUT: Duration = Duration::from_secs(1);
/// Max Information-request timeout `INF_MAX_RT` from [RFC 8415, Section 7.6].
///
/// [RFC 8415, Section 7.6]: https://tools.ietf.org/html/rfc8415#section-7.6
const MAX_INFO_REQ_TIMEOUT: Duration = Duration::from_secs(3600);
/// Default information refresh time from [RFC 8415, Section 7.6].
///
/// [RFC 8415, Section 7.6]: https://tools.ietf.org/html/rfc8415#section-7.6
const IRT_DEFAULT: Duration = Duration::from_secs(86400);

/// The max duration in seconds `std::time::Duration` supports.
///
/// NOTE: it is possible for `Duration` to be bigger by filling in the nanos
/// field, but this value is good enough for the purpose of this crate.
const MAX_DURATION: Duration = Duration::from_secs(std::u64::MAX);

/// Initial Solicit timeout `SOL_TIMEOUT` from [RFC 8415, Section 7.6].
///
/// [RFC 8415, Section 7.6]: https://tools.ietf.org/html/rfc8415#section-7.6
const INITIAL_SOLICIT_TIMEOUT: Duration = Duration::from_secs(1);

/// Max Solicit timeout `SOL_MAX_RT` from [RFC 8415, Section 7.6].
///
/// [RFC 8415, Section 7.6]: https://tools.ietf.org/html/rfc8415#section-7.6
const MAX_SOLICIT_TIMEOUT: Duration = Duration::from_secs(3600);

/// The valid range for `SOL_MAX_RT`, as defined in [RFC 8415, Section 21.24].
///
/// [RFC 8415, Section 21.24](https://datatracker.ietf.org/doc/html/rfc8415#section-21.24)
const VALID_MAX_SOLICIT_TIMEOUT_RANGE: std::ops::RangeInclusive<u32> = 60..=86400;

/// The maximum [Preference option] value that can be present in an advertise,
/// as described in [RFC 8415, Section 18.2.1].
///
/// [RFC 8415, Section 18.2.1]: https://datatracker.ietf.org/doc/html/rfc8415#section-18.2.1
/// [Preference option]: https://datatracker.ietf.org/doc/html/rfc8415#section-21.8
const ADVERTISE_MAX_PREFERENCE: u8 = std::u8::MAX;

/// Denominator used for transforming the elapsed time from milliseconds to
/// hundredths of a second.
///
/// [RFC 8415, Section 21.9]: https://tools.ietf.org/html/rfc8415#section-21.9
const ELAPSED_TIME_DENOMINATOR: u128 = 10;

/// The length of the [Client Identifier].
///
/// [Client Identifier]: https://datatracker.ietf.org/doc/html/rfc8415#section-21.2
const CLIENT_ID_LEN: usize = 18;

/// The minimum value for the randomization factor `RAND` used in calculating
/// retransmission timeout, as specified in [RFC 8415, Section 15].
///
/// [RFC 8415, Section 15](https://datatracker.ietf.org/doc/html/rfc8415#section-15)
const RANDOMIZATION_FACTOR_MIN: f64 = -0.1;

/// The maximum value for the randomization factor `RAND` used in calculating
/// retransmission timeout, as specified in [RFC 8415, Section 15].
///
/// [RFC 8415, Section 15](https://datatracker.ietf.org/doc/html/rfc8415#section-15)
const RANDOMIZATION_FACTOR_MAX: f64 = 0.1;

/// Initial Request timeout `REQ_TIMEOUT` from [RFC 8415, Section 7.6].
///
/// [RFC 8415, Section 7.6]: https://tools.ietf.org/html/rfc8415#section-7.6
const INITIAL_REQUEST_TIMEOUT: Duration = Duration::from_secs(1);

/// Max Request timeout `REQ_MAX_RT` from [RFC 8415, Section 7.6].
///
/// [RFC 8415, Section 7.6]: https://tools.ietf.org/html/rfc8415#section-7.6
const MAX_REQUEST_TIMEOUT: Duration = Duration::from_secs(30);

/// Max Request retry attempts `REQ_MAX_RC` from [RFC 8415, Section 7.6].
///
/// [RFC 8415, Section 7.6]: https://tools.ietf.org/html/rfc8415#section-7.6
const REQUEST_MAX_RC: u8 = 10;

/// The ratio used for calculating T1 based on the shortest preferred lifetime,
/// when the T1 value received from the server is 0.
///
/// When T1 is set to 0 by the server, the value is left to the discretion of
/// the client, as described in [RFC 8415, Section 14.2]. The client computes
/// T1 using the recommended ratio from [RFC 8415, Section 21.4]:
///    T1 = shortest lifetime * 0.5
///
/// [RFC 8415, Section 14.2]: https://datatracker.ietf.org/doc/html/rfc8415#section-14.2
/// [RFC 8415, Section 21.4]: https://datatracker.ietf.org/doc/html/rfc8415#section-21.4
const T1_MIN_LIFETIME_RATIO: Ratio<u32> = Ratio::new_raw(1, 2);

/// The ratio used for calculating T2 based on T1, when the T2 value received
/// from the server is 0.
///
/// When T2 is set to 0 by the server, the value is left to the discretion of
/// the client, as described in [RFC 8415, Section 14.2]. The client computes
/// T2 using the recommended ratios from [RFC 8415, Section 21.4]:
///    T2 = T1 * 0.8 / 0.5
///
/// [RFC 8415, Section 14.2]: https://datatracker.ietf.org/doc/html/rfc8415#section-14.2
/// [RFC 8415, Section 21.4]: https://datatracker.ietf.org/doc/html/rfc8415#section-21.4
const T2_T1_RATIO: Ratio<u32> = Ratio::new_raw(8, 5);

/// Initial Renew timeout `REN_TIMEOUT` from [RFC 8415, Section 7.6].
///
/// [RFC 8415, Section 7.6]: https://tools.ietf.org/html/rfc8415#section-7.6
const INITIAL_RENEW_TIMEOUT: Duration = Duration::from_secs(10);

/// Max Renew timeout `REN_MAX_RT` from [RFC 8415, Section 7.6].
///
/// [RFC 8415, Section 7.6]: https://tools.ietf.org/html/rfc8415#section-7.6
const MAX_RENEW_TIMEOUT: Duration = Duration::from_secs(600);

/// Initial Rebind timeout `REB_TIMEOUT` from [RFC 8415, Section 7.6].
///
/// [RFC 8415, Section 7.6]: https://tools.ietf.org/html/rfc8415#section-7.6
const INITIAL_REBIND_TIMEOUT: Duration = Duration::from_secs(10);

/// Max Rebind timeout `REB_MAX_RT` from [RFC 8415, Section 7.6].
///
/// [RFC 8415, Section 7.6]: https://tools.ietf.org/html/rfc8415#section-7.6
const MAX_REBIND_TIMEOUT: Duration = Duration::from_secs(600);

const IA_NA_NAME: &'static str = "IA_NA";
const IA_PD_NAME: &'static str = "IA_PD";

/// Calculates retransmission timeout based on formulas defined in [RFC 8415, Section 15].
/// A zero `prev_retrans_timeout` indicates this is the first transmission, so
/// `initial_retrans_timeout` will be used.
///
/// Relevant formulas from [RFC 8415, Section 15]:
///
/// ```text
/// RT      Retransmission timeout
/// IRT     Initial retransmission time
/// MRT     Maximum retransmission time
/// RAND    Randomization factor
///
/// RT for the first message transmission is based on IRT:
///
///     RT = IRT + RAND*IRT
///
/// RT for each subsequent message transmission is based on the previous value of RT:
///
///     RT = 2*RTprev + RAND*RTprev
///
/// MRT specifies an upper bound on the value of RT (disregarding the randomization added by
/// the use of RAND).  If MRT has a value of 0, there is no upper limit on the value of RT.
/// Otherwise:
///
///     if (RT > MRT)
///         RT = MRT + RAND*MRT
/// ```
///
/// [RFC 8415, Section 15]: https://tools.ietf.org/html/rfc8415#section-15
fn retransmission_timeout<R: Rng>(
    prev_retrans_timeout: Duration,
    initial_retrans_timeout: Duration,
    max_retrans_timeout: Duration,
    rng: &mut R,
) -> Duration {
    let rand = rng.gen_range(RANDOMIZATION_FACTOR_MIN..RANDOMIZATION_FACTOR_MAX);

    let next_rt = if prev_retrans_timeout.as_nanos() == 0 {
        let irt = initial_retrans_timeout.as_secs_f64();
        irt + rand * irt
    } else {
        let rt = prev_retrans_timeout.as_secs_f64();
        2. * rt + rand * rt
    };

    if max_retrans_timeout.as_nanos() == 0 || next_rt < max_retrans_timeout.as_secs_f64() {
        clipped_duration(next_rt)
    } else {
        let mrt = max_retrans_timeout.as_secs_f64();
        clipped_duration(mrt + rand * mrt)
    }
}

/// Clips overflow and returns a duration using the input seconds.
fn clipped_duration(secs: f64) -> Duration {
    if secs <= 0. {
        Duration::from_nanos(0)
    } else if secs >= MAX_DURATION.as_secs_f64() {
        MAX_DURATION
    } else {
        Duration::from_secs_f64(secs)
    }
}

/// Creates a transaction ID used by the client to match outgoing messages with
/// server replies, as defined in [RFC 8415, Section 16.1].
///
/// [RFC 8415, Section 16.1]: https://tools.ietf.org/html/rfc8415#section-16.1
pub fn transaction_id() -> [u8; 3] {
    let mut id = [0u8; 3];
    thread_rng().fill(&mut id[..]);
    id
}

/// Identifies what event should be triggered when a timer fires.
#[derive(Debug, PartialEq, Eq, Hash, Copy, Clone)]
pub enum ClientTimerType {
    Retransmission,
    Refresh,
    Renew,
    Rebind,
    RestartServerDiscovery,
}

/// Possible actions that need to be taken for a state transition to happen successfully.
#[derive(Debug, PartialEq, Clone)]
pub enum Action<I> {
    SendMessage(Vec<u8>),
    /// Schedules a timer to fire at a specified time instant.
    ///
    /// If the timer is already scheduled to fire at some time, this action
    /// will result in the timer being rescheduled to the new time.
    ScheduleTimer(ClientTimerType, I),
    /// Cancels a timer.
    ///
    /// If the timer is not scheduled, this action should effectively be a
    /// no-op.
    CancelTimer(ClientTimerType),
    UpdateDnsServers(Vec<Ipv6Addr>),
    /// The updates for IA_NA bindings.
    ///
    /// Only changes to an existing bindings is conveyed through this
    /// variant. That is, an update missing for an (`IAID`, `Ipv6Addr`) means
    /// no new change for the address.
    ///
    /// Updates include the preferred/valid lifetimes for an address and it
    /// is up to the action-taker to deprecate/invalidate addresses after the
    /// appropriate lifetimes. That is, there will be no dedicated update
    /// for preferred/valid lifetime expiration.
    IaNaUpdates(HashMap<v6::IAID, HashMap<Ipv6Addr, IaValueUpdateKind>>),
    /// The updates for IA_PD bindings.
    ///
    /// Only changes to an existing bindings is conveyed through this
    /// variant. That is, an update missing for an (`IAID`, `Subnet<Ipv6Addr>`)
    /// means no new change for the prefix.
    ///
    /// Updates include the preferred/valid lifetimes for a prefix and it
    /// is up to the action-taker to deprecate/invalidate prefixes after the
    /// appropriate lifetimes. That is, there will be no dedicated update
    /// for preferred/valid lifetime expiration.
    IaPdUpdates(HashMap<v6::IAID, HashMap<Subnet<Ipv6Addr>, IaValueUpdateKind>>),
}

pub type Actions<I> = Vec<Action<I>>;

/// Holds data and provides methods for handling state transitions from information requesting
/// state.
#[derive(Debug)]
struct InformationRequesting<I> {
    retrans_timeout: Duration,
    _marker: PhantomData<I>,
}

impl<I: Instant> InformationRequesting<I> {
    /// Starts in information requesting state following [RFC 8415, Section 18.2.6].
    ///
    /// [RFC 8415, Section 18.2.6]: https://tools.ietf.org/html/rfc8415#section-18.2.6
    fn start<R: Rng>(
        transaction_id: [u8; 3],
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        let info_req = Self { retrans_timeout: Default::default(), _marker: Default::default() };
        info_req.send_and_schedule_retransmission(transaction_id, options_to_request, rng, now)
    }

    /// Calculates timeout for retransmitting information requests using parameters specified in
    /// [RFC 8415, Section 18.2.6].
    ///
    /// [RFC 8415, Section 18.2.6]: https://tools.ietf.org/html/rfc8415#section-18.2.6
    fn retransmission_timeout<R: Rng>(&self, rng: &mut R) -> Duration {
        let Self { retrans_timeout, _marker } = self;
        retransmission_timeout(
            *retrans_timeout,
            INITIAL_INFO_REQ_TIMEOUT,
            MAX_INFO_REQ_TIMEOUT,
            rng,
        )
    }

    /// A helper function that returns a transition to stay in `InformationRequesting`,
    /// with actions to send an information request and schedules retransmission.
    fn send_and_schedule_retransmission<R: Rng>(
        self,
        transaction_id: [u8; 3],
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        let options_array = [v6::DhcpOption::Oro(options_to_request)];
        let options = if options_to_request.is_empty() { &[][..] } else { &options_array[..] };

        let builder =
            v6::MessageBuilder::new(v6::MessageType::InformationRequest, transaction_id, options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);

        let retrans_timeout = self.retransmission_timeout(rng);

        Transition {
            state: ClientState::InformationRequesting(InformationRequesting {
                retrans_timeout,
                _marker: Default::default(),
            }),
            actions: vec![
                Action::SendMessage(buf),
                Action::ScheduleTimer(ClientTimerType::Retransmission, now.add(retrans_timeout)),
            ],
            transaction_id: None,
        }
    }

    /// Retransmits information request.
    fn retransmission_timer_expired<R: Rng>(
        self,
        transaction_id: [u8; 3],
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        self.send_and_schedule_retransmission(transaction_id, options_to_request, rng, now)
    }

    /// Handles reply to information requests based on [RFC 8415, Section 18.2.10.4].
    ///
    /// [RFC 8415, Section 18.2.10.4]: https://tools.ietf.org/html/rfc8415#section-18.2.10.4
    fn reply_message_received<B: ByteSlice>(
        self,
        msg: v6::Message<'_, B>,
        now: I,
    ) -> Transition<I> {
        // Note that although RFC 8415 states that SOL_MAX_RT must be handled,
        // we never send Solicit messages when running in stateless mode, so
        // there is no point in storing or doing anything with it.
        let ProcessedOptions { server_id, solicit_max_rt_opt: _, result } = match process_options(
            &msg,
            ExchangeType::ReplyToInformationRequest,
            None,
            &NoIaRequested,
            &NoIaRequested,
        ) {
            Ok(processed_options) => processed_options,
            Err(e) => {
                warn!("ignoring Reply to Information-Request: {}", e);
                return Transition {
                    state: ClientState::InformationRequesting(self),
                    actions: Vec::new(),
                    transaction_id: None,
                };
            }
        };

        let Options {
            success_status_message,
            next_contact_time,
            preference: _,
            non_temporary_addresses: _,
            delegated_prefixes: _,
            dns_servers,
        } = match result {
            Ok(options) => options,
            Err(e) => {
                warn!(
                    "Reply to Information-Request from server {:?} error status code: {}",
                    server_id, e
                );
                return Transition {
                    state: ClientState::InformationRequesting(self),
                    actions: Vec::new(),
                    transaction_id: None,
                };
            }
        };

        // Per RFC 8415 section 21.23:
        //
        //    If the Reply to an Information-request message does not contain this
        //    option, the client MUST behave as if the option with the value
        //    IRT_DEFAULT was provided.
        let information_refresh_time = assert_matches!(
            next_contact_time,
            NextContactTime::InformationRefreshTime(option) => option
        )
        .map(|t| Duration::from_secs(t.into()))
        .unwrap_or(IRT_DEFAULT);

        if let Some(success_status_message) = success_status_message {
            if !success_status_message.is_empty() {
                info!(
                    "Reply to Information-Request from server {:?} \
                    contains success status code message: {}",
                    server_id, success_status_message,
                );
            }
        }

        let actions = [
            Action::CancelTimer(ClientTimerType::Retransmission),
            Action::ScheduleTimer(ClientTimerType::Refresh, now.add(information_refresh_time)),
        ]
        .into_iter()
        .chain(dns_servers.clone().map(|server_addrs| Action::UpdateDnsServers(server_addrs)))
        .collect::<Vec<_>>();

        Transition {
            state: ClientState::InformationReceived(InformationReceived {
                dns_servers: dns_servers.unwrap_or(Vec::new()),
                _marker: Default::default(),
            }),
            actions,
            transaction_id: None,
        }
    }
}

/// Provides methods for handling state transitions from information received state.
#[derive(Debug)]
struct InformationReceived<I> {
    /// Stores the DNS servers received from the reply.
    dns_servers: Vec<Ipv6Addr>,
    _marker: PhantomData<I>,
}

impl<I: Instant> InformationReceived<I> {
    /// Refreshes information by starting another round of information request.
    fn refresh_timer_expired<R: Rng>(
        self,
        transaction_id: [u8; 3],
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        InformationRequesting::start(transaction_id, options_to_request, rng, now)
    }
}

enum IaKind {
    Address,
    Prefix,
}

trait IaValue: Copy + Clone + Debug + PartialEq + Eq + Hash {
    const KIND: IaKind;
}

impl IaValue for Ipv6Addr {
    const KIND: IaKind = IaKind::Address;
}

impl IaValue for Subnet<Ipv6Addr> {
    const KIND: IaKind = IaKind::Prefix;
}

// Holds the information received in an Advertise message.
#[derive(Debug, Clone)]
struct AdvertiseMessage<I> {
    server_id: Vec<u8>,
    /// The advertised non-temporary addresses.
    ///
    /// Each IA has at least one address.
    non_temporary_addresses: HashMap<v6::IAID, HashSet<Ipv6Addr>>,
    /// The advertised delegated prefixes.
    ///
    /// Each IA has at least one prefix.
    delegated_prefixes: HashMap<v6::IAID, HashSet<Subnet<Ipv6Addr>>>,
    dns_servers: Vec<Ipv6Addr>,
    preference: u8,
    receive_time: I,
    preferred_non_temporary_addresses_count: usize,
    preferred_delegated_prefixes_count: usize,
}

impl<I> AdvertiseMessage<I> {
    fn has_ias(&self) -> bool {
        let Self {
            server_id: _,
            non_temporary_addresses,
            delegated_prefixes,
            dns_servers: _,
            preference: _,
            receive_time: _,
            preferred_non_temporary_addresses_count: _,
            preferred_delegated_prefixes_count: _,
        } = self;
        // We know we are performing stateful DHCPv6 since we are performing
        // Server Discovery/Selection as stateless DHCPv6 does not use Advertise
        // messages.
        //
        // We consider an Advertisement acceptable if at least one requested IA
        // is available.
        !(non_temporary_addresses.is_empty() && delegated_prefixes.is_empty())
    }
}

// Orders Advertise by address count, then preference, dns servers count, and
// earliest receive time. This ordering gives precedence to higher address
// count over preference, to maximise the number of assigned addresses, as
// described in RFC 8415, section 18.2.9:
//
//    Those Advertise messages with the highest server preference value SHOULD
//    be preferred over all other Advertise messages. The client MAY choose a
//    less preferred server if that server has a better set of advertised
//    parameters, such as the available set of IAs.
impl<I: Instant> Ord for AdvertiseMessage<I> {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        #[derive(PartialEq, Eq, PartialOrd, Ord)]
        struct Candidate<I> {
            // First prefer the advertisement with at least one IA_NA.
            has_ia_na: bool,
            // Then prefer the advertisement with at least one IA_PD.
            has_ia_pd: bool,
            // Then prefer the advertisement with the most IA_NAs.
            ia_na_count: usize,
            // Then prefer the advertisement with the most IA_PDs.
            ia_pd_count: usize,
            // Then prefer the advertisement with the most addresses in IA_NAs
            // that match the provided hint(s).
            preferred_ia_na_address_count: usize,
            // Then prefer the advertisement with the most prefixes IA_PDs that
            // match the provided hint(s).
            preferred_ia_pd_prefix_count: usize,
            // Then prefer the advertisement with the highest preference value.
            server_preference: u8,
            // Then prefer the advertisement with the most number of DNS
            // servers.
            dns_server_count: usize,
            // Then prefer the advertisement received first.
            other_candidate_rcv_time: I,
        }

        impl<I: Instant> Candidate<I> {
            fn from_advertisements(
                candidate: &AdvertiseMessage<I>,
                other_candidate: &AdvertiseMessage<I>,
            ) -> Self {
                let AdvertiseMessage {
                    server_id: _,
                    non_temporary_addresses,
                    delegated_prefixes,
                    dns_servers,
                    preference,
                    receive_time: _,
                    preferred_non_temporary_addresses_count,
                    preferred_delegated_prefixes_count,
                } = candidate;
                let AdvertiseMessage {
                    server_id: _,
                    non_temporary_addresses: _,
                    delegated_prefixes: _,
                    dns_servers: _,
                    preference: _,
                    receive_time: other_receive_time,
                    preferred_non_temporary_addresses_count: _,
                    preferred_delegated_prefixes_count: _,
                } = other_candidate;

                Self {
                    has_ia_na: !non_temporary_addresses.is_empty(),
                    has_ia_pd: !delegated_prefixes.is_empty(),
                    ia_na_count: non_temporary_addresses.len(),
                    ia_pd_count: delegated_prefixes.len(),
                    preferred_ia_na_address_count: *preferred_non_temporary_addresses_count,
                    preferred_ia_pd_prefix_count: *preferred_delegated_prefixes_count,
                    server_preference: *preference,
                    dns_server_count: dns_servers.len(),
                    other_candidate_rcv_time: *other_receive_time,
                }
            }
        }

        Candidate::from_advertisements(self, other)
            .cmp(&Candidate::from_advertisements(other, self))
    }
}

impl<I: Instant> PartialOrd for AdvertiseMessage<I> {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl<I: Instant> PartialEq for AdvertiseMessage<I> {
    fn eq(&self, other: &Self) -> bool {
        self.cmp(other) == std::cmp::Ordering::Equal
    }
}

impl<I: Instant> Eq for AdvertiseMessage<I> {}

// Returns a count of entries in where the value matches the configured value
// with the same IAID.
fn compute_preferred_ia_count<V: IaValue>(
    got: &HashMap<v6::IAID, HashSet<V>>,
    configured: &HashMap<v6::IAID, HashSet<V>>,
) -> usize {
    got.iter()
        .map(|(iaid, got_values)| {
            configured
                .get(iaid)
                .map_or(0, |configured_values| got_values.intersection(configured_values).count())
        })
        .sum()
}

// Calculates the elapsed time since `start_time`, in centiseconds.
fn elapsed_time_in_centisecs<I: Instant>(start_time: I, now: I) -> u16 {
    u16::try_from(
        now.duration_since(start_time)
            .as_millis()
            .checked_div(ELAPSED_TIME_DENOMINATOR)
            .expect("division should succeed, denominator is non-zero"),
    )
    .unwrap_or(u16::MAX)
}

// Returns the common value in `values` if all the values are equal, or None
// otherwise.
fn get_common_value(values: &Vec<u32>) -> Option<Duration> {
    if !values.is_empty() && values.iter().all(|value| *value == values[0]) {
        return Some(Duration::from_secs(values[0].into()));
    }
    None
}

#[derive(thiserror::Error, Copy, Clone, Debug)]
#[cfg_attr(test, derive(PartialEq))]
enum LifetimesError {
    #[error("valid lifetime is zero")]
    ValidLifetimeZero,
    #[error("preferred lifetime greater than valid lifetime: {0:?}")]
    PreferredLifetimeGreaterThanValidLifetime(Lifetimes),
}

/// The valid and preferred lifetimes.
#[derive(Copy, Clone, Debug, PartialEq)]
pub struct Lifetimes {
    pub preferred_lifetime: v6::TimeValue,
    pub valid_lifetime: v6::NonZeroTimeValue,
}

#[derive(Debug)]
struct IaValueOption<V> {
    value: V,
    lifetimes: Result<Lifetimes, LifetimesError>,
}

#[derive(thiserror::Error, Debug)]
enum IaOptionError<V: IaValue> {
    #[error("T1={t1:?} greater than T2={t2:?}")]
    T1GreaterThanT2 { t1: v6::TimeValue, t2: v6::TimeValue },
    #[error("status code error: {0}")]
    StatusCode(#[from] StatusCodeError),
    // TODO(https://fxbug.dev/104297): Use an owned option type rather
    // than a string of the debug representation of the invalid option.
    #[error("invalid option: {0:?}")]
    InvalidOption(String),
    #[error("IA value={value:?} appeared twice with first={first_lifetimes:?} and second={second_lifetimes:?}")]
    DuplicateIaValue {
        value: V,
        first_lifetimes: Result<Lifetimes, LifetimesError>,
        second_lifetimes: Result<Lifetimes, LifetimesError>,
    },
}

#[derive(Debug)]
#[cfg_attr(test, derive(PartialEq))]
enum IaOption<V: IaValue> {
    Success {
        status_message: Option<String>,
        t1: v6::TimeValue,
        t2: v6::TimeValue,
        ia_values: HashMap<V, Result<Lifetimes, LifetimesError>>,
    },
    Failure(ErrorStatusCode),
}

type IaNaOption = IaOption<Ipv6Addr>;

#[derive(thiserror::Error, Debug)]
enum StatusCodeError {
    #[error("unknown status code {0}")]
    InvalidStatusCode(u16),
    #[error("duplicate Status Code option {0:?} and {1:?}")]
    DuplicateStatusCode((v6::StatusCode, String), (v6::StatusCode, String)),
}

fn check_lifetimes(
    valid_lifetime: v6::TimeValue,
    preferred_lifetime: v6::TimeValue,
) -> Result<Lifetimes, LifetimesError> {
    match valid_lifetime {
        v6::TimeValue::Zero => Err(LifetimesError::ValidLifetimeZero),
        vl @ v6::TimeValue::NonZero(valid_lifetime) => {
            // Ignore IA {Address,Prefix} options with invalid preferred or
            // valid lifetimes.
            //
            // Per RFC 8415 section 21.6,
            //
            //    The client MUST discard any addresses for which the preferred
            //    lifetime is greater than the valid lifetime.
            //
            // Per RFC 8415 section 21.22,
            //
            //    The client MUST discard any prefixes for which the preferred
            //    lifetime is greater than the valid lifetime.
            if preferred_lifetime > vl {
                Err(LifetimesError::PreferredLifetimeGreaterThanValidLifetime(Lifetimes {
                    preferred_lifetime,
                    valid_lifetime,
                }))
            } else {
                Ok(Lifetimes { preferred_lifetime, valid_lifetime })
            }
        }
    }
}

// TODO(https://fxbug.dev/104519): Move this function and associated types
// into packet-formats-dhcp.
fn process_ia<
    'a,
    V: IaValue,
    E: From<IaOptionError<V>> + Debug,
    F: Fn(&v6::ParsedDhcpOption<'a>) -> Result<IaValueOption<V>, E>,
>(
    t1: v6::TimeValue,
    t2: v6::TimeValue,
    options: impl Iterator<Item = v6::ParsedDhcpOption<'a>>,
    check: F,
) -> Result<IaOption<V>, E> {
    // Ignore IA_{NA,PD} options, with invalid T1/T2 values.
    //
    // Per RFC 8415, section 21.4:
    //
    //    If a client receives an IA_NA with T1 greater than T2 and both T1
    //    and T2 are greater than 0, the client discards the IA_NA option
    //    and processes the remainder of the message as though the server
    //    had not included the invalid IA_NA option.
    //
    // Per RFC 8415, section 21.21:
    //
    //    If a client receives an IA_PD with T1 greater than T2 and both T1 and
    //    T2 are greater than 0, the client discards the IA_PD option and
    //    processes the remainder of the message as though the server had not
    //    included the IA_PD option.
    match (t1, t2) {
        (v6::TimeValue::Zero, _) | (_, v6::TimeValue::Zero) => {}
        (t1, t2) => {
            if t1 > t2 {
                return Err(IaOptionError::T1GreaterThanT2 { t1, t2 }.into());
            }
        }
    }

    let mut ia_values = HashMap::new();
    let mut success_status_message = None;
    for opt in options {
        match opt {
            v6::ParsedDhcpOption::StatusCode(code, msg) => {
                let mut status_code = || {
                    let status_code = code.get().try_into().map_err(|e| match e {
                        v6::ParseError::InvalidStatusCode(code) => {
                            StatusCodeError::InvalidStatusCode(code)
                        }
                        e => unreachable!("unreachable status code parse error: {}", e),
                    })?;
                    if let Some(existing) = success_status_message.take() {
                        return Err(StatusCodeError::DuplicateStatusCode(
                            (v6::StatusCode::Success, existing),
                            (status_code, msg.to_string()),
                        ));
                    }

                    Ok(status_code)
                };
                let status_code = status_code().map_err(IaOptionError::StatusCode)?;
                match status_code.into_result() {
                    Ok(()) => {
                        success_status_message = Some(msg.to_string());
                    }
                    Err(error_status_code) => {
                        return Ok(IaOption::Failure(ErrorStatusCode(
                            error_status_code,
                            msg.to_string(),
                        )))
                    }
                }
            }
            opt @ (v6::ParsedDhcpOption::IaAddr(_) | v6::ParsedDhcpOption::IaPrefix(_)) => {
                let IaValueOption { value, lifetimes } = check(&opt)?;
                if let Some(first_lifetimes) = ia_values.insert(value, lifetimes) {
                    return Err(IaOptionError::DuplicateIaValue {
                        value,
                        first_lifetimes,
                        second_lifetimes: lifetimes,
                    }
                    .into());
                }
            }
            v6::ParsedDhcpOption::ClientId(_)
            | v6::ParsedDhcpOption::ServerId(_)
            | v6::ParsedDhcpOption::SolMaxRt(_)
            | v6::ParsedDhcpOption::Preference(_)
            | v6::ParsedDhcpOption::Iana(_)
            | v6::ParsedDhcpOption::InformationRefreshTime(_)
            | v6::ParsedDhcpOption::IaPd(_)
            | v6::ParsedDhcpOption::Oro(_)
            | v6::ParsedDhcpOption::ElapsedTime(_)
            | v6::ParsedDhcpOption::DnsServers(_)
            | v6::ParsedDhcpOption::DomainList(_) => {
                return Err(IaOptionError::InvalidOption(format!("{:?}", opt)).into());
            }
        }
    }

    // Missing status code option means success per RFC 8415 section 7.5:
    //
    //    If the Status Code option (see Section 21.13) does not appear
    //    in a message in which the option could appear, the status
    //    of the message is assumed to be Success.
    Ok(IaOption::Success { status_message: success_status_message, t1, t2, ia_values })
}

// TODO(https://fxbug.dev/104519): Move this function and associated types
// into packet-formats-dhcp.
fn process_ia_na(
    ia_na_data: &v6::IanaData<&'_ [u8]>,
) -> Result<IaNaOption, IaOptionError<Ipv6Addr>> {
    process_ia(ia_na_data.t1(), ia_na_data.t2(), ia_na_data.iter_options(), |opt| match opt {
        v6::ParsedDhcpOption::IaAddr(ia_addr_data) => Ok(IaValueOption {
            value: ia_addr_data.addr(),
            lifetimes: check_lifetimes(
                ia_addr_data.valid_lifetime(),
                ia_addr_data.preferred_lifetime(),
            ),
        }),
        opt @ v6::ParsedDhcpOption::IaPrefix(_) => {
            Err(IaOptionError::InvalidOption(format!("{:?}", opt)))
        }
        opt => unreachable!(
            "other options should be handled before this fn is called; got = {:?}",
            opt
        ),
    })
}

#[derive(thiserror::Error, Debug)]
enum IaPdOptionError {
    #[error("generic IA Option error: {0}")]
    IaOptionError(#[from] IaOptionError<Subnet<Ipv6Addr>>),
    #[error("invalid subnet")]
    InvalidSubnet,
}

type IaPdOption = IaOption<Subnet<Ipv6Addr>>;

// TODO(https://fxbug.dev/104519): Move this function and associated types
// into packet-formats-dhcp.
fn process_ia_pd(ia_pd_data: &v6::IaPdData<&'_ [u8]>) -> Result<IaPdOption, IaPdOptionError> {
    process_ia(ia_pd_data.t1(), ia_pd_data.t2(), ia_pd_data.iter_options(), |opt| match opt {
        v6::ParsedDhcpOption::IaPrefix(ia_prefix_data) => ia_prefix_data
            .prefix()
            .map_err(|_| IaPdOptionError::InvalidSubnet)
            .map(|prefix| IaValueOption {
                value: prefix,
                lifetimes: check_lifetimes(
                    ia_prefix_data.valid_lifetime(),
                    ia_prefix_data.preferred_lifetime(),
                ),
            }),
        opt @ v6::ParsedDhcpOption::IaAddr(_) => {
            Err(IaOptionError::InvalidOption(format!("{:?}", opt)).into())
        }
        opt => unreachable!(
            "other options should be handled before this fn is called; got = {:?}",
            opt
        ),
    })
}

#[derive(Debug)]
enum NextContactTime {
    InformationRefreshTime(Option<u32>),
    RenewRebind { t1: v6::NonZeroTimeValue, t2: v6::NonZeroTimeValue },
}

#[derive(Debug)]
struct Options {
    success_status_message: Option<String>,
    next_contact_time: NextContactTime,
    preference: Option<u8>,
    non_temporary_addresses: HashMap<v6::IAID, IaNaOption>,
    delegated_prefixes: HashMap<v6::IAID, IaPdOption>,
    dns_servers: Option<Vec<Ipv6Addr>>,
}

#[derive(Debug)]
struct ProcessedOptions {
    server_id: Vec<u8>,
    solicit_max_rt_opt: Option<u32>,
    result: Result<Options, ErrorStatusCode>,
}

#[derive(thiserror::Error, Debug)]
#[cfg_attr(test, derive(PartialEq))]
#[error("error status code={0}, message='{1}'")]
struct ErrorStatusCode(v6::ErrorStatusCode, String);

#[derive(thiserror::Error, Debug)]
enum OptionsError {
    // TODO(https://fxbug.dev/104297): Use an owned option type rather
    // than a string of the debug representation of the invalid option.
    #[error("duplicate option with code {0:?} {1} and {2}")]
    DuplicateOption(v6::OptionCode, String, String),
    #[error("unknown status code {0} with message '{1}'")]
    InvalidStatusCode(u16, String),
    #[error("IA_NA option error")]
    IaNaError(#[from] IaOptionError<Ipv6Addr>),
    #[error("IA_PD option error")]
    IaPdError(#[from] IaPdOptionError),
    #[error("duplicate IA_NA option with IAID={0:?} {1:?} and {2:?}")]
    DuplicateIaNaId(v6::IAID, IaNaOption, IaNaOption),
    #[error("duplicate IA_PD option with IAID={0:?} {1:?} and {2:?}")]
    DuplicateIaPdId(v6::IAID, IaPdOption, IaPdOption),
    #[error("IA_NA with unexpected IAID")]
    UnexpectedIaNa(v6::IAID, IaNaOption),
    #[error("IA_PD with unexpected IAID")]
    UnexpectedIaPd(v6::IAID, IaPdOption),
    #[error("missing Server Id option")]
    MissingServerId,
    #[error("missing Client Id option")]
    MissingClientId,
    #[error("got Client ID option {got:?} but want {want:?}")]
    MismatchedClientId { got: Vec<u8>, want: [u8; CLIENT_ID_LEN] },
    #[error("unexpected Client ID in Reply to anonymous Information-Request: {0:?}")]
    UnexpectedClientId(Vec<u8>),
    // TODO(https://fxbug.dev/104297): Use an owned option type rather
    // than a string of the debug representation of the invalid option.
    #[error("invalid option found: {0:?}")]
    InvalidOption(String),
}

/// Message types sent by the client for which a Reply from the server
/// contains IA options with assigned leases.
#[derive(Debug, Copy, Clone, PartialEq, Eq, Hash)]
enum RequestLeasesMessageType {
    Request,
    Renew,
    Rebind,
}

impl std::fmt::Display for RequestLeasesMessageType {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Request => write!(f, "Request"),
            Self::Renew => write!(f, "Renew"),
            Self::Rebind => write!(f, "Rebind"),
        }
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq, Hash)]
enum ExchangeType {
    ReplyToInformationRequest,
    AdvertiseToSolicit,
    ReplyWithLeases(RequestLeasesMessageType),
}

trait IaChecker {
    /// Returns true ifff the IA was requested
    fn was_ia_requested(&self, id: &v6::IAID) -> bool;
}

struct NoIaRequested;

impl IaChecker for NoIaRequested {
    fn was_ia_requested(&self, _id: &v6::IAID) -> bool {
        false
    }
}

impl<V> IaChecker for HashMap<v6::IAID, V> {
    fn was_ia_requested(&self, id: &v6::IAID) -> bool {
        self.get(id).is_some()
    }
}

// TODO(https://fxbug.dev/104025): Make the choice between ignoring invalid
// options and discarding the entire message configurable.
// TODO(https://fxbug.dev/104519): Move this function and associated types
// into packet-formats-dhcp.
/// Process options.
///
/// If any singleton options appears more than once, or there are multiple
/// IA options of the same type with duplicate ID's, the entire message will
/// be ignored as if it was never received.
///
/// Per RFC 8415, section 16:
///
///    This section describes which options are valid in which kinds of
///    message types and explains what to do when a client or server
///    receives a message that contains known options that are invalid for
///    that message. [...]
///
///    Clients and servers MAY choose to either (1) extract information from
///    such a message if the information is of use to the recipient or
///    (2) ignore such a message completely and just discard it.
///
/// The choice made by this function is (2): an error will be returned in such
/// cases to inform callers that they should ignore the entire message.
fn process_options<B: ByteSlice, IaNaChecker: IaChecker, IaPdChecker: IaChecker>(
    msg: &v6::Message<'_, B>,
    exchange_type: ExchangeType,
    want_client_id: Option<[u8; CLIENT_ID_LEN]>,
    iana_checker: &IaNaChecker,
    iapd_checker: &IaPdChecker,
) -> Result<ProcessedOptions, OptionsError> {
    let mut solicit_max_rt_option = None;
    let mut server_id_option = None;
    let mut client_id_option = None;
    let mut preference = None;
    let mut non_temporary_addresses = HashMap::new();
    let mut delegated_prefixes = HashMap::new();
    let mut status_code_option = None;
    let mut dns_servers = None;
    let mut refresh_time_option = None;
    let mut min_t1 = v6::TimeValue::Zero;
    let mut min_t2 = v6::TimeValue::Zero;
    let mut min_preferred_lifetime = v6::TimeValue::Zero;
    // Ok to initialize with Infinity, `get_nonzero_min` will pick a
    // smaller value once we see an IA with a valid lifetime less than
    // Infinity.
    let mut min_valid_lifetime = v6::NonZeroTimeValue::Infinity;

    // Updates the minimum preferred/valid and T1/T2 (life)times in response
    // to an IA option.
    let mut update_min_preferred_valid_lifetimes = |preferred_lifetime, valid_lifetime| {
        min_preferred_lifetime = maybe_get_nonzero_min(min_preferred_lifetime, preferred_lifetime);
        min_valid_lifetime = std::cmp::min(min_valid_lifetime, valid_lifetime);
    };

    let mut update_min_t1_t2 = |t1, t2| {
        // If T1/T2 are set by the server to values greater than 0,
        // compute the minimum T1 and T2 values, per RFC 8415,
        // section 18.2.4:
        //
        //    [..] the client SHOULD renew/rebind all IAs from the
        //    server at the same time, the client MUST select T1 and
        //    T2 times from all IA options that will guarantee that
        //    the client initiates transmissions of Renew/Rebind
        //    messages not later than at the T1/T2 times associated
        //    with any of the client's bindings (earliest T1/T2).
        //
        // Only IAs that with success status are included in the earliest
        // T1/T2 calculation.
        min_t1 = maybe_get_nonzero_min(min_t1, t1);
        min_t2 = maybe_get_nonzero_min(min_t2, t2);
    };

    struct AllowedOptions {
        preference: bool,
        information_refresh_time: bool,
        identity_association: bool,
    }
    // See RFC 8415 appendix B for a summary of which options are allowed in
    // which message types.
    let AllowedOptions {
        preference: preference_allowed,
        information_refresh_time: information_refresh_time_allowed,
        identity_association: identity_association_allowed,
    } = match exchange_type {
        ExchangeType::ReplyToInformationRequest => AllowedOptions {
            preference: false,
            information_refresh_time: true,
            // Per RFC 8415, section 16.12:
            //
            //    Servers MUST discard any received Information-request message that
            //    meets any of the following conditions:
            //
            //    -  the message includes an IA option.
            //
            // Since it's invalid to include IA options in an Information-request message,
            // it is also invalid to receive IA options in a Reply in response to an
            // Information-request message.
            identity_association: false,
        },
        ExchangeType::AdvertiseToSolicit => AllowedOptions {
            preference: true,
            information_refresh_time: false,
            identity_association: true,
        },
        ExchangeType::ReplyWithLeases(
            RequestLeasesMessageType::Request
            | RequestLeasesMessageType::Renew
            | RequestLeasesMessageType::Rebind,
        ) => {
            AllowedOptions {
                preference: false,
                // Per RFC 8415, section 21.23
                //
                //    Information Refresh Time Option
                //
                //    [...] It is only used in Reply messages in response
                //    to Information-request messages.
                information_refresh_time: false,
                identity_association: true,
            }
        }
    };

    for opt in msg.options() {
        match opt {
            v6::ParsedDhcpOption::ClientId(client_id) => {
                if let Some(existing) = client_id_option {
                    return Err(OptionsError::DuplicateOption(
                        v6::OptionCode::ClientId,
                        format!("{:?}", existing),
                        format!("{:?}", client_id.to_vec()),
                    ));
                }
                client_id_option = Some(client_id.to_vec());
            }
            v6::ParsedDhcpOption::ServerId(server_id_opt) => {
                if let Some(existing) = server_id_option {
                    return Err(OptionsError::DuplicateOption(
                        v6::OptionCode::ServerId,
                        format!("{:?}", existing),
                        format!("{:?}", server_id_opt.to_vec()),
                    ));
                }
                server_id_option = Some(server_id_opt.to_vec());
            }
            v6::ParsedDhcpOption::SolMaxRt(sol_max_rt_opt) => {
                if let Some(existing) = solicit_max_rt_option {
                    return Err(OptionsError::DuplicateOption(
                        v6::OptionCode::SolMaxRt,
                        format!("{:?}", existing),
                        format!("{:?}", sol_max_rt_opt.get()),
                    ));
                }
                // Per RFC 8415, section 21.24:
                //
                //    SOL_MAX_RT value MUST be in this range: 60 <= "value" <= 86400
                //
                //    A DHCP client MUST ignore any SOL_MAX_RT option values that are
                //    less than 60 or more than 86400.
                if !VALID_MAX_SOLICIT_TIMEOUT_RANGE.contains(&sol_max_rt_opt.get()) {
                    warn!(
                        "{:?}: ignoring SOL_MAX_RT value {} outside of range {:?}",
                        exchange_type,
                        sol_max_rt_opt.get(),
                        VALID_MAX_SOLICIT_TIMEOUT_RANGE,
                    );
                } else {
                    // TODO(https://fxbug.dev/103407): Use a bounded type to
                    // store SOL_MAX_RT.
                    solicit_max_rt_option = Some(sol_max_rt_opt.get());
                }
            }
            v6::ParsedDhcpOption::Preference(preference_opt) => {
                if !preference_allowed {
                    return Err(OptionsError::InvalidOption(format!("{:?}", opt)));
                }
                if let Some(existing) = preference {
                    return Err(OptionsError::DuplicateOption(
                        v6::OptionCode::Preference,
                        format!("{:?}", existing),
                        format!("{:?}", preference_opt),
                    ));
                }
                preference = Some(preference_opt);
            }
            v6::ParsedDhcpOption::Iana(ref iana_data) => {
                if !identity_association_allowed {
                    return Err(OptionsError::InvalidOption(format!("{:?}", opt)));
                }
                let iaid = v6::IAID::new(iana_data.iaid());
                let processed_ia_na = match process_ia_na(iana_data) {
                    Ok(o) => o,
                    Err(IaOptionError::T1GreaterThanT2 { t1: _, t2: _ }) => {
                        // As per RFC 8415 section 21.4,
                        //
                        //   If a client receives an IA_NA with T1 greater than
                        //   T2 and both T1 and T2 are greater than 0, the
                        //   client discards the IA_NA option and processes the
                        //   remainder of the message as though the server had
                        //   not included the invalid IA_NA option.
                        continue;
                    }
                    Err(
                        e @ IaOptionError::StatusCode(_)
                        | e @ IaOptionError::InvalidOption(_)
                        | e @ IaOptionError::DuplicateIaValue {
                            value: _,
                            first_lifetimes: _,
                            second_lifetimes: _,
                        },
                    ) => {
                        return Err(OptionsError::IaNaError(e));
                    }
                };
                if !iana_checker.was_ia_requested(&iaid) {
                    // The RFC does not explicitly call out what to do with
                    // IAs that were not requested by the client.
                    //
                    // Return an error to cause the entire message to be
                    // ignored.
                    return Err(OptionsError::UnexpectedIaNa(iaid, processed_ia_na));
                }
                match processed_ia_na {
                    IaNaOption::Failure(_) => {}
                    IaNaOption::Success { status_message: _, t1, t2, ref ia_values } => {
                        let mut update_t1_t2 = false;
                        for (_value, lifetimes) in ia_values {
                            match lifetimes {
                                Err(_) => {}
                                Ok(Lifetimes { preferred_lifetime, valid_lifetime }) => {
                                    update_min_preferred_valid_lifetimes(
                                        *preferred_lifetime,
                                        *valid_lifetime,
                                    );
                                    update_t1_t2 = true;
                                }
                            }
                        }
                        if update_t1_t2 {
                            update_min_t1_t2(t1, t2);
                        }
                    }
                }

                // Per RFC 8415, section 21.4, IAIDs are expected to be
                // unique.
                //
                //    A DHCP message may contain multiple IA_NA options
                //    (though each must have a unique IAID).
                match non_temporary_addresses.entry(iaid) {
                    Entry::Occupied(entry) => {
                        return Err(OptionsError::DuplicateIaNaId(
                            iaid,
                            entry.remove(),
                            processed_ia_na,
                        ));
                    }
                    Entry::Vacant(entry) => {
                        let _: &mut IaNaOption = entry.insert(processed_ia_na);
                    }
                };
            }
            v6::ParsedDhcpOption::StatusCode(code, message) => {
                let status_code = match v6::StatusCode::try_from(code.get()) {
                    Ok(status_code) => status_code,
                    Err(v6::ParseError::InvalidStatusCode(invalid)) => {
                        return Err(OptionsError::InvalidStatusCode(invalid, message.to_string()));
                    }
                    Err(e) => {
                        unreachable!("unreachable status code parse error: {}", e);
                    }
                };
                if let Some(existing) = status_code_option {
                    return Err(OptionsError::DuplicateOption(
                        v6::OptionCode::StatusCode,
                        format!("{:?}", existing),
                        format!("{:?}", (status_code, message.to_string())),
                    ));
                }
                status_code_option = Some((status_code, message.to_string()));
            }
            v6::ParsedDhcpOption::IaPd(ref iapd_data) => {
                if !identity_association_allowed {
                    return Err(OptionsError::InvalidOption(format!("{:?}", opt)));
                }
                let iaid = v6::IAID::new(iapd_data.iaid());
                let processed_ia_pd = match process_ia_pd(iapd_data) {
                    Ok(o) => o,
                    Err(IaPdOptionError::IaOptionError(IaOptionError::T1GreaterThanT2 {
                        t1: _,
                        t2: _,
                    })) => {
                        // As per RFC 8415 section 21.4,
                        //
                        //   If a client receives an IA_NA with T1 greater than
                        //   T2 and both T1 and T2 are greater than 0, the
                        //   client discards the IA_NA option and processes the
                        //   remainder of the message as though the server had
                        //   not included the invalid IA_NA option.
                        continue;
                    }
                    Err(
                        e @ IaPdOptionError::IaOptionError(IaOptionError::StatusCode(_))
                        | e @ IaPdOptionError::IaOptionError(IaOptionError::InvalidOption(_))
                        | e @ IaPdOptionError::IaOptionError(IaOptionError::DuplicateIaValue {
                            value: _,
                            first_lifetimes: _,
                            second_lifetimes: _,
                        })
                        | e @ IaPdOptionError::InvalidSubnet,
                    ) => {
                        return Err(OptionsError::IaPdError(e));
                    }
                };
                if !iapd_checker.was_ia_requested(&iaid) {
                    // The RFC does not explicitly call out what to do with
                    // IAs that were not requested by the client.
                    //
                    // Return an error to cause the entire message to be
                    // ignored.
                    return Err(OptionsError::UnexpectedIaPd(iaid, processed_ia_pd));
                }
                match processed_ia_pd {
                    IaPdOption::Failure(_) => {}
                    IaPdOption::Success { status_message: _, t1, t2, ref ia_values } => {
                        let mut update_t1_t2 = false;
                        for (_value, lifetimes) in ia_values {
                            match lifetimes {
                                Err(_) => {}
                                Ok(Lifetimes { preferred_lifetime, valid_lifetime }) => {
                                    update_min_preferred_valid_lifetimes(
                                        *preferred_lifetime,
                                        *valid_lifetime,
                                    );
                                    update_t1_t2 = true;
                                }
                            }
                        }
                        if update_t1_t2 {
                            update_min_t1_t2(t1, t2);
                        }
                    }
                }
                // Per RFC 8415, section 21.21, IAIDs are expected to be unique.
                //
                //   A DHCP message may contain multiple IA_PD options (though
                //   each must have a unique IAID).
                match delegated_prefixes.entry(iaid) {
                    Entry::Occupied(entry) => {
                        return Err(OptionsError::DuplicateIaPdId(
                            iaid,
                            entry.remove(),
                            processed_ia_pd,
                        ));
                    }
                    Entry::Vacant(entry) => {
                        let _: &mut IaPdOption = entry.insert(processed_ia_pd);
                    }
                };
            }
            v6::ParsedDhcpOption::InformationRefreshTime(information_refresh_time) => {
                if !information_refresh_time_allowed {
                    return Err(OptionsError::InvalidOption(format!("{:?}", opt)));
                }
                if let Some(existing) = refresh_time_option {
                    return Err(OptionsError::DuplicateOption(
                        v6::OptionCode::InformationRefreshTime,
                        format!("{:?}", existing),
                        format!("{:?}", information_refresh_time),
                    ));
                }
                refresh_time_option = Some(information_refresh_time);
            }
            v6::ParsedDhcpOption::IaAddr(_)
            | v6::ParsedDhcpOption::IaPrefix(_)
            | v6::ParsedDhcpOption::Oro(_)
            | v6::ParsedDhcpOption::ElapsedTime(_) => {
                return Err(OptionsError::InvalidOption(format!("{:?}", opt)));
            }
            v6::ParsedDhcpOption::DnsServers(server_addrs) => {
                if let Some(existing) = dns_servers {
                    return Err(OptionsError::DuplicateOption(
                        v6::OptionCode::DnsServers,
                        format!("{:?}", existing),
                        format!("{:?}", server_addrs),
                    ));
                }
                dns_servers = Some(server_addrs);
            }
            v6::ParsedDhcpOption::DomainList(_domains) => {
                // TODO(https://fxbug.dev/87176) implement domain list.
            }
        }
    }
    // For all three message types the server sends to the client (Advertise, Reply,
    // and Reconfigue), RFC 8415 sections 16.3, 16.10, and 16.11 respectively state
    // that:
    //
    //    Clients MUST discard any received ... message that meets
    //    any of the following conditions:
    //    -  the message does not include a Server Identifier option (see
    //       Section 21.3).
    let server_id = server_id_option.ok_or(OptionsError::MissingServerId)?;
    // For all three message types the server sends to the client (Advertise, Reply,
    // and Reconfigue), RFC 8415 sections 16.3, 16.10, and 16.11 respectively state
    // that:
    //
    //    Clients MUST discard any received ... message that meets
    //    any of the following conditions:
    //    -  the message does not include a Client Identifier option (see
    //       Section 21.2).
    //    -  the contents of the Client Identifier option do not match the
    //       client's DUID.
    //
    // The exception is that clients may send Information-Request messages
    // without a client ID per RFC 8415 section 18.2.6:
    //
    //    The client SHOULD include a Client Identifier option (see
    //    Section 21.2) to identify itself to the server (however, see
    //    Section 4.3.1 of [RFC7844] for reasons why a client may not want to
    //    include this option).
    match (client_id_option, want_client_id) {
        (None, None) => {}
        (Some(got), None) => return Err(OptionsError::UnexpectedClientId(got)),
        (None, Some::<[u8; CLIENT_ID_LEN]>(_)) => return Err(OptionsError::MissingClientId),
        (Some(got), Some(want)) => {
            if got != want {
                return Err(OptionsError::MismatchedClientId { want, got });
            }
        }
    }
    let success_status_message = match status_code_option {
        Some((status_code, message)) => match status_code.into_result() {
            Ok(()) => Some(message),
            Err(error_code) => {
                return Ok(ProcessedOptions {
                    server_id,
                    solicit_max_rt_opt: solicit_max_rt_option,
                    result: Err(ErrorStatusCode(error_code, message)),
                });
            }
        },
        // Missing status code option means success per RFC 8415 section 7.5:
        //
        //    If the Status Code option (see Section 21.13) does not appear
        //    in a message in which the option could appear, the status
        //    of the message is assumed to be Success.
        None => None,
    };
    let next_contact_time = match exchange_type {
        ExchangeType::ReplyToInformationRequest => {
            NextContactTime::InformationRefreshTime(refresh_time_option)
        }
        ExchangeType::AdvertiseToSolicit
        | ExchangeType::ReplyWithLeases(
            RequestLeasesMessageType::Request
            | RequestLeasesMessageType::Renew
            | RequestLeasesMessageType::Rebind,
        ) => {
            // If not set or 0, choose a value for T1 and T2, per RFC 8415, section
            // 18.2.4:
            //
            //    If T1 or T2 had been set to 0 by the server (for an
            //    IA_NA or IA_PD) or there are no T1 or T2 times (for an
            //    IA_TA) in a previous Reply, the client may, at its
            //    discretion, send a Renew or Rebind message,
            //    respectively.  The client MUST follow the rules
            //    defined in Section 14.2.
            //
            // Per RFC 8415, section 14.2:
            //
            //    When T1 and/or T2 values are set to 0, the client MUST choose a
            //    time to avoid packet storms.  In particular, it MUST NOT transmit
            //    immediately.
            //
            // When left to the client's discretion, the client chooses T1/T1 values
            // following the recommentations in RFC 8415, section 21.4:
            //
            //    Recommended values for T1 and T2 are 0.5 and 0.8 times the
            //    shortest preferred lifetime of the addresses in the IA that the
            //    server is willing to extend, respectively.  If the "shortest"
            //    preferred lifetime is 0xffffffff ("infinity"), the recommended T1
            //    and T2 values are also 0xffffffff.
            //
            // The RFC does not specify how to compute T1 if the shortest preferred
            // lifetime is zero and T1 is zero. In this case, T1 is calculated as a
            // fraction of the shortest valid lifetime.
            let t1 = match min_t1 {
                v6::TimeValue::Zero => {
                    let min = match min_preferred_lifetime {
                        v6::TimeValue::Zero => min_valid_lifetime,
                        v6::TimeValue::NonZero(t) => t,
                    };
                    compute_t(min, T1_MIN_LIFETIME_RATIO)
                }
                v6::TimeValue::NonZero(t) => t,
            };
            // T2 must be >= T1, compute its value based on T1.
            let t2 = match min_t2 {
                v6::TimeValue::Zero => compute_t(t1, T2_T1_RATIO),
                v6::TimeValue::NonZero(t2_val) => {
                    if t2_val < t1 {
                        compute_t(t1, T2_T1_RATIO)
                    } else {
                        t2_val
                    }
                }
            };

            NextContactTime::RenewRebind { t1, t2 }
        }
    };
    Ok(ProcessedOptions {
        server_id,
        solicit_max_rt_opt: solicit_max_rt_option,
        result: Ok(Options {
            success_status_message,
            next_contact_time,
            preference,
            non_temporary_addresses,
            delegated_prefixes,
            dns_servers,
        }),
    })
}

struct StatefulMessageBuilder<'a, AddrIter, PrefixIter, IaNaIter, IaPdIter> {
    transaction_id: [u8; 3],
    message_type: v6::MessageType,
    client_id: &'a [u8],
    server_id: Option<&'a [u8]>,
    elapsed_time_in_centisecs: u16,
    options_to_request: &'a [v6::OptionCode],
    ia_nas: IaNaIter,
    ia_pds: IaPdIter,
    _marker: std::marker::PhantomData<(AddrIter, PrefixIter)>,
}

impl<
        'a,
        AddrIter: Iterator<Item = Ipv6Addr>,
        PrefixIter: Iterator<Item = Subnet<Ipv6Addr>>,
        IaNaIter: Iterator<Item = (v6::IAID, AddrIter)>,
        IaPdIter: Iterator<Item = (v6::IAID, PrefixIter)>,
    > StatefulMessageBuilder<'a, AddrIter, PrefixIter, IaNaIter, IaPdIter>
{
    fn build(self) -> Vec<u8> {
        let StatefulMessageBuilder {
            transaction_id,
            message_type,
            client_id,
            server_id,
            elapsed_time_in_centisecs,
            options_to_request,
            ia_nas,
            ia_pds,
            _marker,
        } = self;

        debug_assert!(!options_to_request.contains(&v6::OptionCode::SolMaxRt));
        let oro = [v6::OptionCode::SolMaxRt]
            .into_iter()
            .chain(options_to_request.into_iter().cloned())
            .collect::<Vec<_>>();

        // Adds IA_{NA,PD} options: one IA_{NA,PD} per hint, plus options
        // without hints, up to the configured count, as described in
        // RFC 8415, section 6.6:
        //
        //   A client can explicitly request multiple addresses by sending
        //   multiple IA_NA options (and/or IA_TA options; see Section 21.5).  A
        //   client can send multiple IA_NA (and/or IA_TA) options in its initial
        //   transmissions. Alternatively, it can send an extra Request message
        //   with additional new IA_NA (and/or IA_TA) options (or include them in
        //   a Renew message).
        //
        //   The same principle also applies to prefix delegation. In principle,
        //   DHCP allows a client to request new prefixes to be delegated by
        //   sending additional IA_PD options (see Section 21.21). However, a
        //   typical operator usually prefers to delegate a single, larger prefix.
        //   In most deployments, it is recommended that the client request a
        //   larger prefix in its initial transmissions rather than request
        //   additional prefixes later on.
        let iaaddr_options = ia_nas
            .map(|(iaid, inner)| {
                (
                    iaid,
                    inner
                        .map(|addr| {
                            v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(addr, 0, 0, &[]))
                        })
                        .collect::<Vec<_>>(),
                )
            })
            .collect::<HashMap<_, _>>();
        let iaprefix_options = ia_pds
            .map(|(iaid, inner)| {
                (
                    iaid,
                    inner
                        .map(|prefix| {
                            v6::DhcpOption::IaPrefix(v6::IaPrefixSerializer::new(0, 0, prefix, &[]))
                        })
                        .collect::<Vec<_>>(),
                )
            })
            .collect::<HashMap<_, _>>();

        let options = server_id
            .into_iter()
            .map(v6::DhcpOption::ServerId)
            .chain([
                v6::DhcpOption::ClientId(client_id),
                v6::DhcpOption::ElapsedTime(elapsed_time_in_centisecs),
                v6::DhcpOption::Oro(&oro),
            ])
            .chain(iaaddr_options.iter().map(|(iaid, iaddr_opt)| {
                v6::DhcpOption::Iana(v6::IanaSerializer::new(*iaid, 0, 0, iaddr_opt.as_slice()))
            }))
            .chain(iaprefix_options.iter().map(|(iaid, iaprefix_opt)| {
                v6::DhcpOption::IaPd(v6::IaPdSerializer::new(*iaid, 0, 0, iaprefix_opt.as_slice()))
            }))
            .collect::<Vec<_>>();

        let builder = v6::MessageBuilder::new(message_type, transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        buf
    }
}

/// Provides methods for handling state transitions from server discovery
/// state.
#[derive(Debug)]
struct ServerDiscovery<I> {
    /// [Client Identifier] used for uniquely identifying the client in
    /// communication with servers.
    ///
    /// [Client Identifier]: https://datatracker.ietf.org/doc/html/rfc8415#section-21.2
    client_id: [u8; CLIENT_ID_LEN],
    /// The non-temporary addresses the client is configured to negotiate.
    configured_non_temporary_addresses: HashMap<v6::IAID, HashSet<Ipv6Addr>>,
    /// The delegated prefixes the client is configured to negotiate.
    configured_delegated_prefixes: HashMap<v6::IAID, HashSet<Subnet<Ipv6Addr>>>,
    /// The time of the first solicit. Used in calculating the [elapsed time].
    ///
    /// [elapsed time]:https://datatracker.ietf.org/doc/html/rfc8415#section-21.9
    first_solicit_time: I,
    /// The solicit retransmission timeout.
    retrans_timeout: Duration,
    /// The [SOL_MAX_RT] used by the client.
    ///
    /// [SOL_MAX_RT]: https://datatracker.ietf.org/doc/html/rfc8415#section-21.24
    solicit_max_rt: Duration,
    /// The advertise collected from servers during [server discovery], with
    /// the best advertise at the top of the heap.
    ///
    /// [server discovery]: https://datatracker.ietf.org/doc/html/rfc8415#section-18
    collected_advertise: BinaryHeap<AdvertiseMessage<I>>,
    /// The valid SOL_MAX_RT options received from servers.
    collected_sol_max_rt: Vec<u32>,
}

impl<I: Instant> ServerDiscovery<I> {
    /// Starts server discovery by sending a solicit message, as described in
    /// [RFC 8415, Section 18.2.1].
    ///
    /// [RFC 8415, Section 18.2.1]: https://datatracker.ietf.org/doc/html/rfc8415#section-18.2.1
    fn start<R: Rng>(
        transaction_id: [u8; 3],
        client_id: [u8; CLIENT_ID_LEN],
        configured_non_temporary_addresses: HashMap<v6::IAID, HashSet<Ipv6Addr>>,
        configured_delegated_prefixes: HashMap<v6::IAID, HashSet<Subnet<Ipv6Addr>>>,
        options_to_request: &[v6::OptionCode],
        solicit_max_rt: Duration,
        rng: &mut R,
        now: I,
        initial_actions: impl Iterator<Item = Action<I>>,
    ) -> Transition<I> {
        Self {
            client_id,
            configured_non_temporary_addresses,
            configured_delegated_prefixes,
            first_solicit_time: now,
            retrans_timeout: Duration::default(),
            solicit_max_rt,
            collected_advertise: BinaryHeap::new(),
            collected_sol_max_rt: Vec::new(),
        }
        .send_and_schedule_retransmission(
            transaction_id,
            options_to_request,
            rng,
            now,
            initial_actions,
        )
    }

    /// Calculates timeout for retransmitting solicits using parameters
    /// specified in [RFC 8415, Section 18.2.1].
    ///
    /// [RFC 8415, Section 18.2.1]: https://datatracker.ietf.org/doc/html/rfc8415#section-18.2.1
    fn retransmission_timeout<R: Rng>(
        prev_retrans_timeout: Duration,
        max_retrans_timeout: Duration,
        rng: &mut R,
    ) -> Duration {
        retransmission_timeout(
            prev_retrans_timeout,
            INITIAL_SOLICIT_TIMEOUT,
            max_retrans_timeout,
            rng,
        )
    }

    /// Returns a transition to stay in `ServerDiscovery`, with actions to send a
    /// solicit and schedule retransmission.
    fn send_and_schedule_retransmission<R: Rng>(
        self,
        transaction_id: [u8; 3],
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
        initial_actions: impl Iterator<Item = Action<I>>,
    ) -> Transition<I> {
        let Self {
            client_id,
            configured_non_temporary_addresses,
            configured_delegated_prefixes,
            first_solicit_time,
            retrans_timeout,
            solicit_max_rt,
            collected_advertise,
            collected_sol_max_rt,
        } = self;

        let elapsed_time = elapsed_time_in_centisecs(first_solicit_time, now);

        // Per RFC 8415, section 18.2.1:
        //
        //   The client sets the "msg-type" field to SOLICIT. The client
        //   generates a transaction ID and inserts this value in the
        //   "transaction-id" field.
        //
        //   The client MUST include a Client Identifier option (see Section
        //   21.2) to identify itself to the server. The client includes IA
        //   options for any IAs to which it wants the server to assign leases.
        //
        //   The client MUST include an Elapsed Time option (see Section 21.9)
        //   to indicate how long the client has been trying to complete the
        //   current DHCP message exchange.
        //
        //   The client uses IA_NA options (see Section 21.4) to request the
        //   assignment of non-temporary addresses, IA_TA options (see
        //   Section 21.5) to request the assignment of temporary addresses, and
        //   IA_PD options (see Section 21.21) to request prefix delegation.
        //   IA_NA, IA_TA, or IA_PD options, or a combination of all, can be
        //   included in DHCP messages. In addition, multiple instances of any
        //   IA option type can be included.
        //
        //   The client MAY include addresses in IA Address options (see
        //   Section 21.6) encapsulated within IA_NA and IA_TA options as hints
        //   to the server about the addresses for which the client has a
        //   preference.
        //
        //   The client MAY include values in IA Prefix options (see
        //   Section 21.22) encapsulated within IA_PD options as hints for the
        //   delegated prefix and/or prefix length for which the client has a
        //   preference. See Section 18.2.4 for more on prefix-length hints.
        //
        //   The client MUST include an Option Request option (ORO) (see
        //   Section 21.7) to request the SOL_MAX_RT option (see Section 21.24)
        //   and any other options the client is interested in receiving. The
        //   client MAY additionally include instances of those options that are
        //   identified in the Option Request option, with data values as hints
        //   to the server about parameter values the client would like to have
        //   returned.
        //
        //   ...
        //
        //   The client MUST NOT include any other options in the Solicit message,
        //   except as specifically allowed in the definition of individual
        //   options.
        let buf = StatefulMessageBuilder {
            transaction_id,
            message_type: v6::MessageType::Solicit,
            server_id: None,
            client_id: &client_id,
            elapsed_time_in_centisecs: elapsed_time,
            options_to_request,
            ia_nas: configured_non_temporary_addresses
                .iter()
                .map(|(iaid, ia)| (*iaid, ia.iter().cloned())),
            ia_pds: configured_delegated_prefixes
                .iter()
                .map(|(iaid, ia)| (*iaid, ia.iter().cloned())),
            _marker: Default::default(),
        }
        .build();

        let retrans_timeout = Self::retransmission_timeout(retrans_timeout, solicit_max_rt, rng);

        Transition {
            state: ClientState::ServerDiscovery(ServerDiscovery {
                client_id,
                configured_non_temporary_addresses,
                configured_delegated_prefixes,
                first_solicit_time,
                retrans_timeout,
                solicit_max_rt,
                collected_advertise,
                collected_sol_max_rt,
            }),
            actions: initial_actions
                .chain([
                    Action::SendMessage(buf),
                    Action::ScheduleTimer(
                        ClientTimerType::Retransmission,
                        now.add(retrans_timeout),
                    ),
                ])
                .collect(),
            transaction_id: None,
        }
    }

    /// Selects a server, or retransmits solicit if no valid advertise were
    /// received.
    fn retransmission_timer_expired<R: Rng>(
        self,
        transaction_id: [u8; 3],
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        let Self {
            client_id,
            configured_non_temporary_addresses,
            configured_delegated_prefixes,
            first_solicit_time,
            retrans_timeout,
            solicit_max_rt,
            mut collected_advertise,
            collected_sol_max_rt,
        } = self;
        let solicit_max_rt = get_common_value(&collected_sol_max_rt).unwrap_or(solicit_max_rt);

        // Update SOL_MAX_RT, per RFC 8415, section 18.2.9:
        //
        //    A client SHOULD only update its SOL_MAX_RT [..] if all received
        //    Advertise messages that contained the corresponding option
        //    specified the same value.
        if let Some(advertise) = collected_advertise.pop() {
            let AdvertiseMessage {
                server_id,
                non_temporary_addresses: advertised_non_temporary_addresses,
                delegated_prefixes: advertised_delegated_prefixes,
                dns_servers: _,
                preference: _,
                receive_time: _,
                preferred_non_temporary_addresses_count: _,
                preferred_delegated_prefixes_count: _,
            } = advertise;
            return Requesting::start(
                client_id,
                server_id,
                advertise_to_ia_entries(
                    advertised_non_temporary_addresses,
                    configured_non_temporary_addresses,
                ),
                advertise_to_ia_entries(
                    advertised_delegated_prefixes,
                    configured_delegated_prefixes,
                ),
                &options_to_request,
                collected_advertise,
                solicit_max_rt,
                rng,
                now,
            );
        }

        ServerDiscovery {
            client_id,
            configured_non_temporary_addresses,
            configured_delegated_prefixes,
            first_solicit_time,
            retrans_timeout,
            solicit_max_rt,
            collected_advertise,
            collected_sol_max_rt,
        }
        .send_and_schedule_retransmission(
            transaction_id,
            options_to_request,
            rng,
            now,
            std::iter::empty(),
        )
    }

    fn advertise_message_received<R: Rng, B: ByteSlice>(
        self,
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        msg: v6::Message<'_, B>,
        now: I,
    ) -> Transition<I> {
        let Self {
            client_id,
            configured_non_temporary_addresses,
            configured_delegated_prefixes,
            first_solicit_time,
            retrans_timeout,
            solicit_max_rt,
            collected_advertise,
            collected_sol_max_rt,
        } = self;

        let ProcessedOptions { server_id, solicit_max_rt_opt, result } = match process_options(
            &msg,
            ExchangeType::AdvertiseToSolicit,
            Some(client_id),
            &configured_non_temporary_addresses,
            &configured_delegated_prefixes,
        ) {
            Ok(processed_options) => processed_options,
            Err(e) => {
                warn!("ignoring Advertise: {}", e);
                return Transition {
                    state: ClientState::ServerDiscovery(ServerDiscovery {
                        client_id,
                        configured_non_temporary_addresses,
                        configured_delegated_prefixes,
                        first_solicit_time,
                        retrans_timeout,
                        solicit_max_rt,
                        collected_advertise,
                        collected_sol_max_rt,
                    }),
                    actions: Vec::new(),
                    transaction_id: None,
                };
            }
        };

        // Process SOL_MAX_RT and discard invalid advertise following RFC 8415,
        // section 18.2.9:
        //
        //    The client MUST process any SOL_MAX_RT option [..] even if the
        //    message contains a Status Code option indicating a failure, and
        //    the Advertise message will be discarded by the client.
        //
        //    The client MUST ignore any Advertise message that contains no
        //    addresses (IA Address options (see Section 21.6) encapsulated in
        //    IA_NA options (see Section 21.4) or IA_TA options (see Section 21.5))
        //    and no delegated prefixes (IA Prefix options (see Section 21.22)
        //    encapsulated in IA_PD options (see Section 21.21)), with the
        //    exception that the client:
        //
        //    -  MUST process an included SOL_MAX_RT option and
        //
        //    -  MUST process an included INF_MAX_RT option.
        let mut collected_sol_max_rt = collected_sol_max_rt;
        if let Some(solicit_max_rt) = solicit_max_rt_opt {
            collected_sol_max_rt.push(solicit_max_rt);
        }
        let Options {
            success_status_message,
            next_contact_time: _,
            preference,
            non_temporary_addresses,
            delegated_prefixes,
            dns_servers,
        } = match result {
            Ok(options) => options,
            Err(e) => {
                warn!("Advertise from server {:?} error status code: {}", server_id, e);
                return Transition {
                    state: ClientState::ServerDiscovery(ServerDiscovery {
                        client_id,
                        configured_non_temporary_addresses,
                        configured_delegated_prefixes,
                        first_solicit_time,
                        retrans_timeout,
                        solicit_max_rt,
                        collected_advertise,
                        collected_sol_max_rt,
                    }),
                    actions: Vec::new(),
                    transaction_id: None,
                };
            }
        };
        match success_status_message {
            Some(success_status_message) if !success_status_message.is_empty() => {
                info!(
                    "Advertise from server {:?} contains success status code message: {}",
                    server_id, success_status_message,
                );
            }
            _ => {
                info!("processing Advertise from server {:?}", server_id);
            }
        }
        let non_temporary_addresses = non_temporary_addresses
            .into_iter()
            .filter_map(|(iaid, ia_na)| {
                let (success_status_message, ia_addrs) = match ia_na {
                    IaNaOption::Success { status_message, t1: _, t2: _, ia_values } => {
                        (status_message, ia_values)
                    }
                    IaNaOption::Failure(e) => {
                        warn!(
                            "Advertise from server {:?} contains IA_NA with error status code: {}",
                            server_id, e
                        );
                        return None;
                    }
                };
                if let Some(success_status_message) = success_status_message {
                    if !success_status_message.is_empty() {
                        info!(
                            "Advertise from server {:?} IA_NA with IAID {:?} \
                            success status code message: {}",
                            server_id, iaid, success_status_message,
                        );
                    }
                }

                let ia_addrs = ia_addrs
                    .into_iter()
                    .filter_map(|(value, lifetimes)| match lifetimes {
                        Ok(Lifetimes { preferred_lifetime: _, valid_lifetime: _ }) => Some(value),
                        e @ Err(
                            LifetimesError::ValidLifetimeZero
                            | LifetimesError::PreferredLifetimeGreaterThanValidLifetime(_),
                        ) => {
                            warn!(
                                "Advertise from server {:?}: ignoring IA Address in \
                                 IA_NA with IAID {:?} because of invalid lifetimes: {:?}",
                                server_id, iaid, e
                            );

                            // Per RFC 8415 section 21.6,
                            //
                            //   The client MUST discard any addresses for which
                            //   the preferred lifetime is greater than the
                            //   valid lifetime.
                            None
                        }
                    })
                    .collect::<HashSet<_>>();

                (!ia_addrs.is_empty()).then(|| (iaid, ia_addrs))
            })
            .collect::<HashMap<_, _>>();
        let delegated_prefixes = delegated_prefixes
            .into_iter()
            .filter_map(|(iaid, ia_pd)| {
                let (success_status_message, ia_prefixes) = match ia_pd {
                    IaPdOption::Success { status_message, t1: _, t2: _, ia_values } => {
                        (status_message, ia_values)
                    }
                    IaPdOption::Failure(e) => {
                        warn!(
                            "Advertise from server {:?} contains IA_PD with error status code: {}",
                            server_id, e
                        );
                        return None;
                    }
                };
                if let Some(success_status_message) = success_status_message {
                    if !success_status_message.is_empty() {
                        info!(
                            "Advertise from server {:?} IA_PD with IAID {:?} \
                            success status code message: {}",
                            server_id, iaid, success_status_message,
                        );
                    }
                }
                let ia_prefixes = ia_prefixes
                    .into_iter()
                    .filter_map(|(value, lifetimes)| match lifetimes {
                        Ok(Lifetimes { preferred_lifetime: _, valid_lifetime: _ }) => Some(value),
                        e @ Err(
                            LifetimesError::ValidLifetimeZero
                            | LifetimesError::PreferredLifetimeGreaterThanValidLifetime(_),
                        ) => {
                            warn!(
                                "Advertise from server {:?}: ignoring IA Prefix in \
                                 IA_PD with IAID {:?} because of invalid lifetimes: {:?}",
                                server_id, iaid, e
                            );

                            // Per RFC 8415 section 21.22,
                            //
                            //   The client MUST discard any prefixes for which
                            //   the preferred lifetime is greater than the
                            //   valid lifetime.
                            None
                        }
                    })
                    .collect::<HashSet<_>>();

                (!ia_prefixes.is_empty()).then(|| (iaid, ia_prefixes))
            })
            .collect::<HashMap<_, _>>();
        let advertise = AdvertiseMessage {
            preferred_non_temporary_addresses_count: compute_preferred_ia_count(
                &non_temporary_addresses,
                &configured_non_temporary_addresses,
            ),
            preferred_delegated_prefixes_count: compute_preferred_ia_count(
                &delegated_prefixes,
                &configured_delegated_prefixes,
            ),
            server_id,
            non_temporary_addresses,
            delegated_prefixes,
            dns_servers: dns_servers.unwrap_or(Vec::new()),
            // Per RFC 8415, section 18.2.1:
            //
            //   Any valid Advertise that does not include a Preference
            //   option is considered to have a preference value of 0.
            preference: preference.unwrap_or(0),
            receive_time: now,
        };
        if !advertise.has_ias() {
            return Transition {
                state: ClientState::ServerDiscovery(ServerDiscovery {
                    client_id,
                    configured_non_temporary_addresses,
                    configured_delegated_prefixes,
                    first_solicit_time,
                    retrans_timeout,
                    solicit_max_rt,
                    collected_advertise,
                    collected_sol_max_rt,
                }),
                actions: Vec::new(),
                transaction_id: None,
            };
        }

        let solicit_timeout = INITIAL_SOLICIT_TIMEOUT.as_secs_f64();
        let is_retransmitting = retrans_timeout.as_secs_f64()
            >= solicit_timeout + solicit_timeout * RANDOMIZATION_FACTOR_MAX;

        // Select server if its preference value is `255` and the advertise is
        // acceptable, as described in RFC 8415, section 18.2.1:
        //
        //    If the client receives a valid Advertise message that includes a
        //    Preference option with a preference value of 255, the client
        //    immediately begins a client-initiated message exchange (as
        //    described in Section 18.2.2) by sending a Request message to the
        //    server from which the Advertise message was received.
        //
        // Per RFC 8415, section 18.2.9:
        //
        //    Those Advertise messages with the highest server preference value
        //    SHOULD be preferred over all other Advertise messages.  The
        //    client MAY choose a less preferred server if that server has a
        //    better set of advertised parameters.
        //
        // During retrasmission, the client select the server that sends the
        // first valid advertise, regardless of preference value or advertise
        // completeness, as described in RFC 8415, section 18.2.1:
        //
        //    The client terminates the retransmission process as soon as it
        //    receives any valid Advertise message, and the client acts on the
        //    received Advertise message without waiting for any additional
        //    Advertise messages.
        if (advertise.preference == ADVERTISE_MAX_PREFERENCE) || is_retransmitting {
            let solicit_max_rt = get_common_value(&collected_sol_max_rt).unwrap_or(solicit_max_rt);
            let AdvertiseMessage {
                server_id,
                non_temporary_addresses: advertised_non_temporary_addresses,
                delegated_prefixes: advertised_delegated_prefixes,
                dns_servers: _,
                preference: _,
                receive_time: _,
                preferred_non_temporary_addresses_count: _,
                preferred_delegated_prefixes_count: _,
            } = advertise;
            return Requesting::start(
                client_id,
                server_id,
                advertise_to_ia_entries(
                    advertised_non_temporary_addresses,
                    configured_non_temporary_addresses,
                ),
                advertise_to_ia_entries(
                    advertised_delegated_prefixes,
                    configured_delegated_prefixes,
                ),
                &options_to_request,
                collected_advertise,
                solicit_max_rt,
                rng,
                now,
            );
        }

        let mut collected_advertise = collected_advertise;
        collected_advertise.push(advertise);
        Transition {
            state: ClientState::ServerDiscovery(ServerDiscovery {
                client_id,
                configured_non_temporary_addresses,
                configured_delegated_prefixes,
                first_solicit_time,
                retrans_timeout,
                solicit_max_rt,
                collected_advertise,
                collected_sol_max_rt,
            }),
            actions: Vec::new(),
            transaction_id: None,
        }
    }
}

// Returns the min value greater than zero, if the arguments are non zero.  If
// the new value is zero, the old value is returned unchanged; otherwise if the
// old value is zero, the new value is returned. Used for calculating the
// minimum T1/T2 as described in RFC 8415, section 18.2.4:
//
//    [..] the client SHOULD renew/rebind all IAs from the
//    server at the same time, the client MUST select T1 and
//    T2 times from all IA options that will guarantee that
//    the client initiates transmissions of Renew/Rebind
//    messages not later than at the T1/T2 times associated
//    with any of the client's bindings (earliest T1/T2).
fn maybe_get_nonzero_min(old_value: v6::TimeValue, new_value: v6::TimeValue) -> v6::TimeValue {
    match old_value {
        v6::TimeValue::Zero => new_value,
        v6::TimeValue::NonZero(old_t) => v6::TimeValue::NonZero(get_nonzero_min(old_t, new_value)),
    }
}

// Returns the min value greater than zero.
fn get_nonzero_min(
    old_value: v6::NonZeroTimeValue,
    new_value: v6::TimeValue,
) -> v6::NonZeroTimeValue {
    match new_value {
        v6::TimeValue::Zero => old_value,
        v6::TimeValue::NonZero(new_val) => std::cmp::min(old_value, new_val),
    }
}

/// Provides methods for handling state transitions from requesting state.
#[derive(Debug)]
struct Requesting<I> {
    /// [Client Identifier] used for uniquely identifying the client in
    /// communication with servers.
    ///
    /// [Client Identifier]:
    /// https://datatracker.ietf.org/doc/html/rfc8415#section-21.2
    client_id: [u8; CLIENT_ID_LEN],
    /// The non-temporary addresses negotiated by the client.
    non_temporary_addresses: HashMap<v6::IAID, AddressEntry<I>>,
    /// The delegated prefixes negotiated by the client.
    delegated_prefixes: HashMap<v6::IAID, PrefixEntry<I>>,
    /// The [server identifier] of the server to which the client sends
    /// requests.
    ///
    /// [Server Identifier]:
    /// https://datatracker.ietf.org/doc/html/rfc8415#section-21.3
    server_id: Vec<u8>,
    /// The advertise collected from servers during [server discovery].
    ///
    /// [server discovery]:
    /// https://datatracker.ietf.org/doc/html/rfc8415#section-18
    collected_advertise: BinaryHeap<AdvertiseMessage<I>>,
    /// The time of the first request. Used in calculating the [elapsed time].
    ///
    /// [elapsed time]: https://datatracker.ietf.org/doc/html/rfc8415#section-21.9
    first_request_time: I,
    /// The request retransmission timeout.
    retrans_timeout: Duration,
    /// The number of request messages transmitted.
    transmission_count: u8,
    /// The [SOL_MAX_RT] used by the client.
    ///
    /// [SOL_MAX_RT]:
    /// https://datatracker.ietf.org/doc/html/rfc8415#section-21.24
    solicit_max_rt: Duration,
}

fn compute_t(min: v6::NonZeroTimeValue, ratio: Ratio<u32>) -> v6::NonZeroTimeValue {
    match min {
        v6::NonZeroTimeValue::Finite(t) => {
            ratio.checked_mul(&Ratio::new_raw(t.get(), 1)).map_or(
                v6::NonZeroTimeValue::Infinity,
                |t| {
                    v6::NonZeroTimeValue::Finite(v6::NonZeroOrMaxU32::new(t.to_integer()).expect(
                        "non-zero ratio of NonZeroOrMaxU32 value should be NonZeroOrMaxU32",
                    ))
                },
            )
        }
        v6::NonZeroTimeValue::Infinity => v6::NonZeroTimeValue::Infinity,
    }
}

#[derive(Debug, thiserror::Error)]
enum ReplyWithLeasesError {
    #[error("option processing error")]
    OptionsError(#[from] OptionsError),
    #[error("mismatched Server ID, got {got:?} want {want:?}")]
    MismatchedServerId { got: Vec<u8>, want: Vec<u8> },
    #[error("status code error")]
    ErrorStatusCode(#[from] ErrorStatusCode),
}

#[derive(Debug, Copy, Clone)]
enum IaStatusError {
    Retry { without_hints: bool },
    Invalid,
    Rerequest,
}

fn process_ia_error_status(
    request_type: RequestLeasesMessageType,
    error_status: v6::ErrorStatusCode,
    ia_kind: IaKind,
) -> IaStatusError {
    match (request_type, error_status, ia_kind) {
        // Per RFC 8415, section 18.3.2:
        //
        //    If any of the prefixes of the included addresses are not
        //    appropriate for the link to which the client is connected,
        //    the server MUST return the IA to the client with a Status Code
        //    option (see Section 21.13) with the value NotOnLink.
        //
        // If the client receives IA_NAs with NotOnLink status, try to obtain
        // other addresses in follow-up messages.
        (RequestLeasesMessageType::Request, v6::ErrorStatusCode::NotOnLink, IaKind::Address) => {
            IaStatusError::Retry { without_hints: true }
        }
        // NotOnLink is not expected for prefixes.
        //
        // Per RFC 8415 section 18.3.2,
        //
        //   For any IA_PD option (see Section 21.21) in the Request message to
        //   which the server cannot assign any delegated prefixes, the server
        //   MUST return the IA_PD option in the Reply message with no prefixes
        //   in the IA_PD and with a Status Code option containing status code
        //   NoPrefixAvail in the IA_PD.
        (RequestLeasesMessageType::Request, v6::ErrorStatusCode::NotOnLink, IaKind::Prefix) => {
            IaStatusError::Invalid
        }
        // NotOnLink is not expected in Reply to Renew/Rebind. The server
        // indicates that the IA is not appropriate for the link by setting
        // lifetime 0, not by using NotOnLink status.
        //
        // For Renewing, per RFC 8415 section 18.3.4:
        //
        //    If the server finds that any of the addresses in the IA are
        //    not appropriate for the link to which the client is attached,
        //    the server returns the address to the client with lifetimes of 0.
        //
        //    If the server finds that any of the delegated prefixes in the IA
        //    are not appropriate for the link to which the client is attached,
        //    the server returns the delegated prefix to the client with
        //    lifetimes of 0.
        //
        // For Rebinding, per RFC 8415 section 18.3.6:
        //
        //    If the server finds that the client entry for the IA and any of
        //    the addresses or delegated prefixes are no longer appropriate for
        //    the link to which the client's interface is attached according to
        //    the server's explicit configuration information, the server
        //    returns those addresses or delegated prefixes to the client with
        //    lifetimes of 0.
        (
            RequestLeasesMessageType::Renew | RequestLeasesMessageType::Rebind,
            v6::ErrorStatusCode::NotOnLink,
            IaKind::Address | IaKind::Prefix,
        ) => IaStatusError::Invalid,

        // Per RFC 18.2.10,
        //
        //   If the client receives a Reply message with a status code of
        //   UnspecFail, the server is indicating that it was unable to process
        //   the client's message due to an unspecified failure condition. If
        //   the client retransmits the original message to the same server to
        //   retry the desired operation, the client MUST limit the rate at
        //   which it retransmits the message and limit the duration of the time
        //   during which it retransmits the message (see Section 14.1).
        (
            RequestLeasesMessageType::Request
            | RequestLeasesMessageType::Renew
            | RequestLeasesMessageType::Rebind,
            v6::ErrorStatusCode::UnspecFail,
            IaKind::Address | IaKind::Prefix,
        ) => IaStatusError::Retry { without_hints: false },

        // When responding to Request messages, per section 18.3.2:
        //
        //    If the server [..] cannot assign any IP addresses to an IA,
        //    the server MUST return the IA option in the Reply message with
        //    no addresses in the IA and a Status Code option containing
        //    status code NoAddrsAvail in the IA.
        //
        // When responding to Renew messages, per section 18.3.4:
        //
        //    -  If the server is configured to create new bindings as
        //    a result of processing Renew messages but the server will
        //    not assign any leases to an IA, the server returns the IA
        //    option containing a Status Code option with the NoAddrsAvail.
        //
        // When responding to Rebind messages, per section 18.3.5:
        //
        //    -  If the server is configured to create new bindings as a result
        //    of processing Rebind messages but the server will not assign any
        //    leases to an IA, the server returns the IA option containing a
        //    Status Code option (see Section 21.13) with the NoAddrsAvail or
        //    NoPrefixAvail status code and a status message for a user.
        //
        // Retry obtaining this IA_NA in subsequent messages.
        //
        // TODO(https://fxbug.dev/81086): implement rate limiting.
        (
            RequestLeasesMessageType::Request
            | RequestLeasesMessageType::Renew
            | RequestLeasesMessageType::Rebind,
            v6::ErrorStatusCode::NoAddrsAvail,
            IaKind::Address,
        ) => IaStatusError::Retry { without_hints: false },
        // NoAddrsAvail is not expected for prefixes. The equivalent error for
        // prefixes is NoPrefixAvail.
        (
            RequestLeasesMessageType::Request
            | RequestLeasesMessageType::Renew
            | RequestLeasesMessageType::Rebind,
            v6::ErrorStatusCode::NoAddrsAvail,
            IaKind::Prefix,
        ) => IaStatusError::Invalid,

        // When responding to Request messages, per section 18.3.2:
        //
        //    For any IA_PD option (see Section 21.21) in the Request message to
        //    which the server cannot assign any delegated prefixes, the server
        //    MUST return the IA_PD option in the Reply message with no prefixes
        //    in the IA_PD and with a Status Code option containing status code
        //    NoPrefixAvail in the IA_PD.
        //
        // When responding to Renew messages, per section 18.3.4:
        //
        //    -  If the server is configured to create new bindings as
        //    a result of processing Renew messages but the server will
        //    not assign any leases to an IA, the server returns the IA
        //    option containing a Status Code option with the NoAddrsAvail
        //    or NoPrefixAvail status code and a status message for a user.
        //
        // When responding to Rebind messages, per section 18.3.5:
        //
        //    -  If the server is configured to create new bindings as a result
        //    of processing Rebind messages but the server will not assign any
        //    leases to an IA, the server returns the IA option containing a
        //    Status Code option (see Section 21.13) with the NoAddrsAvail or
        //    NoPrefixAvail status code and a status message for a user.
        //
        // Retry obtaining this IA_PD in subsequent messages.
        //
        // TODO(https://fxbug.dev/81086): implement rate limiting.
        (
            RequestLeasesMessageType::Request
            | RequestLeasesMessageType::Renew
            | RequestLeasesMessageType::Rebind,
            v6::ErrorStatusCode::NoPrefixAvail,
            IaKind::Prefix,
        ) => IaStatusError::Retry { without_hints: false },
        (
            RequestLeasesMessageType::Request
            | RequestLeasesMessageType::Renew
            | RequestLeasesMessageType::Rebind,
            v6::ErrorStatusCode::NoPrefixAvail,
            IaKind::Address,
        ) => IaStatusError::Invalid,

        // Per RFC 8415 section 18.2.10.1:
        //
        //    When the client receives a Reply message in response to a Renew or
        //    Rebind message, the client:
        //
        //    -  Sends a Request message to the server that responded if any of
        //    the IAs in the Reply message contain the NoBinding status code.
        //    The client places IA options in this message for all IAs.  The
        //    client continues to use other bindings for which the server did
        //    not return an error.
        //
        // The client removes the IA not found by the server, and transitions to
        // Requesting after processing all the received IAs.
        (
            RequestLeasesMessageType::Renew | RequestLeasesMessageType::Rebind,
            v6::ErrorStatusCode::NoBinding,
            IaKind::Address | IaKind::Prefix,
        ) => IaStatusError::Rerequest,
        // NoBinding is not expected in Requesting as the Request message is
        // asking for a new binding, not attempting to refresh lifetimes for
        // an existing binding.
        (
            RequestLeasesMessageType::Request,
            v6::ErrorStatusCode::NoBinding,
            IaKind::Address | IaKind::Prefix,
        ) => IaStatusError::Invalid,

        // Per RFC 8415 section 18.2.10,
        //
        //   If the client receives a Reply message with a status code of
        //   UseMulticast, the client records the receipt of the message and
        //   sends subsequent messages to the server through the interface on
        //   which the message was received using multicast. The client resends
        //   the original message using multicast.
        //
        // We currently always multicast our messages so we do not expect the
        // UseMulticast error.
        //
        // TODO(https://fxbug.dev/76764): Do not consider this an invalid error
        // when unicasting messages.
        (
            RequestLeasesMessageType::Request | RequestLeasesMessageType::Renew,
            v6::ErrorStatusCode::UseMulticast,
            IaKind::Address | IaKind::Prefix,
        ) => IaStatusError::Invalid,
        // Per RFC 8415 section 16,
        //
        //   A server MUST discard any Solicit, Confirm, Rebind, or
        //   Information-request messages it receives with a Layer 3 unicast
        //   destination address.
        //
        // Since we must never unicast Rebind messages, we always multicast them
        // so we consider a UseMulticast error invalid.
        (
            RequestLeasesMessageType::Rebind,
            v6::ErrorStatusCode::UseMulticast,
            IaKind::Address | IaKind::Prefix,
        ) => IaStatusError::Invalid,
    }
}

// Possible states to move to after processing a Reply containing leases.
#[derive(Debug)]
enum StateAfterReplyWithLeases {
    RequestNextServer,
    Assigned,
    StayRenewingRebinding,
    Requesting,
}

#[derive(Debug)]
struct ProcessedReplyWithLeases<I> {
    server_id: Vec<u8>,
    non_temporary_addresses: HashMap<v6::IAID, AddressEntry<I>>,
    delegated_prefixes: HashMap<v6::IAID, PrefixEntry<I>>,
    dns_servers: Option<Vec<Ipv6Addr>>,
    actions: Vec<Action<I>>,
    next_state: StateAfterReplyWithLeases,
}

fn has_no_assigned_ias<V: IaValue, I>(entries: &HashMap<v6::IAID, IaEntry<V, I>>) -> bool {
    entries.iter().all(|(_iaid, entry)| match entry {
        IaEntry::ToRequest(_) => true,
        IaEntry::Assigned(_) => false,
    })
}

struct ComputeNewEntriesWithCurrentIasAndReplyResult<V: IaValue, I> {
    new_entries: HashMap<v6::IAID, IaEntry<V, I>>,
    go_to_requesting: bool,
    missing_ias_in_reply: bool,
    updates: HashMap<v6::IAID, HashMap<V, IaValueUpdateKind>>,
    all_ias_invalidates_at: Option<AllIasInvalidatesAt<I>>,
}

#[derive(Copy, Clone, Eq, Ord, PartialEq, PartialOrd)]
enum AllIasInvalidatesAt<I> {
    At(I),
    Never,
}

fn compute_new_entries_with_current_ias_and_reply<V: IaValue, I: Instant>(
    ia_name: &str,
    request_type: RequestLeasesMessageType,
    ias_in_reply: HashMap<v6::IAID, IaOption<V>>,
    current_entries: &HashMap<v6::IAID, IaEntry<V, I>>,
    now: I,
) -> ComputeNewEntriesWithCurrentIasAndReplyResult<V, I> {
    let mut go_to_requesting = false;
    let mut all_ias_invalidates_at = None;

    let mut update_all_ias_invalidates_at = |LifetimesInfo::<I> {
                                                 lifetimes:
                                                     Lifetimes { valid_lifetime, preferred_lifetime: _ },
                                                 updated_at,
                                             }| {
        all_ias_invalidates_at = core::cmp::max(
            all_ias_invalidates_at,
            Some(match valid_lifetime {
                v6::NonZeroTimeValue::Finite(lifetime) => AllIasInvalidatesAt::At(
                    updated_at.add(Duration::from_secs(lifetime.get().into())),
                ),
                v6::NonZeroTimeValue::Infinity => AllIasInvalidatesAt::Never,
            }),
        );
    };

    // As per RFC 8415 section 18.2.10.1:
    //
    //   If the Reply was received in response to a Solicit (with a
    //   Rapid Commit option), Request, Renew, or Rebind message, the
    //   client updates the information it has recorded about IAs from
    //   the IA options contained in the Reply message:
    //
    //   ...
    //
    //   -  Add any new leases in the IA option to the IA as recorded
    //      by the client.
    //
    //   -  Update lifetimes for any leases in the IA option that the
    //      client already has recorded in the IA.
    //
    //   -  Discard any leases from the IA, as recorded by the client,
    //      that have a valid lifetime of 0 in the IA Address or IA
    //      Prefix option.
    //
    //   -  Leave unchanged any information about leases the client has
    //      recorded in the IA but that were not included in the IA from
    //      the server
    let mut updates = HashMap::new();

    let mut new_entries = ias_in_reply
        .into_iter()
        .map(|(iaid, ia)| {
            let current_entry = current_entries
                .get(&iaid)
                .expect("process_options should have caught unrequested IAs");

            let (success_status_message, ia_values) = match ia {
                IaOption::Success { status_message, t1: _, t2: _, ia_values } => {
                    (status_message, ia_values)
                }
                IaOption::Failure(ErrorStatusCode(error_code, msg)) => {
                    if !msg.is_empty() {
                        warn!(
                            "Reply to {}: {} with IAID {:?} status code {:?} message: {}",
                            request_type, ia_name, iaid, error_code, msg
                        );
                    }
                    let error = process_ia_error_status(request_type, error_code, V::KIND);
                    let without_hints = match error {
                        IaStatusError::Retry { without_hints } => without_hints,
                        IaStatusError::Invalid => {
                            warn!(
                                "Reply to {}: received unexpected status code {:?} in {} option with IAID {:?}",
                                request_type, error_code, ia_name, iaid,
                            );
                            false
                        }
                        IaStatusError::Rerequest => {
                            go_to_requesting = true;
                            false
                        }
                    };

                    // Let bindings know that the previously assigned values
                    // should no longer be used.
                    match current_entry {
                        IaEntry::Assigned(values) => {
                            assert_matches!(
                                updates.insert(
                                    iaid,
                                    values
                                        .keys()
                                        .cloned()
                                        .map(|value| (value, IaValueUpdateKind::Removed))
                                        .collect()
                                ),
                                None
                            );
                        },
                        IaEntry::ToRequest(_) => {},
                    }

                    return (iaid, current_entry.to_request(without_hints));
                }
            };

            if let Some(success_status_message) = success_status_message {
                if !success_status_message.is_empty() {
                    info!(
                        "Reply to {}: {} with IAID {:?} success status code message: {}",
                        request_type, ia_name, iaid, success_status_message,
                    );
                }
            }

            // The server has not included an IA Address/Prefix option in the
            // IA, keep the previously recorded information,
            // per RFC 8415 section 18.2.10.1:
            //
            //     -  Leave unchanged any information about leases the client
            //        has recorded in the IA but that were not included in the
            //        IA from the server.
            //
            // The address/prefix remains assigned until the end of its valid
            // lifetime, or it is requested later if it was not assigned.
            if ia_values.is_empty() {
                return (iaid, current_entry.clone());
            }

            let mut inner_updates = HashMap::new();
            let mut ia_values = ia_values
                .into_iter()
                .filter_map(|(value, lifetimes)| {
                    match lifetimes {
                        // Let bindings know about the assigned lease in the
                        // reply.
                        Ok(lifetimes) => {
                            assert_matches!(
                                inner_updates.insert(
                                    value,
                                    match current_entry {
                                        IaEntry::Assigned(values) => {
                                            if values.contains_key(&value) {
                                                IaValueUpdateKind::UpdatedLifetimes(lifetimes)
                                            } else {
                                                IaValueUpdateKind::Added(lifetimes)
                                            }
                                        },
                                        IaEntry::ToRequest(_) => IaValueUpdateKind::Added(lifetimes),
                                    },
                                ),
                                None
                            );

                            let lifetimes_info = LifetimesInfo { lifetimes, updated_at: now };
                            update_all_ias_invalidates_at(lifetimes_info);

                            Some((
                                value,
                                lifetimes_info,
                            ))
                        },
                        Err(LifetimesError::PreferredLifetimeGreaterThanValidLifetime(Lifetimes {
                            preferred_lifetime,
                            valid_lifetime,
                        })) => {
                            // As per RFC 8415 section 21.6,
                            //
                            //   The client MUST discard any addresses for which
                            //   the preferred lifetime is greater than the
                            //   valid lifetime.
                            //
                            // As per RFC 8415 section 21.22,
                            //
                            //   The client MUST discard any prefixes for which
                            //   the preferred lifetime is greater than the
                            //   valid lifetime.
                            warn!(
                                "Reply to {}: {} with IAID {:?}: ignoring value={:?} because \
                                 preferred lifetime={:?} greater than valid lifetime={:?}",
                                request_type, ia_name, iaid, value, preferred_lifetime, valid_lifetime
                            );

                            None
                        },
                        Err(LifetimesError::ValidLifetimeZero) => {
                            info!(
                                "Reply to {}: {} with IAID {:?}: invalidating value={:?} \
                                 with zero lifetime",
                                request_type, ia_name, iaid, value
                            );

                            // Let bindings know when a previously assigned
                            // value should be immediately invalidated when the
                            // reply includes it with a zero valid lifetime.
                            match current_entry {
                                IaEntry::Assigned(values) => {
                                    if values.contains_key(&value) {
                                        assert_matches!(
                                            inner_updates.insert(
                                                value,
                                                IaValueUpdateKind::Removed,
                                            ),
                                            None
                                        );
                                    }
                                }
                                IaEntry::ToRequest(_) => {},
                            }

                            None
                        }
                    }
                })
                .collect::<HashMap<_, _>>();

            // Merge existing values that were not present in the new IA.
            match current_entry {
                IaEntry::Assigned(values) => {
                    for (value, lifetimes) in values {
                        match ia_values.entry(*value) {
                            // If we got the value in the Reply, do nothing
                            // further for this value.
                            Entry::Occupied(_) => {},

                            // We are missing the value in the new IA.
                            //
                            // Either the value is missing from the IA in the
                            // Reply or the Reply invalidated the value.
                            Entry::Vacant(e) => match inner_updates.get(value) {
                                // If we have an update, it MUST be a removal
                                // since add/lifetime change events should have
                                // resulted in the value being present in the
                                // new IA's values.
                                Some(update) => assert_eq!(update, &IaValueUpdateKind::Removed),
                                // The Reply is missing this value so we just copy
                                // it into the new set of values.
                                None => {
                                    let lifetimes = lifetimes.clone();
                                    update_all_ias_invalidates_at(lifetimes);
                                    let _: &mut LifetimesInfo<_> = e.insert(lifetimes);
                                }
                            }

                        }
                    }
                },
                IaEntry::ToRequest(_) => {},
            }

            assert_matches!(updates.insert(iaid, inner_updates), None);

            if ia_values.is_empty() {
                (iaid, IaEntry::ToRequest(current_entry.value().collect()))
            } else {
                // At this point we know the IA will be considered assigned.
                //
                // Any current values not in the replied IA should be left alone
                // as per RFC 8415 section 18.2.10.1:
                //
                //   -  Leave unchanged any information about leases the client
                //      has recorded in the IA but that were not included in the
                //      IA from the server.
                (iaid, IaEntry::Assigned(ia_values))
            }
        })
        .collect::<HashMap<_, _>>();

    // Add current entries that were not received in this Reply.
    let mut missing_ias_in_reply = false;
    for (iaid, entry) in current_entries {
        match new_entries.entry(*iaid) {
            Entry::Occupied(_) => {
                // We got the entry in the Reply, do nothing further for this
                // IA.
            }
            Entry::Vacant(e) => {
                // We did not get this entry in the IA.
                missing_ias_in_reply = true;

                let _: &mut IaEntry<_, _> = e.insert(match entry {
                    IaEntry::ToRequest(address_to_request) => {
                        IaEntry::ToRequest(address_to_request.clone())
                    }
                    IaEntry::Assigned(ia) => IaEntry::Assigned(ia.clone()),
                });
            }
        }
    }

    ComputeNewEntriesWithCurrentIasAndReplyResult {
        new_entries,
        go_to_requesting,
        missing_ias_in_reply,
        updates,
        all_ias_invalidates_at,
    }
}

/// An update for an IA value.
#[derive(Debug, PartialEq, Clone)]
pub struct IaValueUpdate<V> {
    pub value: V,
    pub kind: IaValueUpdateKind,
}

/// An IA Value's update kind.
#[derive(Debug, PartialEq, Clone)]
pub enum IaValueUpdateKind {
    Added(Lifetimes),
    UpdatedLifetimes(Lifetimes),
    Removed,
}

/// An IA update.
#[derive(Debug, PartialEq, Clone)]
pub struct IaUpdate<V> {
    pub iaid: v6::IAID,
    pub values: Vec<IaValueUpdate<V>>,
}

// Processes a Reply to Solicit (with fast commit), Request, Renew, or Rebind.
//
// If an error is returned, the message should be ignored.
fn process_reply_with_leases<B: ByteSlice, I: Instant>(
    client_id: [u8; CLIENT_ID_LEN],
    server_id: &[u8],
    current_non_temporary_addresses: &HashMap<v6::IAID, AddressEntry<I>>,
    current_delegated_prefixes: &HashMap<v6::IAID, PrefixEntry<I>>,
    solicit_max_rt: &mut Duration,
    msg: &v6::Message<'_, B>,
    request_type: RequestLeasesMessageType,
    now: I,
) -> Result<ProcessedReplyWithLeases<I>, ReplyWithLeasesError> {
    let ProcessedOptions { server_id: got_server_id, solicit_max_rt_opt, result } =
        process_options(
            &msg,
            ExchangeType::ReplyWithLeases(request_type),
            Some(client_id),
            current_non_temporary_addresses,
            current_delegated_prefixes,
        )?;

    match request_type {
        RequestLeasesMessageType::Request | RequestLeasesMessageType::Renew => {
            if got_server_id != server_id {
                return Err(ReplyWithLeasesError::MismatchedServerId {
                    got: got_server_id,
                    want: server_id.to_vec(),
                });
            }
        }
        // Accept a message from any server if this is a reply to a rebind
        // message.
        RequestLeasesMessageType::Rebind => {}
    }

    // Always update SOL_MAX_RT, per RFC 8415, section 18.2.10:
    //
    //    The client MUST process any SOL_MAX_RT option (see Section 21.24)
    //    and INF_MAX_RT option (see Section
    //    21.25) present in a Reply message, even if the message contains a
    //    Status Code option indicating a failure.
    *solicit_max_rt = solicit_max_rt_opt
        .map_or(*solicit_max_rt, |solicit_max_rt| Duration::from_secs(solicit_max_rt.into()));

    let Options {
        success_status_message,
        next_contact_time,
        preference: _,
        non_temporary_addresses,
        delegated_prefixes,
        dns_servers,
    } = result?;

    let (t1, t2) = assert_matches!(
        next_contact_time,
        NextContactTime::RenewRebind { t1, t2 } => (t1, t2)
    );

    if let Some(success_status_message) = success_status_message {
        if !success_status_message.is_empty() {
            info!(
                "Reply to {} success status code message: {}",
                request_type, success_status_message
            );
        }
    }

    let (
        non_temporary_addresses,
        ia_na_updates,
        delegated_prefixes,
        ia_pd_updates,
        go_to_requesting,
        missing_ias_in_reply,
        all_ias_invalidates_at,
    ) = {
        let ComputeNewEntriesWithCurrentIasAndReplyResult {
            new_entries: non_temporary_addresses,
            go_to_requesting: go_to_requesting_iana,
            missing_ias_in_reply: missing_ias_in_reply_iana,
            updates: ia_na_updates,
            all_ias_invalidates_at: all_ia_nas_invalidates_at,
        } = compute_new_entries_with_current_ias_and_reply(
            IA_NA_NAME,
            request_type,
            non_temporary_addresses,
            current_non_temporary_addresses,
            now,
        );
        let ComputeNewEntriesWithCurrentIasAndReplyResult {
            new_entries: delegated_prefixes,
            go_to_requesting: go_to_requesting_iapd,
            missing_ias_in_reply: missing_ias_in_reply_iapd,
            updates: ia_pd_updates,
            all_ias_invalidates_at: all_ia_pds_invalidates_at,
        } = compute_new_entries_with_current_ias_and_reply(
            IA_PD_NAME,
            request_type,
            delegated_prefixes,
            current_delegated_prefixes,
            now,
        );
        (
            non_temporary_addresses,
            ia_na_updates,
            delegated_prefixes,
            ia_pd_updates,
            go_to_requesting_iana || go_to_requesting_iapd,
            missing_ias_in_reply_iana || missing_ias_in_reply_iapd,
            core::cmp::max(all_ia_nas_invalidates_at, all_ia_pds_invalidates_at),
        )
    };

    // Per RFC 8415, section 18.2.10.1:
    //
    //    If the Reply message contains any IAs but the client finds no
    //    usable addresses and/or delegated prefixes in any of these IAs,
    //    the client may either try another server (perhaps restarting the
    //    DHCP server discovery process) or use the Information-request
    //    message to obtain other configuration information only.
    //
    // If there are no usable addresses/prefixecs and no other servers to
    // select, the client restarts server discovery instead of requesting
    // configuration information only. This option is preferred when the
    // client operates in stateful mode, where the main goal for the client is
    // to negotiate addresses/prefixes.
    let next_state = if has_no_assigned_ias(&non_temporary_addresses)
        && has_no_assigned_ias(&delegated_prefixes)
    {
        warn!("Reply to {}: no usable lease returned", request_type);
        StateAfterReplyWithLeases::RequestNextServer
    } else if go_to_requesting {
        StateAfterReplyWithLeases::Requesting
    } else {
        match request_type {
            RequestLeasesMessageType::Request => StateAfterReplyWithLeases::Assigned,
            RequestLeasesMessageType::Renew | RequestLeasesMessageType::Rebind => {
                if missing_ias_in_reply {
                    // Stay in Renewing/Rebinding if any of the assigned IAs that the client
                    // is trying to renew are not included in the Reply, per RFC 8451 section
                    // 18.2.10.1:
                    //
                    //    When the client receives a Reply message in response to a Renew or
                    //    Rebind message, the client: [..] Sends a Renew/Rebind if any of
                    //    the IAs are not in the Reply message, but as this likely indicates
                    //    that the server that responded does not support that IA type, sending
                    //    immediately is unlikely to produce a different result.  Therefore,
                    //    the client MUST rate-limit its transmissions (see Section 14.1) and
                    //    MAY just wait for the normal retransmission time (as if the Reply
                    //    message had not been received).  The client continues to use other
                    //    bindings for which the server did return information.
                    //
                    // TODO(https://fxbug.dev/81086): implement rate limiting.
                    warn!(
                        "Reply to {}: allowing retransmit timeout to retry due to missing IA",
                        request_type
                    );
                    StateAfterReplyWithLeases::StayRenewingRebinding
                } else {
                    StateAfterReplyWithLeases::Assigned
                }
            }
        }
    };
    let actions = match next_state {
        StateAfterReplyWithLeases::Assigned => Some(
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                // Set timer to start renewing addresses, per RFC 8415, section
                // 18.2.4:
                //
                //    At time T1, the client initiates a Renew/Reply message
                //    exchange to extend the lifetimes on any leases in the IA.
                //
                // Addresses are not renewed if T1 is infinity, per RFC 8415,
                // section 7.7:
                //
                //    A client will never attempt to extend the lifetimes of any
                //    addresses in an IA with T1 set to 0xffffffff.
                //
                // If the Renew time (T1) is equal to the Rebind time (T2), we
                // skip setting the Renew timer.
                //
                // This is a slight deviation from the RFC which does not
                // mention any special-case when `T1 == T2`. We do this here
                // so that we can strictly enforce that when a Rebind timer
                // fires, no Renew timers exist, preventing a state machine
                // from transitioning from `Assigned -> Rebind -> Renew`
                // which is clearly wrong as Rebind is only entered _after_
                // Renew (when Renewing fails). Per RFC 8415 section 18.2.5,
                //
                //   At time T2 (which will only be reached if the server to
                //   which the Renew message was sent starting at time T1
                //   has not responded), the client initiates a Rebind/Reply
                //   message exchange with any available server.
                //
                // Note that, the alternative to this is to always schedule
                // the Renew and Rebind timers at T1 and T2, respectively,
                // but unconditionally cancel the Renew timer when entering
                // the Rebind state. This will be _almost_ the same but
                // allows for a situation where the state-machine may enter
                // Renewing (and send a Renew message) then immedaitely
                // transition to Rebinding (and send a Rebind message with a
                // new transaction ID). In this situation, the server will
                // handle the Renew message and send a Reply but this client
                // would be likely to drop that message as the client would
                // have almost immediately transitioned to the Rebinding state
                // (at which point the transaction ID would have changed).
                if t1 == t2 {
                    Action::CancelTimer(ClientTimerType::Renew)
                } else if t1 < t2 {
                    assert_matches!(
                        t1,
                        v6::NonZeroTimeValue::Finite(t1_val) => Action::ScheduleTimer(
                            ClientTimerType::Renew,
                            now.add(Duration::from_secs(t1_val.get().into())),
                        ),
                        "must be Finite since Infinity is the largest possible value so if T1 \
                         is Infinity, T2 must also be Infinity as T1 must always be less than \
                         or equal to T2 in which case we would have not reached this point"
                    )
                } else {
                    unreachable!("should have rejected T1={:?} > T2={:?}", t1, t2);
                },
                // Per RFC 8415 section 18.2.5, set timer to enter rebind state:
                //
                //   At time T2 (which will only be reached if the server to
                //   which the Renew message was sent starting at time T1 has
                //   not responded), the client initiates a Rebind/Reply message
                //   exchange with any available server.
                //
                // Per RFC 8415 section 7.7, do not enter the Rebind state if
                // T2 is infinity:
                //
                //   A client will never attempt to use a Rebind message to
                //   locate a different server to extend the lifetimes of any
                //   addresses in an IA with T2 set to 0xffffffff.
                match t2 {
                    v6::NonZeroTimeValue::Finite(t2_val) => Action::ScheduleTimer(
                        ClientTimerType::Rebind,
                        now.add(Duration::from_secs(t2_val.get().into())),
                    ),
                    v6::NonZeroTimeValue::Infinity => Action::CancelTimer(ClientTimerType::Rebind),
                },
            ]
            .into_iter()
            .chain(dns_servers.clone().map(Action::UpdateDnsServers)),
        ),
        StateAfterReplyWithLeases::RequestNextServer
        | StateAfterReplyWithLeases::StayRenewingRebinding
        | StateAfterReplyWithLeases::Requesting => None,
    }
    .into_iter()
    .flatten()
    .chain((!ia_na_updates.is_empty()).then(|| Action::IaNaUpdates(ia_na_updates)))
    .chain((!ia_pd_updates.is_empty()).then(|| Action::IaPdUpdates(ia_pd_updates)))
    .chain(all_ias_invalidates_at.into_iter().map(|all_ias_invalidates_at| {
        match all_ias_invalidates_at {
            AllIasInvalidatesAt::At(instant) => {
                Action::ScheduleTimer(ClientTimerType::RestartServerDiscovery, instant)
            }
            AllIasInvalidatesAt::Never => {
                Action::CancelTimer(ClientTimerType::RestartServerDiscovery)
            }
        }
    }))
    .collect();

    Ok(ProcessedReplyWithLeases {
        server_id: got_server_id,
        non_temporary_addresses,
        delegated_prefixes,
        dns_servers,
        actions,
        next_state,
    })
}

/// Create a map of IA entries to be requested, combining the IAs in the
/// Advertise with the configured IAs that are not included in the Advertise.
fn advertise_to_ia_entries<V: IaValue, I>(
    mut advertised: HashMap<v6::IAID, HashSet<V>>,
    configured: HashMap<v6::IAID, HashSet<V>>,
) -> HashMap<v6::IAID, IaEntry<V, I>> {
    configured
        .into_iter()
        .map(|(iaid, configured)| {
            let addresses_to_request = match advertised.remove(&iaid) {
                Some(ias) => {
                    // Note that the advertised address/prefix for an IAID may
                    // be different from what was solicited by the client.
                    ias
                }
                // The configured address/prefix was not advertised; the client
                // will continue to request it in subsequent messages, per
                // RFC 8415 section 18.2:
                //
                //    When possible, the client SHOULD use the best
                //    configuration available and continue to request the
                //    additional IAs in subsequent messages.
                None => configured,
            };
            (iaid, IaEntry::ToRequest(addresses_to_request))
        })
        .collect()
}

impl<I: Instant> Requesting<I> {
    /// Starts in requesting state following [RFC 8415, Section 18.2.2].
    ///
    /// [RFC 8415, Section 18.2.2]: https://tools.ietf.org/html/rfc8415#section-18.2.2
    fn start<R: Rng>(
        client_id: [u8; CLIENT_ID_LEN],
        server_id: Vec<u8>,
        non_temporary_addresses: HashMap<v6::IAID, AddressEntry<I>>,
        delegated_prefixes: HashMap<v6::IAID, PrefixEntry<I>>,
        options_to_request: &[v6::OptionCode],
        collected_advertise: BinaryHeap<AdvertiseMessage<I>>,
        solicit_max_rt: Duration,
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        Self {
            client_id,
            non_temporary_addresses,
            delegated_prefixes,
            server_id,
            collected_advertise,
            first_request_time: now,
            retrans_timeout: Duration::default(),
            transmission_count: 0,
            solicit_max_rt,
        }
        .send_and_reschedule_retransmission(
            transaction_id(),
            options_to_request,
            rng,
            now,
            std::iter::empty(),
        )
    }

    /// Calculates timeout for retransmitting requests using parameters
    /// specified in [RFC 8415, Section 18.2.2].
    ///
    /// [RFC 8415, Section 18.2.2]: https://tools.ietf.org/html/rfc8415#section-18.2.2
    fn retransmission_timeout<R: Rng>(prev_retrans_timeout: Duration, rng: &mut R) -> Duration {
        retransmission_timeout(
            prev_retrans_timeout,
            INITIAL_REQUEST_TIMEOUT,
            MAX_REQUEST_TIMEOUT,
            rng,
        )
    }

    /// A helper function that returns a transition to stay in `Requesting`, with
    /// actions to cancel current retransmission timer, send a request and
    /// schedules retransmission.
    fn send_and_reschedule_retransmission<R: Rng>(
        self,
        transaction_id: [u8; 3],
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
        initial_actions: impl Iterator<Item = Action<I>>,
    ) -> Transition<I> {
        let Transition { state, actions: request_actions, transaction_id } = self
            .send_and_schedule_retransmission(
                transaction_id,
                options_to_request,
                rng,
                now,
                initial_actions,
            );
        let actions = std::iter::once(Action::CancelTimer(ClientTimerType::Retransmission))
            .chain(request_actions.into_iter())
            .collect();
        Transition { state, actions, transaction_id }
    }

    /// A helper function that returns a transition to stay in `Requesting`, with
    /// actions to send a request and schedules retransmission.
    ///
    /// # Panics
    ///
    /// Panics if `options_to_request` contains SOLICIT_MAX_RT.
    fn send_and_schedule_retransmission<R: Rng>(
        self,
        transaction_id: [u8; 3],
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
        initial_actions: impl Iterator<Item = Action<I>>,
    ) -> Transition<I> {
        let Self {
            client_id,
            server_id,
            non_temporary_addresses,
            delegated_prefixes,
            collected_advertise,
            first_request_time,
            retrans_timeout: prev_retrans_timeout,
            transmission_count,
            solicit_max_rt,
        } = self;
        let retrans_timeout = Self::retransmission_timeout(prev_retrans_timeout, rng);
        let elapsed_time = elapsed_time_in_centisecs(first_request_time, now);

        // Per RFC 8415, section 18.2.2:
        //
        //   The client uses a Request message to populate IAs with leases and
        //   obtain other configuration information. The client includes one or
        //   more IA options in the Request message. The server then returns
        //   leases and other information about the IAs to the client in IA
        //   options in a Reply message.
        //
        //   The client sets the "msg-type" field to REQUEST. The client
        //   generates a transaction ID and inserts this value in the
        //   "transaction-id" field.
        //
        //   The client MUST include the identifier of the destination server in
        //   a Server Identifier option (see Section 21.3).
        //
        //   The client MUST include a Client Identifier option (see Section
        //   21.2) to identify itself to the server. The client adds any other
        //   appropriate options, including one or more IA options.
        //
        //   The client MUST include an Elapsed Time option (see Section 21.9)
        //   to indicate how long the client has been trying to complete the
        //   current DHCP message exchange.
        //
        //   The client MUST include an Option Request option (see Section 21.7)
        //   to request the SOL_MAX_RT option (see Section 21.24) and any other
        //   options the client is interested in receiving. The client MAY
        //   additionally include instances of those options that are identified
        //   in the Option Request option, with data values as hints to the
        //   server about parameter values the client would like to have
        //   returned.
        let buf = StatefulMessageBuilder {
            transaction_id,
            message_type: v6::MessageType::Request,
            server_id: Some(&server_id),
            client_id: &client_id,
            elapsed_time_in_centisecs: elapsed_time,
            options_to_request,
            ia_nas: non_temporary_addresses.iter().map(|(iaid, ia)| (*iaid, ia.value())),
            ia_pds: delegated_prefixes.iter().map(|(iaid, ia)| (*iaid, ia.value())),
            _marker: Default::default(),
        }
        .build();

        Transition {
            state: ClientState::Requesting(Requesting {
                client_id,
                non_temporary_addresses,
                delegated_prefixes,
                server_id,
                collected_advertise,
                first_request_time,
                retrans_timeout,
                transmission_count: transmission_count + 1,
                solicit_max_rt,
            }),
            actions: initial_actions
                .chain([
                    Action::SendMessage(buf),
                    Action::ScheduleTimer(
                        ClientTimerType::Retransmission,
                        now.add(retrans_timeout),
                    ),
                ])
                .collect(),
            transaction_id: Some(transaction_id),
        }
    }

    /// Retransmits request. Per RFC 8415, section 18.2.2:
    ///
    ///    The client transmits the message according to Section 15, using the
    ///    following parameters:
    ///
    ///       IRT     REQ_TIMEOUT
    ///       MRT     REQ_MAX_RT
    ///       MRC     REQ_MAX_RC
    ///       MRD     0
    ///
    /// Per RFC 8415, section 15:
    ///
    ///    MRC specifies an upper bound on the number of times a client may
    ///    retransmit a message.  Unless MRC is zero, the message exchange fails
    ///    once the client has transmitted the message MRC times.
    ///
    /// Per RFC 8415, section 18.2.2:
    ///
    ///    If the message exchange fails, the client takes an action based on
    ///    the client's local policy.  Examples of actions the client might take
    ///    include the following:
    ///    -  Select another server from a list of servers known to the client
    ///       -- for example, servers that responded with an Advertise message.
    ///    -  Initiate the server discovery process described in Section 18.
    ///    -  Terminate the configuration process and report failure.
    ///
    /// The client's policy on message exchange failure is to select another
    /// server; if there are no  more servers available, restart server
    /// discovery.
    /// TODO(https://fxbug.dev/88117): make the client policy configurable.
    fn retransmission_timer_expired<R: Rng>(
        self,
        request_transaction_id: [u8; 3],
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        let Self {
            client_id: _,
            non_temporary_addresses: _,
            delegated_prefixes: _,
            server_id: _,
            collected_advertise: _,
            first_request_time: _,
            retrans_timeout: _,
            transmission_count,
            solicit_max_rt: _,
        } = &self;
        if *transmission_count > REQUEST_MAX_RC {
            self.request_from_alternate_server_or_restart_server_discovery(
                options_to_request,
                rng,
                now,
            )
        } else {
            self.send_and_schedule_retransmission(
                request_transaction_id,
                options_to_request,
                rng,
                now,
                std::iter::empty(),
            )
        }
    }

    fn reply_message_received<R: Rng, B: ByteSlice>(
        self,
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        msg: v6::Message<'_, B>,
        now: I,
    ) -> Transition<I> {
        let Self {
            client_id,
            non_temporary_addresses: mut current_non_temporary_addresses,
            delegated_prefixes: mut current_delegated_prefixes,
            server_id,
            collected_advertise,
            first_request_time,
            retrans_timeout,
            transmission_count,
            mut solicit_max_rt,
        } = self;
        let ProcessedReplyWithLeases {
            server_id: got_server_id,
            non_temporary_addresses,
            delegated_prefixes,
            dns_servers,
            actions,
            next_state,
        } = match process_reply_with_leases(
            client_id,
            &server_id,
            &current_non_temporary_addresses,
            &current_delegated_prefixes,
            &mut solicit_max_rt,
            &msg,
            RequestLeasesMessageType::Request,
            now,
        ) {
            Ok(processed) => processed,
            Err(e) => {
                match e {
                    ReplyWithLeasesError::ErrorStatusCode(ErrorStatusCode(error_code, message)) => {
                        match error_code {
                            v6::ErrorStatusCode::NotOnLink => {
                                // Per RFC 8415, section 18.2.10.1:
                                //
                                //    If the client receives a NotOnLink status from the server
                                //    in response to a Solicit (with a Rapid Commit option;
                                //    see Section 21.14) or a Request, the client can either
                                //    reissue the message without specifying any addresses or
                                //    restart the DHCP server discovery process (see Section 18).
                                //
                                // The client reissues the message without specifying addresses,
                                // leaving it up to the server to assign addresses appropriate
                                // for the client's link.

                                fn get_updates_and_reset_to_empty_request<V: IaValue, I>(
                                    current: &mut HashMap<v6::IAID, IaEntry<V, I>>,
                                ) -> HashMap<v6::IAID, HashMap<V, IaValueUpdateKind>>
                                {
                                    let mut updates = HashMap::new();
                                    current.iter_mut().for_each(|(iaid, entry)| {
                                        // Discard all currently-assigned values.
                                        match entry {
                                            IaEntry::Assigned(values) => {
                                                assert_matches!(
                                                    updates.insert(
                                                        *iaid,
                                                        values
                                                            .keys()
                                                            .cloned()
                                                            .map(|value| (
                                                                value,
                                                                IaValueUpdateKind::Removed
                                                            ),)
                                                            .collect()
                                                    ),
                                                    None
                                                );
                                            }
                                            IaEntry::ToRequest(_) => {}
                                        };

                                        *entry = IaEntry::ToRequest(Default::default());
                                    });

                                    updates
                                }

                                let ia_na_updates = get_updates_and_reset_to_empty_request(
                                    &mut current_non_temporary_addresses,
                                );
                                let ia_pd_updates = get_updates_and_reset_to_empty_request(
                                    &mut current_delegated_prefixes,
                                );

                                let initial_actions = (!ia_na_updates.is_empty())
                                    .then(|| Action::IaNaUpdates(ia_na_updates))
                                    .into_iter()
                                    .chain(
                                        (!ia_pd_updates.is_empty())
                                            .then(|| Action::IaPdUpdates(ia_pd_updates)),
                                    );

                                warn!(
                                    "Reply to Request: retrying Request without hints due to \
                                    NotOnLink error status code with message '{}'",
                                    message,
                                );
                                return Requesting {
                                    client_id,
                                    non_temporary_addresses: current_non_temporary_addresses,
                                    delegated_prefixes: current_delegated_prefixes,
                                    server_id,
                                    collected_advertise,
                                    first_request_time,
                                    retrans_timeout,
                                    transmission_count,
                                    solicit_max_rt,
                                }
                                .send_and_reschedule_retransmission(
                                    *msg.transaction_id(),
                                    options_to_request,
                                    rng,
                                    now,
                                    initial_actions,
                                );
                            }
                            // Per RFC 8415, section 18.2.10:
                            //
                            //    If the client receives a Reply message with a status code
                            //    of UnspecFail, the server is indicating that it was unable
                            //    to process the client's message due to an unspecified
                            //    failure condition.  If the client retransmits the original
                            //    message to the same server to retry the desired operation,
                            //    the client MUST limit the rate at which it retransmits
                            //    the message and limit the duration of the time during
                            //    which it retransmits the message (see Section 14.1).
                            //
                            // Ignore this Reply and rely on timeout for retransmission.
                            // TODO(https://fxbug.dev/81086): implement rate limiting.
                            v6::ErrorStatusCode::UnspecFail => {
                                warn!(
                                    "ignoring Reply to Request: ignoring due to UnspecFail error
                                    status code with message '{}'",
                                    message,
                                );
                            }
                            // TODO(https://fxbug.dev/76764): implement unicast.
                            // The client already uses multicast.
                            v6::ErrorStatusCode::UseMulticast => {
                                warn!(
                                    "ignoring Reply to Request: ignoring due to UseMulticast \
                                        with message '{}', but Request was already using multicast",
                                    message,
                                );
                            }
                            // Not expected as top level status.
                            v6::ErrorStatusCode::NoAddrsAvail
                            | v6::ErrorStatusCode::NoPrefixAvail
                            | v6::ErrorStatusCode::NoBinding => {
                                warn!(
                                    "ignoring Reply to Request due to unexpected top level error
                                    {:?} with message '{}'",
                                    error_code, message,
                                );
                            }
                        }
                        return Transition {
                            state: ClientState::Requesting(Self {
                                client_id,
                                non_temporary_addresses: current_non_temporary_addresses,
                                delegated_prefixes: current_delegated_prefixes,
                                server_id,
                                collected_advertise,
                                first_request_time,
                                retrans_timeout,
                                transmission_count,
                                solicit_max_rt,
                            }),
                            actions: Vec::new(),
                            transaction_id: None,
                        };
                    }
                    _ => {}
                }
                warn!("ignoring Reply to Request: {:?}", e);
                return Transition {
                    state: ClientState::Requesting(Self {
                        client_id,
                        non_temporary_addresses: current_non_temporary_addresses,
                        delegated_prefixes: current_delegated_prefixes,
                        server_id,
                        collected_advertise,
                        first_request_time,
                        retrans_timeout,
                        transmission_count,
                        solicit_max_rt,
                    }),
                    actions: Vec::new(),
                    transaction_id: None,
                };
            }
        };
        assert_eq!(
            server_id, got_server_id,
            "should be invalid to accept a reply to Request with mismatched server ID"
        );

        match next_state {
            StateAfterReplyWithLeases::StayRenewingRebinding => {
                unreachable!("cannot stay in Renewing/Rebinding state while in Requesting state");
            }
            StateAfterReplyWithLeases::Requesting => {
                unreachable!(
                    "cannot go back to Requesting from Requesting \
                    (only possible from Renewing/Rebinding)"
                );
            }
            StateAfterReplyWithLeases::RequestNextServer => {
                warn!("Reply to Request: trying next server");
                Self {
                    client_id,
                    non_temporary_addresses: current_non_temporary_addresses,
                    delegated_prefixes: current_delegated_prefixes,
                    server_id,
                    collected_advertise,
                    first_request_time,
                    retrans_timeout,
                    transmission_count,
                    solicit_max_rt,
                }
                .request_from_alternate_server_or_restart_server_discovery(
                    options_to_request,
                    rng,
                    now,
                )
            }
            StateAfterReplyWithLeases::Assigned => {
                // Note that we drop the list of collected advertisements when
                // we transition to Assigned to avoid picking servers using
                // stale advertisements if we ever need to pick a new server in
                // the future.
                //
                // Once we transition into the Assigned state, we will not
                // attempt to communicate with a different server for some time
                // before an error occurs that requires the client to pick an
                // alternate server. In this time, the set of advertisements may
                // have gone stale as the server may have assigned advertised
                // IAs to some other client.
                //
                // TODO(https://fxbug.dev/72701) Send AddressWatcher update with
                // assigned addresses.
                Transition {
                    state: ClientState::Assigned(Assigned {
                        client_id,
                        non_temporary_addresses,
                        delegated_prefixes,
                        server_id,
                        dns_servers: dns_servers.unwrap_or(Vec::new()),
                        solicit_max_rt,
                        _marker: Default::default(),
                    }),
                    actions,
                    transaction_id: None,
                }
            }
        }
    }

    fn restart_server_discovery<R: Rng>(
        self,
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        let Self {
            client_id,
            non_temporary_addresses,
            delegated_prefixes,
            server_id: _,
            collected_advertise: _,
            first_request_time: _,
            retrans_timeout: _,
            transmission_count: _,
            solicit_max_rt: _,
        } = self;

        restart_server_discovery(
            client_id,
            non_temporary_addresses,
            delegated_prefixes,
            Vec::new(),
            options_to_request,
            rng,
            now,
        )
    }

    /// Helper function to send a request to an alternate server, or if there are no
    /// other collected servers, restart server discovery.
    ///
    /// The client removes currently assigned addresses, per RFC 8415, section
    /// 18.2.10.1:
    ///
    ///    Whenever a client restarts the DHCP server discovery process or
    ///    selects an alternate server as described in Section 18.2.9, the client
    ///    SHOULD stop using all the addresses and delegated prefixes for which
    ///    it has bindings and try to obtain all required leases from the new
    ///    server.
    fn request_from_alternate_server_or_restart_server_discovery<R: Rng>(
        self,
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        let Self {
            client_id,
            server_id: _,
            non_temporary_addresses,
            delegated_prefixes,
            mut collected_advertise,
            first_request_time: _,
            retrans_timeout: _,
            transmission_count: _,
            solicit_max_rt,
        } = self;

        if let Some(advertise) = collected_advertise.pop() {
            fn to_configured_values<V: IaValue, I: Instant>(
                entries: HashMap<v6::IAID, IaEntry<V, I>>,
            ) -> HashMap<v6::IAID, HashSet<V>> {
                entries
                    .into_iter()
                    .map(|(iaid, entry)| {
                        (
                            iaid,
                            match entry {
                                IaEntry::Assigned(values) => unreachable!(
                                    "should not have advertisements after an IA was assigned; \
                         iaid={:?}, values={:?}",
                                    iaid, values,
                                ),
                                IaEntry::ToRequest(values) => values,
                            },
                        )
                    })
                    .collect()
            }

            let configured_non_temporary_addresses = to_configured_values(non_temporary_addresses);
            let configured_delegated_prefixes = to_configured_values(delegated_prefixes);

            // TODO(https://fxbug.dev/96674): Before selecting a different server,
            // add actions to remove the existing assigned addresses, if any.
            let AdvertiseMessage {
                server_id,
                non_temporary_addresses: advertised_non_temporary_addresses,
                delegated_prefixes: advertised_delegated_prefixes,
                dns_servers: _,
                preference: _,
                receive_time: _,
                preferred_non_temporary_addresses_count: _,
                preferred_delegated_prefixes_count: _,
            } = advertise;
            Requesting::start(
                client_id,
                server_id,
                advertise_to_ia_entries(
                    advertised_non_temporary_addresses,
                    configured_non_temporary_addresses,
                ),
                advertise_to_ia_entries(
                    advertised_delegated_prefixes,
                    configured_delegated_prefixes,
                ),
                options_to_request,
                collected_advertise,
                solicit_max_rt,
                rng,
                now,
            )
        } else {
            restart_server_discovery(
                client_id,
                non_temporary_addresses,
                delegated_prefixes,
                Vec::new(), /* dns_servers */
                options_to_request,
                rng,
                now,
            )
        }
    }
}

#[derive(Copy, Clone, Debug, PartialEq)]
struct LifetimesInfo<I> {
    lifetimes: Lifetimes,
    updated_at: I,
}

#[derive(Debug, PartialEq, Clone)]
enum IaEntry<V: IaValue, I> {
    /// The IA is assigned.
    Assigned(HashMap<V, LifetimesInfo<I>>),
    /// The IA is not assigned, and is to be requested in subsequent
    /// messages.
    ToRequest(HashSet<V>),
}

impl<V: IaValue, I> IaEntry<V, I> {
    fn value(&self) -> impl Iterator<Item = V> + '_ {
        match self {
            Self::Assigned(ias) => either::Either::Left(ias.keys().copied()),
            Self::ToRequest(values) => either::Either::Right(values.iter().copied()),
        }
    }

    fn to_request(&self, without_hints: bool) -> Self {
        Self::ToRequest(if without_hints { Default::default() } else { self.value().collect() })
    }
}

type AddressEntry<I> = IaEntry<Ipv6Addr, I>;
type PrefixEntry<I> = IaEntry<Subnet<Ipv6Addr>, I>;

/// Provides methods for handling state transitions from Assigned state.
#[derive(Debug)]
struct Assigned<I> {
    /// [Client Identifier] used for uniquely identifying the client in
    /// communication with servers.
    ///
    /// [Client Identifier]: https://datatracker.ietf.org/doc/html/rfc8415#section-21.2
    client_id: [u8; CLIENT_ID_LEN],
    /// The non-temporary addresses negotiated by the client.
    non_temporary_addresses: HashMap<v6::IAID, AddressEntry<I>>,
    /// The delegated prefixes negotiated by the client.
    delegated_prefixes: HashMap<v6::IAID, PrefixEntry<I>>,
    /// The [server identifier] of the server to which the client sends
    /// requests.
    ///
    /// [Server Identifier]: https://datatracker.ietf.org/doc/html/rfc8415#section-21.3
    server_id: Vec<u8>,
    /// Stores the DNS servers received from the reply.
    dns_servers: Vec<Ipv6Addr>,
    /// The [SOL_MAX_RT](https://datatracker.ietf.org/doc/html/rfc8415#section-21.24)
    /// used by the client.
    solicit_max_rt: Duration,
    _marker: PhantomData<I>,
}

fn restart_server_discovery<R: Rng, I: Instant>(
    client_id: [u8; CLIENT_ID_LEN],
    non_temporary_addresses: HashMap<v6::IAID, AddressEntry<I>>,
    delegated_prefixes: HashMap<v6::IAID, PrefixEntry<I>>,
    dns_servers: Vec<Ipv6Addr>,
    options_to_request: &[v6::OptionCode],
    rng: &mut R,
    now: I,
) -> Transition<I> {
    #[derive(Derivative)]
    #[derivative(Default(bound = ""))]
    struct ClearValuesResult<V: IaValue> {
        updates: HashMap<v6::IAID, HashMap<V, IaValueUpdateKind>>,
        entries: HashMap<v6::IAID, HashSet<V>>,
    }

    fn clear_values<V: IaValue, I: Instant>(
        values: HashMap<v6::IAID, IaEntry<V, I>>,
    ) -> ClearValuesResult<V> {
        values.into_iter().fold(
            ClearValuesResult::default(),
            |ClearValuesResult { mut updates, mut entries }, (iaid, entry)| {
                match entry {
                    IaEntry::Assigned(values) => {
                        assert_matches!(
                            updates.insert(
                                iaid,
                                values
                                    .keys()
                                    .copied()
                                    .map(|value| (value, IaValueUpdateKind::Removed))
                                    .collect()
                            ),
                            None
                        );

                        assert_matches!(entries.insert(iaid, values.into_keys().collect()), None);
                    }
                    IaEntry::ToRequest(values) => {
                        assert_matches!(entries.insert(iaid, values), None);
                    }
                }

                ClearValuesResult { updates, entries }
            },
        )
    }

    let ClearValuesResult {
        updates: non_temporary_address_updates,
        entries: non_temporary_address_entries,
    } = clear_values(non_temporary_addresses);

    let ClearValuesResult { updates: delegated_prefix_updates, entries: delegated_prefix_entries } =
        clear_values(delegated_prefixes);

    ServerDiscovery::start(
        transaction_id(),
        client_id,
        non_temporary_address_entries,
        delegated_prefix_entries,
        &options_to_request,
        MAX_SOLICIT_TIMEOUT,
        rng,
        now,
        [
            Action::CancelTimer(ClientTimerType::Retransmission),
            Action::CancelTimer(ClientTimerType::Refresh),
            Action::CancelTimer(ClientTimerType::Renew),
            Action::CancelTimer(ClientTimerType::Rebind),
            Action::CancelTimer(ClientTimerType::RestartServerDiscovery),
        ]
        .into_iter()
        .chain((!dns_servers.is_empty()).then(|| Action::UpdateDnsServers(Vec::new())))
        .chain(
            (!non_temporary_address_updates.is_empty())
                .then(|| Action::IaNaUpdates(non_temporary_address_updates)),
        )
        .chain(
            (!delegated_prefix_updates.is_empty())
                .then(|| Action::IaPdUpdates(delegated_prefix_updates)),
        ),
    )
}

impl<I: Instant> Assigned<I> {
    /// Handles renew timer, following [RFC 8415, Section 18.2.4].
    ///
    /// [RFC 8415, Section 18.2.4]: https://tools.ietf.org/html/rfc8415#section-18.2.4
    fn renew_timer_expired<R: Rng>(
        self,
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        let Self {
            client_id,
            non_temporary_addresses,
            delegated_prefixes,
            server_id,
            dns_servers,
            solicit_max_rt,
            _marker,
        } = self;
        // Start renewing bindings, per RFC 8415, section 18.2.4:
        //
        //    At time T1, the client initiates a Renew/Reply message
        //    exchange to extend the lifetimes on any leases in the IA.
        Renewing::start(
            client_id,
            non_temporary_addresses,
            delegated_prefixes,
            server_id,
            options_to_request,
            dns_servers,
            solicit_max_rt,
            rng,
            now,
        )
    }

    /// Handles rebind timer, following [RFC 8415, Section 18.2.5].
    ///
    /// [RFC 8415, Section 18.2.5]: https://tools.ietf.org/html/rfc8415#section-18.2.5
    fn rebind_timer_expired<R: Rng>(
        self,
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        let Self {
            client_id,
            non_temporary_addresses,
            delegated_prefixes,
            server_id,
            dns_servers,
            solicit_max_rt,
            _marker,
        } = self;
        // Start rebinding bindings, per RFC 8415, section 18.2.5:
        //
        //   At time T2 (which will only be reached if the server to which the
        //   Renew message was sent starting at time T1 has not responded), the
        //   client initiates a Rebind/Reply message exchange with any available
        //   server.
        Rebinding::start(
            client_id,
            non_temporary_addresses,
            delegated_prefixes,
            server_id,
            options_to_request,
            dns_servers,
            solicit_max_rt,
            rng,
            now,
        )
    }

    fn restart_server_discovery<R: Rng>(
        self,
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        let Self {
            client_id,
            non_temporary_addresses,
            delegated_prefixes,
            server_id: _,
            dns_servers,
            solicit_max_rt: _,
            _marker,
        } = self;

        restart_server_discovery(
            client_id,
            non_temporary_addresses,
            delegated_prefixes,
            dns_servers,
            options_to_request,
            rng,
            now,
        )
    }
}

type Renewing<I> = RenewingOrRebinding<I, false /* IS_REBINDING */>;
type Rebinding<I> = RenewingOrRebinding<I, true /* IS_REBINDING */>;

impl<I: Instant> Renewing<I> {
    /// Handles rebind timer, following [RFC 8415, Section 18.2.5].
    ///
    /// [RFC 8415, Section 18.2.5]: https://tools.ietf.org/html/rfc8415#section-18.2.4
    fn rebind_timer_expired<R: Rng>(
        self,
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        let Self(RenewingOrRebindingInner {
            client_id,
            non_temporary_addresses,
            delegated_prefixes,
            server_id,
            dns_servers,
            start_time: _,
            retrans_timeout: _,
            solicit_max_rt,
        }) = self;

        // Start rebinding, per RFC 8415, section 18.2.5:
        //
        //   At time T2 (which will only be reached if the server to which the
        //   Renew message was sent starting at time T1 has not responded), the
        //   client initiates a Rebind/Reply message exchange with any available
        //   server.
        Rebinding::start(
            client_id,
            non_temporary_addresses,
            delegated_prefixes,
            server_id,
            options_to_request,
            dns_servers,
            solicit_max_rt,
            rng,
            now,
        )
    }
}

#[derive(Debug)]
#[cfg_attr(test, derive(Clone))]
struct RenewingOrRebindingInner<I> {
    /// [Client Identifier](https://datatracker.ietf.org/doc/html/rfc8415#section-21.2)
    /// used for uniquely identifying the client in communication with servers.
    client_id: [u8; CLIENT_ID_LEN],
    /// The non-temporary addresses negotiated by the client.
    non_temporary_addresses: HashMap<v6::IAID, AddressEntry<I>>,
    /// The delegated prefixes negotiated by the client.
    delegated_prefixes: HashMap<v6::IAID, PrefixEntry<I>>,
    /// [Server Identifier](https://datatracker.ietf.org/doc/html/rfc8415#section-21.2)
    /// of the server selected during server discovery.
    server_id: Vec<u8>,
    /// Stores the DNS servers received from the reply.
    dns_servers: Vec<Ipv6Addr>,
    /// The time of the first renew/rebind. Used in calculating the
    /// [elapsed time].
    ///
    /// [elapsed time](https://datatracker.ietf.org/doc/html/rfc8415#section-21.9).
    start_time: I,
    /// The renew/rebind message retransmission timeout.
    retrans_timeout: Duration,
    /// The [SOL_MAX_RT](https://datatracker.ietf.org/doc/html/rfc8415#section-21.24)
    /// used by the client.
    solicit_max_rt: Duration,
}

impl<I, const IS_REBINDING: bool> From<RenewingOrRebindingInner<I>>
    for RenewingOrRebinding<I, IS_REBINDING>
{
    fn from(inner: RenewingOrRebindingInner<I>) -> Self {
        Self(inner)
    }
}

// TODO(https://github.com/rust-lang/rust/issues/76560): Use an enum for the
// constant generic instead of a boolean for readability.
#[derive(Debug)]
struct RenewingOrRebinding<I, const IS_REBINDING: bool>(RenewingOrRebindingInner<I>);

impl<I: Instant, const IS_REBINDING: bool> RenewingOrRebinding<I, IS_REBINDING> {
    /// Starts renewing or rebinding, following [RFC 8415, Section 18.2.4] or
    /// [RFC 8415, Section 18.2.5], respectively.
    ///
    /// [RFC 8415, Section 18.2.4]: https://tools.ietf.org/html/rfc8415#section-18.2.4
    /// [RFC 8415, Section 18.2.5]: https://tools.ietf.org/html/rfc8415#section-18.2.5
    fn start<R: Rng>(
        client_id: [u8; CLIENT_ID_LEN],
        non_temporary_addresses: HashMap<v6::IAID, AddressEntry<I>>,
        delegated_prefixes: HashMap<v6::IAID, PrefixEntry<I>>,
        server_id: Vec<u8>,
        options_to_request: &[v6::OptionCode],
        dns_servers: Vec<Ipv6Addr>,
        solicit_max_rt: Duration,
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        Self(RenewingOrRebindingInner {
            client_id,
            non_temporary_addresses,
            delegated_prefixes,
            server_id,
            dns_servers,
            start_time: now,
            retrans_timeout: Duration::default(),
            solicit_max_rt,
        })
        .send_and_schedule_retransmission(transaction_id(), options_to_request, rng, now)
    }

    /// Calculates timeout for retransmitting Renew/Rebind using parameters
    /// specified in [RFC 8415, Section 18.2.4]/[RFC 8415, Section 18.2.5].
    ///
    /// [RFC 8415, Section 18.2.4]: https://tools.ietf.org/html/rfc8415#section-18.2.4
    /// [RFC 8415, Section 18.2.5]: https://tools.ietf.org/html/rfc8415#section-18.2.5
    fn retransmission_timeout<R: Rng>(prev_retrans_timeout: Duration, rng: &mut R) -> Duration {
        let (initial, max) = if IS_REBINDING {
            (INITIAL_REBIND_TIMEOUT, MAX_REBIND_TIMEOUT)
        } else {
            (INITIAL_RENEW_TIMEOUT, MAX_RENEW_TIMEOUT)
        };

        retransmission_timeout(prev_retrans_timeout, initial, max, rng)
    }

    /// Returns a transition to stay in the current state, with actions to send
    /// a message and schedule retransmission.
    fn send_and_schedule_retransmission<R: Rng>(
        self,
        transaction_id: [u8; 3],
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        let Self(RenewingOrRebindingInner {
            client_id,
            non_temporary_addresses,
            delegated_prefixes,
            server_id,
            dns_servers,
            start_time,
            retrans_timeout: prev_retrans_timeout,
            solicit_max_rt,
        }) = self;
        let elapsed_time = elapsed_time_in_centisecs(start_time, now);

        // As per RFC 8415 section 18.2.4,
        //
        //   The client sets the "msg-type" field to RENEW. The client generates
        //   a transaction ID and inserts this value in the "transaction-id"
        //   field.
        //
        //   The client MUST include a Server Identifier option (see Section
        //   21.3) in the Renew message, identifying the server with which the
        //   client most recently communicated.
        //
        //   The client MUST include a Client Identifier option (see Section
        //   21.2) to identify itself to the server. The client adds any
        //   appropriate options, including one or more IA options.
        //
        //   The client MUST include an Elapsed Time option (see Section 21.9)
        //   to indicate how long the client has been trying to complete the
        //   current DHCP message exchange.
        //
        //   For IAs to which leases have been assigned, the client includes a
        //   corresponding IA option containing an IA Address option for each
        //   address assigned to the IA and an IA Prefix option for each prefix
        //   assigned to the IA. The client MUST NOT include addresses and
        //   prefixes in any IA option that the client did not obtain from the
        //   server or that are no longer valid (that have a valid lifetime of
        //   0).
        //
        //   The client MAY include an IA option for each binding it desires but
        //   has been unable to obtain. In this case, if the client includes the
        //   IA_PD option to request prefix delegation, the client MAY include
        //   the IA Prefix option encapsulated within the IA_PD option, with the
        //   "IPv6-prefix" field set to 0 and the "prefix-length" field set to
        //   the desired length of the prefix to be delegated. The server MAY
        //   use this value as a hint for the prefix length. The client SHOULD
        //   NOT include an IA Prefix option with the "IPv6-prefix" field set to
        //   0 unless it is supplying a hint for the prefix length.
        //
        //   The client includes an Option Request option (see Section 21.7) to
        //   request the SOL_MAX_RT option (see Section 21.24) and any other
        //   options the client is interested in receiving. The client MAY
        //   include options with data values as hints to the server about
        //   parameter values the client would like to have returned.
        //
        // As per RFC 8415 section 18.2.5,
        //
        //   The client constructs the Rebind message as described in Section
        //   18.2.4, with the following differences:
        //
        //   -  The client sets the "msg-type" field to REBIND.
        //
        //   -  The client does not include the Server Identifier option (see
        //      Section 21.3) in the Rebind message.
        let (message_type, maybe_server_id) = if IS_REBINDING {
            (v6::MessageType::Rebind, None)
        } else {
            (v6::MessageType::Renew, Some(server_id.as_slice()))
        };

        let buf = StatefulMessageBuilder {
            transaction_id,
            message_type,
            client_id: &client_id,
            server_id: maybe_server_id,
            elapsed_time_in_centisecs: elapsed_time,
            options_to_request,
            ia_nas: non_temporary_addresses.iter().map(|(iaid, ia)| (*iaid, ia.value())),
            ia_pds: delegated_prefixes.iter().map(|(iaid, ia)| (*iaid, ia.value())),
            _marker: Default::default(),
        }
        .build();

        let retrans_timeout = Self::retransmission_timeout(prev_retrans_timeout, rng);

        Transition {
            state: {
                let inner = RenewingOrRebindingInner {
                    client_id,
                    non_temporary_addresses,
                    delegated_prefixes,
                    server_id,
                    dns_servers,
                    start_time,
                    retrans_timeout,
                    solicit_max_rt,
                };

                if IS_REBINDING {
                    ClientState::Rebinding(inner.into())
                } else {
                    ClientState::Renewing(inner.into())
                }
            },
            actions: vec![
                Action::SendMessage(buf),
                Action::ScheduleTimer(ClientTimerType::Retransmission, now.add(retrans_timeout)),
            ],
            transaction_id: Some(transaction_id),
        }
    }

    /// Retransmits the renew or rebind message.
    fn retransmission_timer_expired<R: Rng>(
        self,
        transaction_id: [u8; 3],
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        self.send_and_schedule_retransmission(transaction_id, options_to_request, rng, now)
    }

    fn reply_message_received<R: Rng, B: ByteSlice>(
        self,
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        msg: v6::Message<'_, B>,
        now: I,
    ) -> Transition<I> {
        let Self(RenewingOrRebindingInner {
            client_id,
            non_temporary_addresses: current_non_temporary_addresses,
            delegated_prefixes: current_delegated_prefixes,
            server_id,
            dns_servers: current_dns_servers,
            start_time,
            retrans_timeout,
            mut solicit_max_rt,
        }) = self;
        let ProcessedReplyWithLeases {
            server_id: got_server_id,
            non_temporary_addresses,
            delegated_prefixes,
            dns_servers,
            actions,
            next_state,
        } = match process_reply_with_leases(
            client_id,
            &server_id,
            &current_non_temporary_addresses,
            &current_delegated_prefixes,
            &mut solicit_max_rt,
            &msg,
            if IS_REBINDING {
                RequestLeasesMessageType::Rebind
            } else {
                RequestLeasesMessageType::Renew
            },
            now,
        ) {
            Ok(processed) => processed,
            Err(e) => {
                match e {
                    // Per RFC 8415, section 18.2.10:
                    //
                    //    If the client receives a Reply message with a status code of
                    //    UnspecFail, the server is indicating that it was unable to process
                    //    the client's message due to an unspecified failure condition.  If
                    //    the client retransmits the original message to the same server to
                    //    retry the desired operation, the client MUST limit the rate at
                    //    which it retransmits the message and limit the duration of the
                    //    time during which it retransmits the message (see Section 14.1).
                    //
                    // TODO(https://fxbug.dev/81086): implement rate limiting. Without
                    // rate limiting support, the client relies on the regular
                    // retransmission mechanism to rate limit retransmission.
                    // Similarly, for other status codes indicating failure that are not
                    // expected in Reply to Renew, the client behaves as if the Reply
                    // message had not been received. Note the RFC does not specify what
                    // to do in this case; the client ignores the Reply in order to
                    // preserve existing bindings.
                    ReplyWithLeasesError::ErrorStatusCode(ErrorStatusCode(
                        v6::ErrorStatusCode::UnspecFail,
                        message,
                    )) => {
                        warn!(
                            "ignoring Reply to Renew with status code UnspecFail \
                                and message '{}'",
                            message
                        );
                    }
                    ReplyWithLeasesError::ErrorStatusCode(ErrorStatusCode(
                        v6::ErrorStatusCode::UseMulticast,
                        message,
                    )) => {
                        // TODO(https://fxbug.dev/76764): Implement unicast.
                        warn!(
                            "ignoring Reply to Renew with status code UseMulticast \
                                and message '{}' as Reply was already sent as multicast",
                            message
                        );
                    }
                    ReplyWithLeasesError::ErrorStatusCode(ErrorStatusCode(
                        error_code @ (v6::ErrorStatusCode::NoAddrsAvail
                        | v6::ErrorStatusCode::NoBinding
                        | v6::ErrorStatusCode::NotOnLink
                        | v6::ErrorStatusCode::NoPrefixAvail),
                        message,
                    )) => {
                        warn!(
                            "ignoring Reply to Renew with unexpected status code {:?} \
                                and message '{}'",
                            error_code, message
                        );
                    }
                    e @ (ReplyWithLeasesError::OptionsError(_)
                    | ReplyWithLeasesError::MismatchedServerId { got: _, want: _ }) => {
                        warn!("ignoring Reply to Renew: {}", e);
                    }
                }

                return Transition {
                    state: {
                        let inner = RenewingOrRebindingInner {
                            client_id,
                            non_temporary_addresses: current_non_temporary_addresses,
                            delegated_prefixes: current_delegated_prefixes,
                            server_id,
                            dns_servers: current_dns_servers,
                            start_time,
                            retrans_timeout,
                            solicit_max_rt,
                        };

                        if IS_REBINDING {
                            ClientState::Rebinding(inner.into())
                        } else {
                            ClientState::Renewing(inner.into())
                        }
                    },
                    actions: Vec::new(),
                    transaction_id: None,
                };
            }
        };
        if !IS_REBINDING {
            assert_eq!(
                server_id, got_server_id,
                "should be invalid to accept a reply to Renew with mismatched server ID"
            );
        } else if server_id != got_server_id {
            warn!(
                "using Reply to Rebind message from different server; current={:?}, new={:?}",
                server_id, got_server_id
            );
        }
        let server_id = got_server_id;

        match next_state {
            // We need to restart server discovery to pick the next server when
            // we are in the Renewing/Rebinding state. Unlike Requesting (which
            // holds collected advertisements obtained during Server Discovery),
            // we do not know about any other servers. Note that all collected
            // advertisements are dropped when we transition from Requesting to
            // Assigned.
            StateAfterReplyWithLeases::RequestNextServer => restart_server_discovery(
                client_id,
                current_non_temporary_addresses,
                current_delegated_prefixes,
                current_dns_servers,
                &options_to_request,
                rng,
                now,
            ),
            StateAfterReplyWithLeases::StayRenewingRebinding => Transition {
                state: {
                    let inner = RenewingOrRebindingInner {
                        client_id,
                        non_temporary_addresses,
                        delegated_prefixes,
                        server_id,
                        dns_servers: dns_servers.unwrap_or_else(|| Vec::new()),
                        start_time,
                        retrans_timeout,
                        solicit_max_rt,
                    };

                    if IS_REBINDING {
                        ClientState::Rebinding(inner.into())
                    } else {
                        ClientState::Renewing(inner.into())
                    }
                },
                actions: Vec::new(),
                transaction_id: None,
            },
            StateAfterReplyWithLeases::Assigned => {
                // TODO(https://fxbug.dev/72701) Send AddressWatcher update with
                // assigned addresses.
                Transition {
                    state: ClientState::Assigned(Assigned {
                        client_id,
                        non_temporary_addresses,
                        delegated_prefixes,
                        server_id,
                        dns_servers: dns_servers.unwrap_or_else(|| Vec::new()),
                        solicit_max_rt,
                        _marker: Default::default(),
                    }),
                    actions,
                    transaction_id: None,
                }
            }
            StateAfterReplyWithLeases::Requesting => Requesting::start(
                client_id,
                server_id,
                non_temporary_addresses,
                delegated_prefixes,
                &options_to_request,
                Default::default(), /* collected_advertise */
                solicit_max_rt,
                rng,
                now,
            ),
        }
    }

    fn restart_server_discovery<R: Rng>(
        self,
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        let Self(RenewingOrRebindingInner {
            client_id,
            non_temporary_addresses,
            delegated_prefixes,
            server_id: _,
            dns_servers,
            start_time: _,
            retrans_timeout: _,
            solicit_max_rt: _,
        }) = self;

        restart_server_discovery(
            client_id,
            non_temporary_addresses,
            delegated_prefixes,
            dns_servers,
            options_to_request,
            rng,
            now,
        )
    }
}

/// All possible states of a DHCPv6 client.
///
/// States not found in this enum are not supported yet.
#[derive(Debug)]
enum ClientState<I> {
    /// Creating and (re)transmitting an information request, and waiting for
    /// a reply.
    InformationRequesting(InformationRequesting<I>),
    /// Client is waiting to refresh, after receiving a valid reply to a
    /// previous information request.
    InformationReceived(InformationReceived<I>),
    /// Sending solicit messages, collecting advertise messages, and selecting
    /// a server from which to obtain addresses and other optional
    /// configuration information.
    ServerDiscovery(ServerDiscovery<I>),
    /// Creating and (re)transmitting a request message, and waiting for a
    /// reply.
    Requesting(Requesting<I>),
    /// Client is waiting to renew, after receiving a valid reply to a previous request.
    Assigned(Assigned<I>),
    /// Creating and (re)transmitting a renew message, and awaiting reply.
    Renewing(Renewing<I>),
    /// Creating and (re)transmitting a rebind message, and awaiting reply.
    Rebinding(Rebinding<I>),
}

/// State transition, containing the next state, and the actions the client
/// should take to transition to that state, and the new transaction ID if it
/// has been updated.
struct Transition<I> {
    state: ClientState<I>,
    actions: Actions<I>,
    transaction_id: Option<[u8; 3]>,
}

impl<I: Instant> ClientState<I> {
    /// Handles a received advertise message.
    fn advertise_message_received<R: Rng, B: ByteSlice>(
        self,
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        msg: v6::Message<'_, B>,
        now: I,
    ) -> Transition<I> {
        match self {
            ClientState::ServerDiscovery(s) => {
                s.advertise_message_received(options_to_request, rng, msg, now)
            }
            ClientState::InformationRequesting(_)
            | ClientState::InformationReceived(_)
            | ClientState::Requesting(_)
            | ClientState::Assigned(_)
            | ClientState::Renewing(_)
            | ClientState::Rebinding(_) => {
                Transition { state: self, actions: vec![], transaction_id: None }
            }
        }
    }

    /// Handles a received reply message.
    fn reply_message_received<R: Rng, B: ByteSlice>(
        self,
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        msg: v6::Message<'_, B>,
        now: I,
    ) -> Transition<I> {
        match self {
            ClientState::InformationRequesting(s) => s.reply_message_received(msg, now),
            ClientState::Requesting(s) => {
                s.reply_message_received(options_to_request, rng, msg, now)
            }
            ClientState::Renewing(s) => s.reply_message_received(options_to_request, rng, msg, now),
            ClientState::Rebinding(s) => {
                s.reply_message_received(options_to_request, rng, msg, now)
            }
            ClientState::InformationReceived(_)
            | ClientState::ServerDiscovery(_)
            | ClientState::Assigned(_) => {
                Transition { state: self, actions: vec![], transaction_id: None }
            }
        }
    }

    /// Handles retransmission timeout.
    fn retransmission_timer_expired<R: Rng>(
        self,
        transaction_id: [u8; 3],
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        match self {
            ClientState::InformationRequesting(s) => {
                s.retransmission_timer_expired(transaction_id, options_to_request, rng, now)
            }
            ClientState::ServerDiscovery(s) => {
                s.retransmission_timer_expired(transaction_id, options_to_request, rng, now)
            }
            ClientState::Requesting(s) => {
                s.retransmission_timer_expired(transaction_id, options_to_request, rng, now)
            }
            ClientState::Renewing(s) => {
                s.retransmission_timer_expired(transaction_id, options_to_request, rng, now)
            }
            ClientState::Rebinding(s) => {
                s.retransmission_timer_expired(transaction_id, options_to_request, rng, now)
            }
            ClientState::InformationReceived(_) | ClientState::Assigned(_) => {
                unreachable!("received unexpected retransmission timeout in state {:?}.", self);
            }
        }
    }

    /// Handles refresh timeout.
    fn refresh_timer_expired<R: Rng>(
        self,
        transaction_id: [u8; 3],
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        match self {
            ClientState::InformationReceived(s) => {
                s.refresh_timer_expired(transaction_id, options_to_request, rng, now)
            }
            ClientState::InformationRequesting(_)
            | ClientState::ServerDiscovery(_)
            | ClientState::Requesting(_)
            | ClientState::Assigned(_)
            | ClientState::Renewing(_)
            | ClientState::Rebinding(_) => {
                unreachable!("received unexpected refresh timeout in state {:?}.", self);
            }
        }
    }

    /// Handles renew timeout.
    fn renew_timer_expired<R: Rng>(
        self,
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        match self {
            ClientState::Assigned(s) => s.renew_timer_expired(options_to_request, rng, now),
            ClientState::InformationRequesting(_)
            | ClientState::InformationReceived(_)
            | ClientState::ServerDiscovery(_)
            | ClientState::Requesting(_)
            | ClientState::Renewing(_)
            | ClientState::Rebinding(_) => {
                unreachable!("received unexpected renew timeout in state {:?}.", self);
            }
        }
    }

    /// Handles rebind timeout.
    fn rebind_timer_expired<R: Rng>(
        self,
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        match self {
            ClientState::Assigned(s) => s.rebind_timer_expired(options_to_request, rng, now),
            ClientState::Renewing(s) => s.rebind_timer_expired(options_to_request, rng, now),
            ClientState::InformationRequesting(_)
            | ClientState::InformationReceived(_)
            | ClientState::ServerDiscovery(_)
            | ClientState::Requesting(_)
            | ClientState::Rebinding(_) => {
                unreachable!("received unexpected rebind timeout in state {:?}.", self);
            }
        }
    }

    fn restart_server_discovery<R: Rng>(
        self,
        options_to_request: &[v6::OptionCode],
        rng: &mut R,
        now: I,
    ) -> Transition<I> {
        match self {
            ClientState::Requesting(s) => s.restart_server_discovery(options_to_request, rng, now),
            ClientState::Assigned(s) => s.restart_server_discovery(options_to_request, rng, now),
            ClientState::Renewing(s) => s.restart_server_discovery(options_to_request, rng, now),
            ClientState::Rebinding(s) => s.restart_server_discovery(options_to_request, rng, now),
            ClientState::InformationRequesting(_)
            | ClientState::InformationReceived(_)
            | ClientState::ServerDiscovery(_) => {
                unreachable!("received unexpected rebind timeout in state {:?}.", self);
            }
        }
    }

    /// Returns the DNS servers advertised by the server.
    fn get_dns_servers(&self) -> Vec<Ipv6Addr> {
        match self {
            ClientState::InformationReceived(InformationReceived { dns_servers, _marker }) => {
                dns_servers.clone()
            }
            ClientState::Assigned(Assigned {
                client_id: _,
                non_temporary_addresses: _,
                delegated_prefixes: _,
                server_id: _,
                dns_servers,
                solicit_max_rt: _,
                _marker: _,
            })
            | ClientState::Renewing(RenewingOrRebinding(RenewingOrRebindingInner {
                client_id: _,
                non_temporary_addresses: _,
                delegated_prefixes: _,
                server_id: _,
                dns_servers,
                start_time: _,
                retrans_timeout: _,
                solicit_max_rt: _,
            }))
            | ClientState::Rebinding(RenewingOrRebinding(RenewingOrRebindingInner {
                client_id: _,
                non_temporary_addresses: _,
                delegated_prefixes: _,
                server_id: _,
                dns_servers,
                start_time: _,
                retrans_timeout: _,
                solicit_max_rt: _,
            })) => dns_servers.clone(),
            ClientState::InformationRequesting(InformationRequesting {
                retrans_timeout: _,
                _marker: _,
            })
            | ClientState::ServerDiscovery(ServerDiscovery {
                client_id: _,
                configured_non_temporary_addresses: _,
                configured_delegated_prefixes: _,
                first_solicit_time: _,
                retrans_timeout: _,
                solicit_max_rt: _,
                collected_advertise: _,
                collected_sol_max_rt: _,
            })
            | ClientState::Requesting(Requesting {
                client_id: _,
                non_temporary_addresses: _,
                delegated_prefixes: _,
                server_id: _,
                collected_advertise: _,
                first_request_time: _,
                retrans_timeout: _,
                transmission_count: _,
                solicit_max_rt: _,
            }) => Vec::new(),
        }
    }
}

/// The DHCPv6 core state machine.
///
/// This struct maintains the state machine for a DHCPv6 client, and expects an imperative shell to
/// drive it by taking necessary actions (e.g. send packets, schedule timers, etc.) and dispatch
/// events (e.g. packets received, timer expired, etc.). All the functions provided by this struct
/// are pure-functional. All state transition functions return a list of actions that the
/// imperative shell should take to complete the transition.
#[derive(Debug)]
pub struct ClientStateMachine<I, R: Rng> {
    /// [Transaction ID] the client is using to communicate with servers.
    ///
    /// [Transaction ID]: https://tools.ietf.org/html/rfc8415#section-16.1
    transaction_id: [u8; 3],
    /// Options to include in [Option Request Option].
    /// [Option Request Option]: https://tools.ietf.org/html/rfc8415#section-21.7
    options_to_request: Vec<v6::OptionCode>,
    /// Current state of the client, must not be `None`.
    ///
    /// Using an `Option` here allows the client to consume and replace the state during
    /// transitions.
    state: Option<ClientState<I>>,
    /// Used by the client to generate random numbers.
    rng: R,
}

impl<I: Instant, R: Rng> ClientStateMachine<I, R> {
    /// Starts the client in Stateless mode, as defined in [RFC 8415, Section 6.1].
    /// The client exchanges messages with servers to obtain the configuration
    /// information specified in `options_to_request`.
    ///
    /// [RFC 8415, Section 6.1]: https://tools.ietf.org/html/rfc8415#section-6.1
    pub fn start_stateless(
        transaction_id: [u8; 3],
        options_to_request: Vec<v6::OptionCode>,
        mut rng: R,
        now: I,
    ) -> (Self, Actions<I>) {
        let Transition { state, actions, transaction_id: new_transaction_id } =
            InformationRequesting::start(transaction_id, &options_to_request, &mut rng, now);
        (
            Self {
                state: Some(state),
                transaction_id: new_transaction_id.unwrap_or(transaction_id),
                options_to_request,
                rng,
            },
            actions,
        )
    }

    /// Starts the client in Stateful mode, as defined in [RFC 8415, Section 6.2]
    /// and [RFC 8415, Section 6.3].
    ///
    /// The client exchanges messages with server(s) to obtain non-temporary
    /// addresses in `configured_non_temporary_addresses`, delegated prefixes in
    /// `configured_delegated_prefixes` and the configuration information in
    /// `options_to_request`.
    ///
    /// [RFC 8415, Section 6.2]: https://tools.ietf.org/html/rfc8415#section-6.2
    /// [RFC 8415, Section 6.3]: https://tools.ietf.org/html/rfc8415#section-6.3
    pub fn start_stateful(
        transaction_id: [u8; 3],
        client_id: [u8; CLIENT_ID_LEN],
        configured_non_temporary_addresses: HashMap<v6::IAID, HashSet<Ipv6Addr>>,
        configured_delegated_prefixes: HashMap<v6::IAID, HashSet<Subnet<Ipv6Addr>>>,
        options_to_request: Vec<v6::OptionCode>,
        mut rng: R,
        now: I,
    ) -> (Self, Actions<I>) {
        let Transition { state, actions, transaction_id: new_transaction_id } =
            ServerDiscovery::start(
                transaction_id,
                client_id,
                configured_non_temporary_addresses,
                configured_delegated_prefixes,
                &options_to_request,
                MAX_SOLICIT_TIMEOUT,
                &mut rng,
                now,
                std::iter::empty(),
            );
        (
            Self {
                state: Some(state),
                transaction_id: new_transaction_id.unwrap_or(transaction_id),
                options_to_request,
                rng,
            },
            actions,
        )
    }

    pub fn get_dns_servers(&self) -> Vec<Ipv6Addr> {
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } = self;
        state.as_ref().expect("state should not be empty").get_dns_servers()
    }

    /// Handles a timeout event, dispatches based on timeout type.
    ///
    /// # Panics
    ///
    /// `handle_timeout` panics if current state is None.
    pub fn handle_timeout(&mut self, timeout_type: ClientTimerType, now: I) -> Actions<I> {
        let ClientStateMachine { transaction_id, options_to_request, state, rng } = self;
        let old_state = state.take().expect("state should not be empty");
        let Transition { state: new_state, actions, transaction_id: new_transaction_id } =
            match timeout_type {
                ClientTimerType::Retransmission => old_state.retransmission_timer_expired(
                    *transaction_id,
                    &options_to_request,
                    rng,
                    now,
                ),
                ClientTimerType::Refresh => {
                    old_state.refresh_timer_expired(*transaction_id, &options_to_request, rng, now)
                }
                ClientTimerType::Renew => {
                    old_state.renew_timer_expired(&options_to_request, rng, now)
                }
                ClientTimerType::Rebind => {
                    old_state.rebind_timer_expired(&options_to_request, rng, now)
                }
                ClientTimerType::RestartServerDiscovery => {
                    old_state.restart_server_discovery(&options_to_request, rng, now)
                }
            };
        *state = Some(new_state);
        *transaction_id = new_transaction_id.unwrap_or(*transaction_id);
        actions
    }

    /// Handles a received DHCPv6 message.
    ///
    /// # Panics
    ///
    /// `handle_reply` panics if current state is None.
    pub fn handle_message_receive<B: ByteSlice>(
        &mut self,
        msg: v6::Message<'_, B>,
        now: I,
    ) -> Actions<I> {
        let ClientStateMachine { transaction_id, options_to_request, state, rng } = self;
        if msg.transaction_id() != transaction_id {
            Vec::new() // Ignore messages for other clients.
        } else {
            match msg.msg_type() {
                v6::MessageType::Reply => {
                    let Transition {
                        state: new_state,
                        actions,
                        transaction_id: new_transaction_id,
                    } = state.take().expect("state should not be empty").reply_message_received(
                        &options_to_request,
                        rng,
                        msg,
                        now,
                    );
                    *state = Some(new_state);
                    *transaction_id = new_transaction_id.unwrap_or(*transaction_id);
                    actions
                }
                v6::MessageType::Advertise => {
                    let Transition {
                        state: new_state,
                        actions,
                        transaction_id: new_transaction_id,
                    } = state
                        .take()
                        .expect("state should not be empty")
                        .advertise_message_received(&options_to_request, rng, msg, now);
                    *state = Some(new_state);
                    *transaction_id = new_transaction_id.unwrap_or(*transaction_id);
                    actions
                }
                v6::MessageType::Reconfigure => {
                    // TODO(jayzhuang): support Reconfigure messages when needed.
                    // https://tools.ietf.org/html/rfc8415#section-18.2.11
                    Vec::new()
                }
                v6::MessageType::Solicit
                | v6::MessageType::Request
                | v6::MessageType::Confirm
                | v6::MessageType::Renew
                | v6::MessageType::Rebind
                | v6::MessageType::Release
                | v6::MessageType::Decline
                | v6::MessageType::InformationRequest
                | v6::MessageType::RelayForw
                | v6::MessageType::RelayRepl => {
                    // Ignore unexpected message types.
                    Vec::new()
                }
            }
        }
    }
}

#[cfg(test)]
pub(crate) mod testconsts {
    use net_declare::{net_ip_v6, net_subnet_v6};
    use net_types::ip::{Ipv6Addr, Subnet};
    use packet_formats_dhcp::v6;

    use super::*;

    pub(super) trait IaValueTestExt: IaValue {
        const CONFIGURED: [Self; 3];
    }

    impl IaValueTestExt for Ipv6Addr {
        const CONFIGURED: [Self; 3] = CONFIGURED_NON_TEMPORARY_ADDRESSES;
    }

    impl IaValueTestExt for Subnet<Ipv6Addr> {
        const CONFIGURED: [Self; 3] = CONFIGURED_DELEGATED_PREFIXES;
    }

    pub(crate) const INFINITY: u32 = u32::MAX;
    pub(crate) const DNS_SERVERS: [Ipv6Addr; 2] =
        [net_ip_v6!("ff01::0102"), net_ip_v6!("ff01::0304")];
    pub(crate) const CLIENT_ID: [u8; 18] =
        [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17];
    pub(crate) const MISMATCHED_CLIENT_ID: [u8; 18] =
        [20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37];
    pub(crate) const TEST_SERVER_ID_LEN: usize = 3;
    pub(crate) const SERVER_ID: [[u8; TEST_SERVER_ID_LEN]; 3] =
        [[100, 101, 102], [110, 111, 112], [120, 121, 122]];

    pub(crate) const RENEW_NON_TEMPORARY_ADDRESSES: [Ipv6Addr; 3] = [
        net_ip_v6!("::ffff:4e45:123"),
        net_ip_v6!("::ffff:4e45:456"),
        net_ip_v6!("::ffff:4e45:789"),
    ];
    pub(crate) const REPLY_NON_TEMPORARY_ADDRESSES: [Ipv6Addr; 3] = [
        net_ip_v6!("::ffff:5447:123"),
        net_ip_v6!("::ffff:5447:456"),
        net_ip_v6!("::ffff:5447:789"),
    ];
    pub(crate) const CONFIGURED_NON_TEMPORARY_ADDRESSES: [Ipv6Addr; 3] = [
        net_ip_v6!("::ffff:c00a:123"),
        net_ip_v6!("::ffff:c00a:456"),
        net_ip_v6!("::ffff:c00a:789"),
    ];
    pub(crate) const RENEW_DELEGATED_PREFIXES: [Subnet<Ipv6Addr>; 3] =
        [net_subnet_v6!("1::/64"), net_subnet_v6!("2::/60"), net_subnet_v6!("3::/56")];
    pub(crate) const REPLY_DELEGATED_PREFIXES: [Subnet<Ipv6Addr>; 3] =
        [net_subnet_v6!("d::/64"), net_subnet_v6!("e::/60"), net_subnet_v6!("f::/56")];
    pub(crate) const CONFIGURED_DELEGATED_PREFIXES: [Subnet<Ipv6Addr>; 3] =
        [net_subnet_v6!("a::/64"), net_subnet_v6!("b::/60"), net_subnet_v6!("c::/56")];

    pub(crate) const T1: v6::NonZeroOrMaxU32 =
        const_unwrap::const_unwrap_option(v6::NonZeroOrMaxU32::new(30));
    pub(crate) const T2: v6::NonZeroOrMaxU32 =
        const_unwrap::const_unwrap_option(v6::NonZeroOrMaxU32::new(70));
    pub(crate) const PREFERRED_LIFETIME: v6::NonZeroOrMaxU32 =
        const_unwrap::const_unwrap_option(v6::NonZeroOrMaxU32::new(40));
    pub(crate) const VALID_LIFETIME: v6::NonZeroOrMaxU32 =
        const_unwrap::const_unwrap_option(v6::NonZeroOrMaxU32::new(80));

    pub(crate) const RENEWED_T1: v6::NonZeroOrMaxU32 =
        const_unwrap::const_unwrap_option(v6::NonZeroOrMaxU32::new(130));
    pub(crate) const RENEWED_T2: v6::NonZeroOrMaxU32 =
        const_unwrap::const_unwrap_option(v6::NonZeroOrMaxU32::new(170));
    pub(crate) const RENEWED_PREFERRED_LIFETIME: v6::NonZeroOrMaxU32 =
        const_unwrap::const_unwrap_option(v6::NonZeroOrMaxU32::new(140));
    pub(crate) const RENEWED_VALID_LIFETIME: v6::NonZeroOrMaxU32 =
        const_unwrap::const_unwrap_option(v6::NonZeroOrMaxU32::new(180));
}

#[cfg(test)]
pub(crate) mod testutil {
    use std::time::Instant;

    use super::*;
    use packet::ParsablePacket;
    use testconsts::*;

    pub(crate) fn to_configured_addresses(
        address_count: usize,
        preferred_addresses: impl IntoIterator<Item = HashSet<Ipv6Addr>>,
    ) -> HashMap<v6::IAID, HashSet<Ipv6Addr>> {
        let addresses = preferred_addresses
            .into_iter()
            .chain(std::iter::repeat_with(HashSet::new))
            .take(address_count);

        (0..).map(v6::IAID::new).zip(addresses).collect()
    }

    pub(crate) fn to_configured_prefixes(
        prefix_count: usize,
        preferred_prefixes: impl IntoIterator<Item = HashSet<Subnet<Ipv6Addr>>>,
    ) -> HashMap<v6::IAID, HashSet<Subnet<Ipv6Addr>>> {
        let prefixes = preferred_prefixes
            .into_iter()
            .chain(std::iter::repeat_with(HashSet::new))
            .take(prefix_count);

        (0..).map(v6::IAID::new).zip(prefixes).collect()
    }

    pub(super) fn to_default_ias_map<A: IaValue>(addresses: &[A]) -> HashMap<v6::IAID, HashSet<A>> {
        (0..)
            .map(v6::IAID::new)
            .zip(addresses.iter().map(|value| HashSet::from([*value])))
            .collect()
    }

    pub(super) fn assert_server_discovery(
        state: &Option<ClientState<Instant>>,
        client_id: [u8; CLIENT_ID_LEN],
        configured_non_temporary_addresses: HashMap<v6::IAID, HashSet<Ipv6Addr>>,
        configured_delegated_prefixes: HashMap<v6::IAID, HashSet<Subnet<Ipv6Addr>>>,
        first_solicit_time: Instant,
        buf: &[u8],
        options_to_request: &[v6::OptionCode],
    ) {
        assert_matches!(
            state,
            Some(ClientState::ServerDiscovery(ServerDiscovery {
                client_id: got_client_id,
                configured_non_temporary_addresses: got_configured_non_temporary_addresses,
                configured_delegated_prefixes: got_configured_delegated_prefixes,
                first_solicit_time: got_first_solicit_time,
                retrans_timeout: INITIAL_SOLICIT_TIMEOUT,
                solicit_max_rt: MAX_SOLICIT_TIMEOUT,
                collected_advertise,
                collected_sol_max_rt,
            })) => {
                assert_eq!(got_client_id, &client_id);
                assert_eq!(
                    got_configured_non_temporary_addresses,
                    &configured_non_temporary_addresses,
                );
                assert_eq!(
                    got_configured_delegated_prefixes,
                    &configured_delegated_prefixes,
                );
                assert!(
                    collected_advertise.is_empty(),
                    "collected_advertise={:?}",
                    collected_advertise,
                );
                assert_eq!(collected_sol_max_rt, &[]);
                assert_eq!(*got_first_solicit_time, first_solicit_time);
            }
        );

        assert_outgoing_stateful_message(
            buf,
            v6::MessageType::Solicit,
            &client_id,
            None,
            &options_to_request,
            &configured_non_temporary_addresses,
            &configured_delegated_prefixes,
        );
    }

    /// Creates a stateful client and asserts that:
    ///    - the client is started in ServerDiscovery state
    ///    - the state contain the expected value
    ///    - the actions are correct
    ///    - the Solicit message is correct
    ///
    /// Returns the client in ServerDiscovery state.
    pub(crate) fn start_and_assert_server_discovery<R: Rng + std::fmt::Debug>(
        transaction_id: [u8; 3],
        client_id: [u8; CLIENT_ID_LEN],
        configured_non_temporary_addresses: HashMap<v6::IAID, HashSet<Ipv6Addr>>,
        configured_delegated_prefixes: HashMap<v6::IAID, HashSet<Subnet<Ipv6Addr>>>,
        options_to_request: Vec<v6::OptionCode>,
        rng: R,
        now: Instant,
    ) -> ClientStateMachine<Instant, R> {
        let (client, actions) = ClientStateMachine::start_stateful(
            transaction_id.clone(),
            client_id.clone(),
            configured_non_temporary_addresses.clone(),
            configured_delegated_prefixes.clone(),
            options_to_request.clone(),
            rng,
            now,
        );

        let ClientStateMachine {
            transaction_id: got_transaction_id,
            options_to_request: got_options_to_request,
            state,
            rng: _,
        } = &client;
        assert_eq!(got_transaction_id, &transaction_id);
        assert_eq!(got_options_to_request, &options_to_request);

        // Start of server discovery should send a solicit and schedule a
        // retransmission timer.
        let buf = assert_matches!( &actions[..],
            [
                Action::SendMessage(buf),
                Action::ScheduleTimer(ClientTimerType::Retransmission, instant)
            ] => {
                assert_eq!(*instant, now.add(INITIAL_SOLICIT_TIMEOUT));
                buf
            }
        );

        assert_server_discovery(
            state,
            client_id,
            configured_non_temporary_addresses,
            configured_delegated_prefixes,
            now,
            buf,
            &options_to_request,
        );

        client
    }

    impl Lifetimes {
        pub(crate) const fn new_default() -> Self {
            Lifetimes::new_finite(PREFERRED_LIFETIME, VALID_LIFETIME)
        }

        pub(crate) fn new(preferred_lifetime: u32, non_zero_valid_lifetime: u32) -> Self {
            Lifetimes {
                preferred_lifetime: v6::TimeValue::new(preferred_lifetime),
                valid_lifetime: assert_matches!(
                    v6::TimeValue::new(non_zero_valid_lifetime),
                    v6::TimeValue::NonZero(v) => v
                ),
            }
        }

        pub(crate) const fn new_finite(
            preferred_lifetime: v6::NonZeroOrMaxU32,
            valid_lifetime: v6::NonZeroOrMaxU32,
        ) -> Lifetimes {
            Lifetimes {
                preferred_lifetime: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(
                    preferred_lifetime,
                )),
                valid_lifetime: v6::NonZeroTimeValue::Finite(valid_lifetime),
            }
        }

        pub(crate) const fn new_renewed() -> Lifetimes {
            Lifetimes {
                preferred_lifetime: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(
                    RENEWED_PREFERRED_LIFETIME,
                )),
                valid_lifetime: v6::NonZeroTimeValue::Finite(RENEWED_VALID_LIFETIME),
            }
        }
    }

    impl<V: IaValue> IaEntry<V, Instant> {
        pub(crate) fn new_assigned(
            value: V,
            preferred_lifetime: v6::NonZeroOrMaxU32,
            valid_lifetime: v6::NonZeroOrMaxU32,
            updated_at: Instant,
        ) -> Self {
            Self::Assigned(HashMap::from([(
                value,
                LifetimesInfo {
                    lifetimes: Lifetimes {
                        preferred_lifetime: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(
                            preferred_lifetime,
                        )),
                        valid_lifetime: v6::NonZeroTimeValue::Finite(valid_lifetime),
                    },
                    updated_at,
                },
            )]))
        }
    }

    impl AdvertiseMessage<Instant> {
        pub(crate) fn new_default(
            server_id: [u8; TEST_SERVER_ID_LEN],
            non_temporary_addresses: &[Ipv6Addr],
            delegated_prefixes: &[Subnet<Ipv6Addr>],
            dns_servers: &[Ipv6Addr],
            configured_non_temporary_addresses: &HashMap<v6::IAID, HashSet<Ipv6Addr>>,
            configured_delegated_prefixes: &HashMap<v6::IAID, HashSet<Subnet<Ipv6Addr>>>,
        ) -> AdvertiseMessage<Instant> {
            let non_temporary_addresses = (0..)
                .map(v6::IAID::new)
                .zip(non_temporary_addresses.iter().map(|address| HashSet::from([*address])))
                .collect();
            let delegated_prefixes = (0..)
                .map(v6::IAID::new)
                .zip(delegated_prefixes.iter().map(|prefix| HashSet::from([*prefix])))
                .collect();
            let preferred_non_temporary_addresses_count = compute_preferred_ia_count(
                &non_temporary_addresses,
                &configured_non_temporary_addresses,
            );
            let preferred_delegated_prefixes_count =
                compute_preferred_ia_count(&delegated_prefixes, &configured_delegated_prefixes);
            AdvertiseMessage {
                server_id: server_id.to_vec(),
                non_temporary_addresses,
                delegated_prefixes,
                dns_servers: dns_servers.to_vec(),
                preference: 0,
                receive_time: Instant::now(),
                preferred_non_temporary_addresses_count,
                preferred_delegated_prefixes_count,
            }
        }
    }

    /// Parses `buf` and returns the DHCPv6 message type.
    ///
    /// # Panics
    ///
    /// `msg_type` panics if parsing fails.
    pub(crate) fn msg_type(mut buf: &[u8]) -> v6::MessageType {
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        msg.msg_type()
    }

    /// A helper identity association test type specifying T1/T2, for testing
    /// T1/T2 variations across IAs.
    #[derive(Clone)]
    pub(super) struct TestIa<V: IaValue> {
        pub(crate) values: HashMap<V, Lifetimes>,
        pub(crate) t1: v6::TimeValue,
        pub(crate) t2: v6::TimeValue,
    }

    impl<V: IaValue> TestIa<V> {
        /// Creates a `TestIa` with default valid values for
        /// lifetimes.
        pub(crate) fn new_default(value: V) -> TestIa<V> {
            TestIa::new_default_with_values(HashMap::from([(value, Lifetimes::new_default())]))
        }

        pub(crate) fn new_default_with_values(values: HashMap<V, Lifetimes>) -> TestIa<V> {
            TestIa {
                values,
                t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T1)),
                t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T2)),
            }
        }

        /// Creates a `TestIa` with default valid values for
        /// renewed lifetimes.
        pub(crate) fn new_renewed_default(value: V) -> TestIa<V> {
            TestIa::new_renewed_default_with_values([value].into_iter())
        }

        pub(crate) fn new_renewed_default_with_values(
            values: impl Iterator<Item = V>,
        ) -> TestIa<V> {
            TestIa {
                values: values.map(|v| (v, Lifetimes::new_renewed())).collect(),
                t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(RENEWED_T1)),
                t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(RENEWED_T2)),
            }
        }
    }

    pub(super) struct TestMessageBuilder<'a, IaNaIter, IaPdIter> {
        pub(super) transaction_id: [u8; 3],
        pub(super) message_type: v6::MessageType,
        pub(super) client_id: &'a [u8],
        pub(super) server_id: &'a [u8],
        pub(super) preference: Option<u8>,
        pub(super) dns_servers: Option<&'a [Ipv6Addr]>,
        pub(super) ia_nas: IaNaIter,
        pub(super) ia_pds: IaPdIter,
    }

    impl<
            'a,
            IaNaIter: Iterator<Item = (v6::IAID, TestIa<Ipv6Addr>)>,
            IaPdIter: Iterator<Item = (v6::IAID, TestIa<Subnet<Ipv6Addr>>)>,
        > TestMessageBuilder<'a, IaNaIter, IaPdIter>
    {
        pub(super) fn build(self) -> Vec<u8> {
            let TestMessageBuilder {
                transaction_id,
                message_type,
                client_id,
                server_id,
                preference,
                dns_servers,
                ia_nas,
                ia_pds,
            } = self;

            struct Inner<'a> {
                opt: Vec<v6::DhcpOption<'a>>,
                t1: v6::TimeValue,
                t2: v6::TimeValue,
            }

            let iaaddr_options = ia_nas
                .map(|(iaid, TestIa { values, t1, t2 })| {
                    (
                        iaid,
                        Inner {
                            opt: values
                                .into_iter()
                                .map(|(value, Lifetimes { preferred_lifetime, valid_lifetime })| {
                                    v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
                                        value,
                                        get_value(preferred_lifetime),
                                        get_value(valid_lifetime.into()),
                                        &[],
                                    ))
                                })
                                .collect(),
                            t1,
                            t2,
                        },
                    )
                })
                .collect::<HashMap<_, _>>();
            let iaprefix_options = ia_pds
                .map(|(iaid, TestIa { values, t1, t2 })| {
                    (
                        iaid,
                        Inner {
                            opt: values
                                .into_iter()
                                .map(|(value, Lifetimes { preferred_lifetime, valid_lifetime })| {
                                    v6::DhcpOption::IaPrefix(v6::IaPrefixSerializer::new(
                                        get_value(preferred_lifetime),
                                        get_value(valid_lifetime.into()),
                                        value,
                                        &[],
                                    ))
                                })
                                .collect(),
                            t1,
                            t2,
                        },
                    )
                })
                .collect::<HashMap<_, _>>();

            let options =
                [v6::DhcpOption::ServerId(&server_id), v6::DhcpOption::ClientId(client_id)]
                    .into_iter()
                    .chain(preference.into_iter().map(v6::DhcpOption::Preference))
                    .chain(dns_servers.into_iter().map(v6::DhcpOption::DnsServers))
                    .chain(iaaddr_options.iter().map(|(iaid, Inner { opt, t1, t2 })| {
                        v6::DhcpOption::Iana(v6::IanaSerializer::new(
                            *iaid,
                            get_value(*t1),
                            get_value(*t2),
                            opt.as_ref(),
                        ))
                    }))
                    .chain(iaprefix_options.iter().map(|(iaid, Inner { opt, t1, t2 })| {
                        v6::DhcpOption::IaPd(v6::IaPdSerializer::new(
                            *iaid,
                            get_value(*t1),
                            get_value(*t2),
                            opt.as_ref(),
                        ))
                    }))
                    .collect::<Vec<_>>();

            let builder = v6::MessageBuilder::new(message_type, transaction_id, &options);
            let mut buf = vec![0; builder.bytes_len()];
            builder.serialize(&mut buf);
            buf
        }
    }

    pub(super) type TestIaNa = TestIa<Ipv6Addr>;
    pub(super) type TestIaPd = TestIa<Subnet<Ipv6Addr>>;

    /// Creates a stateful client, exchanges messages to bring it in Requesting
    /// state, and sends a Request message. Returns the client in Requesting
    /// state and the transaction ID for the Request-Reply exchange. Asserts the
    /// content of the sent Request message and of the Requesting state.
    ///
    /// # Panics
    ///
    /// `request_and_assert` panics if the Request message cannot be
    /// parsed or does not contain the expected options, or the Requesting state
    /// is incorrect.
    pub(super) fn request_and_assert<R: Rng + std::fmt::Debug>(
        client_id: [u8; CLIENT_ID_LEN],
        server_id: [u8; TEST_SERVER_ID_LEN],
        non_temporary_addresses_to_assign: Vec<TestIaNa>,
        delegated_prefixes_to_assign: Vec<TestIaPd>,
        expected_dns_servers: &[Ipv6Addr],
        rng: R,
        now: Instant,
    ) -> (ClientStateMachine<Instant, R>, [u8; 3]) {
        // Generate a transaction_id for the Solicit - Advertise message
        // exchange.
        let transaction_id = [1, 2, 3];
        let configured_non_temporary_addresses = to_configured_addresses(
            non_temporary_addresses_to_assign.len(),
            non_temporary_addresses_to_assign
                .iter()
                .map(|TestIaNa { values, t1: _, t2: _ }| values.keys().cloned().collect()),
        );
        let configured_delegated_prefixes = to_configured_prefixes(
            delegated_prefixes_to_assign.len(),
            delegated_prefixes_to_assign
                .iter()
                .map(|TestIaPd { values, t1: _, t2: _ }| values.keys().cloned().collect()),
        );
        let options_to_request = if expected_dns_servers.is_empty() {
            Vec::new()
        } else {
            vec![v6::OptionCode::DnsServers]
        };
        let mut client = testutil::start_and_assert_server_discovery(
            transaction_id.clone(),
            client_id.clone(),
            configured_non_temporary_addresses.clone(),
            configured_delegated_prefixes.clone(),
            options_to_request.clone(),
            rng,
            now,
        );

        let buf = TestMessageBuilder {
            transaction_id,
            message_type: v6::MessageType::Advertise,
            client_id: &CLIENT_ID,
            server_id: &server_id,
            preference: Some(ADVERTISE_MAX_PREFERENCE),
            dns_servers: (!expected_dns_servers.is_empty()).then(|| expected_dns_servers),
            ia_nas: (0..).map(v6::IAID::new).zip(non_temporary_addresses_to_assign),
            ia_pds: (0..).map(v6::IAID::new).zip(delegated_prefixes_to_assign),
        }
        .build();
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        // The client should select the server that sent the best advertise and
        // transition to Requesting.
        let actions = client.handle_message_receive(msg, now);
        let buf = assert_matches!(
            &actions[..],
           [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::SendMessage(buf),
                Action::ScheduleTimer(ClientTimerType::Retransmission, instant)
           ] => {
               assert_eq!(*instant, now.add(INITIAL_REQUEST_TIMEOUT));
               buf
           }
        );
        testutil::assert_outgoing_stateful_message(
            &buf,
            v6::MessageType::Request,
            &client_id,
            Some(&server_id),
            &options_to_request,
            &configured_non_temporary_addresses,
            &configured_delegated_prefixes,
        );
        let ClientStateMachine { transaction_id, options_to_request: _, state, rng: _ } = &client;
        let request_transaction_id = *transaction_id;
        {
            let Requesting {
                client_id: got_client_id,
                server_id: got_server_id,
                collected_advertise,
                retrans_timeout,
                transmission_count,
                solicit_max_rt,
                non_temporary_addresses: _,
                delegated_prefixes: _,
                first_request_time: _,
            } = assert_matches!(&state, Some(ClientState::Requesting(requesting)) => requesting);
            assert_eq!(*got_client_id, client_id);
            assert_eq!(*got_server_id, server_id);
            assert!(
                collected_advertise.is_empty(),
                "collected_advertise = {:?}",
                collected_advertise
            );
            assert_eq!(*retrans_timeout, INITIAL_REQUEST_TIMEOUT);
            assert_eq!(*transmission_count, 1);
            assert_eq!(*solicit_max_rt, MAX_SOLICIT_TIMEOUT);
        }
        (client, request_transaction_id)
    }

    /// Creates a stateful client and exchanges messages to assign the
    /// configured addresses/prefixes. Returns the client in Assigned state and
    /// the actions returned on transitioning to the Assigned state.
    /// Asserts the content of the client state.
    ///
    /// # Panics
    ///
    /// `assign_and_assert` panics if assignment fails.
    pub(super) fn assign_and_assert<R: Rng + std::fmt::Debug>(
        client_id: [u8; CLIENT_ID_LEN],
        server_id: [u8; TEST_SERVER_ID_LEN],
        non_temporary_addresses_to_assign: Vec<TestIaNa>,
        delegated_prefixes_to_assign: Vec<TestIaPd>,
        expected_dns_servers: &[Ipv6Addr],
        rng: R,
        now: Instant,
    ) -> (ClientStateMachine<Instant, R>, Actions<Instant>) {
        let (mut client, transaction_id) = testutil::request_and_assert(
            client_id.clone(),
            server_id.clone(),
            non_temporary_addresses_to_assign.clone(),
            delegated_prefixes_to_assign.clone(),
            expected_dns_servers,
            rng,
            now,
        );

        let non_temporary_addresses_to_assign = (0..)
            .map(v6::IAID::new)
            .zip(non_temporary_addresses_to_assign)
            .collect::<HashMap<_, _>>();
        let delegated_prefixes_to_assign =
            (0..).map(v6::IAID::new).zip(delegated_prefixes_to_assign).collect::<HashMap<_, _>>();

        let buf = TestMessageBuilder {
            transaction_id,
            message_type: v6::MessageType::Reply,
            client_id: &CLIENT_ID,
            server_id: &SERVER_ID[0],
            preference: None,
            dns_servers: (!expected_dns_servers.is_empty()).then(|| expected_dns_servers),
            ia_nas: non_temporary_addresses_to_assign.iter().map(|(k, v)| (*k, v.clone())),
            ia_pds: delegated_prefixes_to_assign.iter().map(|(k, v)| (*k, v.clone())),
        }
        .build();
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let actions = client.handle_message_receive(msg, now);
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
            &client;
        let expected_non_temporary_addresses = non_temporary_addresses_to_assign
            .iter()
            .map(|(iaid, TestIaNa { values, t1: _, t2: _ })| {
                (
                    *iaid,
                    AddressEntry::Assigned(
                        values
                            .iter()
                            .map(|(v, lifetimes)| {
                                (*v, LifetimesInfo { lifetimes: *lifetimes, updated_at: now })
                            })
                            .collect(),
                    ),
                )
            })
            .collect::<HashMap<_, _>>();
        let expected_delegated_prefixes = delegated_prefixes_to_assign
            .iter()
            .map(|(iaid, TestIaPd { values, t1: _, t2: _ })| {
                (
                    *iaid,
                    PrefixEntry::Assigned(
                        values
                            .iter()
                            .map(|(v, lifetimes)| {
                                (*v, LifetimesInfo { lifetimes: *lifetimes, updated_at: now })
                            })
                            .collect(),
                    ),
                )
            })
            .collect::<HashMap<_, _>>();
        let Assigned {
            client_id: got_client_id,
            non_temporary_addresses,
            delegated_prefixes,
            server_id: got_server_id,
            dns_servers,
            solicit_max_rt,
            _marker,
        } = assert_matches!(
            &state,
            Some(ClientState::Assigned(assigned)) => assigned
        );
        assert_eq!(*got_client_id, client_id);
        assert_eq!(non_temporary_addresses, &expected_non_temporary_addresses);
        assert_eq!(delegated_prefixes, &expected_delegated_prefixes);
        assert_eq!(*got_server_id, server_id);
        assert_eq!(dns_servers, expected_dns_servers);
        assert_eq!(*solicit_max_rt, MAX_SOLICIT_TIMEOUT);
        (client, actions)
    }

    /// Gets the `u32` value inside a `v6::TimeValue`.
    pub(crate) fn get_value(t: v6::TimeValue) -> u32 {
        const INFINITY: u32 = u32::MAX;
        match t {
            v6::TimeValue::Zero => 0,
            v6::TimeValue::NonZero(non_zero_tv) => match non_zero_tv {
                v6::NonZeroTimeValue::Finite(t) => t.get(),
                v6::NonZeroTimeValue::Infinity => INFINITY,
            },
        }
    }

    /// Checks that the buffer contains the expected type and options for an
    /// outgoing message in stateful mode.
    ///
    /// # Panics
    ///
    /// `assert_outgoing_stateful_message` panics if the message cannot be
    /// parsed, or does not contain the expected options.
    pub(crate) fn assert_outgoing_stateful_message(
        mut buf: &[u8],
        expected_msg_type: v6::MessageType,
        expected_client_id: &[u8; CLIENT_ID_LEN],
        expected_server_id: Option<&[u8; TEST_SERVER_ID_LEN]>,
        expected_oro: &[v6::OptionCode],
        expected_non_temporary_addresses: &HashMap<v6::IAID, HashSet<Ipv6Addr>>,
        expected_delegated_prefixes: &HashMap<v6::IAID, HashSet<Subnet<Ipv6Addr>>>,
    ) {
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        assert_eq!(msg.msg_type(), expected_msg_type);

        let (mut non_ia_opts, iana_opts, iapd_opts, other) = msg.options().fold(
            (Vec::new(), Vec::new(), Vec::new(), Vec::new()),
            |(mut non_ia_opts, mut iana_opts, mut iapd_opts, mut other), opt| {
                match opt {
                    v6::ParsedDhcpOption::ClientId(_)
                    | v6::ParsedDhcpOption::ElapsedTime(_)
                    | v6::ParsedDhcpOption::Oro(_) => non_ia_opts.push(opt),
                    v6::ParsedDhcpOption::ServerId(_) if expected_server_id.is_some() => {
                        non_ia_opts.push(opt)
                    }
                    v6::ParsedDhcpOption::Iana(iana_data) => iana_opts.push(iana_data),
                    v6::ParsedDhcpOption::IaPd(iapd_data) => iapd_opts.push(iapd_data),
                    opt => other.push(opt),
                }
                (non_ia_opts, iana_opts, iapd_opts, other)
            },
        );
        let option_sorter: fn(
            &v6::ParsedDhcpOption<'_>,
            &v6::ParsedDhcpOption<'_>,
        ) -> std::cmp::Ordering =
            |opt1, opt2| (u16::from(opt1.code())).cmp(&(u16::from(opt2.code())));

        // Check that the non-IA options are correct.
        non_ia_opts.sort_by(option_sorter);
        let expected_non_ia_opts = {
            let oro = std::iter::once(v6::OptionCode::SolMaxRt)
                .chain(expected_oro.iter().copied())
                .collect();
            let mut expected_non_ia_opts = vec![
                v6::ParsedDhcpOption::ClientId(expected_client_id),
                v6::ParsedDhcpOption::ElapsedTime(0),
                v6::ParsedDhcpOption::Oro(oro),
            ];
            if let Some(server_id) = expected_server_id {
                expected_non_ia_opts.push(v6::ParsedDhcpOption::ServerId(server_id));
            }
            expected_non_ia_opts.sort_by(option_sorter);
            expected_non_ia_opts
        };
        assert_eq!(non_ia_opts, expected_non_ia_opts);

        // Check that the IA options are correct.
        let sent_non_temporary_addresses = {
            let mut sent_non_temporary_addresses: HashMap<v6::IAID, HashSet<Ipv6Addr>> =
                HashMap::new();
            for iana_data in iana_opts.iter() {
                let mut opts = HashSet::new();

                for iana_option in iana_data.iter_options() {
                    match iana_option {
                        v6::ParsedDhcpOption::IaAddr(iaaddr_data) => {
                            assert!(opts.insert(iaaddr_data.addr()));
                        }
                        option => panic!("unexpected option {:?}", option),
                    }
                }

                assert_eq!(
                    sent_non_temporary_addresses.insert(v6::IAID::new(iana_data.iaid()), opts),
                    None
                );
            }
            sent_non_temporary_addresses
        };
        assert_eq!(&sent_non_temporary_addresses, expected_non_temporary_addresses);

        let sent_prefixes = {
            let mut sent_prefixes: HashMap<v6::IAID, HashSet<Subnet<Ipv6Addr>>> = HashMap::new();
            for iapd_data in iapd_opts.iter() {
                let mut opts = HashSet::new();

                for iapd_option in iapd_data.iter_options() {
                    match iapd_option {
                        v6::ParsedDhcpOption::IaPrefix(iaprefix_data) => {
                            assert!(opts.insert(iaprefix_data.prefix().unwrap()));
                        }
                        option => panic!("unexpected option {:?}", option),
                    }
                }

                assert_eq!(sent_prefixes.insert(v6::IAID::new(iapd_data.iaid()), opts), None);
            }
            sent_prefixes
        };
        assert_eq!(&sent_prefixes, expected_delegated_prefixes);

        // Check that there are no other options besides the expected non-IA and
        // IA options.
        assert_eq!(&other, &[]);
    }

    /// Creates a stateful client, exchanges messages to assign the configured
    /// leases, and sends a Renew message. Asserts the content of the client
    /// state and of the renew message, and returns the client in Renewing
    /// state.
    ///
    /// # Panics
    ///
    /// `send_renew_and_assert` panics if assignment fails, or if sending a
    /// renew fails.
    pub(super) fn send_renew_and_assert<R: Rng + std::fmt::Debug>(
        client_id: [u8; CLIENT_ID_LEN],
        server_id: [u8; TEST_SERVER_ID_LEN],
        non_temporary_addresses_to_assign: Vec<TestIaNa>,
        delegated_prefixes_to_assign: Vec<TestIaPd>,
        expected_dns_servers: Option<&[Ipv6Addr]>,
        expected_t1_secs: v6::NonZeroOrMaxU32,
        expected_t2_secs: v6::NonZeroOrMaxU32,
        max_valid_lifetime: v6::NonZeroTimeValue,
        rng: R,
        now: Instant,
    ) -> ClientStateMachine<Instant, R> {
        let expected_dns_servers_as_slice = expected_dns_servers.unwrap_or(&[]);
        let (client, actions) = testutil::assign_and_assert(
            client_id.clone(),
            server_id.clone(),
            non_temporary_addresses_to_assign.clone(),
            delegated_prefixes_to_assign.clone(),
            expected_dns_servers_as_slice,
            rng,
            now,
        );
        let ClientStateMachine { transaction_id, options_to_request: _, state, rng: _ } = &client;
        let old_transaction_id = *transaction_id;
        {
            let Assigned {
                client_id: _,
                non_temporary_addresses: _,
                delegated_prefixes: _,
                server_id: _,
                dns_servers: _,
                solicit_max_rt: _,
                _marker,
            } = assert_matches!(
                state,
                Some(ClientState::Assigned(assigned)) => assigned
            );
        }
        let (expected_oro, maybe_dns_server_action) =
            if let Some(expected_dns_servers) = expected_dns_servers {
                (
                    Some([v6::OptionCode::DnsServers]),
                    Some(Action::UpdateDnsServers(expected_dns_servers.to_vec())),
                )
            } else {
                (None, None)
            };
        let iana_updates = (0..)
            .map(v6::IAID::new)
            .zip(non_temporary_addresses_to_assign.iter())
            .map(|(iaid, TestIa { values, t1: _, t2: _ })| {
                (
                    iaid,
                    values
                        .iter()
                        .map(|(value, lifetimes)| (*value, IaValueUpdateKind::Added(*lifetimes)))
                        .collect(),
                )
            })
            .collect::<HashMap<_, _>>();
        let iapd_updates = (0..)
            .map(v6::IAID::new)
            .zip(delegated_prefixes_to_assign.iter())
            .map(|(iaid, TestIa { values, t1: _, t2: _ })| {
                (
                    iaid,
                    values
                        .iter()
                        .map(|(value, lifetimes)| (*value, IaValueUpdateKind::Added(*lifetimes)))
                        .collect(),
                )
            })
            .collect::<HashMap<_, _>>();
        let expected_actions = [
            Action::CancelTimer(ClientTimerType::Retransmission),
            Action::ScheduleTimer(
                ClientTimerType::Renew,
                now.add(Duration::from_secs(expected_t1_secs.get().into())),
            ),
            Action::ScheduleTimer(
                ClientTimerType::Rebind,
                now.add(Duration::from_secs(expected_t2_secs.get().into())),
            ),
        ]
        .into_iter()
        .chain(maybe_dns_server_action)
        .chain((!iana_updates.is_empty()).then(|| Action::IaNaUpdates(iana_updates)))
        .chain((!iapd_updates.is_empty()).then(|| Action::IaPdUpdates(iapd_updates)))
        .chain([match max_valid_lifetime {
            v6::NonZeroTimeValue::Finite(max_valid_lifetime) => Action::ScheduleTimer(
                ClientTimerType::RestartServerDiscovery,
                now.add(Duration::from_secs(max_valid_lifetime.get().into())),
            ),
            v6::NonZeroTimeValue::Infinity => {
                Action::CancelTimer(ClientTimerType::RestartServerDiscovery)
            }
        }])
        .collect::<Vec<_>>();
        assert_eq!(actions, expected_actions);

        handle_renew_or_rebind_timer(
            client,
            old_transaction_id,
            client_id,
            server_id,
            non_temporary_addresses_to_assign,
            delegated_prefixes_to_assign,
            expected_dns_servers_as_slice,
            expected_oro.as_ref().map_or(&[], |oro| &oro[..]),
            now,
            RENEW_TEST_STATE,
        )
    }

    pub(super) struct RenewRebindTestState {
        initial_timeout: Duration,
        timer_type: ClientTimerType,
        message_type: v6::MessageType,
        expect_server_id: bool,
        with_state: fn(&Option<ClientState<Instant>>) -> &RenewingOrRebindingInner<Instant>,
    }

    pub(super) const RENEW_TEST_STATE: RenewRebindTestState = RenewRebindTestState {
        initial_timeout: INITIAL_RENEW_TIMEOUT,
        timer_type: ClientTimerType::Renew,
        message_type: v6::MessageType::Renew,
        expect_server_id: true,
        with_state: |state| {
            assert_matches!(
                state,
                Some(ClientState::Renewing(RenewingOrRebinding(inner))) => inner
            )
        },
    };

    pub(super) const REBIND_TEST_STATE: RenewRebindTestState = RenewRebindTestState {
        initial_timeout: INITIAL_REBIND_TIMEOUT,
        timer_type: ClientTimerType::Rebind,
        message_type: v6::MessageType::Rebind,
        expect_server_id: false,
        with_state: |state| {
            assert_matches!(
                state,
                Some(ClientState::Rebinding(RenewingOrRebinding(inner))) => inner
            )
        },
    };

    pub(super) fn handle_renew_or_rebind_timer<R: Rng>(
        mut client: ClientStateMachine<Instant, R>,
        old_transaction_id: [u8; 3],
        client_id: [u8; CLIENT_ID_LEN],
        server_id: [u8; TEST_SERVER_ID_LEN],
        non_temporary_addresses_to_assign: Vec<TestIaNa>,
        delegated_prefixes_to_assign: Vec<TestIaPd>,
        expected_dns_servers_as_slice: &[Ipv6Addr],
        expected_oro: &[v6::OptionCode],
        now: Instant,
        RenewRebindTestState {
            initial_timeout,
            timer_type,
            message_type,
            expect_server_id,
            with_state,
        }: RenewRebindTestState,
    ) -> ClientStateMachine<Instant, R> {
        let actions = client.handle_timeout(timer_type, now);
        let buf = assert_matches!(
            &actions[..],
            [
                Action::SendMessage(buf),
                Action::ScheduleTimer(ClientTimerType::Retransmission, got_time)
            ] => {
                assert_eq!(*got_time, now.add(initial_timeout));
                buf
            }
        );
        let ClientStateMachine { transaction_id, options_to_request: _, state, rng: _ } = &client;
        // Assert that sending a renew starts a new transaction.
        assert_ne!(*transaction_id, old_transaction_id);
        let RenewingOrRebindingInner {
            client_id: got_client_id,
            server_id: got_server_id,
            dns_servers,
            solicit_max_rt,
            non_temporary_addresses: _,
            delegated_prefixes: _,
            start_time: _,
            retrans_timeout: _,
        } = with_state(state);
        assert_eq!(*got_client_id, client_id);
        assert_eq!(*got_server_id, server_id);
        assert_eq!(dns_servers, expected_dns_servers_as_slice);
        assert_eq!(*solicit_max_rt, MAX_SOLICIT_TIMEOUT);
        let expected_addresses_to_renew: HashMap<v6::IAID, HashSet<Ipv6Addr>> = (0..)
            .map(v6::IAID::new)
            .zip(
                non_temporary_addresses_to_assign
                    .iter()
                    .map(|TestIaNa { values, t1: _, t2: _ }| values.keys().cloned().collect()),
            )
            .collect();
        let expected_prefixes_to_renew: HashMap<v6::IAID, HashSet<Subnet<Ipv6Addr>>> = (0..)
            .map(v6::IAID::new)
            .zip(
                delegated_prefixes_to_assign
                    .iter()
                    .map(|TestIaPd { values, t1: _, t2: _ }| values.keys().cloned().collect()),
            )
            .collect();
        testutil::assert_outgoing_stateful_message(
            &buf,
            message_type,
            &client_id,
            expect_server_id.then(|| &server_id),
            expected_oro,
            &expected_addresses_to_renew,
            &expected_prefixes_to_renew,
        );
        client
    }

    /// Creates a stateful client, exchanges messages to assign the configured
    /// leases, and sends a Renew then Rebind message. Asserts the content of
    /// the client state and of the rebind message, and returns the client in
    /// Rebinding state.
    ///
    /// # Panics
    ///
    /// `send_rebind_and_assert` panics if assignmentment fails, or if sending a
    /// rebind fails.
    pub(super) fn send_rebind_and_assert<R: Rng + std::fmt::Debug>(
        client_id: [u8; CLIENT_ID_LEN],
        server_id: [u8; TEST_SERVER_ID_LEN],
        non_temporary_addresses_to_assign: Vec<TestIaNa>,
        delegated_prefixes_to_assign: Vec<TestIaPd>,
        expected_dns_servers: Option<&[Ipv6Addr]>,
        expected_t1_secs: v6::NonZeroOrMaxU32,
        expected_t2_secs: v6::NonZeroOrMaxU32,
        max_valid_lifetime: v6::NonZeroTimeValue,
        rng: R,
        now: Instant,
    ) -> ClientStateMachine<Instant, R> {
        let client = testutil::send_renew_and_assert(
            client_id,
            server_id,
            non_temporary_addresses_to_assign.clone(),
            delegated_prefixes_to_assign.clone(),
            expected_dns_servers,
            expected_t1_secs,
            expected_t2_secs,
            max_valid_lifetime,
            rng,
            now,
        );
        let old_transaction_id = client.transaction_id;
        let (expected_oro, expected_dns_servers) =
            if let Some(expected_dns_servers) = expected_dns_servers {
                (Some([v6::OptionCode::DnsServers]), expected_dns_servers)
            } else {
                (None, &[][..])
            };

        handle_renew_or_rebind_timer(
            client,
            old_transaction_id,
            client_id,
            server_id,
            non_temporary_addresses_to_assign,
            delegated_prefixes_to_assign,
            expected_dns_servers,
            expected_oro.as_ref().map_or(&[], |oro| &oro[..]),
            now,
            REBIND_TEST_STATE,
        )
    }
}

#[cfg(test)]
mod tests {
    use std::{cmp::Ordering, time::Instant};

    use super::*;
    use packet::ParsablePacket;
    use rand::rngs::mock::StepRng;
    use test_case::test_case;
    use testconsts::*;
    use testutil::{
        handle_renew_or_rebind_timer, RenewRebindTestState, TestIa, TestIaNa, TestIaPd,
        TestMessageBuilder, REBIND_TEST_STATE, RENEW_TEST_STATE,
    };

    #[test]
    fn send_information_request_and_receive_reply() {
        // Try to start information request with different list of requested options.
        for options in [
            Vec::new(),
            vec![v6::OptionCode::DnsServers],
            vec![v6::OptionCode::DnsServers, v6::OptionCode::DomainList],
        ] {
            let now = Instant::now();
            let (mut client, actions) = ClientStateMachine::start_stateless(
                [0, 1, 2],
                options.clone(),
                StepRng::new(std::u64::MAX / 2, 0),
                now,
            );

            let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
                &client;
            assert_matches!(
                *state,
                Some(ClientState::InformationRequesting(InformationRequesting {
                    retrans_timeout: INITIAL_INFO_REQ_TIMEOUT,
                    _marker,
                }))
            );

            // Start of information requesting should send an information request and schedule a
            // retransmission timer.
            let want_options_array = [v6::DhcpOption::Oro(&options)];
            let want_options = if options.is_empty() { &[][..] } else { &want_options_array[..] };
            let ClientStateMachine { transaction_id, options_to_request: _, state: _, rng: _ } =
                &client;
            let builder = v6::MessageBuilder::new(
                v6::MessageType::InformationRequest,
                *transaction_id,
                want_options,
            );
            let mut want_buf = vec![0; builder.bytes_len()];
            builder.serialize(&mut want_buf);
            assert_eq!(
                actions[..],
                [
                    Action::SendMessage(want_buf),
                    Action::ScheduleTimer(
                        ClientTimerType::Retransmission,
                        now.add(INITIAL_INFO_REQ_TIMEOUT),
                    )
                ]
            );

            let test_dhcp_refresh_time = 42u32;
            let options = [
                v6::DhcpOption::ServerId(&SERVER_ID[0]),
                v6::DhcpOption::InformationRefreshTime(test_dhcp_refresh_time),
                v6::DhcpOption::DnsServers(&DNS_SERVERS),
            ];
            let builder =
                v6::MessageBuilder::new(v6::MessageType::Reply, *transaction_id, &options);
            let mut buf = vec![0; builder.bytes_len()];
            builder.serialize(&mut buf);
            let mut buf = &buf[..]; // Implements BufferView.
            let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");

            let now = Instant::now();
            let actions = client.handle_message_receive(msg, now);
            let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
                client;

            {
                assert_matches!(
                    state,
                    Some(ClientState::InformationReceived(InformationReceived { dns_servers, _marker }))
                        if dns_servers == DNS_SERVERS.to_vec()
                );
            }
            // Upon receiving a valid reply, client should set up for refresh based on the reply.
            assert_eq!(
                actions[..],
                [
                    Action::CancelTimer(ClientTimerType::Retransmission),
                    Action::ScheduleTimer(
                        ClientTimerType::Refresh,
                        now.add(Duration::from_secs(u64::from(test_dhcp_refresh_time))),
                    ),
                    Action::UpdateDnsServers(DNS_SERVERS.to_vec()),
                ]
            );
        }
    }

    #[test]
    fn send_information_request_on_retransmission_timeout() {
        let now = Instant::now();
        let (mut client, actions) = ClientStateMachine::start_stateless(
            [0, 1, 2],
            Vec::new(),
            StepRng::new(std::u64::MAX / 2, 0),
            now,
        );
        assert_matches!(
            actions[..],
            [_, Action::ScheduleTimer(ClientTimerType::Retransmission, instant)] => {
                assert_eq!(instant, now.add(INITIAL_INFO_REQ_TIMEOUT));
            }
        );

        let actions = client.handle_timeout(ClientTimerType::Retransmission, now);
        // Following exponential backoff defined in https://tools.ietf.org/html/rfc8415#section-15.
        assert_matches!(
            actions[..],
            [
                _,
                Action::ScheduleTimer(ClientTimerType::Retransmission, instant)
            ] => assert_eq!(instant, now.add(2 * INITIAL_INFO_REQ_TIMEOUT))
        );
    }

    #[test]
    fn send_information_request_on_refresh_timeout() {
        let (mut client, _) = ClientStateMachine::start_stateless(
            [0, 1, 2],
            Vec::new(),
            StepRng::new(std::u64::MAX / 2, 0),
            Instant::now(),
        );

        let ClientStateMachine { transaction_id, options_to_request: _, state: _, rng: _ } =
            &client;
        let options = [v6::DhcpOption::ServerId(&SERVER_ID[0])];
        let builder = v6::MessageBuilder::new(v6::MessageType::Reply, *transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");

        // Transition to InformationReceived state.
        let time = Instant::now();
        assert_eq!(
            client.handle_message_receive(msg, time)[..],
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::ScheduleTimer(ClientTimerType::Refresh, time.add(IRT_DEFAULT))
            ]
        );

        // Refresh should start another round of information request.
        let actions = client.handle_timeout(ClientTimerType::Refresh, time);
        let ClientStateMachine { transaction_id, options_to_request: _, state: _, rng: _ } =
            &client;
        let builder =
            v6::MessageBuilder::new(v6::MessageType::InformationRequest, *transaction_id, &[]);
        let mut want_buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut want_buf);
        assert_eq!(
            actions[..],
            [
                Action::SendMessage(want_buf),
                Action::ScheduleTimer(
                    ClientTimerType::Retransmission,
                    time.add(INITIAL_INFO_REQ_TIMEOUT)
                )
            ]
        );
    }

    // Test starting the client in stateful mode with different address
    // and prefix configurations.
    #[test_case(
        0, std::iter::empty(),
        2, (&CONFIGURED_DELEGATED_PREFIXES[0..2]).iter().copied(),
        Vec::new()
    )]
    #[test_case(
        2, (&CONFIGURED_NON_TEMPORARY_ADDRESSES[0..2]).iter().copied(),
        0, std::iter::empty(),
        vec![v6::OptionCode::DnsServers]
    )]
    #[test_case(
        1, std::iter::empty(),
        2, (&CONFIGURED_DELEGATED_PREFIXES[0..2]).iter().copied(),
        Vec::new()
    )]
    #[test_case(
        2, std::iter::once(CONFIGURED_NON_TEMPORARY_ADDRESSES[0]),
        1, std::iter::empty(),
        vec![v6::OptionCode::DnsServers]
    )]
    #[test_case(
        2, (&CONFIGURED_NON_TEMPORARY_ADDRESSES[0..2]).iter().copied(),
        2, std::iter::once(CONFIGURED_DELEGATED_PREFIXES[0]),
        vec![v6::OptionCode::DnsServers]
    )]
    fn send_solicit(
        address_count: usize,
        preferred_non_temporary_addresses: impl IntoIterator<Item = Ipv6Addr>,
        prefix_count: usize,
        preferred_delegated_prefixes: impl IntoIterator<Item = Subnet<Ipv6Addr>>,
        options_to_request: Vec<v6::OptionCode>,
    ) {
        // The client is checked inside `start_and_assert_server_discovery`.
        let _client = testutil::start_and_assert_server_discovery(
            [0, 1, 2],
            CLIENT_ID,
            testutil::to_configured_addresses(
                address_count,
                preferred_non_temporary_addresses.into_iter().map(|a| HashSet::from([a])),
            ),
            testutil::to_configured_prefixes(
                prefix_count,
                preferred_delegated_prefixes.into_iter().map(|a| HashSet::from([a])),
            ),
            options_to_request,
            StepRng::new(std::u64::MAX / 2, 0),
            Instant::now(),
        );
    }

    #[test_case(
        1, std::iter::empty(), std::iter::once(CONFIGURED_NON_TEMPORARY_ADDRESSES[0]), 0;
        "zero"
    )]
    #[test_case(
        2, CONFIGURED_NON_TEMPORARY_ADDRESSES[0..2].iter().copied(), CONFIGURED_NON_TEMPORARY_ADDRESSES[0..2].iter().copied(), 2;
        "two"
    )]
    #[test_case(
        4,
        CONFIGURED_NON_TEMPORARY_ADDRESSES.iter().copied(),
        std::iter::once(CONFIGURED_NON_TEMPORARY_ADDRESSES[0]).chain(REPLY_NON_TEMPORARY_ADDRESSES.iter().copied()),
        1;
        "one"
    )]
    fn compute_preferred_address_count(
        configure_count: usize,
        hints: impl IntoIterator<Item = Ipv6Addr>,
        got_addresses: impl IntoIterator<Item = Ipv6Addr>,
        want: usize,
    ) {
        // No preferred addresses configured.
        let got_addresses: HashMap<_, _> = (0..)
            .map(v6::IAID::new)
            .zip(got_addresses.into_iter().map(|a| HashSet::from([a])))
            .collect();
        let configured_non_temporary_addresses = testutil::to_configured_addresses(
            configure_count,
            hints.into_iter().map(|a| HashSet::from([a])),
        );
        assert_eq!(
            super::compute_preferred_ia_count(&got_addresses, &configured_non_temporary_addresses),
            want,
        );
    }

    #[test_case(&CONFIGURED_NON_TEMPORARY_ADDRESSES[0..2], &CONFIGURED_DELEGATED_PREFIXES[0..2], true)]
    #[test_case(&CONFIGURED_NON_TEMPORARY_ADDRESSES[0..1], &CONFIGURED_DELEGATED_PREFIXES[0..1], true)]
    #[test_case(&REPLY_NON_TEMPORARY_ADDRESSES[0..2], &REPLY_DELEGATED_PREFIXES[0..2], true)]
    #[test_case(&[], &[], false)]
    fn advertise_message_has_ias(
        non_temporary_addresses: &[Ipv6Addr],
        delegated_prefixes: &[Subnet<Ipv6Addr>],
        expected: bool,
    ) {
        let configured_non_temporary_addresses = testutil::to_configured_addresses(
            2,
            std::iter::once(HashSet::from([CONFIGURED_NON_TEMPORARY_ADDRESSES[0]])),
        );

        let configured_delegated_prefixes = testutil::to_configured_prefixes(
            2,
            std::iter::once(HashSet::from([CONFIGURED_DELEGATED_PREFIXES[0]])),
        );

        // Advertise is acceptable even though it does not contain the solicited
        // preferred address.
        let advertise = AdvertiseMessage::new_default(
            SERVER_ID[0],
            non_temporary_addresses,
            delegated_prefixes,
            &[],
            &configured_non_temporary_addresses,
            &configured_delegated_prefixes,
        );
        assert_eq!(advertise.has_ias(), expected);
    }

    struct AdvertiseMessageOrdTestCase<'a> {
        adv1_non_temporary_addresses: &'a [Ipv6Addr],
        adv1_delegated_prefixes: &'a [Subnet<Ipv6Addr>],
        adv2_non_temporary_addresses: &'a [Ipv6Addr],
        adv2_delegated_prefixes: &'a [Subnet<Ipv6Addr>],
        expected: Ordering,
    }

    #[test_case(AdvertiseMessageOrdTestCase{
        adv1_non_temporary_addresses: &CONFIGURED_NON_TEMPORARY_ADDRESSES[0..2],
        adv1_delegated_prefixes: &CONFIGURED_DELEGATED_PREFIXES[0..2],
        adv2_non_temporary_addresses: &CONFIGURED_NON_TEMPORARY_ADDRESSES[0..3],
        adv2_delegated_prefixes: &CONFIGURED_DELEGATED_PREFIXES[0..3],
        expected: Ordering::Less,
    }; "adv1 has less IAs")]
    #[test_case(AdvertiseMessageOrdTestCase{
        adv1_non_temporary_addresses: &CONFIGURED_NON_TEMPORARY_ADDRESSES[0..2],
        adv1_delegated_prefixes: &CONFIGURED_DELEGATED_PREFIXES[0..2],
        adv2_non_temporary_addresses: &CONFIGURED_NON_TEMPORARY_ADDRESSES[1..3],
        adv2_delegated_prefixes: &CONFIGURED_DELEGATED_PREFIXES[1..3],
        expected: Ordering::Greater,
    }; "adv1 has IAs matching hint")]
    #[test_case(AdvertiseMessageOrdTestCase{
        adv1_non_temporary_addresses: &[],
        adv1_delegated_prefixes: &CONFIGURED_DELEGATED_PREFIXES[0..3],
        adv2_non_temporary_addresses: &CONFIGURED_NON_TEMPORARY_ADDRESSES[0..1],
        adv2_delegated_prefixes: &CONFIGURED_DELEGATED_PREFIXES[0..1],
        expected: Ordering::Less,
    }; "adv1 missing IA_NA")]
    #[test_case(AdvertiseMessageOrdTestCase{
        adv1_non_temporary_addresses: &CONFIGURED_NON_TEMPORARY_ADDRESSES[0..3],
        adv1_delegated_prefixes: &CONFIGURED_DELEGATED_PREFIXES[0..1],
        adv2_non_temporary_addresses: &CONFIGURED_NON_TEMPORARY_ADDRESSES[0..3],
        adv2_delegated_prefixes: &[],
        expected: Ordering::Greater,
    }; "adv2 missing IA_PD")]
    fn advertise_message_ord(
        AdvertiseMessageOrdTestCase {
            adv1_non_temporary_addresses,
            adv1_delegated_prefixes,
            adv2_non_temporary_addresses,
            adv2_delegated_prefixes,
            expected,
        }: AdvertiseMessageOrdTestCase<'_>,
    ) {
        let configured_non_temporary_addresses = testutil::to_configured_addresses(
            3,
            std::iter::once(HashSet::from([CONFIGURED_NON_TEMPORARY_ADDRESSES[0]])),
        );

        let configured_delegated_prefixes = testutil::to_configured_prefixes(
            3,
            std::iter::once(HashSet::from([CONFIGURED_DELEGATED_PREFIXES[0]])),
        );

        let advertise1 = AdvertiseMessage::new_default(
            SERVER_ID[0],
            adv1_non_temporary_addresses,
            adv1_delegated_prefixes,
            &[],
            &configured_non_temporary_addresses,
            &configured_delegated_prefixes,
        );
        let advertise2 = AdvertiseMessage::new_default(
            SERVER_ID[1],
            adv2_non_temporary_addresses,
            adv2_delegated_prefixes,
            &[],
            &configured_non_temporary_addresses,
            &configured_delegated_prefixes,
        );
        assert_eq!(advertise1.cmp(&advertise2), expected);
    }

    #[test_case(v6::DhcpOption::StatusCode(v6::StatusCode::Success.into(), ""); "status_code")]
    #[test_case(v6::DhcpOption::ClientId(&CLIENT_ID); "client_id")]
    #[test_case(v6::DhcpOption::ServerId(&SERVER_ID[0]); "server_id")]
    #[test_case(v6::DhcpOption::Preference(ADVERTISE_MAX_PREFERENCE); "preference")]
    #[test_case(v6::DhcpOption::SolMaxRt(*VALID_MAX_SOLICIT_TIMEOUT_RANGE.end()); "sol_max_rt")]
    #[test_case(v6::DhcpOption::DnsServers(&DNS_SERVERS); "dns_servers")]
    fn process_options_duplicates<'a>(opt: v6::DhcpOption<'a>) {
        let client_id = v6::duid_uuid();
        let iana_options = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
            CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
            60,
            60,
            &[],
        ))];
        let iaid = v6::IAID::new(0);
        let options = [
            v6::DhcpOption::StatusCode(v6::StatusCode::Success.into(), ""),
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::Preference(ADVERTISE_MAX_PREFERENCE),
            v6::DhcpOption::SolMaxRt(*VALID_MAX_SOLICIT_TIMEOUT_RANGE.end()),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(iaid, T1.get(), T2.get(), &iana_options)),
            v6::DhcpOption::DnsServers(&DNS_SERVERS),
            opt,
        ];
        let builder = v6::MessageBuilder::new(v6::MessageType::Advertise, [0, 1, 2], &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let requested_ia_nas = HashMap::from([(iaid, None::<Ipv6Addr>)]);
        assert_matches!(
            process_options(
                &msg,
                ExchangeType::AdvertiseToSolicit,
                Some(client_id),
                &requested_ia_nas,
                &NoIaRequested
            ),
            Err(OptionsError::DuplicateOption(_, _, _))
        );
    }

    #[derive(Copy, Clone)]
    enum DupIaValue {
        Address,
        Prefix,
    }

    impl DupIaValue {
        fn second_address(self) -> Ipv6Addr {
            match self {
                DupIaValue::Address => CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
                DupIaValue::Prefix => CONFIGURED_NON_TEMPORARY_ADDRESSES[1],
            }
        }

        fn second_prefix(self) -> Subnet<Ipv6Addr> {
            match self {
                DupIaValue::Address => CONFIGURED_DELEGATED_PREFIXES[1],
                DupIaValue::Prefix => CONFIGURED_DELEGATED_PREFIXES[0],
            }
        }
    }

    #[test_case(
        DupIaValue::Address,
        |res| {
            assert_matches!(
                res,
                Err(OptionsError::IaNaError(IaOptionError::DuplicateIaValue {
                    value,
                    first_lifetimes,
                    second_lifetimes,
                })) => {
                    assert_eq!(value, CONFIGURED_NON_TEMPORARY_ADDRESSES[0]);
                    (first_lifetimes, second_lifetimes)
                }
            )
        }; "duplicate address")]
    #[test_case(
        DupIaValue::Prefix,
        |res| {
            assert_matches!(
                res,
                Err(OptionsError::IaPdError(IaPdOptionError::IaOptionError(
                    IaOptionError::DuplicateIaValue {
                        value,
                        first_lifetimes,
                        second_lifetimes,
                    }
                ))) => {
                    assert_eq!(value, CONFIGURED_DELEGATED_PREFIXES[0]);
                    (first_lifetimes, second_lifetimes)
                }
            )
        }; "duplicate prefix")]
    fn process_options_duplicate_ia_value(
        dup_ia_value: DupIaValue,
        check: fn(
            Result<ProcessedOptions, OptionsError>,
        )
            -> (Result<Lifetimes, LifetimesError>, Result<Lifetimes, LifetimesError>),
    ) {
        const IA_VALUE1_LIFETIME: v6::NonZeroOrMaxU32 =
            const_unwrap::const_unwrap_option(v6::NonZeroOrMaxU32::new(60));
        const IA_VALUE2_LIFETIME: v6::NonZeroOrMaxU32 =
            const_unwrap::const_unwrap_option(v6::NonZeroOrMaxU32::new(100));
        let iana_options = [
            v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
                CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
                IA_VALUE1_LIFETIME.get(),
                IA_VALUE1_LIFETIME.get(),
                &[],
            )),
            v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
                dup_ia_value.second_address(),
                IA_VALUE2_LIFETIME.get(),
                IA_VALUE2_LIFETIME.get(),
                &[],
            )),
        ];
        let iapd_options = [
            v6::DhcpOption::IaPrefix(v6::IaPrefixSerializer::new(
                IA_VALUE1_LIFETIME.get(),
                IA_VALUE1_LIFETIME.get(),
                CONFIGURED_DELEGATED_PREFIXES[0],
                &[],
            )),
            v6::DhcpOption::IaPrefix(v6::IaPrefixSerializer::new(
                IA_VALUE2_LIFETIME.get(),
                IA_VALUE2_LIFETIME.get(),
                dup_ia_value.second_prefix(),
                &[],
            )),
        ];
        let iaid = v6::IAID::new(0);
        let options = [
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(iaid, T1.get(), T2.get(), &iana_options)),
            v6::DhcpOption::IaPd(v6::IaPdSerializer::new(iaid, T1.get(), T2.get(), &iapd_options)),
        ];
        let builder = v6::MessageBuilder::new(v6::MessageType::Advertise, [0, 1, 2], &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let requested_ia_nas = HashMap::from([(iaid, None::<Ipv6Addr>)]);
        let (first_lifetimes, second_lifetimes) = check(process_options(
            &msg,
            ExchangeType::AdvertiseToSolicit,
            Some(CLIENT_ID),
            &requested_ia_nas,
            &NoIaRequested,
        ));
        assert_eq!(
            first_lifetimes,
            Ok(Lifetimes::new_finite(IA_VALUE1_LIFETIME, IA_VALUE1_LIFETIME))
        );
        assert_eq!(
            second_lifetimes,
            Ok(Lifetimes::new_finite(IA_VALUE2_LIFETIME, IA_VALUE2_LIFETIME))
        )
    }

    #[test]
    fn process_options_t1_greather_than_t2() {
        let iana_options1 = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
            CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
            MEDIUM_NON_ZERO_OR_MAX_U32.get(),
            MEDIUM_NON_ZERO_OR_MAX_U32.get(),
            &[],
        ))];
        let iana_options2 = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
            CONFIGURED_NON_TEMPORARY_ADDRESSES[1],
            MEDIUM_NON_ZERO_OR_MAX_U32.get(),
            MEDIUM_NON_ZERO_OR_MAX_U32.get(),
            &[],
        ))];
        let iapd_options1 = [v6::DhcpOption::IaPrefix(v6::IaPrefixSerializer::new(
            LARGE_NON_ZERO_OR_MAX_U32.get(),
            LARGE_NON_ZERO_OR_MAX_U32.get(),
            CONFIGURED_DELEGATED_PREFIXES[0],
            &[],
        ))];
        let iapd_options2 = [v6::DhcpOption::IaPrefix(v6::IaPrefixSerializer::new(
            LARGE_NON_ZERO_OR_MAX_U32.get(),
            LARGE_NON_ZERO_OR_MAX_U32.get(),
            CONFIGURED_DELEGATED_PREFIXES[1],
            &[],
        ))];

        let iaid1 = v6::IAID::new(1);
        let iaid2 = v6::IAID::new(2);
        let options = [
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                iaid1,
                MEDIUM_NON_ZERO_OR_MAX_U32.get(),
                SMALL_NON_ZERO_OR_MAX_U32.get(),
                &iana_options1,
            )),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                iaid2,
                SMALL_NON_ZERO_OR_MAX_U32.get(),
                MEDIUM_NON_ZERO_OR_MAX_U32.get(),
                &iana_options2,
            )),
            v6::DhcpOption::IaPd(v6::IaPdSerializer::new(
                iaid1,
                LARGE_NON_ZERO_OR_MAX_U32.get(),
                TINY_NON_ZERO_OR_MAX_U32.get(),
                &iapd_options1,
            )),
            v6::DhcpOption::IaPd(v6::IaPdSerializer::new(
                iaid2,
                TINY_NON_ZERO_OR_MAX_U32.get(),
                LARGE_NON_ZERO_OR_MAX_U32.get(),
                &iapd_options2,
            )),
        ];
        let builder = v6::MessageBuilder::new(v6::MessageType::Advertise, [0, 1, 2], &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let requested_ia_nas = HashMap::from([(iaid1, None::<Ipv6Addr>), (iaid2, None)]);
        let requested_ia_pds = HashMap::from([(iaid1, None::<Subnet<Ipv6Addr>>), (iaid2, None)]);
        assert_matches!(
            process_options(&msg, ExchangeType::AdvertiseToSolicit, Some(CLIENT_ID), &requested_ia_nas, &requested_ia_pds),
            Ok(ProcessedOptions {
                server_id: _,
                solicit_max_rt_opt: _,
                result: Ok(Options {
                    success_status_message: _,
                    next_contact_time: _,
                    non_temporary_addresses,
                    delegated_prefixes,
                    dns_servers: _,
                    preference: _,
                }),
            }) => {
                assert_eq!(non_temporary_addresses, HashMap::from([(iaid2, IaOption::Success {
                    status_message: None,
                    t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(SMALL_NON_ZERO_OR_MAX_U32)),
                    t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(MEDIUM_NON_ZERO_OR_MAX_U32)),
                    ia_values: HashMap::from([(CONFIGURED_NON_TEMPORARY_ADDRESSES[1], Ok(Lifetimes{
                        preferred_lifetime: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(MEDIUM_NON_ZERO_OR_MAX_U32)),
                        valid_lifetime: v6::NonZeroTimeValue::Finite(MEDIUM_NON_ZERO_OR_MAX_U32),
                    }))]),
                })]));
                assert_eq!(delegated_prefixes, HashMap::from([(iaid2, IaOption::Success {
                    status_message: None,
                    t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(TINY_NON_ZERO_OR_MAX_U32)),
                    t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(LARGE_NON_ZERO_OR_MAX_U32)),
                    ia_values: HashMap::from([(CONFIGURED_DELEGATED_PREFIXES[1], Ok(Lifetimes{
                        preferred_lifetime: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(LARGE_NON_ZERO_OR_MAX_U32)),
                        valid_lifetime: v6::NonZeroTimeValue::Finite(LARGE_NON_ZERO_OR_MAX_U32),
                    }))]),
                })]));
            }
        );
    }

    #[test]
    fn process_options_duplicate_ia_na_id() {
        let iana_options = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
            CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
            60,
            60,
            &[],
        ))];
        let iaid = v6::IAID::new(0);
        let options = [
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(iaid, T1.get(), T2.get(), &iana_options)),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(iaid, T1.get(), T2.get(), &iana_options)),
        ];
        let builder = v6::MessageBuilder::new(v6::MessageType::Advertise, [0, 1, 2], &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let requested_ia_nas = HashMap::from([(iaid, None::<Ipv6Addr>)]);
        assert_matches!(
            process_options(&msg, ExchangeType::AdvertiseToSolicit, Some(CLIENT_ID), &requested_ia_nas, &NoIaRequested),
            Err(OptionsError::DuplicateIaNaId(got_iaid, _, _)) if got_iaid == iaid
        );
    }

    #[test]
    fn process_options_missing_server_id() {
        let options = [v6::DhcpOption::ClientId(&CLIENT_ID)];
        let builder = v6::MessageBuilder::new(v6::MessageType::Advertise, [0, 1, 2], &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        assert_matches!(
            process_options(
                &msg,
                ExchangeType::AdvertiseToSolicit,
                Some(CLIENT_ID),
                &NoIaRequested,
                &NoIaRequested
            ),
            Err(OptionsError::MissingServerId)
        );
    }

    #[test]
    fn process_options_missing_client_id() {
        let options = [v6::DhcpOption::ServerId(&SERVER_ID[0])];
        let builder = v6::MessageBuilder::new(v6::MessageType::Advertise, [0, 1, 2], &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        assert_matches!(
            process_options(
                &msg,
                ExchangeType::AdvertiseToSolicit,
                Some(CLIENT_ID),
                &NoIaRequested,
                &NoIaRequested
            ),
            Err(OptionsError::MissingClientId)
        );
    }

    #[test]
    fn process_options_mismatched_client_id() {
        let options = [
            v6::DhcpOption::ClientId(&MISMATCHED_CLIENT_ID),
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
        ];
        let builder = v6::MessageBuilder::new(v6::MessageType::Advertise, [0, 1, 2], &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        assert_matches!(
            process_options(&msg, ExchangeType::AdvertiseToSolicit, Some(CLIENT_ID), &NoIaRequested, &NoIaRequested),
            Err(OptionsError::MismatchedClientId { got, want })
                if got[..] == MISMATCHED_CLIENT_ID && want == CLIENT_ID
        );
    }

    #[test]
    fn process_options_unexpected_client_id() {
        let options =
            [v6::DhcpOption::ClientId(&CLIENT_ID), v6::DhcpOption::ServerId(&SERVER_ID[0])];
        let builder = v6::MessageBuilder::new(v6::MessageType::Reply, [0, 1, 2], &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        assert_matches!(
            process_options(&msg, ExchangeType::ReplyToInformationRequest, None, &NoIaRequested, &NoIaRequested),
            Err(OptionsError::UnexpectedClientId(got))
                if got[..] == CLIENT_ID
        );
    }

    #[test_case(
        v6::MessageType::Reply,
        ExchangeType::ReplyToInformationRequest,
        v6::DhcpOption::Preference(ADVERTISE_MAX_PREFERENCE);
        "reply_to_information_request_preference"
    )]
    #[test_case(
        v6::MessageType::Reply,
        ExchangeType::ReplyToInformationRequest,
        v6::DhcpOption::Iana(v6::IanaSerializer::new(v6::IAID::new(0), T1.get(),T2.get(), &[]));
        "reply_to_information_request_ia_na"
    )]
    #[test_case(
        v6::MessageType::Advertise,
        ExchangeType::AdvertiseToSolicit,
        v6::DhcpOption::InformationRefreshTime(42u32);
        "advertise_to_solicit_information_refresh_time"
    )]
    #[test_case(
        v6::MessageType::Reply,
        ExchangeType::ReplyWithLeases(RequestLeasesMessageType::Request),
        v6::DhcpOption::Preference(ADVERTISE_MAX_PREFERENCE);
        "reply_to_request_preference"
    )]
    fn process_options_invalid<'a>(
        message_type: v6::MessageType,
        exchange_type: ExchangeType,
        opt: v6::DhcpOption<'a>,
    ) {
        let options =
            [v6::DhcpOption::ClientId(&CLIENT_ID), v6::DhcpOption::ServerId(&SERVER_ID[0]), opt];
        let builder = v6::MessageBuilder::new(message_type, [0, 1, 2], &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        assert_matches!(
            process_options(&msg, exchange_type, Some(CLIENT_ID), &NoIaRequested, &NoIaRequested),
            Err(OptionsError::InvalidOption(_))
        );
    }

    mod process_reply_with_leases_unexpected_iaid {
        use super::*;

        use test_case::test_case;

        const EXPECTED_IAID: v6::IAID = v6::IAID::new(1);
        const UNEXPECTED_IAID: v6::IAID = v6::IAID::new(2);

        struct TestCase {
            assigned_addresses: fn(Instant) -> HashMap<v6::IAID, AddressEntry<Instant>>,
            assigned_prefixes: fn(Instant) -> HashMap<v6::IAID, PrefixEntry<Instant>>,
            check_res: fn(Result<ProcessedReplyWithLeases<Instant>, ReplyWithLeasesError>),
        }

        fn expected_iaids<V: IaValueTestExt>(
            time: Instant,
        ) -> HashMap<v6::IAID, IaEntry<V, Instant>> {
            HashMap::from([(
                EXPECTED_IAID,
                IaEntry::new_assigned(V::CONFIGURED[0], PREFERRED_LIFETIME, VALID_LIFETIME, time),
            )])
        }

        fn unexpected_iaids<V: IaValueTestExt>(
            time: Instant,
        ) -> HashMap<v6::IAID, IaEntry<V, Instant>> {
            [EXPECTED_IAID, UNEXPECTED_IAID]
                .into_iter()
                .enumerate()
                .map(|(i, iaid)| {
                    (
                        iaid,
                        IaEntry::new_assigned(
                            V::CONFIGURED[i],
                            PREFERRED_LIFETIME,
                            VALID_LIFETIME,
                            time,
                        ),
                    )
                })
                .collect()
        }

        #[test_case(
            TestCase {
                assigned_addresses: expected_iaids::<Ipv6Addr>,
                assigned_prefixes: unexpected_iaids::<Subnet<Ipv6Addr>>,
                check_res: |res| {
                    assert_matches!(
                        res,
                        Err(ReplyWithLeasesError::OptionsError(
                            OptionsError::UnexpectedIaNa(iaid, _),
                        )) => {
                            assert_eq!(iaid, UNEXPECTED_IAID);
                        }
                    );
                },
            }
        ; "unknown IA_NA IAID")]
        #[test_case(
            TestCase {
                assigned_addresses: unexpected_iaids::<Ipv6Addr>,
                assigned_prefixes: expected_iaids::<Subnet<Ipv6Addr>>,
                check_res: |res| {
                    assert_matches!(
                        res,
                        Err(ReplyWithLeasesError::OptionsError(
                            OptionsError::UnexpectedIaPd(iaid, _),
                        )) => {
                            assert_eq!(iaid, UNEXPECTED_IAID);
                        }
                    );
                },
            }
        ; "unknown IA_PD IAID")]
        fn test(TestCase { assigned_addresses, assigned_prefixes, check_res }: TestCase) {
            let options =
                [v6::DhcpOption::ClientId(&CLIENT_ID), v6::DhcpOption::ServerId(&SERVER_ID[0])]
                    .into_iter()
                    .chain([EXPECTED_IAID, UNEXPECTED_IAID].into_iter().map(|iaid| {
                        v6::DhcpOption::Iana(v6::IanaSerializer::new(iaid, T1.get(), T2.get(), &[]))
                    }))
                    .chain([EXPECTED_IAID, UNEXPECTED_IAID].into_iter().map(|iaid| {
                        v6::DhcpOption::IaPd(v6::IaPdSerializer::new(iaid, T1.get(), T2.get(), &[]))
                    }))
                    .collect::<Vec<_>>();
            let builder =
                v6::MessageBuilder::new(v6::MessageType::Reply, [0, 1, 2], options.as_slice());
            let mut buf = vec![0; builder.bytes_len()];
            builder.serialize(&mut buf);
            let mut buf = &buf[..]; // Implements BufferView.
            let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");

            let mut solicit_max_rt = MAX_SOLICIT_TIMEOUT;
            let time = Instant::now();
            check_res(process_reply_with_leases(
                CLIENT_ID,
                &SERVER_ID[0],
                &assigned_addresses(time),
                &assigned_prefixes(time),
                &mut solicit_max_rt,
                &msg,
                RequestLeasesMessageType::Request,
                time,
            ))
        }
    }

    #[test]
    fn ignore_advertise_with_unknown_ia() {
        let time = Instant::now();
        let mut client = testutil::start_and_assert_server_discovery(
            [0, 1, 2],
            CLIENT_ID,
            testutil::to_configured_addresses(
                1,
                std::iter::once(HashSet::from([CONFIGURED_NON_TEMPORARY_ADDRESSES[0]])),
            ),
            Default::default(),
            Vec::new(),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );

        let iana_options_0 = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
            CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
            60,
            60,
            &[],
        ))];
        let iana_options_99 = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
            CONFIGURED_NON_TEMPORARY_ADDRESSES[1],
            60,
            60,
            &[],
        ))];
        let options = [
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::Preference(42),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                v6::IAID::new(0),
                T1.get(),
                T2.get(),
                &iana_options_0,
            )),
            // An IA_NA with an IAID that was not included in the sent solicit
            // message.
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                v6::IAID::new(99),
                T1.get(),
                T2.get(),
                &iana_options_99,
            )),
        ];

        let ClientStateMachine { transaction_id, options_to_request: _, state: _, rng: _ } =
            &client;
        let builder =
            v6::MessageBuilder::new(v6::MessageType::Advertise, *transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");

        // The client should have dropped the Advertise with the unrecognized
        // IA_NA IAID.
        assert_eq!(client.handle_message_receive(msg, time), []);
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
            &client;
        assert_matches!(
            state,
            Some(ClientState::ServerDiscovery(ServerDiscovery {
                client_id: _,
                configured_non_temporary_addresses: _,
                configured_delegated_prefixes: _,
                first_solicit_time: _,
                retrans_timeout: _,
                solicit_max_rt: _,
                collected_advertise,
                collected_sol_max_rt: _,
            })) => {
                assert!(collected_advertise.is_empty(), "{:?}", collected_advertise);
            }
        );
    }

    #[test]
    fn receive_advertise_with_max_preference() {
        let time = Instant::now();
        let mut client = testutil::start_and_assert_server_discovery(
            [0, 1, 2],
            CLIENT_ID,
            testutil::to_configured_addresses(
                2,
                std::iter::once(HashSet::from([CONFIGURED_NON_TEMPORARY_ADDRESSES[0]])),
            ),
            Default::default(),
            Vec::new(),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );

        let iana_options = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
            CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
            60,
            60,
            &[],
        ))];

        // The client should stay in ServerDiscovery when it gets an Advertise
        // with:
        //   - Preference < 255 & and at least one IA, or...
        //   - Preference == 255 but no IAs
        for (preference, iana) in [
            (
                42,
                Some(v6::DhcpOption::Iana(v6::IanaSerializer::new(
                    v6::IAID::new(0),
                    T1.get(),
                    T2.get(),
                    &iana_options,
                ))),
            ),
            (255, None),
        ]
        .into_iter()
        {
            let options = [
                v6::DhcpOption::ClientId(&CLIENT_ID),
                v6::DhcpOption::ServerId(&SERVER_ID[0]),
                v6::DhcpOption::Preference(preference),
            ]
            .into_iter()
            .chain(iana)
            .collect::<Vec<_>>();
            let ClientStateMachine { transaction_id, options_to_request: _, state: _, rng: _ } =
                &client;
            let builder =
                v6::MessageBuilder::new(v6::MessageType::Advertise, *transaction_id, &options);
            let mut buf = vec![0; builder.bytes_len()];
            builder.serialize(&mut buf);
            let mut buf = &buf[..]; // Implements BufferView.
            let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
            assert_eq!(client.handle_message_receive(msg, time), []);
        }
        let iana_options = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
            CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
            60,
            60,
            &[],
        ))];
        let options = [
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::Preference(255),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                v6::IAID::new(0),
                T1.get(),
                T2.get(),
                &iana_options,
            )),
        ];
        let ClientStateMachine { transaction_id, options_to_request: _, state: _, rng: _ } =
            &client;
        let builder =
            v6::MessageBuilder::new(v6::MessageType::Advertise, *transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");

        // The client should transition to Requesting when receiving a complete
        // advertise with preference 255.
        let actions = client.handle_message_receive(msg, time);
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } = client;
        let Requesting {
            client_id: _,
            non_temporary_addresses: _,
            delegated_prefixes: _,
            server_id: _,
            collected_advertise: _,
            first_request_time: _,
            retrans_timeout: _,
            transmission_count: _,
            solicit_max_rt: _,
        } = assert_matches!(
            state,
            Some(ClientState::Requesting(requesting)) => requesting
        );
        let buf = assert_matches!(
            &actions[..],
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::SendMessage(buf),
                Action::ScheduleTimer(ClientTimerType::Retransmission, instant)
            ] => {
                assert_eq!(*instant, time.add(INITIAL_REQUEST_TIMEOUT));
                buf
            }
        );
        assert_eq!(testutil::msg_type(buf), v6::MessageType::Request);
    }

    // T1 and T2 are non-zero and T1 > T2, the client should ignore this IA_NA option.
    #[test_case(T2.get() + 1, T2.get(), true)]
    #[test_case(INFINITY, T2.get(), true)]
    // T1 > T2, but T2 is zero, the client should process this IA_NA option.
    #[test_case(T1.get(), 0, false)]
    // T1 is zero, the client should process this IA_NA option.
    #[test_case(0, T2.get(), false)]
    // T1 <= T2, the client should process this IA_NA option.
    #[test_case(T1.get(), T2.get(), false)]
    #[test_case(T1.get(), INFINITY, false)]
    #[test_case(INFINITY, INFINITY, false)]
    fn receive_advertise_with_invalid_iana(t1: u32, t2: u32, ignore_iana: bool) {
        let transaction_id = [0, 1, 2];
        let time = Instant::now();
        let mut client = testutil::start_and_assert_server_discovery(
            transaction_id,
            CLIENT_ID,
            testutil::to_configured_addresses(
                1,
                std::iter::once(HashSet::from([CONFIGURED_NON_TEMPORARY_ADDRESSES[0]])),
            ),
            Default::default(),
            Vec::new(),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );

        let iana_options = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
            CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
            PREFERRED_LIFETIME.get(),
            VALID_LIFETIME.get(),
            &[],
        ))];
        let options = [
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(v6::IAID::new(0), t1, t2, &iana_options)),
        ];
        let builder = v6::MessageBuilder::new(v6::MessageType::Advertise, transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");

        assert_matches!(client.handle_message_receive(msg, time)[..], []);
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
            &client;
        let collected_advertise = assert_matches!(
            state,
            Some(ClientState::ServerDiscovery(ServerDiscovery {
                client_id: _,
                configured_non_temporary_addresses: _,
                configured_delegated_prefixes: _,
                first_solicit_time: _,
                retrans_timeout: _,
                solicit_max_rt: _,
                collected_advertise,
                collected_sol_max_rt: _,
            })) => collected_advertise
        );
        match ignore_iana {
            true => assert!(collected_advertise.is_empty(), "{:?}", collected_advertise),
            false => {
                assert_matches!(
                    collected_advertise.peek(),
                    Some(AdvertiseMessage {
                        server_id: _,
                        non_temporary_addresses,
                        delegated_prefixes: _,
                        dns_servers: _,
                        preference: _,
                        receive_time: _,
                        preferred_non_temporary_addresses_count: _,
                        preferred_delegated_prefixes_count: _,
                    }) => {
                        assert_eq!(
                            non_temporary_addresses,
                            &HashMap::from([(
                                v6::IAID::new(0),
                                HashSet::from([CONFIGURED_NON_TEMPORARY_ADDRESSES[0]])
                            )])
                        );
                    }
                )
            }
        }
    }

    #[test]
    fn select_first_server_while_retransmitting() {
        let time = Instant::now();
        let mut client = testutil::start_and_assert_server_discovery(
            [0, 1, 2],
            CLIENT_ID,
            testutil::to_configured_addresses(
                1,
                std::iter::once(HashSet::from([CONFIGURED_NON_TEMPORARY_ADDRESSES[0]])),
            ),
            Default::default(),
            Vec::new(),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );

        // On transmission timeout, if no advertise were received the client
        // should stay in server discovery and resend solicit.
        let actions = client.handle_timeout(ClientTimerType::Retransmission, time);
        assert_matches!(
            &actions[..],
            [
                Action::SendMessage(buf),
                Action::ScheduleTimer(ClientTimerType::Retransmission, instant)
            ] => {
                assert_eq!(testutil::msg_type(buf), v6::MessageType::Solicit);
                assert_eq!(*instant, time.add(2 * INITIAL_SOLICIT_TIMEOUT));
                buf
            }
        );
        let ClientStateMachine { transaction_id, options_to_request: _, state, rng: _ } = &client;
        {
            let ServerDiscovery {
                client_id: _,
                configured_non_temporary_addresses: _,
                configured_delegated_prefixes: _,
                first_solicit_time: _,
                retrans_timeout: _,
                solicit_max_rt: _,
                collected_advertise,
                collected_sol_max_rt: _,
            } = assert_matches!(
                state,
                Some(ClientState::ServerDiscovery(server_discovery)) => server_discovery
            );
            assert!(collected_advertise.is_empty(), "{:?}", collected_advertise);
        }

        let iana_options = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
            CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
            60,
            60,
            &[],
        ))];
        let options = [
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                v6::IAID::new(0),
                T1.get(),
                T2.get(),
                &iana_options,
            )),
        ];
        let builder =
            v6::MessageBuilder::new(v6::MessageType::Advertise, *transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");

        // The client should transition to Requesting when receiving any
        // advertise while retransmitting.
        let actions = client.handle_message_receive(msg, time);
        assert_matches!(
            &actions[..],
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::SendMessage(buf),
                Action::ScheduleTimer(ClientTimerType::Retransmission, instant)
            ] => {
                assert_eq!(*instant, time.add(INITIAL_REQUEST_TIMEOUT));
                assert_eq!(testutil::msg_type(buf), v6::MessageType::Request);
        }
        );
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } = client;
        let Requesting {
            client_id: _,
            non_temporary_addresses: _,
            delegated_prefixes: _,
            server_id: _,
            collected_advertise,
            first_request_time: _,
            retrans_timeout: _,
            transmission_count: _,
            solicit_max_rt: _,
        } = assert_matches!(
            state,
            Some(ClientState::Requesting(requesting )) => requesting
        );
        assert!(collected_advertise.is_empty(), "{:?}", collected_advertise);
    }

    #[test]
    fn send_request() {
        let (mut _client, _transaction_id) = testutil::request_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            CONFIGURED_NON_TEMPORARY_ADDRESSES.into_iter().map(TestIaNa::new_default).collect(),
            CONFIGURED_DELEGATED_PREFIXES.into_iter().map(TestIaPd::new_default).collect(),
            &[],
            StepRng::new(std::u64::MAX / 2, 0),
            Instant::now(),
        );
    }

    // TODO(https://fxbug.dev/109224): Refactor this test into independent test cases.
    #[test]
    fn requesting_receive_reply_with_failure_status_code() {
        let options_to_request = vec![];
        let configured_non_temporary_addresses = testutil::to_configured_addresses(1, vec![]);
        let advertised_non_temporary_addresses = [CONFIGURED_NON_TEMPORARY_ADDRESSES[0]];
        let configured_delegated_prefixes = HashMap::new();
        let mut want_collected_advertise = [
            AdvertiseMessage::new_default(
                SERVER_ID[1],
                &CONFIGURED_NON_TEMPORARY_ADDRESSES[1..=1],
                &[],
                &[],
                &configured_non_temporary_addresses,
                &configured_delegated_prefixes,
            ),
            AdvertiseMessage::new_default(
                SERVER_ID[2],
                &CONFIGURED_NON_TEMPORARY_ADDRESSES[2..=2],
                &[],
                &[],
                &configured_non_temporary_addresses,
                &configured_delegated_prefixes,
            ),
        ]
        .into_iter()
        .collect::<BinaryHeap<_>>();
        let mut rng = StepRng::new(std::u64::MAX / 2, 0);

        let time = Instant::now();
        let Transition { state, actions: _, transaction_id } = Requesting::start(
            CLIENT_ID,
            SERVER_ID[0].to_vec(),
            advertise_to_ia_entries(
                testutil::to_default_ias_map(&advertised_non_temporary_addresses),
                configured_non_temporary_addresses.clone(),
            ),
            Default::default(), /* delegated_prefixes */
            &options_to_request[..],
            want_collected_advertise.clone(),
            MAX_SOLICIT_TIMEOUT,
            &mut rng,
            time,
        );

        let expected_non_temporary_addresses = (0..)
            .map(v6::IAID::new)
            .zip(
                advertised_non_temporary_addresses
                    .iter()
                    .map(|addr| AddressEntry::ToRequest(HashSet::from([*addr]))),
            )
            .collect::<HashMap<v6::IAID, AddressEntry<_>>>();
        {
            let Requesting {
                non_temporary_addresses: got_non_temporary_addresses,
                delegated_prefixes: _,
                server_id,
                collected_advertise,
                client_id: _,
                first_request_time: _,
                retrans_timeout: _,
                transmission_count: _,
                solicit_max_rt: _,
            } = assert_matches!(&state, ClientState::Requesting(requesting) => requesting);
            assert_eq!(server_id[..], SERVER_ID[0]);
            assert_eq!(*got_non_temporary_addresses, expected_non_temporary_addresses);
            assert_eq!(
                collected_advertise.clone().into_sorted_vec(),
                want_collected_advertise.clone().into_sorted_vec()
            );
        }

        // If the reply contains a top level UnspecFail status code, the reply
        // should be ignored.
        let options = [
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                v6::IAID::new(0),
                T1.get(),
                T2.get(),
                &[],
            )),
            v6::DhcpOption::StatusCode(v6::ErrorStatusCode::UnspecFail.into(), ""),
        ];
        let request_transaction_id = transaction_id.unwrap();
        let builder =
            v6::MessageBuilder::new(v6::MessageType::Reply, request_transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let Transition { state, actions, transaction_id: got_transaction_id } =
            state.reply_message_received(&options_to_request, &mut rng, msg, time);
        {
            let Requesting {
                client_id: _,
                non_temporary_addresses: got_non_temporary_addresses,
                delegated_prefixes: _,
                server_id,
                collected_advertise,
                first_request_time: _,
                retrans_timeout: _,
                transmission_count: _,
                solicit_max_rt: _,
            } = assert_matches!(&state, ClientState::Requesting(requesting) => requesting);
            assert_eq!(server_id[..], SERVER_ID[0]);
            assert_eq!(
                collected_advertise.clone().into_sorted_vec(),
                want_collected_advertise.clone().into_sorted_vec()
            );
            assert_eq!(*got_non_temporary_addresses, expected_non_temporary_addresses);
        }
        assert_eq!(got_transaction_id, None);
        assert_eq!(actions[..], []);

        // If the reply contains a top level NotOnLink status code, the
        // request should be resent without specifying any addresses.
        let options = [
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                v6::IAID::new(0),
                T1.get(),
                T2.get(),
                &[],
            )),
            v6::DhcpOption::StatusCode(v6::ErrorStatusCode::NotOnLink.into(), ""),
        ];
        let request_transaction_id = transaction_id.unwrap();
        let builder =
            v6::MessageBuilder::new(v6::MessageType::Reply, request_transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let Transition { state, actions: _, transaction_id } =
            state.reply_message_received(&options_to_request, &mut rng, msg, time);

        let expected_non_temporary_addresses: HashMap<v6::IAID, AddressEntry<_>> =
            HashMap::from([(v6::IAID::new(0), AddressEntry::ToRequest(Default::default()))]);
        {
            let Requesting {
                client_id: _,
                non_temporary_addresses: got_non_temporary_addresses,
                delegated_prefixes: _,
                server_id,
                collected_advertise,
                first_request_time: _,
                retrans_timeout: _,
                transmission_count: _,
                solicit_max_rt: _,
            } = assert_matches!(
                &state,
                ClientState::Requesting(requesting) => requesting
            );
            assert_eq!(server_id[..], SERVER_ID[0]);
            assert_eq!(
                collected_advertise.clone().into_sorted_vec(),
                want_collected_advertise.clone().into_sorted_vec()
            );
            assert_eq!(*got_non_temporary_addresses, expected_non_temporary_addresses);
        }
        assert!(transaction_id.is_some());

        // If the reply contains no usable addresses, the client selects
        // another server and sends a request to it.
        let iana_options =
            [v6::DhcpOption::StatusCode(v6::ErrorStatusCode::NoAddrsAvail.into(), "")];
        let options = [
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                v6::IAID::new(0),
                T1.get(),
                T2.get(),
                &iana_options,
            )),
        ];
        let builder =
            v6::MessageBuilder::new(v6::MessageType::Reply, transaction_id.unwrap(), &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let Transition { state, actions, transaction_id } =
            state.reply_message_received(&options_to_request, &mut rng, msg, time);
        {
            let Requesting {
                server_id,
                collected_advertise,
                client_id: _,
                non_temporary_addresses: _,
                delegated_prefixes: _,
                first_request_time: _,
                retrans_timeout: _,
                transmission_count: _,
                solicit_max_rt: _,
            } = assert_matches!(
                state,
                ClientState::Requesting(requesting) => requesting
            );
            assert_eq!(server_id[..], SERVER_ID[1]);
            let _: Option<AdvertiseMessage<_>> = want_collected_advertise.pop();
            assert_eq!(
                collected_advertise.clone().into_sorted_vec(),
                want_collected_advertise.clone().into_sorted_vec(),
            );
        }
        assert_matches!(
            &actions[..],
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::SendMessage(_buf),
                Action::ScheduleTimer(ClientTimerType::Retransmission, instant)
            ] => {
                assert_eq!(*instant, time.add(INITIAL_REQUEST_TIMEOUT));
            }
        );
        assert!(transaction_id.is_some());
    }

    #[test]
    fn requesting_receive_reply_with_ia_not_on_link() {
        let options_to_request = vec![];
        let configured_non_temporary_addresses = testutil::to_configured_addresses(
            2,
            std::iter::once(HashSet::from([CONFIGURED_NON_TEMPORARY_ADDRESSES[0]])),
        );
        let mut rng = StepRng::new(std::u64::MAX / 2, 0);

        let time = Instant::now();
        let Transition { state, actions: _, transaction_id } = Requesting::start(
            CLIENT_ID,
            SERVER_ID[0].to_vec(),
            advertise_to_ia_entries(
                testutil::to_default_ias_map(&CONFIGURED_NON_TEMPORARY_ADDRESSES[0..2]),
                configured_non_temporary_addresses.clone(),
            ),
            Default::default(), /* delegated_prefixes */
            &options_to_request[..],
            BinaryHeap::new(),
            MAX_SOLICIT_TIMEOUT,
            &mut rng,
            time,
        );

        // If the reply contains an address with status code NotOnLink, the
        // client should request the IAs without specifying any addresses in
        // subsequent messages.
        let iana_options1 = [v6::DhcpOption::StatusCode(v6::ErrorStatusCode::NotOnLink.into(), "")];
        let iana_options2 = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
            CONFIGURED_NON_TEMPORARY_ADDRESSES[1],
            PREFERRED_LIFETIME.get(),
            VALID_LIFETIME.get(),
            &[],
        ))];
        let iaid1 = v6::IAID::new(0);
        let iaid2 = v6::IAID::new(1);
        let options = [
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                iaid1,
                T1.get(),
                T2.get(),
                &iana_options1,
            )),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                iaid2,
                T1.get(),
                T2.get(),
                &iana_options2,
            )),
        ];
        let builder =
            v6::MessageBuilder::new(v6::MessageType::Reply, transaction_id.unwrap(), &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let Transition { state, actions, transaction_id } =
            state.reply_message_received(&options_to_request, &mut rng, msg, time);
        let expected_non_temporary_addresses = HashMap::from([
            (iaid1, AddressEntry::ToRequest(Default::default())),
            (
                iaid2,
                AddressEntry::Assigned(HashMap::from([(
                    CONFIGURED_NON_TEMPORARY_ADDRESSES[1],
                    LifetimesInfo {
                        lifetimes: Lifetimes {
                            preferred_lifetime: v6::TimeValue::NonZero(
                                v6::NonZeroTimeValue::Finite(PREFERRED_LIFETIME),
                            ),
                            valid_lifetime: v6::NonZeroTimeValue::Finite(VALID_LIFETIME),
                        },
                        updated_at: time,
                    },
                )])),
            ),
        ]);
        {
            let Assigned {
                client_id: _,
                non_temporary_addresses,
                delegated_prefixes,
                server_id,
                dns_servers: _,
                solicit_max_rt: _,
                _marker,
            } = assert_matches!(
                state,
                ClientState::Assigned(assigned) => assigned
            );
            assert_eq!(server_id[..], SERVER_ID[0]);
            assert_eq!(non_temporary_addresses, expected_non_temporary_addresses);
            assert_eq!(delegated_prefixes, HashMap::new());
        }
        assert_matches!(
            &actions[..],
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::ScheduleTimer(ClientTimerType::Renew, t1),
                Action::ScheduleTimer(ClientTimerType::Rebind, t2),
                Action::IaNaUpdates(iana_updates),
                Action::ScheduleTimer(ClientTimerType::RestartServerDiscovery, restart_time),
            ] => {
                assert_eq!(*t1, time.add(Duration::from_secs(T1.get().into())));
                assert_eq!(*t2, time.add(Duration::from_secs(T2.get().into())));
                assert_eq!(
                    *restart_time,
                    time.add(Duration::from_secs(VALID_LIFETIME.get().into())),
                );
                assert_eq!(
                    iana_updates,
                    &HashMap::from([(
                        iaid2,
                        HashMap::from([(
                            CONFIGURED_NON_TEMPORARY_ADDRESSES[1],
                            IaValueUpdateKind::Added(Lifetimes::new_default()),
                        )]),
                    )]),
                );
            }
        );
        assert!(transaction_id.is_none());
    }

    #[test_case(0, VALID_LIFETIME.get(), true)]
    #[test_case(PREFERRED_LIFETIME.get(), 0, false)]
    #[test_case(VALID_LIFETIME.get() + 1, VALID_LIFETIME.get(), false)]
    #[test_case(0, 0, false)]
    #[test_case(PREFERRED_LIFETIME.get(), VALID_LIFETIME.get(), true)]
    fn requesting_receive_reply_with_invalid_ia_lifetimes(
        preferred_lifetime: u32,
        valid_lifetime: u32,
        valid_ia: bool,
    ) {
        let options_to_request = vec![];
        let configured_non_temporary_addresses = testutil::to_configured_addresses(1, vec![]);
        let mut rng = StepRng::new(std::u64::MAX / 2, 0);

        let time = Instant::now();
        let Transition { state, actions: _, transaction_id } = Requesting::start(
            CLIENT_ID,
            SERVER_ID[0].to_vec(),
            advertise_to_ia_entries(
                testutil::to_default_ias_map(&CONFIGURED_NON_TEMPORARY_ADDRESSES[0..1]),
                configured_non_temporary_addresses.clone(),
            ),
            Default::default(), /* delegated_prefixes */
            &options_to_request[..],
            BinaryHeap::new(),
            MAX_SOLICIT_TIMEOUT,
            &mut rng,
            time,
        );

        // The client should discard the IAs with invalid lifetimes.
        let iana_options = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
            CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
            preferred_lifetime,
            valid_lifetime,
            &[],
        ))];
        let options = [
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                v6::IAID::new(0),
                T1.get(),
                T2.get(),
                &iana_options,
            )),
        ];
        let builder =
            v6::MessageBuilder::new(v6::MessageType::Reply, transaction_id.unwrap(), &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let Transition { state, actions: _, transaction_id: _ } =
            state.reply_message_received(&options_to_request, &mut rng, msg, time);
        match valid_ia {
            true =>
            // The client should transition to Assigned if the reply contains
            // a valid IA.
            {
                let Assigned {
                    client_id: _,
                    non_temporary_addresses: _,
                    delegated_prefixes: _,
                    server_id: _,
                    dns_servers: _,
                    solicit_max_rt: _,
                    _marker,
                } = assert_matches!(
                    state,
                    ClientState::Assigned(assigned) => assigned
                );
            }
            false =>
            // The client should transition to ServerDiscovery if the reply contains
            // no valid IAs.
            {
                let ServerDiscovery {
                    client_id: _,
                    configured_non_temporary_addresses: _,
                    configured_delegated_prefixes: _,
                    first_solicit_time: _,
                    retrans_timeout: _,
                    solicit_max_rt: _,
                    collected_advertise,
                    collected_sol_max_rt: _,
                } = assert_matches!(
                    state,
                    ClientState::ServerDiscovery(server_discovery) => server_discovery
                );
                assert!(collected_advertise.is_empty(), "{:?}", collected_advertise);
            }
        }
    }

    // Test that T1/T2 are calculated correctly on receiving a Reply to Request.
    #[test]
    fn compute_t1_t2_on_reply_to_request() {
        let mut rng = StepRng::new(std::u64::MAX / 2, 0);

        for (
            (ia1_preferred_lifetime, ia1_valid_lifetime, ia1_t1, ia1_t2),
            (ia2_preferred_lifetime, ia2_valid_lifetime, ia2_t1, ia2_t2),
            expected_t1,
            expected_t2,
        ) in vec![
            // If T1/T2 are 0, they should be computed as as 0.5 * minimum
            // preferred lifetime, and 0.8 * minimum preferred lifetime
            // respectively.
            (
                (100, 160, 0, 0),
                (120, 180, 0, 0),
                v6::NonZeroTimeValue::Finite(v6::NonZeroOrMaxU32::new(50).expect("should succeed")),
                v6::NonZeroTimeValue::Finite(v6::NonZeroOrMaxU32::new(80).expect("should succeed")),
            ),
            (
                (INFINITY, INFINITY, 0, 0),
                (120, 180, 0, 0),
                v6::NonZeroTimeValue::Finite(v6::NonZeroOrMaxU32::new(60).expect("should succeed")),
                v6::NonZeroTimeValue::Finite(v6::NonZeroOrMaxU32::new(96).expect("should succeed")),
            ),
            // If T1/T2 are 0, and the minimum preferred lifetime, is infinity,
            // T1/T2 should also be infinity.
            (
                (INFINITY, INFINITY, 0, 0),
                (INFINITY, INFINITY, 0, 0),
                v6::NonZeroTimeValue::Infinity,
                v6::NonZeroTimeValue::Infinity,
            ),
            // T2 may be infinite if T1 is finite.
            (
                (INFINITY, INFINITY, 50, INFINITY),
                (INFINITY, INFINITY, 50, INFINITY),
                v6::NonZeroTimeValue::Finite(v6::NonZeroOrMaxU32::new(50).expect("should succeed")),
                v6::NonZeroTimeValue::Infinity,
            ),
            // If T1/T2 are set, and have different values across IAs, T1/T2
            // should be computed as the minimum T1/T2. NOTE: the server should
            // send the same T1/T2 across all IA, but the client should be
            // prepared for the server sending different T1/T2 values.
            (
                (100, 160, 40, 70),
                (120, 180, 50, 80),
                v6::NonZeroTimeValue::Finite(v6::NonZeroOrMaxU32::new(40).expect("should succeed")),
                v6::NonZeroTimeValue::Finite(v6::NonZeroOrMaxU32::new(70).expect("should succeed")),
            ),
        ] {
            let time = Instant::now();
            let Transition { state, actions: _, transaction_id } = Requesting::start(
                CLIENT_ID,
                SERVER_ID[0].to_vec(),
                advertise_to_ia_entries(
                    testutil::to_default_ias_map(&CONFIGURED_NON_TEMPORARY_ADDRESSES[0..2]),
                    testutil::to_configured_addresses(
                        2,
                        std::iter::once(HashSet::from([CONFIGURED_NON_TEMPORARY_ADDRESSES[0]])),
                    ),
                ),
                Default::default(), /* delegated_prefixes */
                &[],
                BinaryHeap::new(),
                MAX_SOLICIT_TIMEOUT,
                &mut rng,
                time,
            );

            let iana_options1 = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
                CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
                ia1_preferred_lifetime,
                ia1_valid_lifetime,
                &[],
            ))];
            let iana_options2 = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
                CONFIGURED_NON_TEMPORARY_ADDRESSES[1],
                ia2_preferred_lifetime,
                ia2_valid_lifetime,
                &[],
            ))];
            let iaid1 = v6::IAID::new(0);
            let iaid2 = v6::IAID::new(1);
            let options = [
                v6::DhcpOption::ServerId(&SERVER_ID[0]),
                v6::DhcpOption::ClientId(&CLIENT_ID),
                v6::DhcpOption::Iana(v6::IanaSerializer::new(
                    iaid1,
                    ia1_t1,
                    ia1_t2,
                    &iana_options1,
                )),
                v6::DhcpOption::Iana(v6::IanaSerializer::new(
                    iaid2,
                    ia2_t1,
                    ia2_t2,
                    &iana_options2,
                )),
            ];
            let builder =
                v6::MessageBuilder::new(v6::MessageType::Reply, transaction_id.unwrap(), &options);
            let mut buf = vec![0; builder.bytes_len()];
            builder.serialize(&mut buf);
            let mut buf = &buf[..]; // Implements BufferView.
            let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
            let Transition { state, actions, transaction_id: _ } =
                state.reply_message_received(&[], &mut rng, msg, time);
            let Assigned {
                client_id: _,
                non_temporary_addresses: _,
                delegated_prefixes: _,
                server_id: _,
                dns_servers: _,
                solicit_max_rt: _,
                _marker,
            } = assert_matches!(
                state,
                ClientState::Assigned(assigned) => assigned
            );

            let update_actions = [Action::IaNaUpdates(HashMap::from([
                (
                    iaid1,
                    HashMap::from([(
                        CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
                        IaValueUpdateKind::Added(Lifetimes::new(
                            ia1_preferred_lifetime,
                            ia1_valid_lifetime,
                        )),
                    )]),
                ),
                (
                    iaid2,
                    HashMap::from([(
                        CONFIGURED_NON_TEMPORARY_ADDRESSES[1],
                        IaValueUpdateKind::Added(Lifetimes::new(
                            ia2_preferred_lifetime,
                            ia2_valid_lifetime,
                        )),
                    )]),
                ),
            ]))];

            let timer_action = |timer, tv| match tv {
                v6::NonZeroTimeValue::Finite(tv) => {
                    Action::ScheduleTimer(timer, time.add(Duration::from_secs(tv.get().into())))
                }
                v6::NonZeroTimeValue::Infinity => Action::CancelTimer(timer),
            };

            let non_zero_time_value = |v| {
                assert_matches!(
                    v6::TimeValue::new(v),
                    v6::TimeValue::NonZero(v) => v
                )
            };

            assert!(expected_t1 <= expected_t2);
            assert_eq!(
                actions,
                [
                    Action::CancelTimer(ClientTimerType::Retransmission),
                    timer_action(ClientTimerType::Renew, expected_t1),
                    timer_action(ClientTimerType::Rebind, expected_t2),
                ]
                .into_iter()
                .chain(update_actions)
                .chain([timer_action(
                    ClientTimerType::RestartServerDiscovery,
                    std::cmp::max(
                        non_zero_time_value(ia1_valid_lifetime),
                        non_zero_time_value(ia2_valid_lifetime),
                    ),
                )])
                .collect::<Vec<_>>(),
            );
        }
    }

    #[test]
    fn use_advertise_from_best_server() {
        let transaction_id = [0, 1, 2];
        let time = Instant::now();
        let mut client = testutil::start_and_assert_server_discovery(
            transaction_id,
            CLIENT_ID,
            testutil::to_configured_addresses(
                CONFIGURED_NON_TEMPORARY_ADDRESSES.len(),
                CONFIGURED_NON_TEMPORARY_ADDRESSES.map(|a| HashSet::from([a])),
            ),
            testutil::to_configured_prefixes(
                CONFIGURED_DELEGATED_PREFIXES.len(),
                CONFIGURED_DELEGATED_PREFIXES.map(|a| HashSet::from([a])),
            ),
            Vec::new(),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );

        // Server0 advertises only IA_NA but all matching our hints.
        let buf = TestMessageBuilder {
            transaction_id,
            message_type: v6::MessageType::Advertise,
            client_id: &CLIENT_ID,
            server_id: &SERVER_ID[0],
            preference: None,
            dns_servers: None,
            ia_nas: (0..)
                .map(v6::IAID::new)
                .zip(CONFIGURED_NON_TEMPORARY_ADDRESSES)
                .map(|(iaid, value)| (iaid, TestIa::new_default(value))),
            ia_pds: std::iter::empty(),
        }
        .build();
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        assert_matches!(client.handle_message_receive(msg, time)[..], []);

        // Server1 advertises only IA_PD but all matching our hints.
        let buf = TestMessageBuilder {
            transaction_id,
            message_type: v6::MessageType::Advertise,
            client_id: &CLIENT_ID,
            server_id: &SERVER_ID[1],
            preference: None,
            dns_servers: None,
            ia_nas: std::iter::empty(),
            ia_pds: (0..)
                .map(v6::IAID::new)
                .zip(CONFIGURED_DELEGATED_PREFIXES)
                .map(|(iaid, value)| (iaid, TestIa::new_default(value))),
        }
        .build();
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        assert_matches!(client.handle_message_receive(msg, time)[..], []);

        // Server2 advertises only a single IA_NA and IA_PD but not matching our
        // hint.
        //
        // This should be the best advertisement the client receives since it
        // allows the client to get the most diverse set of IAs which the client
        // prefers over a large quantity of a single IA type.
        let buf = TestMessageBuilder {
            transaction_id,
            message_type: v6::MessageType::Advertise,
            client_id: &CLIENT_ID,
            server_id: &SERVER_ID[2],
            preference: None,
            dns_servers: None,
            ia_nas: std::iter::once((
                v6::IAID::new(0),
                TestIa {
                    values: HashMap::from([(
                        REPLY_NON_TEMPORARY_ADDRESSES[0],
                        Lifetimes {
                            preferred_lifetime: v6::TimeValue::NonZero(
                                v6::NonZeroTimeValue::Finite(PREFERRED_LIFETIME),
                            ),
                            valid_lifetime: v6::NonZeroTimeValue::Finite(VALID_LIFETIME),
                        },
                    )]),
                    t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T1)),
                    t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T2)),
                },
            )),
            ia_pds: std::iter::once((
                v6::IAID::new(0),
                TestIa {
                    values: HashMap::from([(
                        REPLY_DELEGATED_PREFIXES[0],
                        Lifetimes {
                            preferred_lifetime: v6::TimeValue::NonZero(
                                v6::NonZeroTimeValue::Finite(PREFERRED_LIFETIME),
                            ),
                            valid_lifetime: v6::NonZeroTimeValue::Finite(VALID_LIFETIME),
                        },
                    )]),
                    t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T1)),
                    t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T2)),
                },
            )),
        }
        .build();
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        assert_matches!(client.handle_message_receive(msg, time)[..], []);

        // Handle the retransmission timeout for the first time which should
        // pick a server and transition to requesting with the best server.
        //
        // The best server should be `SERVER_ID[2]` and we should have replaced
        // our hint for IA_NA/IA_PD with IAID == 0 to what was in the server's
        // advertise message. We keep the hints for the other IAIDs since the
        // server did not include those IAID in its advertise so the client will
        // continue to request the hints with the selected server.
        let actions = client.handle_timeout(ClientTimerType::Retransmission, time);
        assert_matches!(
            &actions[..],
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::SendMessage(buf),
                Action::ScheduleTimer(ClientTimerType::Retransmission, instant),
            ] => {
                assert_eq!(testutil::msg_type(buf), v6::MessageType::Request);
                assert_eq!(*instant, time.add(INITIAL_REQUEST_TIMEOUT));
            }
        );
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } = client;
        assert_matches!(
            state,
            Some(ClientState::Requesting(Requesting {
                client_id: _,
                non_temporary_addresses,
                delegated_prefixes,
                server_id,
                collected_advertise: _,
                first_request_time: _,
                retrans_timeout: _,
                transmission_count: _,
                solicit_max_rt: _,
            })) => {
                assert_eq!(&server_id, &SERVER_ID[2]);
                assert_eq!(
                    non_temporary_addresses,
                    [REPLY_NON_TEMPORARY_ADDRESSES[0]]
                        .iter()
                        .chain(CONFIGURED_NON_TEMPORARY_ADDRESSES[1..3].iter())
                        .enumerate().map(|(iaid, addr)| {
                            (v6::IAID::new(iaid.try_into().unwrap()), AddressEntry::ToRequest(HashSet::from([*addr])))
                        }).collect::<HashMap<_, _>>()
                );
                assert_eq!(
                    delegated_prefixes,
                    [REPLY_DELEGATED_PREFIXES[0]]
                        .iter()
                        .chain(CONFIGURED_DELEGATED_PREFIXES[1..3].iter())
                        .enumerate().map(|(iaid, addr)| {
                            (v6::IAID::new(iaid.try_into().unwrap()), PrefixEntry::ToRequest(HashSet::from([*addr])))
                        }).collect::<HashMap<_, _>>()
                );
            }
        );
    }

    // Test that Request retransmission respects max retransmission count.
    #[test]
    fn requesting_retransmit_max_retrans_count() {
        let transaction_id = [0, 1, 2];
        let time = Instant::now();
        let mut client = testutil::start_and_assert_server_discovery(
            transaction_id,
            CLIENT_ID,
            testutil::to_configured_addresses(
                1,
                std::iter::once(HashSet::from([CONFIGURED_NON_TEMPORARY_ADDRESSES[0]])),
            ),
            Default::default(),
            Vec::new(),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );

        for i in 0..2 {
            let buf = TestMessageBuilder {
                transaction_id,
                message_type: v6::MessageType::Advertise,
                client_id: &CLIENT_ID,
                server_id: &SERVER_ID[i],
                preference: None,
                dns_servers: None,
                ia_nas: std::iter::once((
                    v6::IAID::new(0),
                    TestIa::new_default(CONFIGURED_NON_TEMPORARY_ADDRESSES[i]),
                )),
                ia_pds: std::iter::empty(),
            }
            .build();
            let mut buf = &buf[..]; // Implements BufferView.
            let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
            assert_matches!(client.handle_message_receive(msg, time)[..], []);
        }
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
            &client;
        let ServerDiscovery {
            client_id: _,
            configured_non_temporary_addresses: _,
            configured_delegated_prefixes: _,
            first_solicit_time: _,
            retrans_timeout: _,
            solicit_max_rt: _,
            collected_advertise: want_collected_advertise,
            collected_sol_max_rt: _,
        } = assert_matches!(
            state,
            Some(ClientState::ServerDiscovery(server_discovery)) => server_discovery
        );
        let mut want_collected_advertise = want_collected_advertise.clone();
        let _: Option<AdvertiseMessage<_>> = want_collected_advertise.pop();

        // The client should transition to Requesting and select the server that
        // sent the best advertise.
        assert_matches!(
            &client.handle_timeout(ClientTimerType::Retransmission, time)[..],
           [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::SendMessage(buf),
                Action::ScheduleTimer(ClientTimerType::Retransmission, instant)
           ] => {
               assert_eq!(testutil::msg_type(buf), v6::MessageType::Request);
               assert_eq!(*instant, time.add(INITIAL_REQUEST_TIMEOUT));
           }
        );
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
            &client;
        {
            let Requesting {
                client_id: _,
                non_temporary_addresses: _,
                delegated_prefixes: _,
                server_id,
                collected_advertise,
                first_request_time: _,
                retrans_timeout: _,
                transmission_count,
                solicit_max_rt: _,
            } = assert_matches!(state, Some(ClientState::Requesting(requesting)) => requesting);
            assert_eq!(
                collected_advertise.clone().into_sorted_vec(),
                want_collected_advertise.clone().into_sorted_vec()
            );
            assert_eq!(server_id[..], SERVER_ID[0]);
            assert_eq!(*transmission_count, 1);
        }

        for count in 2..=(REQUEST_MAX_RC + 1) {
            assert_matches!(
                &client.handle_timeout(ClientTimerType::Retransmission, time)[..],
               [
                    Action::SendMessage(buf),
                    // `_timeout` is not checked because retransmission timeout
                    // calculation is covered in its respective test.
                    Action::ScheduleTimer(ClientTimerType::Retransmission, _timeout)
               ] if testutil::msg_type(buf) == v6::MessageType::Request
            );
            let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
                &client;
            let Requesting {
                client_id: _,
                non_temporary_addresses: _,
                delegated_prefixes: _,
                server_id,
                collected_advertise,
                first_request_time: _,
                retrans_timeout: _,
                transmission_count,
                solicit_max_rt: _,
            } = assert_matches!(state, Some(ClientState::Requesting(requesting)) => requesting);
            assert_eq!(
                collected_advertise.clone().into_sorted_vec(),
                want_collected_advertise.clone().into_sorted_vec()
            );
            assert_eq!(server_id[..], SERVER_ID[0]);
            assert_eq!(*transmission_count, count);
        }

        // When the retransmission count reaches REQUEST_MAX_RC, the client
        // should select another server.
        assert_matches!(
            &client.handle_timeout(ClientTimerType::Retransmission, time)[..],
           [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::SendMessage(buf),
                Action::ScheduleTimer(ClientTimerType::Retransmission, instant)
           ] => {
               assert_eq!(testutil::msg_type(buf), v6::MessageType::Request);
               assert_eq!(*instant, time.add(INITIAL_REQUEST_TIMEOUT));
           }
        );
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
            &client;
        let Requesting {
            client_id: _,
            non_temporary_addresses: _,
            delegated_prefixes: _,
            server_id,
            collected_advertise,
            first_request_time: _,
            retrans_timeout: _,
            transmission_count,
            solicit_max_rt: _,
        } = assert_matches!(state, Some(ClientState::Requesting(requesting)) => requesting);
        assert!(collected_advertise.is_empty(), "{:?}", collected_advertise);
        assert_eq!(server_id[..], SERVER_ID[1]);
        assert_eq!(*transmission_count, 1);

        for count in 2..=(REQUEST_MAX_RC + 1) {
            assert_matches!(
                &client.handle_timeout(ClientTimerType::Retransmission, time)[..],
               [
                    Action::SendMessage(buf),
                    // `_timeout` is not checked because retransmission timeout
                    // calculation is covered in its respective test.
                    Action::ScheduleTimer(ClientTimerType::Retransmission, _timeout)
               ] if testutil::msg_type(buf) == v6::MessageType::Request
            );
            let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
                &client;
            let Requesting {
                client_id: _,
                non_temporary_addresses: _,
                delegated_prefixes: _,
                server_id,
                collected_advertise,
                first_request_time: _,
                retrans_timeout: _,
                transmission_count,
                solicit_max_rt: _,
            } = assert_matches!(state, Some(ClientState::Requesting(requesting)) => requesting);
            assert!(collected_advertise.is_empty(), "{:?}", collected_advertise);
            assert_eq!(server_id[..], SERVER_ID[1]);
            assert_eq!(*transmission_count, count);
        }

        // When the retransmission count reaches REQUEST_MAX_RC, and the client
        // does not have information about another server, the client should
        // restart server discovery.
        assert_matches!(
            &client.handle_timeout(ClientTimerType::Retransmission, time)[..],
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::CancelTimer(ClientTimerType::Refresh),
                Action::CancelTimer(ClientTimerType::Renew),
                Action::CancelTimer(ClientTimerType::Rebind),
                Action::CancelTimer(ClientTimerType::RestartServerDiscovery),
                Action::SendMessage(buf),
                Action::ScheduleTimer(ClientTimerType::Retransmission, instant)
            ] => {
                assert_eq!(testutil::msg_type(buf), v6::MessageType::Solicit);
                assert_eq!(*instant, time.add(INITIAL_SOLICIT_TIMEOUT));
            }
        );
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } = client;
        assert_matches!(state,
            Some(ClientState::ServerDiscovery(ServerDiscovery {
                client_id: _,
                configured_non_temporary_addresses: _,
                configured_delegated_prefixes: _,
                first_solicit_time: _,
                retrans_timeout: _,
                solicit_max_rt: _,
                collected_advertise,
                collected_sol_max_rt: _,
            })) if collected_advertise.is_empty()
        );
    }

    // Test 4-msg exchange for assignment.
    #[test]
    fn assignment() {
        let now = Instant::now();
        let (client, actions) = testutil::assign_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            CONFIGURED_NON_TEMPORARY_ADDRESSES[0..2]
                .iter()
                .copied()
                .map(TestIaNa::new_default)
                .collect(),
            CONFIGURED_DELEGATED_PREFIXES[0..2]
                .iter()
                .copied()
                .map(TestIaPd::new_default)
                .collect(),
            &[],
            StepRng::new(std::u64::MAX / 2, 0),
            now,
        );

        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
            &client;
        let Assigned {
            client_id: _,
            non_temporary_addresses: _,
            delegated_prefixes: _,
            server_id: _,
            dns_servers: _,
            solicit_max_rt: _,
            _marker,
        } = assert_matches!(
            state,
            Some(ClientState::Assigned(assigned)) => assigned
        );
        assert_matches!(
            &actions[..],
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::ScheduleTimer(ClientTimerType::Renew, t1),
                Action::ScheduleTimer(ClientTimerType::Rebind, t2),
                Action::IaNaUpdates(iana_updates),
                Action::IaPdUpdates(iapd_updates),
                Action::ScheduleTimer(ClientTimerType::RestartServerDiscovery, restart_time),
            ] => {
                assert_eq!(*t1, now.add(Duration::from_secs(T1.get().into())));
                assert_eq!(*t2, now.add(Duration::from_secs(T2.get().into())));
                assert_eq!(
                    *restart_time,
                    now.add(Duration::from_secs(VALID_LIFETIME.get().into())),
                );
                assert_eq!(
                    iana_updates,
                    &(0..).map(v6::IAID::new)
                        .zip(CONFIGURED_NON_TEMPORARY_ADDRESSES[0..2].iter().cloned())
                        .map(|(iaid, value)| (
                            iaid,
                            HashMap::from([(value, IaValueUpdateKind::Added(Lifetimes::new_default()))])
                        ))
                        .collect::<HashMap<_, _>>(),
                );
                assert_eq!(
                    iapd_updates,
                    &(0..).map(v6::IAID::new)
                        .zip(CONFIGURED_DELEGATED_PREFIXES[0..2].iter().cloned())
                        .map(|(iaid, value)| (
                            iaid,
                            HashMap::from([(value, IaValueUpdateKind::Added(Lifetimes::new_default()))])
                        ))
                        .collect::<HashMap<_, _>>(),
                );
            }
        );
    }

    #[test]
    fn assigned_get_dns_servers() {
        let now = Instant::now();
        let (client, actions) = testutil::assign_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            vec![TestIaNa::new_default(CONFIGURED_NON_TEMPORARY_ADDRESSES[0])],
            Default::default(), /* delegated_prefixes_to_assign */
            &DNS_SERVERS,
            StepRng::new(std::u64::MAX / 2, 0),
            now,
        );
        assert_matches!(
            &actions[..],
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::ScheduleTimer(ClientTimerType::Renew, t1),
                Action::ScheduleTimer(ClientTimerType::Rebind, t2),
                Action::UpdateDnsServers(dns_servers),
                Action::IaNaUpdates(iana_updates),
                Action::ScheduleTimer(ClientTimerType::RestartServerDiscovery, restart_time),
            ] => {
                assert_eq!(dns_servers[..], DNS_SERVERS);
                assert_eq!(*t1, now.add(Duration::from_secs(T1.get().into())));
                assert_eq!(*t2, now.add(Duration::from_secs(T2.get().into())));
                assert_eq!(
                    *restart_time,
                    now.add(Duration::from_secs(VALID_LIFETIME.get().into())),
                );
                assert_eq!(
                    iana_updates,
                    &HashMap::from([
                        (
                            v6::IAID::new(0),
                            HashMap::from([(
                                CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
                                IaValueUpdateKind::Added(Lifetimes::new_default()),
                            )]),
                        ),
                    ]),
                );
            }
        );
        assert_eq!(client.get_dns_servers()[..], DNS_SERVERS);
    }

    #[test]
    fn update_sol_max_rt_on_reply_to_request() {
        let options_to_request = vec![];
        let configured_non_temporary_addresses = testutil::to_configured_addresses(1, vec![]);
        let mut rng = StepRng::new(std::u64::MAX / 2, 0);
        let time = Instant::now();
        let Transition { state, actions: _, transaction_id } = Requesting::start(
            CLIENT_ID,
            SERVER_ID[0].to_vec(),
            advertise_to_ia_entries(
                testutil::to_default_ias_map(&CONFIGURED_NON_TEMPORARY_ADDRESSES[0..1]),
                configured_non_temporary_addresses.clone(),
            ),
            Default::default(), /* delegated_prefixes */
            &options_to_request[..],
            BinaryHeap::new(),
            MAX_SOLICIT_TIMEOUT,
            &mut rng,
            time,
        );
        {
            let Requesting {
                collected_advertise,
                solicit_max_rt,
                client_id: _,
                non_temporary_addresses: _,
                delegated_prefixes: _,
                server_id: _,
                first_request_time: _,
                retrans_timeout: _,
                transmission_count: _,
            } = assert_matches!(&state, ClientState::Requesting(requesting) => requesting);
            assert!(collected_advertise.is_empty(), "{:?}", collected_advertise);
            assert_eq!(*solicit_max_rt, MAX_SOLICIT_TIMEOUT);
        }
        let received_sol_max_rt = 4800;

        // If the reply does not contain a server ID, the reply should be
        // discarded and the `solicit_max_rt` should not be updated.
        let iana_options = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
            CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
            60,
            120,
            &[],
        ))];
        let options = [
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                v6::IAID::new(0),
                T1.get(),
                T2.get(),
                &iana_options,
            )),
            v6::DhcpOption::SolMaxRt(received_sol_max_rt),
        ];
        let request_transaction_id = transaction_id.unwrap();
        let builder =
            v6::MessageBuilder::new(v6::MessageType::Reply, request_transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let Transition { state, actions: _, transaction_id: _ } =
            state.reply_message_received(&options_to_request, &mut rng, msg, time);
        {
            let Requesting {
                collected_advertise,
                solicit_max_rt,
                client_id: _,
                non_temporary_addresses: _,
                delegated_prefixes: _,
                server_id: _,
                first_request_time: _,
                retrans_timeout: _,
                transmission_count: _,
            } = assert_matches!(&state, ClientState::Requesting(requesting) => requesting);
            assert!(collected_advertise.is_empty(), "{:?}", collected_advertise);
            assert_eq!(*solicit_max_rt, MAX_SOLICIT_TIMEOUT);
        }

        // If the reply has a different client ID than the test client's client ID,
        // the `solicit_max_rt` should not be updated.
        let options = [
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::ClientId(&MISMATCHED_CLIENT_ID),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                v6::IAID::new(0),
                T1.get(),
                T2.get(),
                &iana_options,
            )),
            v6::DhcpOption::SolMaxRt(received_sol_max_rt),
        ];
        let builder =
            v6::MessageBuilder::new(v6::MessageType::Reply, request_transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let Transition { state, actions: _, transaction_id: _ } =
            state.reply_message_received(&options_to_request, &mut rng, msg, time);
        {
            let Requesting {
                collected_advertise,
                solicit_max_rt,
                client_id: _,
                non_temporary_addresses: _,
                delegated_prefixes: _,
                server_id: _,
                first_request_time: _,
                retrans_timeout: _,
                transmission_count: _,
            } = assert_matches!(&state, ClientState::Requesting(requesting) => requesting);
            assert!(collected_advertise.is_empty(), "{:?}", collected_advertise);
            assert_eq!(*solicit_max_rt, MAX_SOLICIT_TIMEOUT);
        }

        // If the client receives a valid reply containing a SOL_MAX_RT option,
        // the `solicit_max_rt` should be updated.
        let options = [
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                v6::IAID::new(0),
                T1.get(),
                T2.get(),
                &iana_options,
            )),
            v6::DhcpOption::SolMaxRt(received_sol_max_rt),
        ];
        let builder =
            v6::MessageBuilder::new(v6::MessageType::Reply, request_transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let Transition { state, actions: _, transaction_id: _ } =
            state.reply_message_received(&options_to_request, &mut rng, msg, time);
        {
            let Assigned {
                solicit_max_rt,
                client_id: _,
                non_temporary_addresses: _,
                delegated_prefixes: _,
                server_id: _,
                dns_servers: _,
                _marker,
            } = assert_matches!(&state, ClientState::Assigned(assigned) => assigned);
            assert_eq!(*solicit_max_rt, Duration::from_secs(received_sol_max_rt.into()));
        }
    }

    struct RenewRebindTest {
        send_and_assert: fn(
            [u8; CLIENT_ID_LEN],
            [u8; TEST_SERVER_ID_LEN],
            Vec<TestIaNa>,
            Vec<TestIaPd>,
            Option<&[Ipv6Addr]>,
            v6::NonZeroOrMaxU32,
            v6::NonZeroOrMaxU32,
            v6::NonZeroTimeValue,
            StepRng,
            Instant,
        ) -> ClientStateMachine<Instant, StepRng>,
        message_type: v6::MessageType,
        expect_server_id: bool,
        with_state: fn(&Option<ClientState<Instant>>) -> &RenewingOrRebindingInner<Instant>,
        allow_response_from_any_server: bool,
    }

    const RENEW_TEST: RenewRebindTest = RenewRebindTest {
        send_and_assert: testutil::send_renew_and_assert,
        message_type: v6::MessageType::Renew,
        expect_server_id: true,
        with_state: |state| {
            assert_matches!(
                state,
                Some(ClientState::Renewing(RenewingOrRebinding(inner))) => inner
            )
        },
        allow_response_from_any_server: false,
    };

    const REBIND_TEST: RenewRebindTest = RenewRebindTest {
        send_and_assert: testutil::send_rebind_and_assert,
        message_type: v6::MessageType::Rebind,
        expect_server_id: false,
        with_state: |state| {
            assert_matches!(
                state,
                Some(ClientState::Rebinding(RenewingOrRebinding(inner))) => inner
            )
        },
        allow_response_from_any_server: true,
    };

    struct RenewRebindSendTestCase {
        ia_nas: Vec<TestIaNa>,
        ia_pds: Vec<TestIaPd>,
    }

    impl RenewRebindSendTestCase {
        fn single_value_per_ia() -> RenewRebindSendTestCase {
            RenewRebindSendTestCase {
                ia_nas: CONFIGURED_NON_TEMPORARY_ADDRESSES[0..2]
                    .into_iter()
                    .map(|&addr| TestIaNa::new_default(addr))
                    .collect(),
                ia_pds: CONFIGURED_DELEGATED_PREFIXES[0..2]
                    .into_iter()
                    .map(|&addr| TestIaPd::new_default(addr))
                    .collect(),
            }
        }

        fn multiple_values_per_ia() -> RenewRebindSendTestCase {
            RenewRebindSendTestCase {
                ia_nas: vec![TestIaNa::new_default_with_values(
                    CONFIGURED_NON_TEMPORARY_ADDRESSES
                        .into_iter()
                        .map(|a| (a, Lifetimes::new_default()))
                        .collect(),
                )],
                ia_pds: vec![TestIaPd::new_default_with_values(
                    CONFIGURED_DELEGATED_PREFIXES
                        .into_iter()
                        .map(|a| (a, Lifetimes::new_default()))
                        .collect(),
                )],
            }
        }
    }

    #[test_case(
        RENEW_TEST,
        RenewRebindSendTestCase::single_value_per_ia(); "renew single value per IA")]
    #[test_case(
        RENEW_TEST,
        RenewRebindSendTestCase::multiple_values_per_ia(); "renew multiple value per IA")]
    #[test_case(
        REBIND_TEST,
        RenewRebindSendTestCase::single_value_per_ia(); "rebind single value per IA")]
    #[test_case(
        REBIND_TEST,
        RenewRebindSendTestCase::multiple_values_per_ia(); "rebind multiple value per IA")]
    fn send(
        RenewRebindTest {
            send_and_assert,
            message_type: _,
            expect_server_id: _,
            with_state: _,
            allow_response_from_any_server: _,
        }: RenewRebindTest,
        RenewRebindSendTestCase { ia_nas, ia_pds }: RenewRebindSendTestCase,
    ) {
        let _client = send_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            ia_nas,
            ia_pds,
            None,
            T1,
            T2,
            v6::NonZeroTimeValue::Finite(VALID_LIFETIME),
            StepRng::new(std::u64::MAX / 2, 0),
            Instant::now(),
        );
    }

    #[test_case(RENEW_TEST)]
    #[test_case(REBIND_TEST)]
    fn get_dns_server(
        RenewRebindTest {
            send_and_assert,
            message_type: _,
            expect_server_id: _,
            with_state: _,
            allow_response_from_any_server: _,
        }: RenewRebindTest,
    ) {
        let client = send_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            CONFIGURED_NON_TEMPORARY_ADDRESSES[0..2]
                .into_iter()
                .map(|&addr| TestIaNa::new_default(addr))
                .collect(),
            Default::default(), /* delegated_prefixes_to_assign */
            Some(&DNS_SERVERS),
            T1,
            T2,
            v6::NonZeroTimeValue::Finite(VALID_LIFETIME),
            StepRng::new(std::u64::MAX / 2, 0),
            Instant::now(),
        );
        assert_eq!(client.get_dns_servers()[..], DNS_SERVERS);
    }

    struct ScheduleRenewAndRebindTimersAfterAssignmentTestCase {
        ia_na_t1: v6::TimeValue,
        ia_na_t2: v6::TimeValue,
        ia_pd_t1: v6::TimeValue,
        ia_pd_t2: v6::TimeValue,
        expected_timer_actions: fn(Instant) -> [Action<Instant>; 2],
        next_timer: Option<RenewRebindTestState>,
    }

    // Make sure that both IA_NA and IA_PD is considered when calculating
    // renew/rebind timers.
    #[test_case(ScheduleRenewAndRebindTimersAfterAssignmentTestCase{
        ia_na_t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        ia_na_t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        ia_pd_t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        ia_pd_t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        expected_timer_actions: |_| [
            Action::CancelTimer(ClientTimerType::Renew),
            Action::CancelTimer(ClientTimerType::Rebind),
        ],
        next_timer: None,
    }; "all infinite time values")]
    #[test_case(ScheduleRenewAndRebindTimersAfterAssignmentTestCase{
        ia_na_t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T1)),
        ia_na_t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T2)),
        ia_pd_t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T1)),
        ia_pd_t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T2)),
        expected_timer_actions: |time| [
            Action::ScheduleTimer(
                ClientTimerType::Renew,
                time.add(Duration::from_secs(T1.get().into())),
            ),
            Action::ScheduleTimer(
                ClientTimerType::Rebind,
                time.add(Duration::from_secs(T2.get().into())),
            ),
        ],
        next_timer: Some(RENEW_TEST_STATE),
    }; "all finite time values")]
    #[test_case(ScheduleRenewAndRebindTimersAfterAssignmentTestCase{
        ia_na_t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T2)),
        ia_na_t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T2)),
        ia_pd_t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T2)),
        ia_pd_t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T2)),
        expected_timer_actions: |time| [
            // Skip Renew and just go to Rebind when T2 == T1.
            Action::CancelTimer(ClientTimerType::Renew),
            Action::ScheduleTimer(
                ClientTimerType::Rebind,
                time.add(Duration::from_secs(T2.get().into())),
            ),
        ],
        next_timer: Some(REBIND_TEST_STATE),
    }; "finite T1 equals finite T2")]
    #[test_case(ScheduleRenewAndRebindTimersAfterAssignmentTestCase{
        ia_na_t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T1)),
        ia_na_t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        ia_pd_t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        ia_pd_t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        expected_timer_actions: |time| [
            Action::ScheduleTimer(
                ClientTimerType::Renew,
                time.add(Duration::from_secs(T1.get().into())),
            ),
            Action::CancelTimer(ClientTimerType::Rebind),
        ],
        next_timer: Some(RENEW_TEST_STATE),
    }; "finite IA_NA T1")]
    #[test_case(ScheduleRenewAndRebindTimersAfterAssignmentTestCase{
        ia_na_t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T1)),
        ia_na_t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T2)),
        ia_pd_t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        ia_pd_t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        expected_timer_actions: |time| [
            Action::ScheduleTimer(
                ClientTimerType::Renew,
                time.add(Duration::from_secs(T1.get().into())),
            ),
            Action::ScheduleTimer(
                ClientTimerType::Rebind,
                time.add(Duration::from_secs(T2.get().into())),
            ),
        ],
        next_timer: Some(RENEW_TEST_STATE),
    }; "finite IA_NA T1 and T2")]
    #[test_case(ScheduleRenewAndRebindTimersAfterAssignmentTestCase{
        ia_na_t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        ia_na_t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        ia_pd_t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T1)),
        ia_pd_t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        expected_timer_actions: |time| [
            Action::ScheduleTimer(
                ClientTimerType::Renew,
                time.add(Duration::from_secs(T1.get().into())),
            ),
            Action::CancelTimer(ClientTimerType::Rebind),
        ],
        next_timer: Some(RENEW_TEST_STATE),
    }; "finite IA_PD t1")]
    #[test_case(ScheduleRenewAndRebindTimersAfterAssignmentTestCase{
        ia_na_t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        ia_na_t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        ia_pd_t1: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T1)),
        ia_pd_t2: v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(T2)),
        expected_timer_actions: |time| [
            Action::ScheduleTimer(
                ClientTimerType::Renew,
                time.add(Duration::from_secs(T1.get().into())),
            ),
            Action::ScheduleTimer(
                ClientTimerType::Rebind,
                time.add(Duration::from_secs(T2.get().into())),
            ),
        ],
        next_timer: Some(RENEW_TEST_STATE),
    }; "finite IA_PD T1 and T2")]
    fn schedule_renew_and_rebind_timers_after_assignment(
        ScheduleRenewAndRebindTimersAfterAssignmentTestCase {
            ia_na_t1,
            ia_na_t2,
            ia_pd_t1,
            ia_pd_t2,
            expected_timer_actions,
            next_timer,
        }: ScheduleRenewAndRebindTimersAfterAssignmentTestCase,
    ) {
        fn get_ia_and_updates<V: IaValue>(
            t1: v6::TimeValue,
            t2: v6::TimeValue,
            value: V,
        ) -> (TestIa<V>, HashMap<v6::IAID, HashMap<V, IaValueUpdateKind>>) {
            (
                TestIa { t1, t2, ..TestIa::new_default(value) },
                HashMap::from([(
                    v6::IAID::new(0),
                    HashMap::from([(value, IaValueUpdateKind::Added(Lifetimes::new_default()))]),
                )]),
            )
        }

        let (iana, iana_updates) =
            get_ia_and_updates(ia_na_t1, ia_na_t2, CONFIGURED_NON_TEMPORARY_ADDRESSES[0]);
        let (iapd, iapd_updates) =
            get_ia_and_updates(ia_pd_t1, ia_pd_t2, CONFIGURED_DELEGATED_PREFIXES[0]);
        let iana = vec![iana];
        let iapd = vec![iapd];
        let now = Instant::now();
        let (client, actions) = testutil::assign_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            iana.clone(),
            iapd.clone(),
            &[],
            StepRng::new(std::u64::MAX / 2, 0),
            now,
        );
        let ClientStateMachine {
            transaction_id: old_transaction_id,
            options_to_request: _,
            state,
            rng: _,
        } = &client;
        let old_transaction_id = *old_transaction_id;
        let Assigned {
            client_id: _,
            non_temporary_addresses: _,
            delegated_prefixes: _,
            server_id: _,
            dns_servers: _,
            solicit_max_rt: _,
            _marker,
        } = assert_matches!(
            state,
            Some(ClientState::Assigned(assigned)) => assigned
        );

        assert_eq!(
            actions,
            [Action::CancelTimer(ClientTimerType::Retransmission)]
                .into_iter()
                .chain(expected_timer_actions(now))
                .chain((!iana_updates.is_empty()).then(|| Action::IaNaUpdates(iana_updates)))
                .chain((!iapd_updates.is_empty()).then(|| Action::IaPdUpdates(iapd_updates)))
                .chain([Action::ScheduleTimer(
                    ClientTimerType::RestartServerDiscovery,
                    now.add(Duration::from_secs(VALID_LIFETIME.get().into())),
                ),])
                .collect::<Vec<_>>()
        );

        let _client = if let Some(next_timer) = next_timer {
            handle_renew_or_rebind_timer(
                client,
                old_transaction_id,
                CLIENT_ID,
                SERVER_ID[0],
                iana,
                iapd,
                &[],
                &[],
                Instant::now(),
                next_timer,
            )
        } else {
            client
        };
    }

    #[test_case(RENEW_TEST)]
    #[test_case(REBIND_TEST)]
    fn retransmit(
        RenewRebindTest {
            send_and_assert,
            message_type,
            expect_server_id,
            with_state,
            allow_response_from_any_server: _,
        }: RenewRebindTest,
    ) {
        let non_temporary_addresses_to_assign = CONFIGURED_NON_TEMPORARY_ADDRESSES[0..2]
            .into_iter()
            .map(|&addr| TestIaNa::new_default(addr))
            .collect::<Vec<_>>();
        let delegated_prefixes_to_assign = CONFIGURED_DELEGATED_PREFIXES[0..2]
            .into_iter()
            .map(|&addr| TestIaPd::new_default(addr))
            .collect::<Vec<_>>();
        let time = Instant::now();
        let mut client = send_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            non_temporary_addresses_to_assign.clone(),
            delegated_prefixes_to_assign.clone(),
            None,
            T1,
            T2,
            v6::NonZeroTimeValue::Finite(VALID_LIFETIME),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );
        let ClientStateMachine { transaction_id, options_to_request: _, state, rng: _ } = &client;
        let expected_transaction_id = *transaction_id;
        let RenewingOrRebindingInner {
            client_id: _,
            non_temporary_addresses: _,
            delegated_prefixes: _,
            server_id: _,
            dns_servers: _,
            start_time: _,
            retrans_timeout: _,
            solicit_max_rt: _,
        } = with_state(state);

        // Assert renew is retransmitted on retransmission timeout.
        let actions = client.handle_timeout(ClientTimerType::Retransmission, time);
        let buf = assert_matches!(
            &actions[..],
            [
                Action::SendMessage(buf),
                Action::ScheduleTimer(ClientTimerType::Retransmission, timeout)
            ] => {
                assert_eq!(*timeout, time.add(2 * INITIAL_RENEW_TIMEOUT));
                buf
            }
        );
        let ClientStateMachine { transaction_id, options_to_request: _, state, rng: _ } = &client;
        // Check that the retransmitted renew is part of the same transaction.
        assert_eq!(*transaction_id, expected_transaction_id);
        {
            let RenewingOrRebindingInner {
                client_id,
                server_id,
                dns_servers,
                solicit_max_rt,
                non_temporary_addresses: _,
                delegated_prefixes: _,
                start_time: _,
                retrans_timeout: _,
            } = with_state(state);
            assert_eq!(client_id, &CLIENT_ID);
            assert_eq!(server_id[..], SERVER_ID[0]);
            assert_eq!(dns_servers, &[] as &[Ipv6Addr]);
            assert_eq!(*solicit_max_rt, MAX_SOLICIT_TIMEOUT);
        }
        let expected_non_temporary_addresses: HashMap<v6::IAID, HashSet<Ipv6Addr>> = (0..)
            .map(v6::IAID::new)
            .zip(
                non_temporary_addresses_to_assign
                    .iter()
                    .map(|TestIaNa { values, t1: _, t2: _ }| values.keys().cloned().collect()),
            )
            .collect();
        let expected_delegated_prefixes: HashMap<v6::IAID, HashSet<Subnet<Ipv6Addr>>> = (0..)
            .map(v6::IAID::new)
            .zip(
                delegated_prefixes_to_assign
                    .iter()
                    .map(|TestIaPd { values, t1: _, t2: _ }| values.keys().cloned().collect()),
            )
            .collect();
        testutil::assert_outgoing_stateful_message(
            &buf,
            message_type,
            &CLIENT_ID,
            expect_server_id.then(|| &SERVER_ID[0]),
            &[],
            &expected_non_temporary_addresses,
            &expected_delegated_prefixes,
        );
    }

    #[test_case(
        RENEW_TEST,
        &SERVER_ID[0],
        &SERVER_ID[0],
        RenewRebindSendTestCase::single_value_per_ia()
    )]
    #[test_case(
        REBIND_TEST,
        &SERVER_ID[0],
        &SERVER_ID[0],
        RenewRebindSendTestCase::single_value_per_ia()
    )]
    #[test_case(
        RENEW_TEST,
        &SERVER_ID[0],
        &SERVER_ID[1],
        RenewRebindSendTestCase::single_value_per_ia()
    )]
    #[test_case(
        REBIND_TEST,
        &SERVER_ID[0],
        &SERVER_ID[1],
        RenewRebindSendTestCase::single_value_per_ia()
    )]
    #[test_case(
        RENEW_TEST,
        &SERVER_ID[0],
        &SERVER_ID[0],
        RenewRebindSendTestCase::multiple_values_per_ia()
    )]
    #[test_case(
        REBIND_TEST,
        &SERVER_ID[0],
        &SERVER_ID[0],
        RenewRebindSendTestCase::multiple_values_per_ia()
    )]
    #[test_case(
        RENEW_TEST,
        &SERVER_ID[0],
        &SERVER_ID[1],
        RenewRebindSendTestCase::multiple_values_per_ia()
    )]
    #[test_case(
        REBIND_TEST,
        &SERVER_ID[0],
        &SERVER_ID[1],
        RenewRebindSendTestCase::multiple_values_per_ia()
    )]
    fn receive_reply_extends_lifetime(
        RenewRebindTest {
            send_and_assert,
            message_type: _,
            expect_server_id: _,
            with_state,
            allow_response_from_any_server,
        }: RenewRebindTest,
        original_server_id: &[u8; TEST_SERVER_ID_LEN],
        reply_server_id: &[u8],
        RenewRebindSendTestCase { ia_nas, ia_pds }: RenewRebindSendTestCase,
    ) {
        let time = Instant::now();
        let mut client = send_and_assert(
            CLIENT_ID,
            original_server_id.clone(),
            ia_nas.clone(),
            ia_pds.clone(),
            None,
            T1,
            T2,
            v6::NonZeroTimeValue::Finite(VALID_LIFETIME),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );
        let ClientStateMachine { transaction_id, options_to_request: _, state, rng: _ } = &client;
        let buf = TestMessageBuilder {
            transaction_id: *transaction_id,
            message_type: v6::MessageType::Reply,
            client_id: &CLIENT_ID,
            server_id: reply_server_id,
            preference: None,
            dns_servers: None,
            ia_nas: (0..).map(v6::IAID::new).zip(ia_nas.iter().map(
                |TestIa { values, t1: _, t2: _ }| {
                    TestIa::new_renewed_default_with_values(values.keys().cloned())
                },
            )),
            ia_pds: (0..).map(v6::IAID::new).zip(ia_pds.iter().map(
                |TestIa { values, t1: _, t2: _ }| {
                    TestIa::new_renewed_default_with_values(values.keys().cloned())
                },
            )),
        }
        .build();
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");

        // Make sure we are in renewing/rebinding before we handle the message.
        let original_state = with_state(state).clone();

        let actions = client.handle_message_receive(msg, time);
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
            &client;

        if original_server_id.as_slice() != reply_server_id && !allow_response_from_any_server {
            // Renewing does not allow us to receive replies from a different
            // server but Rebinding does. If we aren't allowed to accept a
            // response from a different server, just make sure we are in the
            // same state.
            let RenewingOrRebindingInner {
                client_id: original_client_id,
                non_temporary_addresses: original_non_temporary_addresses,
                delegated_prefixes: original_delegated_prefixes,
                server_id: original_server_id,
                dns_servers: original_dns_servers,
                start_time: original_start_time,
                retrans_timeout: original_retrans_timeout,
                solicit_max_rt: original_solicit_max_rt,
            } = original_state;
            let RenewingOrRebindingInner {
                client_id: new_client_id,
                non_temporary_addresses: new_non_temporary_addresses,
                delegated_prefixes: new_delegated_prefixes,
                server_id: new_server_id,
                dns_servers: new_dns_servers,
                start_time: new_start_time,
                retrans_timeout: new_retrans_timeout,
                solicit_max_rt: new_solicit_max_rt,
            } = with_state(state);
            assert_eq!(&original_client_id, new_client_id);
            assert_eq!(&original_non_temporary_addresses, new_non_temporary_addresses);
            assert_eq!(&original_delegated_prefixes, new_delegated_prefixes);
            assert_eq!(&original_server_id, new_server_id);
            assert_eq!(&original_dns_servers, new_dns_servers);
            assert_eq!(&original_start_time, new_start_time);
            assert_eq!(&original_retrans_timeout, new_retrans_timeout);
            assert_eq!(&original_solicit_max_rt, new_solicit_max_rt);
            assert_eq!(actions, []);
            return;
        }

        let expected_non_temporary_addresses = (0..)
            .map(v6::IAID::new)
            .zip(ia_nas.iter().map(|TestIa { values, t1: _, t2: _ }| {
                AddressEntry::Assigned(
                    values
                        .keys()
                        .cloned()
                        .map(|value| {
                            (
                                value,
                                LifetimesInfo {
                                    lifetimes: Lifetimes::new_renewed(),
                                    updated_at: time,
                                },
                            )
                        })
                        .collect(),
                )
            }))
            .collect();
        let expected_delegated_prefixes = (0..)
            .map(v6::IAID::new)
            .zip(ia_pds.iter().map(|TestIa { values, t1: _, t2: _ }| {
                PrefixEntry::Assigned(
                    values
                        .keys()
                        .cloned()
                        .map(|value| {
                            (
                                value,
                                LifetimesInfo {
                                    lifetimes: Lifetimes::new_renewed(),
                                    updated_at: time,
                                },
                            )
                        })
                        .collect(),
                )
            }))
            .collect();
        assert_matches!(
            &state,
            Some(ClientState::Assigned(Assigned {
                client_id,
                non_temporary_addresses,
                delegated_prefixes,
                server_id,
                dns_servers,
                solicit_max_rt,
                _marker,
            })) => {
                assert_eq!(client_id, &CLIENT_ID);
                assert_eq!(non_temporary_addresses, &expected_non_temporary_addresses);
                assert_eq!(delegated_prefixes, &expected_delegated_prefixes);
                assert_eq!(server_id.as_slice(), reply_server_id);
                assert_eq!(dns_servers.as_slice(), &[] as &[Ipv6Addr]);
                assert_eq!(*solicit_max_rt, MAX_SOLICIT_TIMEOUT);
            }
        );
        assert_matches!(
            &actions[..],
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::ScheduleTimer(ClientTimerType::Renew, t1),
                Action::ScheduleTimer(ClientTimerType::Rebind, t2),
                Action::IaNaUpdates(iana_updates),
                Action::IaPdUpdates(iapd_updates),
                Action::ScheduleTimer(ClientTimerType::RestartServerDiscovery, restart_time),
            ] => {
                assert_eq!(*t1, time.add(Duration::from_secs(RENEWED_T1.get().into())));
                assert_eq!(*t2, time.add(Duration::from_secs(RENEWED_T2.get().into())));
                assert_eq!(
                    *restart_time,
                    time.add(Duration::from_secs(RENEWED_VALID_LIFETIME.get().into()))
                );

                fn get_updates<V: IaValue>(ias: Vec<TestIa<V>>) -> HashMap<v6::IAID, HashMap<V, IaValueUpdateKind>> {
                    (0..).map(v6::IAID::new).zip(ias.into_iter().map(
                        |TestIa { values, t1: _, t2: _ }| {
                            values.into_keys()
                                .map(|value| (
                                    value,
                                    IaValueUpdateKind::UpdatedLifetimes(Lifetimes::new_renewed())
                                ))
                                .collect()
                        },
                    )).collect()
                }

                assert_eq!(iana_updates, &get_updates(ia_nas));
                assert_eq!(iapd_updates, &get_updates(ia_pds));
            }
        );
    }

    // Tests that receiving a Reply with an error status code other than
    // UseMulticast results in only SOL_MAX_RT being updated, with the rest
    // of the message contents ignored.
    #[test_case(RENEW_TEST, v6::ErrorStatusCode::UnspecFail)]
    #[test_case(RENEW_TEST, v6::ErrorStatusCode::NoBinding)]
    #[test_case(RENEW_TEST, v6::ErrorStatusCode::NotOnLink)]
    #[test_case(RENEW_TEST, v6::ErrorStatusCode::NoAddrsAvail)]
    #[test_case(RENEW_TEST, v6::ErrorStatusCode::NoPrefixAvail)]
    #[test_case(REBIND_TEST, v6::ErrorStatusCode::UnspecFail)]
    #[test_case(REBIND_TEST, v6::ErrorStatusCode::NoBinding)]
    #[test_case(REBIND_TEST, v6::ErrorStatusCode::NotOnLink)]
    #[test_case(REBIND_TEST, v6::ErrorStatusCode::NoAddrsAvail)]
    #[test_case(REBIND_TEST, v6::ErrorStatusCode::NoPrefixAvail)]
    fn renewing_receive_reply_with_error_status(
        RenewRebindTest {
            send_and_assert,
            message_type: _,
            expect_server_id: _,
            with_state,
            allow_response_from_any_server: _,
        }: RenewRebindTest,
        error_status_code: v6::ErrorStatusCode,
    ) {
        let time = Instant::now();
        let addr = CONFIGURED_NON_TEMPORARY_ADDRESSES[0];
        let prefix = CONFIGURED_DELEGATED_PREFIXES[0];
        let mut client = send_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            vec![TestIaNa::new_default(addr)],
            vec![TestIaPd::new_default(prefix)],
            None,
            T1,
            T2,
            v6::NonZeroTimeValue::Finite(VALID_LIFETIME),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );
        let ClientStateMachine { transaction_id, options_to_request: _, state: _, rng: _ } =
            &client;
        let ia_na_options = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
            addr,
            RENEWED_PREFERRED_LIFETIME.get(),
            RENEWED_VALID_LIFETIME.get(),
            &[],
        ))];
        let sol_max_rt = *VALID_MAX_SOLICIT_TIMEOUT_RANGE.start();
        let options = vec![
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::StatusCode(error_status_code.into(), ""),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                v6::IAID::new(0),
                RENEWED_T1.get(),
                RENEWED_T2.get(),
                &ia_na_options,
            )),
            v6::DhcpOption::SolMaxRt(sol_max_rt),
        ];
        let builder = v6::MessageBuilder::new(v6::MessageType::Reply, *transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let actions = client.handle_message_receive(msg, time);
        assert_eq!(actions, &[]);
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
            &client;

        let RenewingOrRebindingInner {
            client_id,
            non_temporary_addresses,
            delegated_prefixes,
            server_id,
            dns_servers,
            start_time: _,
            retrans_timeout: _,
            solicit_max_rt: got_sol_max_rt,
        } = with_state(state);
        assert_eq!(*client_id, CLIENT_ID);
        fn expected_values<V: IaValue>(
            value: V,
            time: Instant,
        ) -> HashMap<v6::IAID, IaEntry<V, Instant>> {
            std::iter::once((
                v6::IAID::new(0),
                IaEntry::new_assigned(value, PREFERRED_LIFETIME, VALID_LIFETIME, time),
            ))
            .collect()
        }
        assert_eq!(*non_temporary_addresses, expected_values(addr, time));
        assert_eq!(*delegated_prefixes, expected_values(prefix, time));
        assert_eq!(*server_id, SERVER_ID[0]);
        assert_eq!(dns_servers, &[] as &[Ipv6Addr]);
        assert_eq!(*got_sol_max_rt, Duration::from_secs(sol_max_rt.into()));
        assert_matches!(&actions[..], []);
    }

    struct ReceiveReplyWithMissingIasTestCase {
        present_ia_na_iaids: Vec<v6::IAID>,
        present_ia_pd_iaids: Vec<v6::IAID>,
    }

    #[test_case(
        REBIND_TEST,
        ReceiveReplyWithMissingIasTestCase {
            present_ia_na_iaids: Vec::new(),
            present_ia_pd_iaids: Vec::new(),
        }; "none presenet")]
    #[test_case(
        RENEW_TEST,
        ReceiveReplyWithMissingIasTestCase {
            present_ia_na_iaids: vec![v6::IAID::new(0)],
            present_ia_pd_iaids: Vec::new(),
        }; "only one IA_NA present")]
    #[test_case(
        RENEW_TEST,
        ReceiveReplyWithMissingIasTestCase {
            present_ia_na_iaids: Vec::new(),
            present_ia_pd_iaids: vec![v6::IAID::new(1)],
        }; "only one IA_PD present")]
    #[test_case(
        REBIND_TEST,
        ReceiveReplyWithMissingIasTestCase {
            present_ia_na_iaids: vec![v6::IAID::new(0), v6::IAID::new(1)],
            present_ia_pd_iaids: Vec::new(),
        }; "only both IA_NAs present")]
    #[test_case(
        REBIND_TEST,
        ReceiveReplyWithMissingIasTestCase {
            present_ia_na_iaids: Vec::new(),
            present_ia_pd_iaids: vec![v6::IAID::new(0), v6::IAID::new(1)],
        }; "only both IA_PDs present")]
    #[test_case(
        REBIND_TEST,
        ReceiveReplyWithMissingIasTestCase {
            present_ia_na_iaids: vec![v6::IAID::new(1)],
            present_ia_pd_iaids: vec![v6::IAID::new(0), v6::IAID::new(1)],
        }; "both IA_PDs and one IA_NA present")]
    #[test_case(
        REBIND_TEST,
        ReceiveReplyWithMissingIasTestCase {
            present_ia_na_iaids: vec![v6::IAID::new(0), v6::IAID::new(1)],
            present_ia_pd_iaids: vec![v6::IAID::new(0)],
        }; "both IA_NAs and one IA_PD present")]
    fn receive_reply_with_missing_ias(
        RenewRebindTest {
            send_and_assert,
            message_type: _,
            expect_server_id: _,
            with_state,
            allow_response_from_any_server: _,
        }: RenewRebindTest,
        ReceiveReplyWithMissingIasTestCase {
            present_ia_na_iaids,
            present_ia_pd_iaids,
        }: ReceiveReplyWithMissingIasTestCase,
    ) {
        let non_temporary_addresses = &CONFIGURED_NON_TEMPORARY_ADDRESSES[0..2];
        let delegated_prefixes = &CONFIGURED_DELEGATED_PREFIXES[0..2];
        let time = Instant::now();
        let mut client = send_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            non_temporary_addresses.iter().copied().map(TestIaNa::new_default).collect(),
            delegated_prefixes.iter().copied().map(TestIaPd::new_default).collect(),
            None,
            T1,
            T2,
            v6::NonZeroTimeValue::Finite(VALID_LIFETIME),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );
        let ClientStateMachine { transaction_id, options_to_request: _, state: _, rng: _ } =
            &client;
        // The server includes only the IA with ID equal to `present_iaid` in the
        // reply.
        let buf = TestMessageBuilder {
            transaction_id: *transaction_id,
            message_type: v6::MessageType::Reply,
            client_id: &CLIENT_ID,
            server_id: &SERVER_ID[0],
            preference: None,
            dns_servers: None,
            ia_nas: present_ia_na_iaids.iter().map(|iaid| {
                (
                    *iaid,
                    TestIa::new_renewed_default(
                        CONFIGURED_NON_TEMPORARY_ADDRESSES[iaid.get() as usize],
                    ),
                )
            }),
            ia_pds: present_ia_pd_iaids.iter().map(|iaid| {
                (
                    *iaid,
                    TestIa::new_renewed_default(CONFIGURED_DELEGATED_PREFIXES[iaid.get() as usize]),
                )
            }),
        }
        .build();
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let actions = client.handle_message_receive(msg, time);
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
            &client;
        // Only the IA that is present will have its lifetimes updated.
        {
            let RenewingOrRebindingInner {
                client_id,
                non_temporary_addresses: got_non_temporary_addresses,
                delegated_prefixes: got_delegated_prefixes,
                server_id,
                dns_servers,
                start_time: _,
                retrans_timeout: _,
                solicit_max_rt,
            } = with_state(state);
            assert_eq!(*client_id, CLIENT_ID);
            fn expected_values<V: IaValue>(
                values: &[V],
                present_iaids: Vec<v6::IAID>,
                time: Instant,
            ) -> HashMap<v6::IAID, IaEntry<V, Instant>> {
                (0..)
                    .map(v6::IAID::new)
                    .zip(values)
                    .map(|(iaid, &value)| {
                        (
                            iaid,
                            if present_iaids.contains(&iaid) {
                                IaEntry::new_assigned(
                                    value,
                                    RENEWED_PREFERRED_LIFETIME,
                                    RENEWED_VALID_LIFETIME,
                                    time,
                                )
                            } else {
                                IaEntry::new_assigned(
                                    value,
                                    PREFERRED_LIFETIME,
                                    VALID_LIFETIME,
                                    time,
                                )
                            },
                        )
                    })
                    .collect()
            }
            assert_eq!(
                *got_non_temporary_addresses,
                expected_values(non_temporary_addresses, present_ia_na_iaids, time)
            );
            assert_eq!(
                *got_delegated_prefixes,
                expected_values(delegated_prefixes, present_ia_pd_iaids, time)
            );
            assert_eq!(*server_id, SERVER_ID[0]);
            assert_eq!(dns_servers, &[] as &[Ipv6Addr]);
            assert_eq!(*solicit_max_rt, MAX_SOLICIT_TIMEOUT);
        }
        // The client relies on retransmission to send another Renew, so no actions are needed.
        assert_matches!(&actions[..], []);
    }

    #[test_case(RENEW_TEST)]
    #[test_case(REBIND_TEST)]
    fn receive_reply_with_missing_ia_suboption_for_assigned_entry_does_not_extend_lifetime(
        RenewRebindTest {
            send_and_assert,
            message_type: _,
            expect_server_id: _,
            with_state: _,
            allow_response_from_any_server: _,
        }: RenewRebindTest,
    ) {
        const IA_NA_WITHOUT_ADDRESS_IAID: v6::IAID = v6::IAID::new(0);
        const IA_PD_WITHOUT_PREFIX_IAID: v6::IAID = v6::IAID::new(1);

        let time = Instant::now();
        let mut client = send_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            CONFIGURED_NON_TEMPORARY_ADDRESSES.iter().copied().map(TestIaNa::new_default).collect(),
            CONFIGURED_DELEGATED_PREFIXES.iter().copied().map(TestIaPd::new_default).collect(),
            None,
            T1,
            T2,
            v6::NonZeroTimeValue::Finite(VALID_LIFETIME),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );
        let ClientStateMachine { transaction_id, options_to_request: _, state: _, rng: _ } =
            &client;
        // The server includes an IA Address/Prefix option in only one of the IAs.
        let iaaddr_opts = (0..)
            .map(v6::IAID::new)
            .zip(CONFIGURED_NON_TEMPORARY_ADDRESSES)
            .map(|(iaid, addr)| {
                (
                    iaid,
                    (iaid != IA_NA_WITHOUT_ADDRESS_IAID).then(|| {
                        [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
                            addr,
                            RENEWED_PREFERRED_LIFETIME.get(),
                            RENEWED_VALID_LIFETIME.get(),
                            &[],
                        ))]
                    }),
                )
            })
            .collect::<HashMap<_, _>>();
        let iaprefix_opts = (0..)
            .map(v6::IAID::new)
            .zip(CONFIGURED_DELEGATED_PREFIXES)
            .map(|(iaid, prefix)| {
                (
                    iaid,
                    (iaid != IA_PD_WITHOUT_PREFIX_IAID).then(|| {
                        [v6::DhcpOption::IaPrefix(v6::IaPrefixSerializer::new(
                            RENEWED_PREFERRED_LIFETIME.get(),
                            RENEWED_VALID_LIFETIME.get(),
                            prefix,
                            &[],
                        ))]
                    }),
                )
            })
            .collect::<HashMap<_, _>>();
        let options =
            [v6::DhcpOption::ClientId(&CLIENT_ID), v6::DhcpOption::ServerId(&SERVER_ID[0])]
                .into_iter()
                .chain(iaaddr_opts.iter().map(|(iaid, iaaddr_opts)| {
                    v6::DhcpOption::Iana(v6::IanaSerializer::new(
                        *iaid,
                        RENEWED_T1.get(),
                        RENEWED_T2.get(),
                        iaaddr_opts.as_ref().map_or(&[], AsRef::as_ref),
                    ))
                }))
                .chain(iaprefix_opts.iter().map(|(iaid, iaprefix_opts)| {
                    v6::DhcpOption::IaPd(v6::IaPdSerializer::new(
                        *iaid,
                        RENEWED_T1.get(),
                        RENEWED_T2.get(),
                        iaprefix_opts.as_ref().map_or(&[], AsRef::as_ref),
                    ))
                }))
                .collect::<Vec<_>>();
        let builder = v6::MessageBuilder::new(v6::MessageType::Reply, *transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let actions = client.handle_message_receive(msg, time);
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
            &client;
        // Expect the client to transition to Assigned and only extend
        // the lifetime for one IA.
        assert_matches!(
            &state,
            Some(ClientState::Assigned(Assigned {
                client_id,
                non_temporary_addresses,
                delegated_prefixes,
                server_id,
                dns_servers,
                solicit_max_rt,
                _marker,
            })) => {
                assert_eq!(client_id, &CLIENT_ID);
                fn expected_values<V: IaValueTestExt>(
                    without_value: v6::IAID,
                    time: Instant,
                ) -> HashMap<v6::IAID, IaEntry<V, Instant>> {
                    (0..)
                        .map(v6::IAID::new)
                        .zip(V::CONFIGURED)
                        .map(|(iaid, value)| {
                            let (preferred_lifetime, valid_lifetime) =
                                if iaid == without_value {
                                    (PREFERRED_LIFETIME, VALID_LIFETIME)
                                } else {
                                    (RENEWED_PREFERRED_LIFETIME, RENEWED_VALID_LIFETIME)
                                };

                            (
                                iaid,
                                IaEntry::new_assigned(
                                    value,
                                    preferred_lifetime,
                                    valid_lifetime,
                                    time,
                                ),
                            )
                        })
                        .collect()
                }
                assert_eq!(
                    non_temporary_addresses,
                    &expected_values::<Ipv6Addr>(IA_NA_WITHOUT_ADDRESS_IAID, time)
                );
                assert_eq!(
                    delegated_prefixes,
                    &expected_values::<Subnet<Ipv6Addr>>(IA_PD_WITHOUT_PREFIX_IAID, time)
                );
                assert_eq!(server_id.as_slice(), &SERVER_ID[0]);
                assert_eq!(dns_servers.as_slice(), &[] as &[Ipv6Addr]);
                assert_eq!(*solicit_max_rt, MAX_SOLICIT_TIMEOUT);
            }
        );
        assert_matches!(
            &actions[..],
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::ScheduleTimer(ClientTimerType::Renew, t1),
                Action::ScheduleTimer(ClientTimerType::Rebind, t2),
                Action::IaNaUpdates(iana_updates),
                Action::IaPdUpdates(iapd_updates),
                 Action::ScheduleTimer(ClientTimerType::RestartServerDiscovery, restart_time),
            ] => {
                assert_eq!(*t1, time.add(Duration::from_secs(RENEWED_T1.get().into())));
                assert_eq!(*t2, time.add(Duration::from_secs(RENEWED_T2.get().into())));
                assert_eq!(
                    *restart_time,
                    time.add(Duration::from_secs(std::cmp::max(
                        VALID_LIFETIME,
                        RENEWED_VALID_LIFETIME,
                    ).get().into()))
                );

                fn get_updates<V: IaValue>(
                    values: &[V],
                    omit_iaid: v6::IAID,
                ) -> HashMap<v6::IAID, HashMap<V, IaValueUpdateKind>> {
                    (0..).map(v6::IAID::new)
                        .zip(values.iter().cloned())
                        .filter_map(|(iaid, value)| {
                            (iaid != omit_iaid).then(|| (
                                iaid,
                                HashMap::from([(
                                    value,
                                    IaValueUpdateKind::UpdatedLifetimes(Lifetimes::new_renewed()),
                                )])
                            ))
                        })
                        .collect()
                }
                assert_eq!(
                    iana_updates,
                    &get_updates(&CONFIGURED_NON_TEMPORARY_ADDRESSES, IA_NA_WITHOUT_ADDRESS_IAID),
                );
                assert_eq!(
                    iapd_updates,
                    &get_updates(&CONFIGURED_DELEGATED_PREFIXES, IA_PD_WITHOUT_PREFIX_IAID),
                );
            }
        );
    }

    #[test_case(RENEW_TEST)]
    #[test_case(REBIND_TEST)]
    fn receive_reply_with_zero_lifetime(
        RenewRebindTest {
            send_and_assert,
            message_type: _,
            expect_server_id: _,
            with_state: _,
            allow_response_from_any_server: _,
        }: RenewRebindTest,
    ) {
        const IA_NA_ZERO_LIFETIMES_ADDRESS_IAID: v6::IAID = v6::IAID::new(0);
        const IA_PD_ZERO_LIFETIMES_PREFIX_IAID: v6::IAID = v6::IAID::new(1);

        let time = Instant::now();
        let mut client = send_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            CONFIGURED_NON_TEMPORARY_ADDRESSES.iter().copied().map(TestIaNa::new_default).collect(),
            CONFIGURED_DELEGATED_PREFIXES.iter().copied().map(TestIaPd::new_default).collect(),
            None,
            T1,
            T2,
            v6::NonZeroTimeValue::Finite(VALID_LIFETIME),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );
        let ClientStateMachine { transaction_id, options_to_request: _, state: _, rng: _ } =
            &client;
        // The server includes an IA Address/Prefix option in only one of the IAs.
        let iaaddr_opts = (0..)
            .map(v6::IAID::new)
            .zip(CONFIGURED_NON_TEMPORARY_ADDRESSES)
            .map(|(iaid, addr)| {
                let (pl, vl) = if iaid == IA_NA_ZERO_LIFETIMES_ADDRESS_IAID {
                    (0, 0)
                } else {
                    (RENEWED_PREFERRED_LIFETIME.get(), RENEWED_VALID_LIFETIME.get())
                };

                (iaid, [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(addr, pl, vl, &[]))])
            })
            .collect::<HashMap<_, _>>();
        let iaprefix_opts = (0..)
            .map(v6::IAID::new)
            .zip(CONFIGURED_DELEGATED_PREFIXES)
            .map(|(iaid, prefix)| {
                let (pl, vl) = if iaid == IA_PD_ZERO_LIFETIMES_PREFIX_IAID {
                    (0, 0)
                } else {
                    (RENEWED_PREFERRED_LIFETIME.get(), RENEWED_VALID_LIFETIME.get())
                };

                (iaid, [v6::DhcpOption::IaPrefix(v6::IaPrefixSerializer::new(pl, vl, prefix, &[]))])
            })
            .collect::<HashMap<_, _>>();
        let options =
            [v6::DhcpOption::ClientId(&CLIENT_ID), v6::DhcpOption::ServerId(&SERVER_ID[0])]
                .into_iter()
                .chain(iaaddr_opts.iter().map(|(iaid, iaaddr_opts)| {
                    v6::DhcpOption::Iana(v6::IanaSerializer::new(
                        *iaid,
                        RENEWED_T1.get(),
                        RENEWED_T2.get(),
                        iaaddr_opts.as_ref(),
                    ))
                }))
                .chain(iaprefix_opts.iter().map(|(iaid, iaprefix_opts)| {
                    v6::DhcpOption::IaPd(v6::IaPdSerializer::new(
                        *iaid,
                        RENEWED_T1.get(),
                        RENEWED_T2.get(),
                        iaprefix_opts.as_ref(),
                    ))
                }))
                .collect::<Vec<_>>();
        let builder = v6::MessageBuilder::new(v6::MessageType::Reply, *transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let actions = client.handle_message_receive(msg, time);
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
            &client;
        // Expect the client to transition to Assigned and only extend
        // the lifetime for one IA.
        assert_matches!(
            &state,
            Some(ClientState::Assigned(Assigned {
                client_id,
                non_temporary_addresses,
                delegated_prefixes,
                server_id,
                dns_servers,
                solicit_max_rt,
                _marker,
            })) => {
                assert_eq!(client_id, &CLIENT_ID);
                fn expected_values<V: IaValueTestExt>(
                    zero_lifetime_iaid: v6::IAID,
                    time: Instant,
                ) -> HashMap<v6::IAID, IaEntry<V, Instant>> {
                    (0..)
                        .map(v6::IAID::new)
                        .zip(V::CONFIGURED)
                        .map(|(iaid, value)| {
                            (
                                iaid,
                                if iaid == zero_lifetime_iaid {
                                    IaEntry::ToRequest(HashSet::from([value]))
                                } else {
                                    IaEntry::new_assigned(
                                        value,
                                        RENEWED_PREFERRED_LIFETIME,
                                        RENEWED_VALID_LIFETIME,
                                        time,
                                    )
                                },
                            )
                        })
                        .collect()
                }
                assert_eq!(
                    non_temporary_addresses,
                    &expected_values::<Ipv6Addr>(IA_NA_ZERO_LIFETIMES_ADDRESS_IAID, time)
                );
                assert_eq!(
                    delegated_prefixes,
                    &expected_values::<Subnet<Ipv6Addr>>(IA_PD_ZERO_LIFETIMES_PREFIX_IAID, time)
                );
                assert_eq!(server_id.as_slice(), &SERVER_ID[0]);
                assert_eq!(dns_servers.as_slice(), &[] as &[Ipv6Addr]);
                assert_eq!(*solicit_max_rt, MAX_SOLICIT_TIMEOUT);
            }
        );
        assert_matches!(
            &actions[..],
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::ScheduleTimer(ClientTimerType::Renew, t1),
                Action::ScheduleTimer(ClientTimerType::Rebind, t2),
                Action::IaNaUpdates(iana_updates),
                Action::IaPdUpdates(iapd_updates),
                Action::ScheduleTimer(ClientTimerType::RestartServerDiscovery, restart_time),
            ] => {
                assert_eq!(*t1, time.add(Duration::from_secs(RENEWED_T1.get().into())));
                assert_eq!(*t2, time.add(Duration::from_secs(RENEWED_T2.get().into())));
                assert_eq!(
                    *restart_time,
                    time.add(Duration::from_secs(RENEWED_VALID_LIFETIME.get().into()))
                );

                fn get_updates<V: IaValue>(
                    values: &[V],
                    omit_iaid: v6::IAID,
                ) -> HashMap<v6::IAID, HashMap<V, IaValueUpdateKind>> {
                    (0..).map(v6::IAID::new)
                        .zip(values.iter().cloned())
                        .map(|(iaid, value)| (
                            iaid,
                            HashMap::from([(
                                value,
                                if iaid == omit_iaid {
                                    IaValueUpdateKind::Removed
                                } else {
                                    IaValueUpdateKind::UpdatedLifetimes(Lifetimes::new_renewed())
                                }
                            )]),
                        ))
                        .collect()
                }
                assert_eq!(
                    iana_updates,
                    &get_updates(
                        &CONFIGURED_NON_TEMPORARY_ADDRESSES,
                        IA_NA_ZERO_LIFETIMES_ADDRESS_IAID,
                    ),
                );
                assert_eq!(
                    iapd_updates,
                    &get_updates(
                        &CONFIGURED_DELEGATED_PREFIXES,
                        IA_PD_ZERO_LIFETIMES_PREFIX_IAID,
                    ),
                );
            }
        );
    }

    #[test_case(RENEW_TEST)]
    #[test_case(REBIND_TEST)]
    fn receive_reply_with_original_ia_value_omitted(
        RenewRebindTest {
            send_and_assert,
            message_type: _,
            expect_server_id: _,
            with_state: _,
            allow_response_from_any_server: _,
        }: RenewRebindTest,
    ) {
        let time = Instant::now();
        let mut client = send_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            vec![TestIaNa::new_default(CONFIGURED_NON_TEMPORARY_ADDRESSES[0])],
            vec![TestIaPd::new_default(CONFIGURED_DELEGATED_PREFIXES[0])],
            None,
            T1,
            T2,
            v6::NonZeroTimeValue::Finite(VALID_LIFETIME),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );
        let ClientStateMachine { transaction_id, options_to_request: _, state: _, rng: _ } =
            &client;
        // The server includes IAs with different values from what was
        // previously assigned.
        let iaid = v6::IAID::new(0);
        let buf = TestMessageBuilder {
            transaction_id: *transaction_id,
            message_type: v6::MessageType::Reply,
            client_id: &CLIENT_ID,
            server_id: &SERVER_ID[0],
            preference: None,
            dns_servers: None,
            ia_nas: std::iter::once((
                iaid,
                TestIa::new_renewed_default(RENEW_NON_TEMPORARY_ADDRESSES[0]),
            )),
            ia_pds: std::iter::once((
                iaid,
                TestIa::new_renewed_default(RENEW_DELEGATED_PREFIXES[0]),
            )),
        }
        .build();
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let actions = client.handle_message_receive(msg, time);
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
            &client;

        // Expect the client to transition to Assigned with both the new value
        // found in the latest Reply and the original value found when we first
        // transitioned to Assigned above. We always keep the old value even
        // though it was missing from the received Reply since the server did
        // not send an IA Address/Prefix option with the zero valid lifetime.
        assert_matches!(
            &state,
            Some(ClientState::Assigned(Assigned {
                client_id,
                non_temporary_addresses,
                delegated_prefixes,
                server_id,
                dns_servers,
                solicit_max_rt,
                _marker,
            })) => {
                assert_eq!(client_id, &CLIENT_ID);
                fn calc_expected<V: IaValue>(
                    iaid: v6::IAID,
                    time: Instant,
                    initial: V,
                    in_renew: V,
                ) -> HashMap<v6::IAID, IaEntry<V, Instant>> {
                    HashMap::from([(
                        iaid,
                        IaEntry::Assigned(HashMap::from([
                            (
                                initial,
                                LifetimesInfo {
                                    lifetimes: Lifetimes::new_finite(
                                        PREFERRED_LIFETIME,
                                        VALID_LIFETIME,
                                    ),
                                    updated_at: time,
                                }
                            ),
                            (
                                in_renew,
                                LifetimesInfo {
                                    lifetimes: Lifetimes::new_finite(
                                        RENEWED_PREFERRED_LIFETIME,
                                        RENEWED_VALID_LIFETIME,
                                    ),
                                    updated_at: time,
                                },
                            ),
                        ])),
                    )])
                }
                assert_eq!(
                    non_temporary_addresses,
                    &calc_expected(
                        iaid,
                        time,
                        CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
                        RENEW_NON_TEMPORARY_ADDRESSES[0],
                    )
                );
                assert_eq!(
                    delegated_prefixes,
                    &calc_expected(
                        iaid,
                        time,
                        CONFIGURED_DELEGATED_PREFIXES[0],
                        RENEW_DELEGATED_PREFIXES[0],
                    )
                );
                assert_eq!(server_id.as_slice(), &SERVER_ID[0]);
                assert_eq!(dns_servers.as_slice(), &[] as &[Ipv6Addr]);
                assert_eq!(*solicit_max_rt, MAX_SOLICIT_TIMEOUT);
            }
        );
        assert_matches!(
            &actions[..],
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::ScheduleTimer(ClientTimerType::Renew, t1),
                Action::ScheduleTimer(ClientTimerType::Rebind, t2),
                Action::IaNaUpdates(iana_updates),
                Action::IaPdUpdates(iapd_updates),
                Action::ScheduleTimer(ClientTimerType::RestartServerDiscovery, restart_time),
            ] => {
                assert_eq!(*t1, time.add(Duration::from_secs(RENEWED_T1.get().into())));
                assert_eq!(*t2, time.add(Duration::from_secs(RENEWED_T2.get().into())));
                assert_eq!(
                    *restart_time,
                    time.add(Duration::from_secs(std::cmp::max(
                        VALID_LIFETIME,
                        RENEWED_VALID_LIFETIME,
                    ).get().into()))
                );

                fn get_updates<V: IaValue>(
                    iaid: v6::IAID,
                    new_value: V,
                ) -> HashMap<v6::IAID, HashMap<V, IaValueUpdateKind>> {
                    HashMap::from([
                        (
                            iaid,
                            HashMap::from([
                                (
                                    new_value,
                                    IaValueUpdateKind::Added(Lifetimes::new_renewed()),
                                ),
                            ])
                        ),
                    ])
                }

                assert_eq!(
                    iana_updates,
                    &get_updates(
                        iaid,
                        RENEW_NON_TEMPORARY_ADDRESSES[0],
                    ),
                );
                assert_eq!(
                    iapd_updates,
                    &get_updates(
                        iaid,
                        RENEW_DELEGATED_PREFIXES[0],
                    ),
                );
            }
        );
    }

    struct NoBindingTestCase {
        ia_na_no_binding: bool,
        ia_pd_no_binding: bool,
    }

    #[test_case(
        RENEW_TEST,
        NoBindingTestCase {
            ia_na_no_binding: true,
            ia_pd_no_binding: false,
        }
    )]
    #[test_case(
        REBIND_TEST,
        NoBindingTestCase {
            ia_na_no_binding: true,
            ia_pd_no_binding: false,
        }
    )]
    #[test_case(
        RENEW_TEST,
        NoBindingTestCase {
            ia_na_no_binding: false,
            ia_pd_no_binding: true,
        }
    )]
    #[test_case(
        REBIND_TEST,
        NoBindingTestCase {
            ia_na_no_binding: false,
            ia_pd_no_binding: true,
        }
    )]
    #[test_case(
        RENEW_TEST,
        NoBindingTestCase {
            ia_na_no_binding: true,
            ia_pd_no_binding: true,
        }
    )]
    #[test_case(
        REBIND_TEST,
        NoBindingTestCase {
            ia_na_no_binding: true,
            ia_pd_no_binding: true,
        }
    )]
    fn no_binding(
        RenewRebindTest {
            send_and_assert,
            message_type: _,
            expect_server_id: _,
            with_state: _,
            allow_response_from_any_server: _,
        }: RenewRebindTest,
        NoBindingTestCase { ia_na_no_binding, ia_pd_no_binding }: NoBindingTestCase,
    ) {
        const NUM_IAS: u32 = 2;
        const NO_BINDING_IA_IDX: usize = (NUM_IAS - 1) as usize;

        fn to_assign<V: IaValueTestExt>() -> Vec<TestIa<V>> {
            V::CONFIGURED[0..usize::try_from(NUM_IAS).unwrap()]
                .iter()
                .copied()
                .map(TestIa::new_default)
                .collect()
        }
        let time = Instant::now();
        let non_temporary_addresses_to_assign = to_assign::<Ipv6Addr>();
        let delegated_prefixes_to_assign = to_assign::<Subnet<Ipv6Addr>>();
        let mut client = send_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            non_temporary_addresses_to_assign.clone(),
            delegated_prefixes_to_assign.clone(),
            None,
            T1,
            T2,
            v6::NonZeroTimeValue::Finite(VALID_LIFETIME),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );
        let ClientStateMachine { transaction_id, options_to_request: _, state: _, rng: _ } =
            &client;

        // Build a reply with NoBinding status..
        let iaaddr_opts = (0..usize::try_from(NUM_IAS).unwrap())
            .map(|i| {
                if i == NO_BINDING_IA_IDX && ia_na_no_binding {
                    [v6::DhcpOption::StatusCode(
                        v6::ErrorStatusCode::NoBinding.into(),
                        "Binding not found.",
                    )]
                } else {
                    [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
                        CONFIGURED_NON_TEMPORARY_ADDRESSES[i],
                        RENEWED_PREFERRED_LIFETIME.get(),
                        RENEWED_VALID_LIFETIME.get(),
                        &[],
                    ))]
                }
            })
            .collect::<Vec<_>>();
        let iaprefix_opts = (0..usize::try_from(NUM_IAS).unwrap())
            .map(|i| {
                if i == NO_BINDING_IA_IDX && ia_pd_no_binding {
                    [v6::DhcpOption::StatusCode(
                        v6::ErrorStatusCode::NoBinding.into(),
                        "Binding not found.",
                    )]
                } else {
                    [v6::DhcpOption::IaPrefix(v6::IaPrefixSerializer::new(
                        RENEWED_PREFERRED_LIFETIME.get(),
                        RENEWED_VALID_LIFETIME.get(),
                        CONFIGURED_DELEGATED_PREFIXES[i],
                        &[],
                    ))]
                }
            })
            .collect::<Vec<_>>();
        let options =
            [v6::DhcpOption::ClientId(&CLIENT_ID), v6::DhcpOption::ServerId(&SERVER_ID[0])]
                .into_iter()
                .chain((0..NUM_IAS).map(|id| {
                    v6::DhcpOption::Iana(v6::IanaSerializer::new(
                        v6::IAID::new(id),
                        RENEWED_T1.get(),
                        RENEWED_T2.get(),
                        &iaaddr_opts[id as usize],
                    ))
                }))
                .chain((0..NUM_IAS).map(|id| {
                    v6::DhcpOption::IaPd(v6::IaPdSerializer::new(
                        v6::IAID::new(id),
                        RENEWED_T1.get(),
                        RENEWED_T2.get(),
                        &iaprefix_opts[id as usize],
                    ))
                }))
                .collect::<Vec<_>>();

        let builder = v6::MessageBuilder::new(v6::MessageType::Reply, *transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        let actions = client.handle_message_receive(msg, time);
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
            &client;
        // Expect the client to transition to Requesting.
        {
            let Requesting {
                client_id,
                non_temporary_addresses,
                delegated_prefixes,
                server_id,
                collected_advertise: _,
                first_request_time: _,
                retrans_timeout: _,
                transmission_count: _,
                solicit_max_rt,
            } = assert_matches!(
                &state,
                Some(ClientState::Requesting(requesting)) => requesting
            );
            assert_eq!(*client_id, CLIENT_ID);
            fn expected_values<V: IaValueTestExt>(
                no_binding: bool,
                time: Instant,
            ) -> HashMap<v6::IAID, IaEntry<V, Instant>> {
                (0..NUM_IAS)
                    .map(|i| {
                        (v6::IAID::new(i), {
                            let i = usize::try_from(i).unwrap();
                            if i == NO_BINDING_IA_IDX && no_binding {
                                IaEntry::ToRequest(HashSet::from([V::CONFIGURED[i]]))
                            } else {
                                IaEntry::new_assigned(
                                    V::CONFIGURED[i],
                                    RENEWED_PREFERRED_LIFETIME,
                                    RENEWED_VALID_LIFETIME,
                                    time,
                                )
                            }
                        })
                    })
                    .collect()
            }
            assert_eq!(
                *non_temporary_addresses,
                expected_values::<Ipv6Addr>(ia_na_no_binding, time)
            );
            assert_eq!(
                *delegated_prefixes,
                expected_values::<Subnet<Ipv6Addr>>(ia_pd_no_binding, time)
            );
            assert_eq!(*server_id, SERVER_ID[0]);
            assert_eq!(*solicit_max_rt, MAX_SOLICIT_TIMEOUT);
        }
        let buf = assert_matches!(
            &actions[..],
            [
                // TODO(https://fxbug.dev/96674): should include action to
                // remove the address of IA with NoBinding status.
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::SendMessage(buf),
                Action::ScheduleTimer(ClientTimerType::Retransmission, instant)
            ] => {
                assert_eq!(*instant, time.add(INITIAL_REQUEST_TIMEOUT));
                buf
            }
        );
        // Expect that the Request message contains both the assigned address
        // and the address to request.
        testutil::assert_outgoing_stateful_message(
            &buf,
            v6::MessageType::Request,
            &CLIENT_ID,
            Some(&SERVER_ID[0]),
            &[],
            &(0..NUM_IAS)
                .map(v6::IAID::new)
                .zip(CONFIGURED_NON_TEMPORARY_ADDRESSES.into_iter().map(|a| HashSet::from([a])))
                .collect(),
            &(0..NUM_IAS)
                .map(v6::IAID::new)
                .zip(CONFIGURED_DELEGATED_PREFIXES.into_iter().map(|p| HashSet::from([p])))
                .collect(),
        );

        // While we are in requesting state after being in Assigned, make sure
        // all addresses may be invalidated.
        handle_all_leases_invalidated(
            client,
            CLIENT_ID,
            non_temporary_addresses_to_assign,
            delegated_prefixes_to_assign,
            ia_na_no_binding.then_some(NO_BINDING_IA_IDX),
            ia_pd_no_binding.then_some(NO_BINDING_IA_IDX),
            &[],
        )
    }

    struct ReceiveReplyCalculateT1T2 {
        ia_na_success_t1: v6::NonZeroOrMaxU32,
        ia_na_success_t2: v6::NonZeroOrMaxU32,
        ia_pd_success_t1: v6::NonZeroOrMaxU32,
        ia_pd_success_t2: v6::NonZeroOrMaxU32,
    }

    const TINY_NON_ZERO_OR_MAX_U32: v6::NonZeroOrMaxU32 =
        const_unwrap::const_unwrap_option(v6::NonZeroOrMaxU32::new(10));
    const SMALL_NON_ZERO_OR_MAX_U32: v6::NonZeroOrMaxU32 =
        const_unwrap::const_unwrap_option(v6::NonZeroOrMaxU32::new(100));
    const MEDIUM_NON_ZERO_OR_MAX_U32: v6::NonZeroOrMaxU32 =
        const_unwrap::const_unwrap_option(v6::NonZeroOrMaxU32::new(1000));
    const LARGE_NON_ZERO_OR_MAX_U32: v6::NonZeroOrMaxU32 =
        const_unwrap::const_unwrap_option(v6::NonZeroOrMaxU32::new(10000));

    #[test_case(
        RENEW_TEST,
        ReceiveReplyCalculateT1T2 {
            ia_na_success_t1: TINY_NON_ZERO_OR_MAX_U32,
            ia_na_success_t2: TINY_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t1: TINY_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t2: TINY_NON_ZERO_OR_MAX_U32,
        }; "renew lifetimes matching erroneous IAs")]
    #[test_case(
        RENEW_TEST,
        ReceiveReplyCalculateT1T2 {
            ia_na_success_t1: MEDIUM_NON_ZERO_OR_MAX_U32,
            ia_na_success_t2: LARGE_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t1: MEDIUM_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t2: LARGE_NON_ZERO_OR_MAX_U32,
        }; "renew same lifetimes")]
    #[test_case(
        RENEW_TEST,
        ReceiveReplyCalculateT1T2 {
            ia_na_success_t1: SMALL_NON_ZERO_OR_MAX_U32,
            ia_na_success_t2: MEDIUM_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t1: MEDIUM_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t2: LARGE_NON_ZERO_OR_MAX_U32,
        }; "renew IA_NA smaller lifetimes")]
    #[test_case(
        RENEW_TEST,
        ReceiveReplyCalculateT1T2 {
            ia_na_success_t1: MEDIUM_NON_ZERO_OR_MAX_U32,
            ia_na_success_t2: LARGE_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t1: SMALL_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t2: MEDIUM_NON_ZERO_OR_MAX_U32,
        }; "renew IA_PD smaller lifetimes")]
    #[test_case(
        RENEW_TEST,
        ReceiveReplyCalculateT1T2 {
            ia_na_success_t1: TINY_NON_ZERO_OR_MAX_U32,
            ia_na_success_t2: LARGE_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t1: SMALL_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t2: MEDIUM_NON_ZERO_OR_MAX_U32,
        }; "renew IA_NA smaller T1 but IA_PD smaller t2")]
    #[test_case(
        RENEW_TEST,
        ReceiveReplyCalculateT1T2 {
            ia_na_success_t1: SMALL_NON_ZERO_OR_MAX_U32,
            ia_na_success_t2: MEDIUM_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t1: TINY_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t2: LARGE_NON_ZERO_OR_MAX_U32,
        }; "renew IA_PD smaller T1 but IA_NA smaller t2")]
    #[test_case(
        REBIND_TEST,
        ReceiveReplyCalculateT1T2 {
            ia_na_success_t1: TINY_NON_ZERO_OR_MAX_U32,
            ia_na_success_t2: TINY_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t1: TINY_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t2: TINY_NON_ZERO_OR_MAX_U32,
        }; "rebind lifetimes matching erroneous IAs")]
    #[test_case(
        REBIND_TEST,
        ReceiveReplyCalculateT1T2 {
            ia_na_success_t1: MEDIUM_NON_ZERO_OR_MAX_U32,
            ia_na_success_t2: LARGE_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t1: MEDIUM_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t2: LARGE_NON_ZERO_OR_MAX_U32,
        }; "rebind same lifetimes")]
    #[test_case(
        REBIND_TEST,
        ReceiveReplyCalculateT1T2 {
            ia_na_success_t1: SMALL_NON_ZERO_OR_MAX_U32,
            ia_na_success_t2: MEDIUM_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t1: MEDIUM_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t2: LARGE_NON_ZERO_OR_MAX_U32,
        }; "rebind IA_NA smaller lifetimes")]
    #[test_case(
        REBIND_TEST,
        ReceiveReplyCalculateT1T2 {
            ia_na_success_t1: MEDIUM_NON_ZERO_OR_MAX_U32,
            ia_na_success_t2: LARGE_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t1: SMALL_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t2: MEDIUM_NON_ZERO_OR_MAX_U32,
        }; "rebind IA_PD smaller lifetimes")]
    #[test_case(
        REBIND_TEST,
        ReceiveReplyCalculateT1T2 {
            ia_na_success_t1: TINY_NON_ZERO_OR_MAX_U32,
            ia_na_success_t2: LARGE_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t1: SMALL_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t2: MEDIUM_NON_ZERO_OR_MAX_U32,
        }; "rebind IA_NA smaller T1 but IA_PD smaller t2")]
    #[test_case(
        REBIND_TEST,
        ReceiveReplyCalculateT1T2 {
            ia_na_success_t1: SMALL_NON_ZERO_OR_MAX_U32,
            ia_na_success_t2: MEDIUM_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t1: TINY_NON_ZERO_OR_MAX_U32,
            ia_pd_success_t2: LARGE_NON_ZERO_OR_MAX_U32,
        }; "rebind IA_PD smaller T1 but IA_NA smaller t2")]
    // Tests that only valid IAs are considered when calculating T1/T2.
    fn receive_reply_calculate_t1_t2(
        RenewRebindTest {
            send_and_assert,
            message_type: _,
            expect_server_id: _,
            with_state: _,
            allow_response_from_any_server: _,
        }: RenewRebindTest,
        ReceiveReplyCalculateT1T2 {
            ia_na_success_t1,
            ia_na_success_t2,
            ia_pd_success_t1,
            ia_pd_success_t2,
        }: ReceiveReplyCalculateT1T2,
    ) {
        let time = Instant::now();
        let mut client = send_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            CONFIGURED_NON_TEMPORARY_ADDRESSES.into_iter().map(TestIaNa::new_default).collect(),
            CONFIGURED_DELEGATED_PREFIXES.into_iter().map(TestIaPd::new_default).collect(),
            None,
            T1,
            T2,
            v6::NonZeroTimeValue::Finite(VALID_LIFETIME),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );
        let ClientStateMachine { transaction_id, options_to_request: _, state: _, rng: _ } =
            &client;
        let ia_addr = [v6::DhcpOption::IaAddr(v6::IaAddrSerializer::new(
            CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
            RENEWED_PREFERRED_LIFETIME.get(),
            RENEWED_VALID_LIFETIME.get(),
            &[],
        ))];
        let ia_no_addrs_avail = [v6::DhcpOption::StatusCode(
            v6::ErrorStatusCode::NoAddrsAvail.into(),
            "No address available.",
        )];
        let ia_prefix = [v6::DhcpOption::IaPrefix(v6::IaPrefixSerializer::new(
            RENEWED_PREFERRED_LIFETIME.get(),
            RENEWED_VALID_LIFETIME.get(),
            CONFIGURED_DELEGATED_PREFIXES[0],
            &[],
        ))];
        let ia_no_prefixes_avail = [v6::DhcpOption::StatusCode(
            v6::ErrorStatusCode::NoPrefixAvail.into(),
            "No prefixes available.",
        )];
        let ok_iaid = v6::IAID::new(0);
        let no_value_avail_iaid = v6::IAID::new(1);
        let empty_values_iaid = v6::IAID::new(2);
        let options = vec![
            v6::DhcpOption::ClientId(&CLIENT_ID),
            v6::DhcpOption::ServerId(&SERVER_ID[0]),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                ok_iaid,
                ia_na_success_t1.get(),
                ia_na_success_t2.get(),
                &ia_addr,
            )),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                no_value_avail_iaid,
                // If the server returns an IA with status code indicating
                // failure, the T1/T2 values for that IA should not be included
                // in the T1/T2 calculation.
                TINY_NON_ZERO_OR_MAX_U32.get(),
                TINY_NON_ZERO_OR_MAX_U32.get(),
                &ia_no_addrs_avail,
            )),
            v6::DhcpOption::Iana(v6::IanaSerializer::new(
                empty_values_iaid,
                // If the server returns an IA_NA with no IA Address option, the
                // T1/T2 values for that IA should not be included in the T1/T2
                // calculation.
                TINY_NON_ZERO_OR_MAX_U32.get(),
                TINY_NON_ZERO_OR_MAX_U32.get(),
                &[],
            )),
            v6::DhcpOption::IaPd(v6::IaPdSerializer::new(
                ok_iaid,
                ia_pd_success_t1.get(),
                ia_pd_success_t2.get(),
                &ia_prefix,
            )),
            v6::DhcpOption::IaPd(v6::IaPdSerializer::new(
                no_value_avail_iaid,
                // If the server returns an IA with status code indicating
                // failure, the T1/T2 values for that IA should not be included
                // in the T1/T2 calculation.
                TINY_NON_ZERO_OR_MAX_U32.get(),
                TINY_NON_ZERO_OR_MAX_U32.get(),
                &ia_no_prefixes_avail,
            )),
            v6::DhcpOption::IaPd(v6::IaPdSerializer::new(
                empty_values_iaid,
                // If the server returns an IA_PD with no IA Prefix option, the
                // T1/T2 values for that IA should not be included in the T1/T2
                // calculation.
                TINY_NON_ZERO_OR_MAX_U32.get(),
                TINY_NON_ZERO_OR_MAX_U32.get(),
                &[],
            )),
        ];

        let builder = v6::MessageBuilder::new(v6::MessageType::Reply, *transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");

        fn get_updates<V: IaValue>(
            ok_iaid: v6::IAID,
            ok_value: V,
            no_value_avail_iaid: v6::IAID,
            no_value_avail_value: V,
        ) -> HashMap<v6::IAID, HashMap<V, IaValueUpdateKind>> {
            HashMap::from([
                (
                    ok_iaid,
                    HashMap::from([(
                        ok_value,
                        IaValueUpdateKind::UpdatedLifetimes(Lifetimes::new_renewed()),
                    )]),
                ),
                (
                    no_value_avail_iaid,
                    HashMap::from([(no_value_avail_value, IaValueUpdateKind::Removed)]),
                ),
            ])
        }
        let expected_t1 = std::cmp::min(ia_na_success_t1, ia_pd_success_t1);
        let expected_t2 = std::cmp::min(ia_na_success_t2, ia_pd_success_t2);
        assert_eq!(
            client.handle_message_receive(msg, time),
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                if expected_t1 == expected_t2 {
                    // Skip Renew and just go to Rebind when T2 == T1.
                    Action::CancelTimer(ClientTimerType::Renew)
                } else {
                    Action::ScheduleTimer(
                        ClientTimerType::Renew,
                        time.add(Duration::from_secs(expected_t1.get().into())),
                    )
                },
                Action::ScheduleTimer(
                    ClientTimerType::Rebind,
                    time.add(Duration::from_secs(expected_t2.get().into())),
                ),
                Action::IaNaUpdates(get_updates(
                    ok_iaid,
                    CONFIGURED_NON_TEMPORARY_ADDRESSES[0],
                    no_value_avail_iaid,
                    CONFIGURED_NON_TEMPORARY_ADDRESSES[1],
                )),
                Action::IaPdUpdates(get_updates(
                    ok_iaid,
                    CONFIGURED_DELEGATED_PREFIXES[0],
                    no_value_avail_iaid,
                    CONFIGURED_DELEGATED_PREFIXES[1],
                )),
                Action::ScheduleTimer(
                    ClientTimerType::RestartServerDiscovery,
                    time.add(Duration::from_secs(
                        std::cmp::max(VALID_LIFETIME, RENEWED_VALID_LIFETIME,).get().into()
                    )),
                ),
            ],
        );
    }

    #[test]
    fn unexpected_messages_are_ignored() {
        let (mut client, _) = ClientStateMachine::start_stateless(
            [0, 1, 2],
            Vec::new(),
            StepRng::new(std::u64::MAX / 2, 0),
            Instant::now(),
        );

        let builder = v6::MessageBuilder::new(
            v6::MessageType::Reply,
            // Transaction ID is different from the client's.
            [4, 5, 6],
            &[],
        );
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");

        assert!(client.handle_message_receive(msg, Instant::now()).is_empty());

        // Messages with unsupported/unexpected types are discarded.
        for msg_type in [
            v6::MessageType::Solicit,
            v6::MessageType::Advertise,
            v6::MessageType::Request,
            v6::MessageType::Confirm,
            v6::MessageType::Renew,
            v6::MessageType::Rebind,
            v6::MessageType::Release,
            v6::MessageType::Decline,
            v6::MessageType::Reconfigure,
            v6::MessageType::InformationRequest,
            v6::MessageType::RelayForw,
            v6::MessageType::RelayRepl,
        ] {
            let ClientStateMachine { transaction_id, options_to_request: _, state: _, rng: _ } =
                &client;
            let builder = v6::MessageBuilder::new(msg_type, *transaction_id, &[]);
            let mut buf = vec![0; builder.bytes_len()];
            builder.serialize(&mut buf);
            let mut buf = &buf[..]; // Implements BufferView.
            let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");

            assert!(client.handle_message_receive(msg, Instant::now()).is_empty());
        }
    }

    #[test]
    #[should_panic(expected = "received unexpected refresh timeout")]
    fn information_requesting_refresh_timeout_is_unreachable() {
        let (mut client, _) = ClientStateMachine::start_stateless(
            [0, 1, 2],
            Vec::new(),
            StepRng::new(std::u64::MAX / 2, 0),
            Instant::now(),
        );

        // Should panic if Refresh timeout is received while in
        // InformationRequesting state.
        let _actions = client.handle_timeout(ClientTimerType::Refresh, Instant::now());
    }

    #[test]
    #[should_panic(expected = "received unexpected retransmission timeout")]
    fn information_received_retransmission_timeout_is_unreachable() {
        let (mut client, _) = ClientStateMachine::start_stateless(
            [0, 1, 2],
            Vec::new(),
            StepRng::new(std::u64::MAX / 2, 0),
            Instant::now(),
        );
        let ClientStateMachine { transaction_id, options_to_request: _, state, rng: _ } = &client;
        assert_matches!(
            *state,
            Some(ClientState::InformationRequesting(InformationRequesting {
                retrans_timeout: INITIAL_INFO_REQ_TIMEOUT,
                _marker,
            }))
        );

        let options = [v6::DhcpOption::ServerId(&SERVER_ID[0])];
        let builder = v6::MessageBuilder::new(v6::MessageType::Reply, *transaction_id, &options);
        let mut buf = vec![0; builder.bytes_len()];
        builder.serialize(&mut buf);
        let mut buf = &buf[..]; // Implements BufferView.
        let msg = v6::Message::parse(&mut buf, ()).expect("failed to parse test buffer");
        // Transition to InformationReceived state.
        let time = Instant::now();
        let actions = client.handle_message_receive(msg, time);
        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
            &client;
        assert_matches!(
            state,
            Some(ClientState::InformationReceived(InformationReceived { dns_servers, _marker }))
                if dns_servers.is_empty()
        );
        assert_eq!(
            actions[..],
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::ScheduleTimer(ClientTimerType::Refresh, time.add(IRT_DEFAULT)),
            ]
        );

        // Should panic if Retransmission timeout is received while in
        // InformationReceived state.
        let _actions = client.handle_timeout(ClientTimerType::Retransmission, time);
    }

    #[test]
    #[should_panic(expected = "received unexpected refresh timeout")]
    fn server_discovery_refresh_timeout_is_unreachable() {
        let time = Instant::now();
        let mut client = testutil::start_and_assert_server_discovery(
            [0, 1, 2],
            v6::duid_uuid(),
            testutil::to_configured_addresses(
                1,
                std::iter::once(HashSet::from([CONFIGURED_NON_TEMPORARY_ADDRESSES[0]])),
            ),
            Default::default(),
            Vec::new(),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );

        // Should panic if Refresh is received while in ServerDiscovery state.
        let _actions = client.handle_timeout(ClientTimerType::Refresh, time);
    }

    #[test]
    #[should_panic(expected = "received unexpected refresh timeout")]
    fn requesting_refresh_timeout_is_unreachable() {
        let time = Instant::now();
        let (mut client, _transaction_id) = testutil::request_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            vec![TestIaNa::new_default(CONFIGURED_NON_TEMPORARY_ADDRESSES[0])],
            Default::default(),
            &[],
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );

        // Should panic if Refresh is received while in Requesting state.
        let _actions = client.handle_timeout(ClientTimerType::Refresh, time);
    }

    #[test_case(ClientTimerType::Refresh)]
    #[test_case(ClientTimerType::Retransmission)]
    #[should_panic(expected = "received unexpected")]
    fn address_assiged_unexpected_timeout_is_unreachable(timeout: ClientTimerType) {
        let time = Instant::now();
        let (mut client, _actions) = testutil::assign_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            vec![TestIaNa::new_default(CONFIGURED_NON_TEMPORARY_ADDRESSES[0])],
            Default::default(), /* delegated_prefixes_to_assign */
            &[],
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );

        // Should panic if Refresh or Retransmission timeout is received while
        // in Assigned state.
        let _actions = client.handle_timeout(timeout, time);
    }

    #[test_case(RENEW_TEST)]
    #[test_case(REBIND_TEST)]
    #[should_panic(expected = "received unexpected refresh timeout")]
    fn refresh_timeout_is_unreachable(
        RenewRebindTest {
            send_and_assert,
            message_type: _,
            expect_server_id: _,
            with_state: _,
            allow_response_from_any_server: _,
        }: RenewRebindTest,
    ) {
        let time = Instant::now();
        let mut client = send_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            vec![TestIaNa::new_default(CONFIGURED_NON_TEMPORARY_ADDRESSES[0])],
            Default::default(), /* delegated_prefixes_to_assign */
            None,
            T1,
            T2,
            v6::NonZeroTimeValue::Finite(VALID_LIFETIME),
            StepRng::new(std::u64::MAX / 2, 0),
            time,
        );

        // Should panic if Refresh is received while in Renewing state.
        let _actions = client.handle_timeout(ClientTimerType::Refresh, time);
    }

    fn handle_all_leases_invalidated<R: Rng>(
        mut client: ClientStateMachine<Instant, R>,
        client_id: [u8; CLIENT_ID_LEN],
        non_temporary_addresses_to_assign: Vec<TestIaNa>,
        delegated_prefixes_to_assign: Vec<TestIaPd>,
        skip_removed_event_for_test_iana_idx: Option<usize>,
        skip_removed_event_for_test_iapd_idx: Option<usize>,
        options_to_request: &[v6::OptionCode],
    ) {
        let time = Instant::now();
        let actions = client.handle_timeout(ClientTimerType::RestartServerDiscovery, time);
        let buf = assert_matches!(
            &actions[..],
            [
                Action::CancelTimer(ClientTimerType::Retransmission),
                Action::CancelTimer(ClientTimerType::Refresh),
                Action::CancelTimer(ClientTimerType::Renew),
                Action::CancelTimer(ClientTimerType::Rebind),
                Action::CancelTimer(ClientTimerType::RestartServerDiscovery),
                Action::IaNaUpdates(ia_na_updates),
                Action::IaPdUpdates(ia_pd_updates),
                Action::SendMessage(buf),
                Action::ScheduleTimer(ClientTimerType::Retransmission, instant)
            ] => {
                fn get_updates<V: IaValue>(
                    to_assign: &Vec<TestIa<V>>,
                    skip_idx: Option<usize>,
                ) -> HashMap<v6::IAID, HashMap<V, IaValueUpdateKind>> {
                    (0..).zip(to_assign.iter())
                        .filter_map(|(iaid, TestIa { values, t1: _, t2: _})| {
                            skip_idx
                                .map_or(true, |skip_idx| skip_idx != iaid)
                                .then(|| (
                                    v6::IAID::new(iaid.try_into().unwrap()),
                                    values.keys().copied().map(|value| (
                                        value,
                                        IaValueUpdateKind::Removed,
                                    )).collect(),
                                ))
                        })
                        .collect()
                }
                assert_eq!(
                    ia_na_updates,
                    &get_updates(
                        &non_temporary_addresses_to_assign,
                        skip_removed_event_for_test_iana_idx
                    ),
                );
                assert_eq!(
                    ia_pd_updates,
                    &get_updates(
                        &delegated_prefixes_to_assign,
                        skip_removed_event_for_test_iapd_idx,
                    ),
                );
                assert_eq!(*instant, time.add(INITIAL_SOLICIT_TIMEOUT));
                buf
            }
        );

        let ClientStateMachine { transaction_id: _, options_to_request: _, state, rng: _ } =
            &client;
        testutil::assert_server_discovery(
            state,
            client_id,
            testutil::to_configured_addresses(
                non_temporary_addresses_to_assign.len(),
                non_temporary_addresses_to_assign
                    .iter()
                    .map(|TestIaNa { values, t1: _, t2: _ }| values.keys().cloned().collect()),
            ),
            testutil::to_configured_prefixes(
                delegated_prefixes_to_assign.len(),
                delegated_prefixes_to_assign
                    .iter()
                    .map(|TestIaPd { values, t1: _, t2: _ }| values.keys().cloned().collect()),
            ),
            time,
            buf,
            options_to_request,
        )
    }

    #[test]
    fn assigned_handle_all_leases_invalidated() {
        let non_temporary_addresses_to_assign = CONFIGURED_NON_TEMPORARY_ADDRESSES
            .iter()
            .copied()
            .map(TestIaNa::new_default)
            .collect::<Vec<_>>();
        let delegated_prefixes_to_assign = CONFIGURED_DELEGATED_PREFIXES
            .iter()
            .copied()
            .map(TestIaPd::new_default)
            .collect::<Vec<_>>();
        let (client, _actions) = testutil::assign_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            non_temporary_addresses_to_assign.clone(),
            delegated_prefixes_to_assign.clone(),
            &[],
            StepRng::new(std::u64::MAX / 2, 0),
            Instant::now(),
        );

        handle_all_leases_invalidated(
            client,
            CLIENT_ID,
            non_temporary_addresses_to_assign,
            delegated_prefixes_to_assign,
            None,
            None,
            &[],
        )
    }

    #[test_case(RENEW_TEST)]
    #[test_case(REBIND_TEST)]
    fn renew_rebind_handle_all_leases_invalidated(
        RenewRebindTest {
            send_and_assert,
            message_type: _,
            expect_server_id: _,
            with_state: _,
            allow_response_from_any_server: _,
        }: RenewRebindTest,
    ) {
        let non_temporary_addresses_to_assign = CONFIGURED_NON_TEMPORARY_ADDRESSES[0..2]
            .into_iter()
            .map(|&addr| TestIaNa::new_default(addr))
            .collect::<Vec<_>>();
        let delegated_prefixes_to_assign = CONFIGURED_DELEGATED_PREFIXES[0..2]
            .into_iter()
            .map(|&addr| TestIaPd::new_default(addr))
            .collect::<Vec<_>>();
        let client = send_and_assert(
            CLIENT_ID,
            SERVER_ID[0],
            non_temporary_addresses_to_assign.clone(),
            delegated_prefixes_to_assign.clone(),
            None,
            T1,
            T2,
            v6::NonZeroTimeValue::Finite(VALID_LIFETIME),
            StepRng::new(std::u64::MAX / 2, 0),
            Instant::now(),
        );

        handle_all_leases_invalidated(
            client,
            CLIENT_ID,
            non_temporary_addresses_to_assign,
            delegated_prefixes_to_assign,
            None,
            None,
            &[],
        )
    }

    // NOTE: All comparisons are done on millisecond, so this test is not affected by precision
    // loss from floating point arithmetic.
    #[test]
    fn retransmission_timeout() {
        let mut rng = StepRng::new(std::u64::MAX / 2, 0);

        let initial_rt = Duration::from_secs(1);
        let max_rt = Duration::from_secs(100);

        // Start with initial timeout if previous timeout is zero.
        let t =
            super::retransmission_timeout(Duration::from_nanos(0), initial_rt, max_rt, &mut rng);
        assert_eq!(t.as_millis(), initial_rt.as_millis());

        // Use previous timeout when it's not zero and apply the formula.
        let t =
            super::retransmission_timeout(Duration::from_secs(10), initial_rt, max_rt, &mut rng);
        assert_eq!(t, Duration::from_secs(20));

        // Cap at max timeout.
        let t = super::retransmission_timeout(100 * max_rt, initial_rt, max_rt, &mut rng);
        assert_eq!(t.as_millis(), max_rt.as_millis());
        let t = super::retransmission_timeout(MAX_DURATION, initial_rt, max_rt, &mut rng);
        assert_eq!(t.as_millis(), max_rt.as_millis());
        // Zero max means no cap.
        let t = super::retransmission_timeout(
            100 * max_rt,
            initial_rt,
            Duration::from_nanos(0),
            &mut rng,
        );
        assert_eq!(t.as_millis(), (200 * max_rt).as_millis());
        // Overflow durations are clipped.
        let t = super::retransmission_timeout(
            MAX_DURATION,
            initial_rt,
            Duration::from_nanos(0),
            &mut rng,
        );
        assert_eq!(t.as_millis(), MAX_DURATION.as_millis());

        // Steps through the range with deterministic randomness, 20% at a time.
        let mut rng = StepRng::new(0, std::u64::MAX / 5);
        [
            (Duration::from_millis(10000), 19000),
            (Duration::from_millis(10000), 19400),
            (Duration::from_millis(10000), 19800),
            (Duration::from_millis(10000), 20200),
            (Duration::from_millis(10000), 20600),
            (Duration::from_millis(10000), 21000),
            (Duration::from_millis(10000), 19400),
            // Cap at max timeout with randomness.
            (100 * max_rt, 98000),
            (100 * max_rt, 102000),
            (100 * max_rt, 106000),
            (100 * max_rt, 110000),
            (100 * max_rt, 94000),
            (100 * max_rt, 98000),
        ]
        .iter()
        .for_each(|(rt, want_ms)| {
            let t = super::retransmission_timeout(*rt, initial_rt, max_rt, &mut rng);
            assert_eq!(t.as_millis(), *want_ms);
        });
    }

    #[test_case(v6::TimeValue::Zero, v6::TimeValue::Zero, v6::TimeValue::Zero)]
    #[test_case(
        v6::TimeValue::Zero,
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(120)
                .expect("should succeed for non-zero or u32::MAX values")
        )),
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(120)
                .expect("should succeed for non-zero or u32::MAX values")
        ))
     )]
    #[test_case(
        v6::TimeValue::Zero,
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity)
    )]
    #[test_case(
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(120)
                .expect("should succeed for non-zero or u32::MAX values")
        )),
        v6::TimeValue::Zero,
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(120)
                .expect("should succeed for non-zero or u32::MAX values")
        ))
     )]
    #[test_case(
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(120)
                .expect("should succeed for non-zero or u32::MAX values")
        )),
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(60)
                .expect("should succeed for non-zero or u32::MAX values")
        )),
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(60)
                .expect("should succeed for non-zero or u32::MAX values")
        ))
     )]
    #[test_case(
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(120)
                .expect("should succeed for non-zero or u32::MAX values")
        )),
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(120)
                .expect("should succeed for non-zero or u32::MAX values")
        ))
     )]
    #[test_case(
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(120)
                .expect("should succeed for non-zero or u32::MAX values")
        )),
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(120)
                .expect("should succeed for non-zero or u32::MAX values")
        ))
     )]
    #[test_case(
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity)
    )]
    fn maybe_get_nonzero_min(
        old_value: v6::TimeValue,
        new_value: v6::TimeValue,
        expected_value: v6::TimeValue,
    ) {
        assert_eq!(super::maybe_get_nonzero_min(old_value, new_value), expected_value);
    }

    #[test_case(
        v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(120)
                .expect("should succeed for non-zero or u32::MAX values")
        ),
        v6::TimeValue::Zero,
        v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(120)
                .expect("should succeed for non-zero or u32::MAX values")
        )
    )]
    #[test_case(
        v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(120)
                .expect("should succeed for non-zero or u32::MAX values")
        ),
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(60)
                .expect("should succeed for non-zero or u32::MAX values")
        )),
        v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(60)
                .expect("should succeed for non-zero or u32::MAX values")
        )
    )]
    #[test_case(
        v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(120)
                .expect("should succeed for non-zero or u32::MAX values")
        ),
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(120)
                .expect("should succeed for non-zero or u32::MAX values")
        )
    )]
    #[test_case(
        v6::NonZeroTimeValue::Infinity,
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(120)
                .expect("should succeed for non-zero or u32::MAX values"))
        ),
        v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(120)
                .expect("should succeed for non-zero or u32::MAX values")
        )
    )]
    #[test_case(
        v6::NonZeroTimeValue::Infinity,
        v6::TimeValue::NonZero(v6::NonZeroTimeValue::Infinity),
        v6::NonZeroTimeValue::Infinity
    )]
    #[test_case(
        v6::NonZeroTimeValue::Infinity,
        v6::TimeValue::Zero,
        v6::NonZeroTimeValue::Infinity
    )]
    fn get_nonzero_min(
        old_value: v6::NonZeroTimeValue,
        new_value: v6::TimeValue,
        expected_value: v6::NonZeroTimeValue,
    ) {
        assert_eq!(super::get_nonzero_min(old_value, new_value), expected_value);
    }

    #[test_case(
        v6::NonZeroTimeValue::Infinity,
        T1_MIN_LIFETIME_RATIO,
        v6::NonZeroTimeValue::Infinity
    )]
    #[test_case(
        v6::NonZeroTimeValue::Finite(v6::NonZeroOrMaxU32::new(100).expect("should succeed")),
        T1_MIN_LIFETIME_RATIO,
        v6::NonZeroTimeValue::Finite(v6::NonZeroOrMaxU32::new(50).expect("should succeed"))
    )]
    #[test_case(v6::NonZeroTimeValue::Infinity, T2_T1_RATIO, v6::NonZeroTimeValue::Infinity)]
    #[test_case(
        v6::NonZeroTimeValue::Finite(
            v6::NonZeroOrMaxU32::new(INFINITY - 1)
                .expect("should succeed")
        ),
        T2_T1_RATIO,
        v6::NonZeroTimeValue::Infinity
    )]
    fn compute_t(min: v6::NonZeroTimeValue, ratio: Ratio<u32>, expected_t: v6::NonZeroTimeValue) {
        assert_eq!(super::compute_t(min, ratio), expected_t);
    }
}
