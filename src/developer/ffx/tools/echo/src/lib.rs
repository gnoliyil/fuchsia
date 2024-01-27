// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use argh::FromArgs;
use async_trait::async_trait;
use fho::{daemon_protocol, FfxContext, FfxMain, FfxTool, MachineWriter, Result, ToolIO};
use fidl_fuchsia_developer_ffx as ffx;

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "echo", description = "run echo test against the daemon")]
pub struct EchoCommand {
    #[argh(positional)]
    /// text string to echo back and forth
    pub text: Option<String>,
}

#[derive(FfxTool)]
pub struct EchoTool {
    #[command]
    cmd: EchoCommand,
    #[with(daemon_protocol())]
    echo_proxy: ffx::EchoProxy,
}

#[async_trait(?Send)]
impl FfxMain for EchoTool {
    type Writer = MachineWriter<String>;
    async fn main(self, mut writer: Self::Writer) -> Result<()> {
        let text = self.cmd.text.as_deref().unwrap_or("FFX");
        let echo_out = self
            .echo_proxy
            .echo_string(text)
            .await
            .user_message("Error returned from echo service")?;
        writer.item(&echo_out)?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fho::macro_deps::ffx_writer::TestBuffer;
    use futures_lite::stream::StreamExt;

    fn setup_fake_echo_proxy() -> ffx::EchoProxy {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<ffx::EchoMarker>().unwrap();
        fuchsia_async::Task::local(async move {
            while let Ok(Some(req)) = stream.try_next().await {
                match req {
                    ffx::EchoRequest::EchoString { value, responder } => {
                        responder.send(value.as_ref()).unwrap();
                    }
                }
            }
        })
        .detach();
        proxy
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_regular_run() {
        const ECHO: &'static str = "foo";
        let cmd = EchoCommand { text: Some(ECHO.to_owned()) };
        let echo_proxy = setup_fake_echo_proxy();
        let test_stdout = TestBuffer::default();
        let writer = MachineWriter::new_buffers(None, test_stdout.clone(), Vec::new());
        let tool = EchoTool { cmd, echo_proxy };
        tool.main(writer).await.unwrap();
        assert_eq!(format!("{ECHO}\n"), test_stdout.into_string());
    }
}
