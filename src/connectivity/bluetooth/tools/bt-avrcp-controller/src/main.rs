// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Context as _, Error},
    argh::FromArgs,
    fidl::endpoints::create_endpoints,
    fidl_fuchsia_bluetooth_avrcp::{
        self as fidl_avrcp, AttributeRequestOption, BrowseControllerMarker, BrowseControllerProxy,
        ControllerEvent, ControllerEventStream, ControllerMarker, ControllerProxy,
        MediaAttributeId, Notifications, PeerManagerMarker, PlayerApplicationSettingAttributeId,
        MAX_ATTRIBUTES,
    },
    fidl_fuchsia_bluetooth_avrcp_test::{
        BrowseControllerExtMarker, BrowseControllerExtProxy, ControllerExtMarker,
        ControllerExtProxy, PeerManagerExtMarker,
    },
    fuchsia_async as fasync,
    fuchsia_bluetooth::types::PeerId,
    fuchsia_component::client::connect_to_protocol,
    futures::{
        channel::mpsc::{channel, SendError},
        select, FutureExt, Sink, SinkExt, Stream, StreamExt, TryStreamExt,
    },
    hex::FromHex,
    pin_utils::pin_mut,
    rustyline::{error::ReadlineError, CompletionType, Config, EditMode, Editor},
    std::thread,
};

use crate::commands::{avc_match_string, Cmd, CmdHelper, ReplControl};

mod commands;

static PROMPT: &str = "\x1b[34mavrcp>\x1b[0m ";
/// Escape code to clear the pty line on which the cursor is located.
/// Used when evented output is intermingled with the REPL prompt.
static CLEAR_LINE: &str = "\x1b[2K";

/// Define the command line arguments that the tool accepts.
#[derive(FromArgs)]
#[argh(description = "Bluetooth AVRCP Controller CLI")]
struct Options {
    /// target device id.
    #[argh(positional, from_str_fn(PeerId::from))]
    device: PeerId,
}

async fn send_passthrough<'a>(
    args: &'a [&'a str],
    controller: &'a ControllerProxy,
) -> Result<String, Error> {
    if args.len() != 1 {
        return Ok(format!("usage: {}", Cmd::AvcCommand.cmd_help()));
    }
    let cmd = avc_match_string(args[0]);
    if cmd.is_none() {
        return Err(format_err!("invalid avc command"));
    }

    // `args[0]` is the identifier of the peer to connect to
    match controller.send_command(cmd.unwrap()).await? {
        Ok(_) => Ok(String::from("")),
        Err(e) => Err(format_err!("Error sending AVC Command: {:?}", e)),
    }
}

async fn get_folder_items_with_attrs<'a>(
    cmd: Cmd,
    args: &'a [&'a str],
    controller: &'a BrowseControllerProxy,
) -> Result<String, Error> {
    if args.len() < 2 {
        return Ok(format!("usage: {}", cmd.cmd_help()));
    }

    let start_index = args[0].to_string().parse::<u32>()?;
    let end_index = args[1].to_string().parse::<u32>()?;
    let attr_request = if args.len() == 3 {
        let arg = args[2].to_string();
        match arg.as_str() {
            "All" => AttributeRequestOption::GetAll(true),
            _ => {
                let attrs = arg.split(",");
                let mut attributes = vec![];
                for a in attrs {
                    let raw_attr = a.parse::<u32>()?;
                    attributes.push(MediaAttributeId::from_primitive(raw_attr).ok_or(
                        format_err!(
                            "Invalid MediaAttributeId value: {:?}. Valid value range is [1 - 8]",
                            raw_attr,
                        ),
                    )?);
                }
                AttributeRequestOption::AttributeList(attributes)
            }
        }
    } else {
        AttributeRequestOption::GetAll(false)
    };

    match cmd {
        Cmd::GetVirtualFileSystem => {
            match controller.get_file_system_items(start_index, end_index, &attr_request).await? {
                Ok(items) => Ok(format!("File system: {:#?}", items)),
                Err(e) => Err(format_err!("Error fetching file system: {:?}", e)),
            }
        }
        Cmd::GetNowPlaying => {
            match controller.get_now_playing_items(start_index, end_index, &attr_request).await? {
                Ok(items) => Ok(format!("Now playing: {:#?}", items)),
                Err(e) => Err(format_err!("Error fetching now playing: {:?}", e)),
            }
        }
        _ => panic!("get_folder_items_with_attrs should not have been called with {:?}", cmd),
    }
}

