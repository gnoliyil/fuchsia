// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod commands;
use {
    crate::commands::{CmdHelper, Command, ReplControl},
    anyhow::{format_err, Error},
    fidl_fuchsia_power_battery as fpower, fidl_fuchsia_power_battery_test as spower,
    fidl_fuchsia_power_battery_test::BatterySimulatorProxy,
    fuchsia_async as fasync,
    fuchsia_component::client::connect_to_protocol,
    futures::{
        channel::mpsc::{channel, SendError},
        Sink, SinkExt, Stream, StreamExt, TryFutureExt,
    },
    pin_utils::pin_mut,
    rustyline::{error::ReadlineError, CompletionType, Config, EditMode, Editor},
    std::{fmt, thread, time::Duration},
};

static PROMPT: &str = "\x1b[34mbattman>\x1b[0m ";

// ParseResult is used to convey the parsed commands input by user
// and display them either as an error or continue executing the commands
enum ParseResult<T> {
    Valid(T),
    Empty,
    Error(String),
}

// BatteryInfo is a wrapper for BatteryInfo so the print fmt could be
// implemented
struct BatteryInfo {
    info: fpower::BatteryInfo,
}

impl fmt::Display for BatteryInfo {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            concat!(
                "Battery Percentage: {:?}\n",
                "Battery Status: {:?}\n",
                "Charge Status: {:?}\n",
                "Level Status: {:?}\n",
                "Charge Source: {:?}\n",
                "Time Remaining: {:?}\n",
            ),
            self.info.level_percent,
            self.info.status,
            self.info.charge_status,
            self.info.level_status,
            self.info.charge_source,
            self.info.time_remaining.as_ref()
        )
    }
}

async fn print_battery_info(service: &BatterySimulatorProxy) -> Result<(), Error> {
    if service.is_simulating().await? {
        let bi = service.get_battery_info().await?;
        println!("{}", BatteryInfo { info: bi });
    } else {
        let battery_manager_server = connect_to_protocol::<fpower::BatteryManagerMarker>()?;
        let bi = battery_manager_server.get_battery_info().await?;
        println!("{}", BatteryInfo { info: bi });
    }
    Ok(())
}

fn print_set_help_message() {
    println!(concat!(
        "'set' modifies the state of the simulated battery. ",
        "Modifications only take place once disconnected.\n",
        "The usage of set is as follows:\n\n",
        "set <battery_attribute battery_attribute_value>\n\n",
        "The corresponding field and field values is as follows:\n\n",
        "\tBatteryPercentage\n",
        "\t\tAny int value from [1..100]\n",
        "\tChargeSource\n",
        "\t\tUNKNOWN\n",
        "\t\tNONE\n",
        "\t\tAC_ADAPTER\n",
        "\t\tUSB\n",
        "\t\tWIRELESS\n",
        "\tChargeStatus\n",
        "\t\tUNKNOWN\n",
        "\t\tNOT_CHARGING\n",
        "\t\tCHARGING\n",
        "\t\tDISCHARGING\n",
        "\tLevelStatus\n",
        "\t\tUNKNOWN\n",
        "\t\tOK\n",
        "\t\tWARNING\n",
        "\t\tLOW\n",
        "\t\tCRITICAL\n",
        "\tBatteryStatus\n",
        "\t\tUNKOWN\n",
        "\t\tOK\n",
        "\t\tNOT_AVAILABLE\n",
        "\t\tNOT_PRESENT\n",
        "\tTimeRemaining\n",
        "\t\tInt value representing time in seconds\n\n",
        "Examples:\n",
        "set ChargeSource WIRELESS BatteryPercentage 12\n",
        "set ChargeStatus DISCHARGING TimeRemaining 100 BatteryPercentage 2"
    ))
}

fn set_time_remaining(time_remaining: &str, service: &BatterySimulatorProxy) -> Result<(), Error> {
    let time = time_remaining.parse::<u64>()?;
    let duration = Duration::new(time, 0);
    service.set_time_remaining(duration.as_nanos() as i64)?;
    Ok(())
}

fn set_charge_source(charge_source: &str, service: &BatterySimulatorProxy) -> Result<(), Error> {
    let res = match charge_source {
        "UNKNOWN" => fpower::ChargeSource::Unknown,
        "NONE" => fpower::ChargeSource::None,
        "AC_ADAPTER" => fpower::ChargeSource::AcAdapter,
        "USB" => fpower::ChargeSource::Usb,
        "WIRELESS" => fpower::ChargeSource::Wireless,
        _ => {
            println!("{} does not exist as an option for ChargeSource. Type set --help for more information", charge_source);
            return Ok(());
        }
    };
    service.set_charge_source(res)?;
    Ok(())
}

