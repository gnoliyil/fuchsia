// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl_fuchsia_component as fcomponent, fidl_fuchsia_component_decl as fdecl,
    fidl_fuchsia_io as fio,
};

/// Creates a child in the specified `Realm`.
///
/// # Parameters
/// - `child_name`: The name of the child to be added.
/// - `child_url`: The component URL of the child to add.
/// - `collection_name`: The name of the collection to which the child will be added.
/// - `realm`: The `Realm` to which the child will be added.
///
/// # Returns
/// `Ok` if the child is created successfully.
pub async fn create_child_component(
    child_name: &str,
    child_url: &str,
    collection_name: &str,
    realm: &fcomponent::RealmProxy,
) -> Result<(), fcomponent::Error> {
    let collection_ref = fdecl::CollectionRef { name: collection_name.to_string() };
    let child_decl = fdecl::Child {
        name: Some(child_name.to_string()),
        url: Some(child_url.to_string()),
        startup: Some(fdecl::StartupMode::Lazy), // Dynamic children can only be started lazily.
        environment: None,
        ..Default::default()
    };
    let child_args = fcomponent::CreateChildArgs { numbered_handles: None, ..Default::default() };
    realm
        .create_child(&collection_ref, &child_decl, child_args)
        .await
        .map_err(|_| fcomponent::Error::Internal)??;

    Ok(())
}

/// Opens the exposed directory of a child in the specified `Realm`. This call
/// is expected to follow a matching call to `create_child`.
///
/// # Parameters
/// - `child_name`: The name of the child to bind.
/// - `collection_name`: The name of collection in which the child was created.
/// - `realm`: The `Realm` the child will bound in.
///
/// # Returns
/// `Ok` Result with a DirectoryProxy bound to the component's `exposed_dir`. This directory
/// contains the capabilities that the child exposed to its realm (as declared, for instance, in the
/// `expose` declaration of the component's `.cml` file).
pub async fn open_child_component_exposed_dir(
    child_name: &str,
    collection_name: &str,
    realm: &fcomponent::RealmProxy,
) -> Result<fio::DirectoryProxy, fcomponent::Error> {
    let child_ref = fdecl::ChildRef {
        name: child_name.to_string(),
        collection: Some(collection_name.to_string()),
    };

    let (client_end, server_end) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()
        .map_err(|_| fcomponent::Error::Internal)?;
    realm
        .open_exposed_dir(&child_ref, server_end)
        .await
        .map_err(|_| fcomponent::Error::Internal)??;

    Ok(client_end)
}

/// Destroys a child in the specified `Realm`. This call is expects a matching call to have been
/// made to `create_child`.
///
/// # Parameters
/// - `child_name`: The name of the child to destroy.
/// - `collection_name`: The name of collection in which the child was created.
/// - `realm`: The `Realm` the child will bound in.
///
/// # Errors
/// Returns an error if the child was not destroyed in the realm.
pub async fn destroy_child_component(
    child_name: &str,
    collection_name: &str,
    realm: &fcomponent::RealmProxy,
) -> Result<(), fcomponent::Error> {
    let child_ref = fdecl::ChildRef {
        name: child_name.to_string(),
        collection: Some(collection_name.to_string()),
    };

    realm.destroy_child(&child_ref).await.map_err(|_| fcomponent::Error::Internal)??;

    Ok(())
}

#[cfg(test)]
mod tests {
    use {
        super::{
            create_child_component, destroy_child_component, open_child_component_exposed_dir,
        },
        fidl::endpoints::{spawn_stream_handler, Proxy},
        fidl_fuchsia_component as fcomponent, fidl_fuchsia_io as fio, fuchsia_async as fasync,
        futures::prelude::*,
        lazy_static::lazy_static,
        test_util::Counter,
    };