async fn get_media<'a>(
    _args: &'a [&'a str],
    controller: &'a ControllerProxy,
) -> Result<String, Error> {
    match controller.get_media_attributes().await? {
        Ok(media) => Ok(format!("Media attributes: {:#?}", media)),
        Err(e) => Err(format_err!("Error fetching media attributes: {:?}", e)),
    }
}

async fn get_media_player_list<'a>(
    args: &'a [&'a str],
    controller: &'a BrowseControllerProxy,
) -> Result<String, Error> {
    if args.len() != 2 {
        return Ok(format!("usage: {}", Cmd::GetMediaPlayerList.cmd_help()));
    }

    let start_index = args[0].to_string().parse::<u32>()?;
    let end_index = args[1].to_string().parse::<u32>()?;

    match controller.get_media_player_items(start_index, end_index).await? {
        Ok(players) => Ok(format!("Media players: {:#?}", players)),
        Err(e) => Err(format_err!("Error fetching media players: {:?}", e)),
    }
}

async fn get_play_status<'a>(
    _args: &'a [&'a str],
    controller: &'a ControllerProxy,
) -> Result<String, Error> {
    match controller.get_play_status().await? {
        Ok(status) => Ok(format!("Play status {:#?}", status)),
        Err(e) => Err(format_err!("Error fetching play status {:?}", e)),
    }
}

fn parse_pas_ids(ids: Vec<&str>) -> Result<Vec<PlayerApplicationSettingAttributeId>, Error> {
    let mut attribute_ids = vec![];
    for attr_id in ids {
        match attr_id {
            "1" => attribute_ids.push(PlayerApplicationSettingAttributeId::Equalizer),
            "2" => attribute_ids.push(PlayerApplicationSettingAttributeId::RepeatStatusMode),
            "3" => attribute_ids.push(PlayerApplicationSettingAttributeId::ShuffleMode),
            "4" => attribute_ids.push(PlayerApplicationSettingAttributeId::ScanMode),
            _ => return Err(format_err!("Invalid attribute id.")),
        }
    }

    Ok(attribute_ids)
}

async fn get_player_application_settings<'a>(
    args: &'a [&'a str],
    controller: &'a ControllerProxy,
) -> Result<String, Error> {
    if args.len() > MAX_ATTRIBUTES as usize {
        return Ok(format!("usage: {}", Cmd::GetPlayerApplicationSettings.cmd_help()));
    }

    let ids = match parse_pas_ids(args.to_vec()) {
        Ok(ids) => ids,
        Err(_) => return Err(format_err!("Invalid id in args {:?}", args)),
    };
    let get_pas_fut = controller.get_player_application_settings(&ids);

    match get_pas_fut.await? {
        Ok(settings) => Ok(format!("Player application setting attribute value: {:#?}", settings)),
        Err(e) => Err(format_err!("Error fetching player application attributes: {:?}", e)),
    }
}

async fn set_player_application_settings<'a>(
    _args: &'a [&'a str],
    controller: &'a ControllerProxy,
) -> Result<String, Error> {
    // Send canned response to AVRCP with Equalizer off.
    let mut settings = fidl_avrcp::PlayerApplicationSettings::default();
    settings.equalizer = Some(fidl_avrcp::Equalizer::Off);

    match controller.set_player_application_settings(&settings).await? {
        Ok(set_settings) => Ok(format!("Set settings with: {:?}", set_settings)),
        Err(e) => Err(format_err!("Error in set settings {:?}", e)),
    }
}

async fn get_events_supported<'a>(
    _args: &'a [&'a str],
    controller: &'a ControllerExtProxy,
) -> Result<String, Error> {
    match controller.get_events_supported().await? {
        Ok(events) => Ok(format!("Supported events: {:#?}", events)),
        Err(e) => Err(format_err!("Error fetching supported events: {:?}", e)),
    }
}

async fn send_raw_vendor<'a>(
    args: &'a [&'a str],
    controller: &'a ControllerExtProxy,
) -> Result<String, Error> {
    if args.len() < 2 {
        return Err(format_err!("usage: {}", Cmd::SendRawVendorCommand.cmd_help()));
    }

    match parse_raw_packet(args) {
        Ok((pdu_id, buf)) => {
            eprintln!("Sending {:#?}", buf);

            match controller.send_raw_vendor_dependent_command(pdu_id, &buf).await? {
                Ok(response) => Ok(format!("response: {:#?}", response)),
                Err(e) => Err(format_err!("Error sending raw dependent command: {:?}", e)),
            }
        }
        Err(message) => Err(format_err!("{:?}", message)),
    }
}

