// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::writer::{OutputSink, Writer},
    anyhow::{anyhow, bail, Context as _, Error, Result},
    diagnostics_data::LogsData,
    fidl_fuchsia_fuzzer as fuzz,
    futures::io::ReadHalf,
    futures::{try_join, AsyncReadExt},
    serde_json::Deserializer,
    std::cell::RefCell,
    std::path::Path,
    std::rc::Rc,
};

/// Compostion of `SocketForwarder`s for standard output, standard errors, and system logs.
#[derive(Debug)]
pub struct Forwarder<O: OutputSink> {
    stdout: Option<SocketForwarder<O>>,
    stderr: Option<SocketForwarder<O>>,
    syslog: Option<SocketForwarder<O>>,
    writer: Writer<O>,
}

impl<O: OutputSink> Forwarder<O> {
    /// Creates a `Forwarder` that can forward data to the `writer`.
    ///
    /// Output will also be saved to the following files under the given `logs_dir` directory:
    ///
    ///   * fuzzer.stdout.txt
    ///   * fuzzer.stderr.txt
    ///   * fuzzer.syslog.json
    ///
    pub fn new(writer: &Writer<O>) -> Self {
        Self { stdout: None, stderr: None, syslog: None, writer: writer.clone() }
    }

    /// Registers the provided output socket.
    pub fn set_output<P: AsRef<Path>>(
        &mut self,
        socket: fidl::Socket,
        output: fuzz::TestOutput,
        logs_dir: &Option<P>,
    ) -> Result<()> {
        match output {
            fuzz::TestOutput::Stdout => {
                let forwarder = self.create_forwarder(logs_dir, "stdout", "txt", socket)?;
                self.stdout = Some(forwarder);
            }
            fuzz::TestOutput::Stderr => {
                let forwarder = self.create_forwarder(logs_dir, "stderr", "txt", socket)?;
                self.stderr = Some(forwarder);
            }
            fuzz::TestOutput::Syslog => {
                let forwarder = self.create_forwarder(logs_dir, "syslog", "json", socket)?;
                self.syslog = Some(forwarder);
            }
            _ => unreachable!(),
        }
        Ok(())
    }

    fn create_forwarder<P: AsRef<Path>>(
        &self,
        logs_dir: &Option<P>,
        name: &str,
        extension: &str,
        socket: fidl::Socket,
    ) -> Result<SocketForwarder<O>> {
        let writer = match logs_dir {
            Some(logs_dir) => self
                .writer
                .tee(logs_dir, format!("fuzzer.{}.{}", name, extension))
                .context(format!("failed to create file for {}", name)),
            None => Ok(self.writer.clone()),
        }?;
        let forwarder = SocketForwarder::try_new(socket, &writer)
            .context(format!("failed to create forwarder for {}", name))?;
        Ok(forwarder)
    }

    /// Forwards output from the fuzzer to the `Writer` using each of the `SocketForwarder`s.
    pub async fn forward_all(&self) -> Result<()> {
        let stdout = self.stdout.clone();
        let stdout_fut = || async move {
            if let Some(stdout) = stdout {
                stdout.forward_text("stdout").await.context("failed to forward stdout")?;
            }
            Ok::<(), Error>(())
        };

        let stderr = self.stderr.clone();
        let stderr_fut = || async move {
            if let Some(stderr) = stderr {
                stderr.forward_text("stderr").await.context("failed to forward stderr")?;
            }
            Ok::<(), Error>(())
        };

        let syslog = self.syslog.clone();
        let syslog_fut = || async move {
            if let Some(syslog) = syslog {
                syslog.forward_json("syslog").await.context("failed to forward syslog")?;
            }
            Ok::<(), Error>(())
        };

        try_join!(stdout_fut(), stderr_fut(), syslog_fut())?;
        Ok(())
    }
}

/// Forwarder for a single output stream.
#[derive(Debug)]
pub struct SocketForwarder<O: OutputSink> {
    reader: Rc<RefCell<ReadHalf<fidl::AsyncSocket>>>,
    writer: Writer<O>,
}

impl<O: OutputSink> Clone for SocketForwarder<O> {
    fn clone(&self) -> Self {
        Self { reader: Rc::clone(&self.reader), writer: self.writer.clone() }
    }
}

impl<O: OutputSink> SocketForwarder<O> {
    /// Converts a a socket into a SocketForwarder.
    ///
    /// Returns an error if conversion to an async socket fails.
    pub fn try_new(socket: fidl::Socket, writer: &Writer<O>) -> Result<Self> {
        let socket = fidl::AsyncSocket::from_socket(socket).context("failed to convert socket")?;
        let (reader, _) = socket.split();
        Ok(Self { reader: Rc::new(RefCell::new(reader)), writer: writer.clone() })
    }

