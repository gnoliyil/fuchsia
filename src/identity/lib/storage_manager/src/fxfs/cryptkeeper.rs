// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::fxfs::log_and_map_err::LogThen,
    fidl::endpoints::{ClientEnd, Proxy},
    fidl_fuchsia_component::{self as fcomponent, RealmMarker},
    fidl_fuchsia_component_decl as fdecl,
    fidl_fuchsia_fxfs::{CryptManagementMarker, CryptMarker, CryptProxy, KeyPurpose},
    fidl_fuchsia_identity_account as faccount, fidl_fuchsia_io as fio,
    fuchsia_component::client::{
        connect_to_protocol, connect_to_protocol_at_dir_root, open_childs_exposed_directory,
    },
    tracing::error,
    typed_builder::TypedBuilder,
};

#[derive(TypedBuilder)]
pub struct Args {
    // The name of the component collection within which crypt components are
    // launched. Defaults to "crypt".
    #[builder(default = "crypt".to_string())]
    crypt_collection_name: String,

    // The name for the crypt component instance. Defaults to "crypt".
    #[builder(default = "crypt".to_string())]
    crypt_component_name: String,
}

const CRYPT_CM_URL: &str = "#meta/fxfs-crypt.cm";

/// A helper which provides access to the underlying Crypt service.
/// Responsible for destroying the Crypt directory when asked.
///
/// FYI: CryptKeeper expects to instantiate an FXFS Crypt component in the same
/// package as the calling component. For now, see:
/// - //src/identity/lib/storage_manager/meta/fxfs.shard.cml>
///   - for the expected capabilities and collections this component needs, and
/// - //src/identity/tests/storage_manager_integration/tests/fxfs.rs
///   - for an example (test) callsite for FxfsStorageManager and CryptKeeper.
pub struct CryptKeeper {
    directory: Option<fio::DirectoryProxy>,

    // The name of the component collection within which crypt components are launched.
    crypt_collection_name: String,
    // The name for the crypt component.
    crypt_component_name: String,
}

impl CryptKeeper {
    // Destroys (and consumes) the crypt component.
    pub async fn destroy(mut self) -> Result<(), faccount::Error> {
        let _ = self.directory.take();

        let proxy = connect_to_protocol::<RealmMarker>()
            .log_error_then("Connect to Realm protocol failed", faccount::Error::Resource)?;

        let () = proxy
            .destroy_child(&mut fdecl::ChildRef {
                name: self.crypt_component_name.to_string(),
                collection: Some(self.crypt_collection_name.to_string()),
            })
            .await
            .log_warn_then("Could not send destroy child request", faccount::Error::Resource)?
            .log_warn_then("Could not destroy child", faccount::Error::Resource)?;

        Ok(())
    }

    // Creates a new CryptKeeper instance (and a new underlying Crypt child
    // component to serve it) given some initializing key.
    pub async fn new_from_key(args: Args, key: &[u8]) -> Result<CryptKeeper, faccount::Error> {
        let realm_proxy = connect_to_protocol::<RealmMarker>()
            .log_error_then("Connect to Realm protocol failed", faccount::Error::Resource)?;

        realm_proxy
            .create_child(
                &mut fdecl::CollectionRef { name: args.crypt_collection_name.clone() },
                fdecl::Child {
                    name: Some(args.crypt_component_name.clone()),
                    url: Some(CRYPT_CM_URL.to_string()),
                    startup: Some(fdecl::StartupMode::Lazy),
                    environment: None,
                    ..fdecl::Child::EMPTY
                },
                fcomponent::CreateChildArgs::EMPTY,
            )
            .await
            .log_warn_then("create_child FIDL failed", faccount::Error::Resource)?
            .log_warn_then("create_child failed", faccount::Error::Resource)?;

        let exposed_dir = open_childs_exposed_directory(
            args.crypt_component_name.clone(),
            Some(args.crypt_collection_name.clone()),
        )
        .await
        .log_warn_then("open_childs_exposed_directory failed", faccount::Error::Resource)?;

        let crypt_management_service = connect_to_protocol_at_dir_root::<CryptManagementMarker>(
            &exposed_dir,
        )
        .log_error_then("Connect to CryptManagement protocol failed", faccount::Error::Resource)?;

        // TODO(fxbug.dev/116244): Use different keys for metadata/data.
        crypt_management_service
            .add_wrapping_key(0, key)
            .await
            .log_warn_then("add_wrapping_key FIDL failed", faccount::Error::Resource)?
            .log_warn_then("add_wrapping_key failed", faccount::Error::Resource)?;

        crypt_management_service
            .add_wrapping_key(1, key)
            .await
            .log_warn_then("add_wrapping_key FIDL failed", faccount::Error::Resource)?
            .log_warn_then("add_wrapping_key failed", faccount::Error::Resource)?;

        crypt_management_service
            .set_active_key(KeyPurpose::Data, 0)
            .await
            .log_warn_then("set_active_key FIDL failed", faccount::Error::Resource)?
            .log_warn_then("set_active_key failed", faccount::Error::Resource)?;

        crypt_management_service
            .set_active_key(KeyPurpose::Metadata, 1)
            .await
            .log_warn_then("set_active_key FIDL failed", faccount::Error::Resource)?
            .log_warn_then("set_active_key failed", faccount::Error::Resource)?;

        Ok(CryptKeeper {
            directory: Some(exposed_dir),
            crypt_collection_name: args.crypt_collection_name,
            crypt_component_name: args.crypt_component_name,
        })
    }

    // Returns a crypt proxy.  This will panic if destroy has been called.
    pub fn crypt_proxy(&self) -> Result<CryptProxy, faccount::Error> {
        if let Some(exposed_dir) = self.directory.as_ref() {
            Ok(connect_to_protocol_at_dir_root::<CryptMarker>(exposed_dir).log_error_then(
                "connect to CryptMarker protocol failed",
                faccount::Error::Resource,
            )?)
        } else {
            error!("Called crypt_proxy, but the exposed directory was absent.");
            Err(faccount::Error::Resource)
        }
    }

    // Returns a crypt proxy (like .crypt_proxy() above), but one which has been
    // preconverted into ClientEnd<CryptMarker>; callsites to Volumes.Create
    // etc. expect this form of the proxy channel.
    pub fn crypt_client_end(&self) -> Result<ClientEnd<CryptMarker>, faccount::Error> {
        Ok(self
            .crypt_proxy()?
            .into_channel()
            .log_warn_then("Failed to turn crypt proxy into channel", faccount::Error::Resource)?
            .into_zx_channel()
            .into())
    }
}

impl Drop for CryptKeeper {
    // When CryptKeeper is dropped, if it contains a directory (i.e. if
    // .destroy() has not already been called), destroy it.
    fn drop(&mut self) {
        if self.directory.is_some() {
            let to_drop = std::mem::replace(
                self,
                CryptKeeper {
                    directory: None,
                    crypt_collection_name: "".to_string(),
                    crypt_component_name: "".to_string(),
                },
            );
            let _ = to_drop.destroy();
        }
    }
}