fn set_battery_percentage(
    battery_percentage: &str,
    service: &BatterySimulatorProxy,
) -> Result<(), Error> {
    let percent: f32 = battery_percentage.parse().unwrap();
    if percent < 0.0 || percent > 100.0 {
        println!("Please enter battery percent from 0 to 100");
        return Ok(());
    }
    service.set_battery_percentage(percent)?;
    Ok(())
}

fn set_battery_status(battery_status: &str, service: &BatterySimulatorProxy) -> Result<(), Error> {
    let res = match battery_status {
        "UNKNOWN" => fpower::BatteryStatus::Unknown,
        "OK" => fpower::BatteryStatus::Ok,
        "NOT_AVAILABLE" => fpower::BatteryStatus::NotAvailable,
        "NOT_PRESENT" => fpower::BatteryStatus::NotPresent,
        _ => {
            println!("{} does not exist as an option for BatteryStatus. Type set --help for more information", battery_status);
            return Ok(());
        }
    };
    service.set_battery_status(res)?;
    Ok(())
}

fn set_level_status(level_status: &str, service: &BatterySimulatorProxy) -> Result<(), Error> {
    let res = match level_status {
        "UNKNOWN" => fpower::LevelStatus::Unknown,
        "OK" => fpower::LevelStatus::Ok,
        "WARNING" => fpower::LevelStatus::Warning,
        "LOW" => fpower::LevelStatus::Low,
        "CRITICAL" => fpower::LevelStatus::Critical,
        _ => {
            println!("{} does not exist as an option for LevelStatus. Type set --help for more information", level_status);
            return Ok(());
        }
    };
    service.set_level_status(res)?;
    Ok(())
}

fn set_charge_status(charge_status: &str, service: &BatterySimulatorProxy) -> Result<(), Error> {
    let res = match charge_status {
        "UNKNOWN" => fpower::ChargeStatus::Unknown,
        "NOT_CHARGING" => fpower::ChargeStatus::NotCharging,
        "CHARGING" => fpower::ChargeStatus::Charging,
        "DISCHARGING" => fpower::ChargeStatus::Discharging,
        "FULL" => fpower::ChargeStatus::Full,
        _ => {
            println!("{} does not exist as an option for ChargeStatus. Type set --help for more information", charge_status);
            return Ok(());
        }
    };
    service.set_charge_status(res)?;
    Ok(())
}

// args is the string of arguments provided by the user. The format is as follows
// args: "<Field> <Value> ...". The <Field> is any field of the BatteryInfo and
// <Value> is the fields corresponding value. The "..." signifies the repetition of
// the previous pattern of field and value.
async fn set_all_information(args: String, service: &BatterySimulatorProxy) -> Result<(), Error> {
    if !service.is_simulating().await? {
        println!("Please disconnect before running set. Type <set --help> for more information");
        return Ok(());
    }
    let commands: Vec<&str> = args.split(" ").collect();
    if commands.len() <= 1 || commands[0] == "--help" {
        print_set_help_message();
        return Ok(());
    } else if commands.len() % 2 != 0 {
        println!("Incorrect number of args. Type <set --help> for more information");
        return Ok(());
    }
    for i in (0..).take(commands.len()).step_by(2) {
        let field = commands[i];
        let arg = commands[i + 1];
        let _ = match field {
            "BatteryPercentage" => set_battery_percentage(arg, service)?,
            "BatteryStatus" => set_battery_status(arg, service)?,
            "ChargeStatus" => set_charge_status(arg, service)?,
            "ChargeSource" => set_charge_source(arg, service)?,
            "LevelStatus" => set_level_status(arg, service)?,
            "TimeRemaining" => set_time_remaining(arg, service)?,
            _ => println!("Incorrect Battery Field {}", field),
        };
    }
    Ok(())
}

/// Handle a single raw input command from a user and indicate whether the command should
/// result in continuation or breaking of the read evaluate print loop.
async fn handle_command(
    service: &BatterySimulatorProxy,
    cmd: Command,
    args: Vec<String>,
) -> Result<ReplControl, Error> {
    let args = args.join(" ");
    match cmd {
        Command::Get => print_battery_info(service).await,
        Command::Set => set_all_information(args, service).await,
        Command::Reconnect => service.reconnect_real_battery().map_err(|e| format_err!("{}", e)),
        Command::Disconnect => service.disconnect_real_battery().map_err(|e| format_err!("{}", e)),
        Command::Help => Ok(println!("{}", Command::help_msg().to_string())),
        Command::Exit | Command::Quit => return Ok(ReplControl::Break),
    }?;

    Ok(ReplControl::Continue)
}

