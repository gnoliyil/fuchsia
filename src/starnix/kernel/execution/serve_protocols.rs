// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Error;
use fidl::endpoints::ServerEnd;
use fidl_fuchsia_component_runner as frunner;
use fidl_fuchsia_io as fio;
use fidl_fuchsia_starnix_container as fstarcontainer;
use fuchsia_async::{self as fasync, DurationExt};
use fuchsia_zircon as zx;
use futures::{AsyncReadExt, AsyncWriteExt, TryStreamExt};
use std::{ffi::CString, rc::Rc, sync::Arc};

use crate::{
    execution::{execute_task, Container},
    fs::{
        buffers::*, devpts::create_main_and_replica, file_server::serve_file_at,
        fuchsia::create_fuchsia_pipe, socket::VsockSocket, *,
    },
    logging::log_error,
    task::*,
    types::*,
};

use super::*;

pub fn expose_root(
    container: &Container,
    server_end: ServerEnd<fio::DirectoryMarker>,
) -> Result<(), Error> {
    let system_task = container.kernel.kthreads.system_task();
    let root_file = system_task.open_file(b"/", OpenFlags::RDONLY)?;
    serve_file_at(server_end.into_channel().into(), system_task, &root_file)?;
    Ok(())
}

pub async fn serve_component_runner(
    mut request_stream: frunner::ComponentRunnerRequestStream,
    container: Rc<Container>,
) -> Result<(), Error> {
    while let Some(event) = request_stream.try_next().await? {
        match event {
            frunner::ComponentRunnerRequest::Start { start_info, controller, .. } => {
                let container = container.clone();
                fasync::Task::local(async move {
                    if let Err(e) = start_component(start_info, controller, &container).await {
                        log_error!("failed to start component: {:?}", e);
                    }
                })
                .detach();
            }
        }
    }
    Ok(())
}

fn to_winsize(window_size: Option<fstarcontainer::ConsoleWindowSize>) -> uapi::winsize {
    window_size
        .map(|window_size| uapi::winsize {
            ws_row: window_size.rows,
            ws_col: window_size.cols,
            ws_xpixel: window_size.x_pixels,
            ws_ypixel: window_size.y_pixels,
        })
        .unwrap_or(uapi::winsize::default())
}

pub async fn serve_container_controller(
    mut request_stream: fstarcontainer::ControllerRequestStream,
    container: &Container,
) -> Result<(), Error> {
    while let Some(event) = request_stream.try_next().await? {
        match event {
            fstarcontainer::ControllerRequest::VsockConnect { port, bridge_socket, .. } => {
                connect_to_vsock(port, bridge_socket, container).await.unwrap_or_else(|e| {
                    log_error!("failed to connect to vsock {:?}", e);
                });
            }
            fstarcontainer::ControllerRequest::SpawnConsole { payload, responder } => {
                if let (Some(console), Some(binary_path)) = (payload.console, payload.binary_path) {
                    let binary_path = CString::new(binary_path)?;
                    let argv = payload
                        .argv
                        .unwrap_or(vec![])
                        .into_iter()
                        .map(CString::new)
                        .collect::<Result<Vec<_>, _>>()?;
                    let environ = payload
                        .environ
                        .unwrap_or(vec![])
                        .into_iter()
                        .map(CString::new)
                        .collect::<Result<Vec<_>, _>>()?;
                    match create_task_with_pty(
                        &container.kernel,
                        binary_path,
                        argv,
                        environ,
                        to_winsize(payload.window_size),
                    ) {
                        Ok((current_task, pty)) => {
                            execute_task(current_task, move |result| {
                                let _ = match result {
                                    Ok(ExitStatus::Exit(exit_code)) => {
                                        responder.send(Ok(exit_code))
                                    }
                                    _ => responder.send(Err(zx::Status::CANCELED.into_raw())),
                                };
                            });
                            let _ = forward_to_pty(container, console, pty).map_err(|e| {
                                log_error!("failed to forward to terminal {:?}", e);
                            });
                        }
                        Err(errno) => {
                            log_error!("failed to create task with pty {:?}", errno);
                            responder.send(Err(zx::Status::IO.into_raw()))?;
                        }
                    }
                } else {
                    responder.send(Err(zx::Status::INVALID_ARGS.into_raw()))?;
                }
            }
        }
    }
    Ok(())
}

async fn connect_to_vsock(
    port: u32,
    bridge_socket: fidl::Socket,
    container: &Container,
) -> Result<(), Error> {
    let socket = loop {
        if let Ok(socket) = container.kernel.default_abstract_vsock_namespace.lookup(&port) {
            break socket;
        };
        fasync::Timer::new(fasync::Duration::from_millis(100).after_now()).await;
    };

    let system_task = container.kernel.kthreads.system_task();
    let pipe =
        create_fuchsia_pipe(system_task, bridge_socket, OpenFlags::RDWR | OpenFlags::NONBLOCK)?;
    socket.downcast_socket::<VsockSocket>().unwrap().remote_connection(
        &socket,
        system_task,
        pipe,
    )?;

    Ok(())
}

fn create_task_with_pty(
    kernel: &Arc<Kernel>,
    binary_path: CString,
    argv: Vec<CString>,
    environ: Vec<CString>,
    window_size: uapi::winsize,
) -> Result<(CurrentTask, FileHandle), Errno> {
    let mut current_task = Task::create_init_child_process(kernel, &binary_path)?;
    let executable = current_task.open_file(binary_path.as_bytes(), OpenFlags::RDONLY)?;
    current_task.exec(executable, binary_path, argv, environ)?;
    let (pty, pts) = create_main_and_replica(&current_task, window_size)?;
    let fd_flags = FdFlags::empty();
    assert_eq!(0, current_task.add_file(pts.clone(), fd_flags)?.raw());
    assert_eq!(1, current_task.add_file(pts.clone(), fd_flags)?.raw());
    assert_eq!(2, current_task.add_file(pts, fd_flags)?.raw());
    Ok((current_task, pty))
}

fn forward_to_pty(
    container: &Container,
    console: fidl::Socket,
    pty: FileHandle,
) -> Result<(), Error> {
    // Matches fuchsia.io.Transfer capacity, somewhat arbitrarily.
    const BUFFER_CAPACITY: usize = 8192;
    let system_task = container.kernel.kthreads.system_task();

    let (mut rx, mut tx) = fuchsia_async::Socket::from_socket(console)?.split();
    let kernel = &container.kernel;
    let current_task = system_task.clone();
    let pty_sink = pty.clone();
    kernel.kthreads.pool.dispatch(move || {
        let _result: Result<(), Error> = fasync::LocalExecutor::new().run_singlethreaded(async {
            let mut buffer = vec![0u8; BUFFER_CAPACITY];
            loop {
                let bytes = rx.read(&mut buffer[..]).await?;
                pty_sink.write(&current_task, &mut VecInputBuffer::new(&buffer[..bytes]))?;
            }
        });
    });

    let current_task = system_task.clone();
    let pty_source = pty;
    kernel.kthreads.pool.dispatch(move || {
        let _result: Result<(), Error> = fasync::LocalExecutor::new().run_singlethreaded(async {
            let mut buffer = VecOutputBuffer::new(BUFFER_CAPACITY);
            loop {
                buffer.reset();
                pty_source.read(&current_task, &mut buffer)?;
                tx.write_all(buffer.data()).await?;
            }
        });
    });

    Ok(())
}