    /// Spawns a local handler for the given `fidl_fuchsia_io::Directory` request stream.
    /// The provided `request_handler` is notified when an incoming request is received.
    ///
    /// # Parameters
    /// - `directory_server`: A server request stream from a Directory proxy server endpoint.
    /// - `request_handler`: A function that is called with incoming requests to the spawned
    ///                      `Directory` server.
    fn spawn_directory_server<F: 'static>(
        mut directory_server: fio::DirectoryRequestStream,
        request_handler: F,
    ) where
        F: Fn(fio::DirectoryRequest) + Send,
    {
        fasync::Task::spawn(async move {
            while let Some(directory_request) = directory_server.try_next().await.unwrap() {
                request_handler(directory_request);
            }
        })
        .detach();
    }

    /// Tests that creating a child results in the appropriate call to the `RealmProxy`.
    #[fuchsia::test]
    async fn create_child_parameters() {
        let child_name = "test_child";
        let child_url = "test_url";
        let child_collection = "test_collection";

        let realm_proxy = spawn_stream_handler(move |realm_request| async move {
            match realm_request {
                fcomponent::RealmRequest::CreateChild { collection, decl, args: _, responder } => {
                    assert_eq!(decl.name.unwrap(), child_name);
                    assert_eq!(decl.url.unwrap(), child_url);
                    assert_eq!(&collection.name, child_collection);

                    let _ = responder.send(&mut Ok(()));
                }
                _ => panic!("Realm handler received an unexpected request"),
            }
        })
        .unwrap();

        assert!(create_child_component(child_name, child_url, child_collection, &realm_proxy)
            .await
            .is_ok());
    }

    /// Tests that a success received when creating a child results in an appropriate result from
    /// `create_child`.
    #[fuchsia::test]
    async fn create_child_success() {
        let realm_proxy = spawn_stream_handler(move |realm_request| async move {
            match realm_request {
                fcomponent::RealmRequest::CreateChild {
                    collection: _,
                    decl: _,
                    args: _,
                    responder,
                } => {
                    let _ = responder.send(&mut Ok(()));
                }
                _ => panic!("Realm handler received an unexpected request"),
            }
        })
        .unwrap();

        assert!(create_child_component("", "", "", &realm_proxy).await.is_ok());
    }

    /// Tests that an error received when creating a child results in an appropriate error from
    /// `create_child`.
    #[fuchsia::test]
    async fn create_child_error() {
        let realm_proxy = spawn_stream_handler(move |realm_request| async move {
            match realm_request {
                fcomponent::RealmRequest::CreateChild {
                    collection: _,
                    decl: _,
                    args: _,
                    responder,
                } => {
                    let _ = responder.send(&mut Err(fcomponent::Error::Internal));
                }
                _ => panic!("Realm handler received an unexpected request"),
            }
        })
        .unwrap();

        assert!(create_child_component("", "", "", &realm_proxy).await.is_err());
    }

    /// Tests that `open_child_component_exposed_dir` results in the appropriate call to `RealmProxy`.
    #[fuchsia::test]
    async fn open_child_component_exposed_dir_parameters() {
        let child_name = "test_child";
        let child_collection = "test_collection";

        let realm_proxy = spawn_stream_handler(move |realm_request| async move {
            match realm_request {
                fcomponent::RealmRequest::OpenExposedDir { child, exposed_dir: _, responder } => {
                    assert_eq!(child.name, child_name);
                    assert_eq!(child.collection, Some(child_collection.to_string()));

                    let _ = responder.send(&mut Ok(()));
                }
                _ => panic!("Realm handler received an unexpected request"),
            }
        })
        .unwrap();

        assert!(open_child_component_exposed_dir(child_name, child_collection, &realm_proxy)
            .await
            .is_ok());
    }

    /// Tests that a success received when opening a child's exposed directory
    /// results in an appropriate result from `open_child_component_exposed_dir`.
    #[fuchsia::test]
    async fn open_child_component_exposed_dir_success() {
        let realm_proxy = spawn_stream_handler(move |realm_request| async move {
            match realm_request {
                fcomponent::RealmRequest::OpenExposedDir {
                    child: _,
                    exposed_dir: _,
                    responder,
                } => {
                    let _ = responder.send(&mut Ok(()));
                }
                _ => panic!("Realm handler received an unexpected request"),
            }
        })
        .unwrap();

        assert!(open_child_component_exposed_dir("", "", &realm_proxy).await.is_ok());
    }

    /// Tests that opening a child's exposed directory returns successfully.
    #[fuchsia::test]
    async fn open_child_exposed_dir_success() {
        // Make a static call counter to avoid unneeded complexity with cloned Arc<Mutex>.
        lazy_static! {
            static ref CALL_COUNT: Counter = Counter::new(0);
        }

        let directory_request_handler = |directory_request| match directory_request {
            fio::DirectoryRequest::Open { path: fake_capability_path, .. } => {
                CALL_COUNT.inc();
                assert_eq!(fake_capability_path, "fake_capability_path");
            }
            _ => panic!("Directory handler received an unexpected request"),
        };

        let realm_proxy = spawn_stream_handler(move |realm_request| async move {
            match realm_request {
                fcomponent::RealmRequest::OpenExposedDir {
                    child: _,
                    exposed_dir: exposed_dir_server,
                    responder,
                } => {
                    CALL_COUNT.inc();
                    spawn_directory_server(
                        exposed_dir_server.into_stream().unwrap(),
                        directory_request_handler,
                    );
                    let _ = responder.send(&mut Ok(()));
                }
                _ => panic!("Realm handler received an unexpected request"),
            }
        })
        .unwrap();

        let exposed_dir = open_child_component_exposed_dir("", "", &realm_proxy).await.unwrap();

        // Create a proxy of any FIDL protocol, with any `await`-able method.
        // (`fio::DirectoryMarker` here is arbitrary.)
        let (client_end, server_end) =
            fidl::endpoints::create_proxy::<fio::DirectoryMarker>().unwrap();

        // Connect should succeed, but it is still an asynchronous operation.
        // The `directory_request_handler` is not called yet.
        assert!(fdio::service_connect_at(
            &exposed_dir.into_channel().unwrap().into_zx_channel(),
            "fake_capability_path",
            server_end.into_channel()
        )
        .is_ok());

        // Attempting to invoke and await an arbitrary method to ensure the
        // `directory_request_handler` responds to the Open() method and increment
        // the DIRECTORY_OPEN_CALL_COUNT.
        //
        // Since this is a fake capability (of any arbitrary type), it should fail.
        assert!(client_end.rewind().await.is_err());

        // Calls to Realm::OpenExposedDir and Directory::Open should have happened.
        assert_eq!(CALL_COUNT.get(), 2);
    }

    /// Tests that an error received when opening a child's exposed directory
    /// results in an appropriate error from `open_exposed_dir`.
    #[fuchsia::test]
    async fn open_child_component_exposed_dir_error() {
        let realm_proxy = spawn_stream_handler(move |realm_request| async move {
            match realm_request {
                fcomponent::RealmRequest::OpenExposedDir {
                    child: _,
                    exposed_dir: _,
                    responder,
                } => {
                    let _ = responder.send(&mut Err(fcomponent::Error::Internal));
                }
                _ => panic!("Realm handler received an unexpected request"),
            }
        })
        .unwrap();

        assert!(open_child_component_exposed_dir("", "", &realm_proxy).await.is_err());
    }

    /// Tests that `destroy_child` results in the appropriate call to `RealmProxy`.
    #[fuchsia::test]
    async fn destroy_child_parameters() {
        let child_name = "test_child";
        let child_collection = "test_collection";

        let realm_proxy = spawn_stream_handler(move |realm_request| async move {
            match realm_request {
                fcomponent::RealmRequest::DestroyChild { child, responder } => {
                    assert_eq!(child.name, child_name);
                    assert_eq!(child.collection, Some(child_collection.to_string()));

                    let _ = responder.send(&mut Ok(()));
                }
                _ => panic!("Realm handler received an unexpected request"),
            }
        })
        .unwrap();

        assert!(destroy_child_component(child_name, child_collection, &realm_proxy).await.is_ok());
    }
}
