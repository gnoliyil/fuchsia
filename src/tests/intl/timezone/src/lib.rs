// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! See the `README.md` file for more detail.

use anyhow::{Context as _, Error, Result};
use crossbeam::channel;
use fidl_fuchsia_intl as fintl;
use fidl_fuchsia_settings as fsettings;
use fidl_fuchsia_ui_app::ViewProviderMarker;
use fidl_test_placeholders as fecho;
use fuchsia_async as fasync;
use fuchsia_async::DurationExt;
use fuchsia_component::client;
use fuchsia_runtime as runtime;
use fuchsia_scenic as scenic;
use fuchsia_zircon as zx;
use icu_data;
use rust_icu_ucal as ucal;
use rust_icu_udat as udat;
use rust_icu_uloc as uloc;
use rust_icu_ustring as ustring;
use std::convert::TryFrom;
use tracing::*;

fn intl_client() -> Result<fsettings::IntlProxy> {
    client::connect_to_protocol::<fsettings::IntlMarker>()
        .context("Failed to connect to fuchsia.settings.Intl")
}

/// Sets a timezone for the duration of its lifetime, and restores the previous timezone at the
/// end.
pub struct ScopedTimezone {
    timezone: String,
    client: fsettings::IntlProxy,
}

impl ScopedTimezone {
    /// Tries to create a new timezone setter.
    pub async fn try_new(timezone: &str) -> Result<ScopedTimezone> {
        let client = intl_client().with_context(|| "while creating intl client")?;
        let response =
            client.watch().await.with_context(|| "while creating intl client watcher")?;
        info!("setting timezone for test: {}", timezone);
        let old_timezone = response.time_zone_id.unwrap().id;
        let setter = ScopedTimezone { timezone: old_timezone, client };
        setter.set_timezone(timezone).await?;
        Ok(setter)
    }

    /// Asynchronously set the timezone based to the supplied value.
    async fn set_timezone(&self, timezone: &str) -> Result<(), Error> {
        info!("setting timezone: {}", timezone);
        let settings = fsettings::IntlSettings {
            time_zone_id: Some(fintl::TimeZoneId { id: timezone.to_string() }),
            locales: None,
            temperature_unit: None,
            hour_cycle: None,
            ..fsettings::IntlSettings::EMPTY
        };
        let _result = self.client.set(settings).await.context("setting timezone").unwrap();
        Ok(())
    }
}

impl Drop for ScopedTimezone {
    /// Restores the previous timezone setting on the system.
    fn drop(&mut self) {
        let timezone = self.timezone.clone();
        let (tx, rx) = channel::bounded(0);
        // This piece of gymnastics is needed because the FIDL call we make
        // here is async, but `drop` implementation must be sync.
        std::thread::spawn(move || {
            let mut executor = fasync::LocalExecutor::new();
            executor.run_singlethreaded(async move {
                info!("restoring timezone: {}", &timezone);
                let client = intl_client().unwrap();
                let settings = fsettings::IntlSettings {
                    time_zone_id: Some(fintl::TimeZoneId { id: timezone }),
                    locales: None,
                    temperature_unit: None,
                    hour_cycle: None,
                    ..fsettings::IntlSettings::EMPTY
                };
                let _result = client.set(settings).await.context("restoring timezone");
                tx.send(()).unwrap();
            });
        });
        rx.recv().unwrap();
        info!("restored timezone: {}", &self.timezone);
    }
}

/// Connects to `fuchsia.ui.app.ViewProvider` to cause the Flutter runner to
/// start.
fn connect_to_view_provider() -> Result<()> {
    // [START flutter_runner_trick]
    // This part is only relevant for launching Flutter apps.  Flutter will not
    // start a Dart VM unless a view is requested.
    let view_provider = client::connect_to_protocol::<ViewProviderMarker>();
    match view_provider {
        Err(_) => {
            debug!("could not connect to view provider.  This is expected in dart.")
        }
        Ok(ref view_provider) => {
            debug!("connected to view provider");
            let token_pair = scenic::ViewTokenPair::new()?;
            let mut viewref_pair = scenic::ViewRefPair::new()?;

            view_provider
                .create_view_with_view_ref(
                    token_pair.view_token.value,
                    &mut viewref_pair.control_ref,
                    &mut viewref_pair.view_ref,
                )
                .with_context(|| "could not create a scenic view")?;
        }
    }
    // [END flutter_runner_trick]
    Ok(())
}

/// Gets a timezone formatter for the given pattern, and the supplied timezone name.  Patterns are
/// described at: http://userguide.icu-project.org/formatparse/datetime
fn formatter_for_timezone(pattern: &str, timezone: &str) -> udat::UDateFormat {
    let locale = uloc::ULoc::try_from("Etc/Unknown").unwrap();
    // Example: "2020-2-26-14", hour 14 of February 26.
    let pattern = ustring::UChar::try_from(pattern).unwrap();

    udat::UDateFormat::new_with_pattern(
        &locale,
        &ustring::UChar::try_from(timezone).unwrap(),
        &pattern,
    )
    .unwrap()
}