/// Parse a single raw input command from a user into the command type and argument lsit
fn parse_command(line: String) -> ParseResult<(Command, Vec<String>)> {
    let components: Vec<_> = line.trim().split_whitespace().collect();
    match components.split_first() {
        Some((raw_cmd, args)) => match raw_cmd.parse() {
            Ok(cmd) => {
                let args = args.into_iter().map(|s| s.to_string()).collect();
                ParseResult::Valid((cmd, args))
            }
            Err(_) => ParseResult::Error(format!("\"{}\" is not a valid command", raw_cmd)),
        },
        None => ParseResult::Empty,
    }
}

// Parses the command passed and if it's a valid command then it would handle what it should do
async fn parse_and_handle_cmd(
    service: &BatterySimulatorProxy,
    line: String,
) -> Result<ReplControl, Error> {
    match parse_command(line) {
        ParseResult::Valid((cmd, args)) => handle_command(service, cmd, args).await,
        ParseResult::Empty => Ok(ReplControl::Continue),
        ParseResult::Error(err) => {
            println!("{}", err);
            Ok(ReplControl::Continue)
        }
    }
}

/// Generates a rustyline `Editor` in a separate thread to manage user input. This input is returned
/// as a `Stream` of lines entered by the user.
///
/// The thread exits and the `Stream` is exhausted when an error occurs on stdin or the user
/// sends a ctrl-c or ctrl-d sequence.
///
/// Because rustyline shares control over output to the screen with other parts of the system, a
/// `Sink` is passed to the caller to send acknowledgements that a command has been processed and
/// that rustyline should handle the next line of input.
fn cmd_stream() -> (impl Stream<Item = String>, impl Sink<(), Error = SendError>) {
    // Editor thread and command processing thread must be synchronized so that output
    // is printed in the correct order.
    let (mut cmd_sender, cmd_receiver) = channel(512);
    let (ack_sender, mut ack_receiver) = channel(512);
    thread::spawn(move || -> Result<(), Error> {
        let mut exec = fasync::LocalExecutor::new();
        let fut = async {
            let config = Config::builder()
                .auto_add_history(true)
                .history_ignore_space(true)
                .completion_type(CompletionType::List)
                .edit_mode(EditMode::Vi)
                .build();
            let mut rl: Editor<CmdHelper> = Editor::with_config(config);
            // Add tab completion
            let c = CmdHelper::new();
            rl.set_helper(Some(c));
            loop {
                let readline = rl.readline(PROMPT);
                match readline {
                    Ok(line) => {
                        cmd_sender.try_send(line)?;
                    }
                    Err(ReadlineError::Eof) | Err(ReadlineError::Interrupted) => {
                        return Ok(());
                    }
                    Err(e) => {
                        println!("Error: {:?}", e);
                        return Err(e.into());
                    }
                }
                // Wait until processing thread is finished evaluating the last command
                // before running the next loop in the repl
                ack_receiver.next().await;
            }
        };
        exec.run_singlethreaded(fut)
    });
    (cmd_receiver, ack_sender)
}

async fn run_repl(service: BatterySimulatorProxy) -> Result<(), Error> {
    // `cmd_stream` blocks on input in a separate thread and passes commands and acks back to
    // the main thread via async channels.
    let (mut commands, mut acks) = cmd_stream();
    while let Some(cmd) = commands.next().await {
        match parse_and_handle_cmd(&service, cmd).await {
            Ok(ReplControl::Continue) => {}
            Ok(ReplControl::Break) => break,
            Err(e) => println!("Error handling command: {}", e),
        }
        acks.send(()).await?;
    }
    Ok(())
}