fn parse_raw_packet(args: &[&str]) -> Result<(u8, Vec<u8>), String> {
    let pdu_id;

    if args[0].starts_with("0x") || args[0].starts_with("0X") {
        if let Ok(hex) = Vec::from_hex(&args[0][2..]) {
            if hex.len() < 1 {
                return Err(format!("invalid pdu_id {}", args[0]));
            }
            pdu_id = hex[0];
        } else {
            return Err(format!("invalid pdu_id {}", args[0]));
        }
    } else {
        if let Ok(b) = args[0].parse::<u8>() {
            pdu_id = b;
        } else {
            return Err(format!("invalid pdu_id {}", args[0]));
        }
    }

    let byte_string = args[1..].join(" ").replace(",", " ");

    let bytes = byte_string.split(" ");

    let mut buf = vec![];
    for b in bytes {
        let b = b.trim();
        if b.eq("") {
            continue;
        } else if b.starts_with("0x") || b.starts_with("0X") {
            if let Ok(hex) = Vec::from_hex(&b[2..]) {
                buf.extend_from_slice(&hex[..]);
            } else {
                return Err(format!("invalid hex string at {}", b));
            }
        } else {
            if let Ok(hex) = Vec::from_hex(&b[..]) {
                buf.extend_from_slice(&hex[..]);
            } else {
                return Err(format!("invalid hex string at {}", b));
            }
        }
    }

    Ok((pdu_id, buf))
}

async fn set_volume<'a>(
    args: &'a [&'a str],
    controller: &'a ControllerProxy,
) -> Result<String, Error> {
    if args.len() != 1 {
        return Ok(format!("usage: {}", Cmd::SetVolume.cmd_help()));
    }

    let volume = if let Ok(val) = args[0].parse::<u8>() {
        if val > 127 {
            return Err(format_err!("invalid volume range {}", args[0]));
        }
        val
    } else {
        return Err(format_err!("unable to parse volume {}", args[0]));
    };

    match controller.set_absolute_volume(volume).await? {
        Ok(set_volume) => Ok(format!("Volume set to: {:?}", set_volume)),
        Err(e) => Err(format_err!("Error setting volume: {:?}", e)),
    }
}

async fn change_path<'a>(
    args: &'a [&'a str],
    controller: &'a BrowseControllerProxy,
) -> Result<String, Error> {
    if args.len() < 1 {
        return Ok(format!("usage: {}", Cmd::ChangePath.cmd_help()));
    }
    let path = match args[0] {
        ".." | "up" => fidl_avrcp::Path::Parent(fidl_avrcp::Parent {}),
        uid => {
            let folder_uid = uid.to_string().parse::<u64>()?;
            fidl_avrcp::Path::ChildFolderUid(folder_uid)
        }
    };
    match controller.change_path(&path).await? {
        Ok(num_of_items) => {
            Ok(format!("Changed path successfully. Current directory has {:?} items", num_of_items))
        }
        Err(e) => Err(format_err!("Failed to change path: {:?}", e)),
    }
}

async fn play_item<'a>(
    cmd: Cmd,
    args: &'a [&'a str],
    controller: &'a BrowseControllerProxy,
) -> Result<String, Error> {
    if args.len() != 1 {
        return Ok(format!("usage: {}", cmd.cmd_help()));
    }

    let uid = args[0].parse::<u64>()?;

    let command = match cmd {
        Cmd::PlayVirtualFileSystem => controller.play_file_system_item(uid),
        Cmd::PlayNowPlaying => controller.play_now_playing_item(uid),
        _ => panic!("play_item should not have been called with {:?}", cmd),
    };
    Ok(command
        .await
        .map(|_| "Successfully played item".to_string())
        .map_err(|e| format_err!("Error playing item: {:?}", e))?)
}

async fn set_browsed_player<'a>(
    args: &'a [&'a str],
    controller: &'a BrowseControllerProxy,
) -> Result<String, Error> {
    if args.len() != 1 {
        return Ok(format!("usage: {}", Cmd::SetVolume.cmd_help()));
    }

    let player_id = args[0].to_string().parse::<u16>()?;

    match controller.set_browsed_player(player_id).await? {
        Ok(_) => Ok(format!("Browsed player set to: {:?}", player_id)),
        Err(e) => Err(format_err!("Error setting browsed player: {:?}", e)),
    }
}