// The timezones used in tests.
static TIMEZONE_NAME: &str = "America/Los_Angeles";
static TIMEZONE_NAME_2: &str = "America/New_York";

// Example: "2020-2-26-14", hour 14 of February 26.
static PARTIAL_TIMESTAMP_FORMAT: &str = "yyyy-M-d-H";
// Formats time with the most detail. Example: "2020-10-19T05:41:22.204-04:00".
static FULL_TIMESTAMP_FORMAT: &str = "yyyy-MM-dd'T'HH:mm:ss.SSSXXX";

/// Polls the echo server until either the local and remote time match, or a timeout
/// occurs.
async fn loop_until_matching_time(
    timezone_name: &str,
    fmt: &udat::UDateFormat,
    detailed_format: &udat::UDateFormat,
    echo: &fecho::EchoProxy,
) -> Result<(), Error> {
    const MAX_ATTEMPTS: usize = 15;
    let sleep = zx::Duration::from_millis(3000);
    let utc_format = formatter_for_timezone(FULL_TIMESTAMP_FORMAT, "UTC");

    // Multiple attempts in case the test is run close to the turn of the hour.
    for attempt in 1..=MAX_ATTEMPTS {
        info!(%attempt, "Requesting some time");
        let out = echo
            .echo_string(Some("Gimme some time!"))
            .await
            .with_context(|| "echo_string failed")?;
        let now = ucal::get_now();
        let date_time = fmt.format(now)?;
        let vm_time = out.unwrap();

        if date_time == vm_time {
            break;
        }

        info!(%vm_time, %date_time, "Times do not match");
        if attempt == MAX_ATTEMPTS {
            let clock = runtime::duplicate_utc_clock_handle(zx::Rights::READ)
                .map_err(|stat| anyhow::anyhow!("Error retreiving clock: {}", stat));
            let userspace_utc_sec = match clock {
                Ok(clk) => clk
                    .read()
                    .map_err(|stat| anyhow::anyhow!("Error reading clock: {}", stat))
                    .map(|zx_time| zx_time.into_nanos() / 1_000_000_000),
                Err(err) => Err(err),
            };

            return Err(anyhow::anyhow!(
                "dart VM time and test fixture time mismatch:\n\t\
                dart VM says the time is:               {:?}\n\t\
                `-- the test fixture says it should be: {:?}\n\t\
                expected test fixture timezone:         {}\n\t\
                the test fixture local time is:         {}\n\t\
                test fixture says UTC time is:          {}\n\t\
                test fixture ucal timestamp is:         {} sec since epoch\n\t\
                test fixture userspace timestamp is:    {:?} sec since epoch",
                vm_time,
                date_time,
                timezone_name,
                detailed_format.format(now)?,
                utc_format.format(now)?,
                now / 1000.0,
                userspace_utc_sec,
            ));
        }
        fasync::Timer::new(sleep.after_now()).await;
    }
    Ok(())
}

/// Starts a component that uses Dart's idea of the system time zone to report time zone
/// information.  The test fixture compares its own idea of local time with the one in the dart VM.
/// Ostensibly, those two times should be the same up to the current date and current hour.
/// 'get_view` is set if launching a Dart time server requires getting a ViewProvider to kick
/// off program execution -- which is unnecessary for a Dart runner but required for a Flutter
/// runner.
pub async fn check_reported_time_with_update(get_view: bool) -> Result<()> {
    let _icu_data_loader = icu_data::Loader::new().with_context(|| "could not load ICU data")?;
    let _setter = ScopedTimezone::try_new(TIMEZONE_NAME).await.unwrap();

    let formatter = formatter_for_timezone(PARTIAL_TIMESTAMP_FORMAT, TIMEZONE_NAME);
    let detailed_format = formatter_for_timezone(FULL_TIMESTAMP_FORMAT, TIMEZONE_NAME);

    if get_view {
        connect_to_view_provider().context("while connecting to fuchsia.ui.app.ViewProvider")?;
    }

    let echo = client::connect_to_protocol::<fecho::EchoMarker>()
        .context("Failed to connect to echo service")?;

    loop_until_matching_time(TIMEZONE_NAME, &formatter, &detailed_format, &echo)
        .await
        .expect("local and remote times should match eventually");

    {
        // Change the time zone again, and verify that the dart VM has seen the change
        // too.
        let _setter = ScopedTimezone::try_new(TIMEZONE_NAME_2).await.unwrap();
        let formatter = formatter_for_timezone(PARTIAL_TIMESTAMP_FORMAT, TIMEZONE_NAME_2);
        let detailed_format = formatter_for_timezone(FULL_TIMESTAMP_FORMAT, TIMEZONE_NAME_2);

        loop_until_matching_time(TIMEZONE_NAME_2, &formatter, &detailed_format, &echo)
            .await
            .expect("local and remote times should match eventually");
    }
    Ok(())
}