#[fuchsia::main(logging_tags = ["battery_simulator"])]
async fn main() -> Result<(), Error> {
    println!("Welcome to the Battery Simulator. Type help and <Enter> to see all the options!");
    let battery_simulator = connect_to_protocol::<spower::BatterySimulatorMarker>()?;
    let repl = run_repl(battery_simulator)
        .unwrap_or_else(|e| eprintln!("REPL failed unexpectedly {:?}", e));

    // Pins repl value on the stack
    pin_mut!(repl);

    repl.await;

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[fuchsia::test]
    async fn test_disconnect() {
        // Connect to service
        let battery_simulator = connect_to_protocol::<spower::BatterySimulatorMarker>().unwrap();
        // Disconnect battery
        let res = battery_simulator.disconnect_real_battery();
        assert!(res.is_ok(), "Failed to disconnect");
        // Test disconnect
        let res = battery_simulator.is_simulating().await;
        assert_eq!(res.unwrap(), true);
        // Reconnect battery
        let res = battery_simulator.reconnect_real_battery();
        assert!(res.is_ok(), "Failed to reconnect");
    }

    #[fuchsia::test]
    async fn test_set_battery_percentage() {
        // Connect to service
        let battery_simulator = connect_to_protocol::<spower::BatterySimulatorMarker>().unwrap();
        // Disconnect battery
        let res = battery_simulator.disconnect_real_battery();
        assert!(res.is_ok(), "Failed to disconnect");
        // Set Battery Percentage
        let res = set_battery_percentage("12.0", &battery_simulator);
        assert!(res.is_ok(), "Failed to set battery percentage");
        // Check Battery Percentage
        let battery_info = battery_simulator.get_battery_info().await;
        assert_eq!(battery_info.unwrap().level_percent.unwrap(), 12.0);
        // Reconnect battery
        let res = battery_simulator.reconnect_real_battery();
        assert!(res.is_ok(), "Failed to reconnect");
    }

    #[fuchsia::test]
    async fn test_set_battery_status() {
        // Connect to service
        let battery_simulator = connect_to_protocol::<spower::BatterySimulatorMarker>().unwrap();
        // Disconnect battery
        let res = battery_simulator.disconnect_real_battery();
        assert!(res.is_ok(), "Failed to disconnect");
        // Set Battery Status
        let res = set_battery_status("OK", &battery_simulator);
        assert!(res.is_ok(), "Failed to set battery status");
        // Check Battery Status
        let battery_info = battery_simulator.get_battery_info().await;
        assert_eq!(battery_info.unwrap().status.unwrap(), fpower::BatteryStatus::Ok);
        // Reconnect battery
        let res = battery_simulator.reconnect_real_battery();
        assert!(res.is_ok(), "Failed to reconnect");
    }

    #[fuchsia::test]
    async fn test_set_charge_status() {
        // Connect to service
        let battery_simulator = connect_to_protocol::<spower::BatterySimulatorMarker>().unwrap();
        // Disconnect battery
        let res = battery_simulator.disconnect_real_battery();
        assert!(res.is_ok(), "Failed to disconnect");
        // Set Charge Status
        let res = set_charge_status("NOT_CHARGING", &battery_simulator);
        assert!(res.is_ok(), "Failed to set charge status");
        // Check Charge Status
        let battery_info = battery_simulator.get_battery_info().await;
        assert_eq!(battery_info.unwrap().charge_status.unwrap(), fpower::ChargeStatus::NotCharging);
        // Reconnect battery
        let res = battery_simulator.reconnect_real_battery();
        assert!(res.is_ok(), "Failed to reconnect");
    }

    #[fuchsia::test]
    async fn test_set_charge_source() {
        // Connect to service
        let battery_simulator = connect_to_protocol::<spower::BatterySimulatorMarker>().unwrap();
        // Disconnect battery
        let res = battery_simulator.disconnect_real_battery();
        assert!(res.is_ok(), "Failed to disconnect");
        // Set Charge Status
        let res = set_charge_source("WIRELESS", &battery_simulator);
        assert!(res.is_ok(), "Failed to set charge source");
        // Check Charge Status
        let battery_info = battery_simulator.get_battery_info().await;
        assert_eq!(battery_info.unwrap().charge_source.unwrap(), fpower::ChargeSource::Wireless);
        // Reconnect battery
        let res = battery_simulator.reconnect_real_battery();
        assert!(res.is_ok(), "Failed to reconnect");
    }

    #[fuchsia::test]
    async fn test_set_level_status() {
        // Connect to service
        let battery_simulator = connect_to_protocol::<spower::BatterySimulatorMarker>().unwrap();
        // Disconnect battery
        let res = battery_simulator.disconnect_real_battery();
        assert!(res.is_ok(), "Failed to disconnect");
        // Set Level Status
        let res = set_level_status("CRITICAL", &battery_simulator);
        assert!(res.is_ok(), "Failed to set charge source");
        // Check Level Status
        let battery_info = battery_simulator.get_battery_info().await;
        assert_eq!(battery_info.unwrap().level_status.unwrap(), fpower::LevelStatus::Critical);
        // Reconnect battery
        let res = battery_simulator.reconnect_real_battery();
        assert!(res.is_ok(), "Failed to reconnect");
    }
}
