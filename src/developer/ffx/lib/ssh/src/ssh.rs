// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use anyhow::{anyhow, Result};
use std::{net::SocketAddr, process::Command};

static DEFAULT_SSH_OPTIONS: &'static [&str] = &[
    "-F",
    "none", // Ignore user and system configuration files.
    "-o",
    "CheckHostIP=no",
    "-o",
    "StrictHostKeyChecking=no",
    "-o",
    "UserKnownHostsFile=/dev/null",
    "-o",
    "ServerAliveInterval=1",
    "-o",
    "ServerAliveCountMax=10",
    "-o",
    "LogLevel=ERROR",
];

#[cfg(not(test))]
async fn get_ssh_key_paths() -> Result<Vec<String>> {
    use anyhow::Context;
    const SSH_PRIV: &str = "ssh.priv";
    ffx_config::query(SSH_PRIV)
        .get_file()
        .await
        .context("getting path to an ssh private key from ssh.priv")
}

#[cfg(test)]
const TEST_SSH_KEY_PATH: &str = "ssh/ssh_key_in_test";
#[cfg(test)]
async fn get_ssh_key_paths() -> Result<Vec<String>> {
    Ok(vec![TEST_SSH_KEY_PATH.to_string()])
}

async fn apply_auth_sock(cmd: &mut Command) {
    const SSH_AUTH_SOCK: &str = "ssh.auth-sock";
    if let Ok(path) = ffx_config::get::<String, _>(SSH_AUTH_SOCK).await {
        cmd.env("SSH_AUTH_SOCK", path);
    }
}

// Setting up the tunnel requires config to be available, so we disable this step in tests.
pub async fn build_ssh_command(addr: SocketAddr, command: Vec<&str>) -> Result<Command> {
    if command.is_empty() {
        return Err(anyhow!("missing SSH command"));
    }

    let keys = get_ssh_key_paths().await?;

    let mut c = Command::new("ssh");
    apply_auth_sock(&mut c).await;
    c.args(DEFAULT_SSH_OPTIONS);

    for key in keys {
        c.arg("-i").arg(key);
    }

    let mut addr_str = format!("{}", addr);
    let colon_port = addr_str.split_off(addr_str.rfind(':').expect("socket format includes port"));

    // Remove the enclosing [] used in IPv6 socketaddrs
    let addr_start = if addr_str.starts_with("[") { 1 } else { 0 };
    let addr_end = addr_str.len() - if addr_str.ends_with("]") { 1 } else { 0 };
    let addr_arg = &addr_str[addr_start..addr_end];

    c.arg("-p").arg(&colon_port[1..]);
    c.arg(addr_arg);

    c.args(&command);

    return Ok(c);
}

#[cfg(test)]
mod test {
    use super::*;
    use itertools::Itertools;
    use std::io::BufRead;

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_build_ssh_command_ipv4() {
        let addr = "192.168.0.1:22".parse().unwrap();

        let result = build_ssh_command(addr, vec!["ls"]).await.unwrap();
        let dbgstr = format!("{:?}", result);

        let expected = format!(
            r#""ssh" {} "-i" "{}" "-p" "22" "192.168.0.1" "ls""#,
            DEFAULT_SSH_OPTIONS.iter().map(|s| format!("\"{}\"", s)).join(" "),
            TEST_SSH_KEY_PATH,
        );
        assert!(dbgstr.contains(&expected), "ssh lines did not match:\n{}\n{}", expected, dbgstr);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_build_ssh_command_ipv6() {
        let addr = "[fe80::12%5]:8022".parse().unwrap();

        let result = build_ssh_command(addr, vec!["ls"]).await.unwrap();
        let dbgstr = format!("{:?}", result);

        let expected = format!(
            r#""ssh" {} "-i" "{}" "-p" "8022" "fe80::12%5" "ls""#,
            DEFAULT_SSH_OPTIONS.iter().map(|s| format!("\"{}\"", s)).join(" "),
            TEST_SSH_KEY_PATH,
        );
        assert!(dbgstr.contains(&expected), "ssh lines did not match:\n{}\n{}", expected, dbgstr);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_apply_auth_sock() {
        let _env = ffx_config::test_init().await.unwrap();
        // XXX(82683): Setting config options in tests is modifying the user environment, so just
        // grab the current value and assert against it.  This needs to get fixed, until it is, this
        // test is of somewhat limited value.
        let expect_path: String = match ffx_config::get("ssh.auth-sock").await {
            Ok(s) => s,
            Err(_) => {
                eprintln!("WARNING: untested case, see fxbug.dev/82683");
                return;
            }
        };

        let mut cmd = Command::new("env");
        apply_auth_sock(&mut cmd).await;
        let lines =
            cmd.output().unwrap().stdout.lines().filter_map(|res| res.ok()).collect::<Vec<_>>();

        let expected_var = format!("SSH_AUTH_SOCK={}", expect_path);
        assert!(
            lines.iter().any(|line| line.starts_with(&expected_var)),
            "Looking for {} in {}",
            expected_var,
            lines.join("\n")
        );
    }
}