    /// Continuously forwards messages from the socket to the writer until the socket is closed.
    pub async fn forward_text(&self, name: &str) -> Result<()> {
        let mut reader = self.reader.borrow_mut();
        let mut buf: [u8; 2048] = [0; 2048];
        let mut raw = Vec::new();
        let newline = '\n' as u8;
        let done_marker = format!("{}\n", fuzz::DONE_MARKER);
        let done_marker = done_marker.as_bytes();
        loop {
            match reader
                .read(&mut buf)
                .await
                .context(format!("failed to read text data from {} socket", name))?
            {
                0 => {
                    self.writer.write_all(&raw);
                    bail!("{} from fuzzer ended prematurely", name);
                }
                num_read => raw.extend_from_slice(&buf[0..num_read]),
            };
            let data = raw;
            raw = Vec::new();
            for message in data.split_inclusive(|&x| x == newline) {
                if message == done_marker {
                    return Ok(());
                } else if message.last() == Some(&newline) {
                    self.writer.write_all(&message);
                } else {
                    raw = message.to_vec();
                }
            }
        }
    }

    /// Continuously forwards JSON data from the socket to the writer until the socket is closed.
    pub async fn forward_json(&self, name: &str) -> Result<()> {
        let mut reader = self.reader.borrow_mut();
        let mut buf: [u8; 2048] = [0; 2048];
        let mut raw = Vec::new();
        loop {
            match reader
                .read(&mut buf)
                .await
                .context(format!("failed to read JSON data from {} socket", name))?
            {
                0 => {
                    self.writer.write_all(&raw);
                    bail!("{} from fuzzer ended prematurely", name);
                }
                num_read => raw.extend_from_slice(&buf[0..num_read]),
            };
            let deserializer = Deserializer::from_slice(&raw);
            let mut stream = deserializer.into_iter::<Vec<LogsData>>();
            while let Some(items) = stream.next() {
                let logs_data = match items {
                    Err(e) if e.is_eof() => break,
                    other => other,
                }
                .map_err(|e| anyhow!(format!("serde_json: {:?}", e)))
                .context("failed to deserialize")?;
                for log_data in logs_data.into_iter() {
                    if let Some(message) = log_data.msg() {
                        if message == fuzz::DONE_MARKER {
                            return Ok(());
                        }
                    }
                    self.writer.log(log_data);
                }
            }
            let num_read = stream.byte_offset();
            raw.drain(0..num_read);
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        anyhow::{Error, Result},
        diagnostics_data::LogsData,
        fidl::Socket,
        fidl_fuchsia_fuzzer as fuzz,
        fuchsia_fuzzctl::{Forwarder, SocketForwarder},
        fuchsia_fuzzctl_test::{send_log_entry, Test},
        futures::{try_join, AsyncWriteExt},
        std::fs,
    };

    #[fuchsia::test]
    async fn test_forward_text() -> Result<()> {
        let mut test = Test::try_new()?;
        let (tx, rx) = Socket::create_stream();
        let forwarder = SocketForwarder::try_new(rx, test.writer())?;
        let socket_fut = || async move {
            let mut tx = fidl::AsyncSocket::from_socket(tx)?;
            tx.write_all(b"hello\nworld!\n").await?;
            let done_marker = format!("{}\n", fuzz::DONE_MARKER);
            tx.write_all(done_marker.as_bytes()).await?;
            Ok::<(), Error>(())
        };
        test.output_matches("hello");
        test.output_matches("world!");
        try_join!(forwarder.forward_text("test"), socket_fut())?;
        test.verify_output()
    }

    #[fuchsia::test]
    async fn test_forward_json() -> Result<()> {
        let mut test = Test::try_new()?;
        let (tx, rx) = Socket::create_stream();
        let forwarder = SocketForwarder::try_new(rx, test.writer())?;
        let socket_fut = || async move {
            let mut tx = fidl::AsyncSocket::from_socket(tx)?;
            send_log_entry(&mut tx, "hello world").await?;
            send_log_entry(&mut tx, fuzz::DONE_MARKER).await?;
            Ok::<(), Error>(())
        };
        try_join!(forwarder.forward_json("test"), socket_fut())?;
        test.output_matches("[0.000][moniker][][I] hello world");
        test.verify_output()
    }

    #[fuchsia::test]
    async fn test_forward_all() -> Result<()> {
        let mut test = Test::try_new()?;
        let logs_dir = test.create_dir("logs")?;
        let logs_dir = Some(logs_dir);
        let mut forwarder = Forwarder::new(test.writer());

        let (stdout_tx, stdout_rx) = Socket::create_stream();
        forwarder.set_output(stdout_rx, fuzz::TestOutput::Stdout, &logs_dir)?;

        let (stderr_tx, stderr_rx) = Socket::create_stream();
        forwarder.set_output(stderr_rx, fuzz::TestOutput::Stderr, &logs_dir)?;

        let (syslog_tx, syslog_rx) = Socket::create_stream();
        forwarder.set_output(syslog_rx, fuzz::TestOutput::Syslog, &logs_dir)?;

        let done_marker = format!("{}\n", fuzz::DONE_MARKER);
        let done_marker_bytes = done_marker.as_bytes();

        let a_done_marker = format!("a{}\n", fuzz::DONE_MARKER);
        test.output_matches(a_done_marker.clone());

        let done_marker_a = format!("{}a\n", fuzz::DONE_MARKER);
        test.output_matches(done_marker_a.clone());

        let socket_fut = || async move {
            let mut stdout_tx = fidl::AsyncSocket::from_socket(stdout_tx)?;
            let mut stderr_tx = fidl::AsyncSocket::from_socket(stderr_tx)?;
            let mut syslog_tx = fidl::AsyncSocket::from_socket(syslog_tx)?;

            // Streams can be sent in any order
            send_log_entry(&mut syslog_tx, fuzz::DONE_MARKER).await?;

            // Data sent after the done marker should not be received.
            stdout_tx.write_all(done_marker_bytes).await?;
            stdout_tx.write_all(b"after\n").await?;

            // Done marker must be exactly delimited by newlines, and can arrive in pieces.
            stderr_tx.write_all(a_done_marker.as_bytes()).await?;
            stderr_tx.write_all(done_marker_a.as_bytes()).await?;
            for i in 0..done_marker_bytes.len() {
                stderr_tx.write_all(&done_marker_bytes[i..i + 1]).await?;
            }
            stderr_tx.write_all(b"after\n").await?;
            Ok::<(), Error>(())
        };

        try_join!(forwarder.forward_all(), socket_fut())?;
        test.verify_output()
    }

    #[fuchsia::test]
    async fn test_forward_to_file() -> Result<()> {
        let test = Test::try_new()?;

        let logs_dir = test.create_dir("logs")?;
        let logs_dir = Some(logs_dir);
        let mut forwarder = Forwarder::new(test.writer());

        let (stdout_tx, stdout_rx) = Socket::create_stream();
        forwarder.set_output(stdout_rx, fuzz::TestOutput::Stdout, &logs_dir)?;

        let (stderr_tx, stderr_rx) = Socket::create_stream();
        forwarder.set_output(stderr_rx, fuzz::TestOutput::Stderr, &logs_dir)?;

        let (syslog_tx, syslog_rx) = Socket::create_stream();
        forwarder.set_output(syslog_rx, fuzz::TestOutput::Syslog, &logs_dir)?;

        let sockets_fut = || async move {
            let done_marker = format!("{}\n", fuzz::DONE_MARKER);
            let done_marker_bytes = done_marker.as_bytes();

            // Write all in one shot.
            let mut stdout_tx = fidl::AsyncSocket::from_socket(stdout_tx)?;
            stdout_tx.write_all(b"hello world!\n").await?;
            stdout_tx.write_all(done_marker_bytes).await?;

            // Write all in pieces.
            let mut stderr_tx = fidl::AsyncSocket::from_socket(stderr_tx)?;
            stderr_tx.write_all(b"hel").await?;
            stderr_tx.write_all(b"lo ").await?;
            stderr_tx.write_all(b"wor").await?;
            stderr_tx.write_all(b"ld!\n").await?;
            stderr_tx.write_all(done_marker_bytes).await?;

            // Write JSON. This should be made prettier when copying, e.g. newlines, spaces, etc.
            let mut syslog_tx = fidl::AsyncSocket::from_socket(syslog_tx)?;
            send_log_entry(&mut syslog_tx, "hello world!").await?;
            send_log_entry(&mut syslog_tx, fuzz::DONE_MARKER).await?;
            Ok::<(), Error>(())
        };
        let sockets_fut = sockets_fut();
        try_join!(forwarder.forward_all(), sockets_fut)?;
        let logs_dir = logs_dir.unwrap();
        assert_eq!(fs::read(logs_dir.join("fuzzer.stdout.txt"))?, b"hello world!\n");
        assert_eq!(fs::read(logs_dir.join("fuzzer.stderr.txt"))?, b"hello world!\n");
        let data = fs::read(logs_dir.join("fuzzer.syslog.json"))?;
        let logs_data: LogsData = serde_json::from_slice(&data)?;
        assert_eq!(logs_data.msg(), Some("hello world!"));
        Ok(())
    }
}