async fn is_connected<'a>(
    _args: &'a [&'a str],
    controller: &'a ControllerExtProxy,
    browse_controller: &'a BrowseControllerExtProxy,
) -> Result<String, Error> {
    let mut s = Vec::new();
    match controller.is_connected().await {
        Ok(status) => s.push(format!("Is control connected: {}", status)),
        Err(e) => return Err(format_err!("Error checking control connection status: {:?}", e)),
    };
    match browse_controller.is_connected().await {
        Ok(status) => s.push(format!("Is browse connected: {}", status)),
        Err(e) => return Err(format_err!("Error checking browse connection status: {:?}", e)),
    };
    Ok(s.iter().fold("".to_owned(), |msg, m| format!("{}\n{}", msg, m)))
}

/// Handle a single raw input command from a user and indicate whether the command should
/// result in continuation or breaking of the read evaluate print loop.
async fn handle_cmd<'a>(
    controller: &'a ControllerProxy,
    test_controller: &'a ControllerExtProxy,
    browse_controller: &'a BrowseControllerProxy,
    test_browse_controller: &'a BrowseControllerExtProxy,
    line: String,
) -> Result<ReplControl, Error> {
    let components: Vec<_> = line.trim().split_whitespace().collect();
    if let Some((raw_cmd, args)) = components.split_first() {
        let cmd = raw_cmd.parse();
        let res = match cmd {
            Ok(Cmd::AvcCommand) => send_passthrough(args, &controller).await,
            Ok(Cmd::ChangePath) => change_path(args, &browse_controller).await,
            Ok(Cmd::GetVirtualFileSystem) => {
                get_folder_items_with_attrs(Cmd::GetVirtualFileSystem, args, &browse_controller)
                    .await
            }
            Ok(Cmd::GetMediaAttributes) => get_media(args, &controller).await,
            Ok(Cmd::GetMediaPlayerList) => get_media_player_list(args, &browse_controller).await,
            Ok(Cmd::GetNowPlaying) => {
                get_folder_items_with_attrs(Cmd::GetNowPlaying, args, &browse_controller).await
            }
            Ok(Cmd::GetPlayStatus) => get_play_status(args, &controller).await,
            Ok(Cmd::GetPlayerApplicationSettings) => {
                get_player_application_settings(args, &controller).await
            }
            Ok(Cmd::PlayVirtualFileSystem) => {
                play_item(Cmd::PlayVirtualFileSystem, args, &browse_controller).await
            }
            Ok(Cmd::PlayNowPlaying) => {
                play_item(Cmd::PlayNowPlaying, args, &browse_controller).await
            }
            Ok(Cmd::SetPlayerApplicationSettings) => {
                set_player_application_settings(args, &controller).await
            }
            Ok(Cmd::SendRawVendorCommand) => send_raw_vendor(args, &test_controller).await,
            Ok(Cmd::SupportedEvents) => get_events_supported(args, &test_controller).await,
            Ok(Cmd::SetVolume) => set_volume(args, &controller).await,
            Ok(Cmd::SetBrowsedPlayer) => set_browsed_player(args, &browse_controller).await,
            Ok(Cmd::IsConnected) => {
                is_connected(args, &test_controller, &test_browse_controller).await
            }
            Ok(Cmd::Help) => Ok(Cmd::help_msg().to_string()),
            Ok(Cmd::Exit) | Ok(Cmd::Quit) => return Ok(ReplControl::Break),
            Err(_) => Ok(format!("\"{}\" is not a valid command", raw_cmd)),
        }?;
        if res != "" {
            println!("{}", res);
        }
    }

    Ok(ReplControl::Continue)
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

    let _ = thread::spawn(move || -> Result<(), Error> {
        let mut exec = fasync::LocalExecutor::new();

        let fut = async {
            let config = Config::builder()
                .auto_add_history(true)
                .history_ignore_space(true)
                .completion_type(CompletionType::List)
                .edit_mode(EditMode::Emacs)
                .build();
            let mut rl: Editor<CmdHelper> = Editor::with_config(config);
            rl.set_helper(Some(CmdHelper::new()));
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
                // wait until processing thread is finished evaluating the last command
                // before running the next loop in the repl
                if ack_receiver.next().await.is_none() {
                    return Ok(());
                };
            }
        };
        exec.run_singlethreaded(fut)
    });
    (cmd_receiver, ack_sender)
}

