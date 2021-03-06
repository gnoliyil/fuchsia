// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use {
    anyhow::{anyhow, Result},
    std::net::SocketAddr,
    std::process::Command,
};

static DEFAULT_SSH_OPTIONS: &'static [&str] = &[
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
    // FIXME(http://fxbug.dev/77015): Temporarily create a reverse tunnel for port 8085, which will
    // be used by `ffx repository` to serve packages to a target device.
    "-R",
    "8085:localhost:8085",
];

#[cfg(not(test))]
async fn get_ssh_key_path() -> Result<String> {
    use anyhow::Context;
    const SSH_PRIV: &str = "ssh.priv";
    ffx_config::file(SSH_PRIV).await.context("getting path to an ssh private key from ssh.priv")
}

#[cfg(test)]
const TEST_SSH_KEY_PATH: &str = "ssh/ssh_key_in_test";
#[cfg(test)]
async fn get_ssh_key_path() -> Result<String> {
    Ok(TEST_SSH_KEY_PATH.to_string())
}

pub async fn build_ssh_command(addr: SocketAddr, command: Vec<&str>) -> Result<Command> {
    if command.is_empty() {
        return Err(anyhow!("missing SSH command"));
    }

    let key = get_ssh_key_path().await?;

    let mut c = Command::new("ssh");
    c.args(DEFAULT_SSH_OPTIONS);
    c.arg("-i").arg(key);

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
    use {super::*, itertools::Itertools};

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
}
