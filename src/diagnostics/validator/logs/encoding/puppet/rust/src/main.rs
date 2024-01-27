// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Error},
    diagnostics_log_encoding::encode::{Encoder, EncodingError},
    fidl_fuchsia_mem::Buffer,
    fidl_fuchsia_validate_logs::{EncodingPuppetRequest, EncodingPuppetRequestStream, PuppetError},
    fuchsia_component::server::ServiceFs,
    fuchsia_zircon::Vmo,
    futures::prelude::*,
    std::io::Cursor,
    tracing::*,
};

const BUFFER_SIZE: usize = 1024;

/// Serves the `fuchsia.validate.logs.Validate` request on the given stream.
async fn run_encoding_service(mut stream: EncodingPuppetRequestStream) -> Result<(), Error> {
    while let Some(request) = stream.try_next().await? {
        let EncodingPuppetRequest::Encode { record, responder } = request;

        let mut buffer = Cursor::new(vec![0u8; BUFFER_SIZE]);
        let mut encoder = Encoder::new(&mut buffer);
        match encoder.write_record(&record) {
            Ok(()) => {
                let encoded = &buffer.get_ref().as_slice()[..buffer.position() as usize];
                let vmo = Vmo::create(BUFFER_SIZE as u64)?;
                vmo.write(&encoded, 0)?;
                responder.send(Ok(Buffer { vmo, size: encoded.len() as u64 }))?;
            }
            Err(EncodingError::Unsupported) => {
                responder.send(Err(PuppetError::UnsupportedRecord))?
            }
            Err(e) => {
                return Err(format_err!("Error parsing record: {:?}", e));
            }
        }
    }
    Ok(())
}

/// An enumeration of the services served by this component.
enum IncomingService {
    Encoding(EncodingPuppetRequestStream),
}

#[fuchsia::main]
async fn main() -> Result<(), Error> {
    let mut fs = ServiceFs::new_local();
    fs.dir("svc").add_fidl_service(IncomingService::Encoding);
    fs.take_and_serve_directory_handle()?;

    // Set concurrent > 1, otherwise additional requests hang on the completion of the Validate
    // service.
    const MAX_CONCURRENT: usize = 4;
    fs.for_each_concurrent(MAX_CONCURRENT, |IncomingService::Encoding(stream)| {
        run_encoding_service(stream).unwrap_or_else(|e| error!("ERROR in puppet's main: {:?}", e))
    })
    .await;

    Ok(())
}