async fn controller_listener(
    controller: &ControllerProxy,
    mut stream: ControllerEventStream,
) -> Result<(), Error> {
    while let Some(evt) = stream.try_next().await? {
        print!("{}", CLEAR_LINE);
        match evt {
            ControllerEvent::OnNotification { timestamp, notification } => {
                if let Some(value) = notification.pos {
                    println!("Pos event: {:?} {:?}", timestamp, value);
                } else if let Some(value) = notification.status {
                    println!("Status event: {:?} {:?}", timestamp, value);
                } else if let Some(value) = notification.track_id {
                    println!("Track event: {:?} {:?}", timestamp, value);
                } else if let Some(value) = notification.volume {
                    println!("Volume event: {:?} {:?}", timestamp, value);
                } else {
                    println!("Other event: {:?} {:?}", timestamp, notification);
                }
                controller.notify_notification_handled()?;
            }
        }
    }
    Ok(())
}

/// REPL execution
async fn run_repl<'a>(
    controller: &'a ControllerProxy,
    test_controller: &'a ControllerExtProxy,
    browse_controller: &'a BrowseControllerProxy,
    test_browse_controller: &'a BrowseControllerExtProxy,
) -> Result<(), Error> {
    // `cmd_stream` blocks on input in a separate thread and passes commands and acks back to
    // the main thread via async channels.
    let (mut commands, mut acks) = cmd_stream();
    loop {
        if let Some(cmd) = commands.next().await {
            match handle_cmd(
                controller,
                test_controller,
                browse_controller,
                test_browse_controller,
                cmd,
            )
            .await
            {
                Ok(ReplControl::Continue) => {}
                Ok(ReplControl::Break) => {
                    println!("\n");
                    break;
                }
                Err(e) => {
                    println!("Error handling command: {}", e);
                }
            }
        } else {
            break;
        }
        acks.send(()).await?;
    }
    Ok(())
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    let opt: Options = argh::from_env();

    let device_id = opt.device;

    // Connect to test controller service first so we fail early if it's not available.

    let test_avrcp_svc = connect_to_protocol::<PeerManagerExtMarker>()
        .context("Failed to connect to Bluetooth Test AVRCP interface")?;

    // Create a channel for our Request<TestController> to live
    let (t_client, t_server) = create_endpoints::<ControllerExtMarker>();

    let _status = test_avrcp_svc.get_controller_for_target(&device_id.into(), t_server).await?;
    eprintln!(
        "Test controller obtained to device \"{device}\" AVRCP remote target service",
        device = &device_id,
    );

    // Create a channel for our Request<TestBrowseController> to live
    let (tb_client, tb_server) = create_endpoints::<BrowseControllerExtMarker>();

    let _status =
        test_avrcp_svc.get_browse_controller_for_target(&device_id.into(), tb_server).await?;
    eprintln!(
        "Test browse controller obtained to device \"{device}\" AVRCP remote target service",
        device = &device_id,
    );

    // Connect to avrcp controller service.

    let avrcp_svc = connect_to_protocol::<PeerManagerMarker>()
        .context("Failed to connect to Bluetooth AVRCP interface")?;

    // Create a channel for our Request<Controller> to live
    let (c_client, c_server) = create_endpoints::<ControllerMarker>();

    let _status = avrcp_svc.get_controller_for_target(&device_id.into(), c_server).await?;
    eprintln!(
        "Controller obtained to device \"{device}\" AVRCP remote target service",
        device = &device_id,
    );

    // Create a channel for our Request<Controller> to live
    let (bc_client, bc_server) = create_endpoints::<BrowseControllerMarker>();

    let _status = avrcp_svc.get_browse_controller_for_target(&device_id.into(), bc_server).await?;
    eprintln!(
        "Browse controller obtained to device \"{device}\" AVRCP remote target service",
        device = &device_id,
    );

    // setup repl

    let controller = c_client.into_proxy().expect("error obtaining controller client proxy");
    let test_controller =
        t_client.into_proxy().expect("error obtaining test controller client proxy");
    let browse_controller =
        bc_client.into_proxy().expect("error obtaining browse controller client proxy");
    let test_browse_controller =
        tb_client.into_proxy().expect("error obtaining test browse controller client proxy");

    let evt_stream = controller.clone().take_event_stream();

    // set controller event filter to ones we support.
    let _ = controller.set_notification_filter(
        Notifications::PLAYBACK_STATUS
            | Notifications::TRACK
            | Notifications::TRACK_POS
            | Notifications::VOLUME,
        1,
    )?;

    let event_fut = controller_listener(&controller, evt_stream).fuse();
    let repl_fut =
        run_repl(&controller, &test_controller, &browse_controller, &test_browse_controller).fuse();

    pin_mut!(event_fut);
    pin_mut!(repl_fut);

    // These futures should only return when something fails.
    select! {
        result = event_fut => {
            eprintln!(
                "Service connection returned {status:?}", status = result);
        },
        _ = repl_fut => {}
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use assert_matches::assert_matches;
    use fidl::endpoints::create_proxy;

    #[test]
    fn test_raw_packet_parsing_01() {
        assert_eq!(parse_raw_packet(&["0x40", "0xaaaa"]), Ok((0x40, vec![0xaa, 0xaa])));
    }
    #[test]
    fn test_raw_packet_parsing_02() {
        assert_eq!(parse_raw_packet(&["0x40", "0xaa", "0xaa"]), Ok((0x40, vec![0xaa, 0xaa])));
    }
    #[test]
    fn test_raw_packet_parsing_03() {
        assert_eq!(
            parse_raw_packet(&["0x40", "0xAa", "0XaA", "0x1234"]),
            Ok((0x40, vec![0xaa, 0xaa, 0x12, 0x34]))
        );
    }
    #[test]
    fn test_raw_packet_parsing_04() {
        assert_eq!(
            parse_raw_packet(&["40", "0xaa", "0xaa", "0x1234"]),
            Ok((40, vec![0xaa, 0xaa, 0x12, 0x34]))
        );
    }
    #[test]
    fn test_raw_packet_parsing_05() {
        assert_eq!(
            parse_raw_packet(&["40", "aa", "aa", "1234"]),
            Ok((40, vec![0xaa, 0xaa, 0x12, 0x34]))
        );
    }
    #[test]
    fn test_raw_packet_parsing_06() {
        assert_eq!(parse_raw_packet(&["40", "aa,aa,1234"]), Ok((40, vec![0xaa, 0xaa, 0x12, 0x34])));
    }
    #[test]
    fn test_raw_packet_parsing_07() {
        assert_eq!(
            parse_raw_packet(&["40", "0xaa, 0xaa,    0x1234"]),
            Ok((40, vec![0xaa, 0xaa, 0x12, 0x34]))
        );
    }
    #[test]
    fn test_raw_packet_parsing_08_err_pdu_overflow() {
        assert_eq!(
            parse_raw_packet(&["300", "0xaa, 0xaa,    0x1234"]),
            Err("invalid pdu_id 300".to_string())
        );
    }
    #[test]
    fn test_raw_packet_parsing_09_err_invalid_hex_long() {
        assert_eq!(
            parse_raw_packet(&["40", "0xzz, 0xaa,    0x1234"]),
            Err("invalid hex string at 0xzz".to_string())
        );
    }
    #[test]
    fn test_raw_packet_parsing_10_err_invalid_hex_short() {
        assert_eq!(
            parse_raw_packet(&["40", "zz, 0xaa, 0xqqqq"]),
            Err("invalid hex string at zz".to_string())
        );
    }
    #[test]
    fn test_raw_packet_parsing_11_err_invalid_hex() {
        assert_eq!(
            parse_raw_packet(&["40", "ab, 0xaa, 0xqqqq"]),
            Err("invalid hex string at 0xqqqq".to_string())
        );
    }

    #[test]
    fn test_parse_pas_id_success() {
        let ids = vec!["1", "2", "3"];
        let result = parse_pas_ids(ids);
        assert!(result.is_ok());
        let result = result.unwrap();
        assert_eq!(
            result,
            vec![
                PlayerApplicationSettingAttributeId::Equalizer,
                PlayerApplicationSettingAttributeId::RepeatStatusMode,
                PlayerApplicationSettingAttributeId::ShuffleMode
            ]
        );
    }

    #[test]
    fn test_parse_pas_id_error() {
        let ids = vec!["fake", "id", "1"];
        let result = parse_pas_ids(ids);
        assert!(result.is_err());
    }

    #[test]
    fn test_parse_pas_id_invalid_id_error() {
        let ids = vec!["1", "2", "5"];
        let result = parse_pas_ids(ids);
        assert!(result.is_err());
    }

    #[fasync::run_singlethreaded]
    #[test]
    /// Tests a set_volume command with no input args does not result in error.
    /// Instead, a help message should be returned.
    async fn test_set_volume_no_args() {
        let (proxy, _stream) = create_proxy::<ControllerMarker>().expect("Creation should work");
        let args = [];

        let res = set_volume(&args, &proxy).await;
        assert_matches!(
            res,
            Ok(m) if m.contains("usage")
        );
    }
}
