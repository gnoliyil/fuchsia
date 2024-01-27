// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod args;

use {
    anyhow::{anyhow, Context, Error},
    component_debug::dirs::{connect_to_instance_protocol_at_path, OpenDirType},
    fidl_fuchsia_identity_account::{AccountManagerMarker, AccountManagerProxy, AccountMetadata},
    fidl_fuchsia_sys2::RealmQueryMarker,
    fuchsia_component::client::connect_to_protocol_at_path,
};

const ACCOUNT_MANAGER_MONIKER: &str = "./core/account/account_manager";
const ACCOUNT_MANAGER_PROTOCOL: &str = "fuchsia.identity.account.AccountManager";
const PASSWORD_AUTHENTICATOR_MONIKER: &str = "./core/account/password_authenticator";
const PASSWORD_AUTHENTICATOR_PROTOCOL: &str = "fuchsia.identity.account.DeprecatedAccountManager";
const REALM_QUERY_PATH: &str = "/svc/fuchsia.sys2.RealmQuery.root";

/// Performs the operation defined by the supplied command line arguments using the supplied
/// `AccountManager`.
async fn perform_command(
    command: args::Command,
    account_manager: &AccountManagerProxy,
) -> Result<(), Error> {
    match command.subcommand {
        args::Subcommand::List(_) => print_account_data(account_manager).await,
        args::Subcommand::RemoveAll(_) => remove_all(account_manager).await,
    }
}

/// Prints the current account IDs and metadata as reported by the supplied account manager.
async fn print_account_data(account_manager: &AccountManagerProxy) -> Result<(), Error> {
    let ids = account_manager.get_account_ids().await?;
    println!("{} account{} present:", ids.len(), if ids.len() == 1 { "" } else { "s" });
    for id in ids {
        let metadata_str = match account_manager.get_account_metadata(id).await? {
            Ok(AccountMetadata { name: Some(name), .. }) => format!("name:{}", name),
            Ok(AccountMetadata { name: None, .. }) => "<name not set>".to_string(),
            Err(_) => "<error getting metadata>".to_string(),
        };
        println!("  ID:{}  {}", id, metadata_str);
    }
    Ok(())
}

/// Removes all accounts reported by the supplied account manager.
async fn remove_all(account_manager: &AccountManagerProxy) -> Result<(), Error> {
    let ids = account_manager.get_account_ids().await?;
    if ids.is_empty() {
        println!("No accounts to remove.");
        return Ok(());
    }
    for id in ids {
        account_manager
            .remove_account(id)
            .await?
            .map_err(|err| anyhow!("Failed to remove account {}: {:?}", id, err))?;
        println!("Removed account {}", id);
    }
    Ok(())
}

/// Connects to a discoverable protocol at the component instance supplied by `moniker`.
async fn connect_to_exposed_protocol<P: fidl::endpoints::DiscoverableProtocolMarker>(
    moniker: &str,
    protocol_name: &str,
) -> Result<P::Proxy, Error> {
    let realm_query = connect_to_protocol_at_path::<RealmQueryMarker>(REALM_QUERY_PATH)
        .context("Failed to connect to realm query")?;
    let moniker = moniker.try_into()?;
    let proxy = connect_to_instance_protocol_at_path::<P>(
        &moniker,
        OpenDirType::Exposed,
        protocol_name,
        &realm_query,
    )
    .await?;
    Ok(proxy)
}

/// Connects to the account manager protocol at the first available location.
async fn connect_to_account_manager() -> Result<AccountManagerProxy, Error> {
    // Connect to the new account system implementation if possible.
    if let Ok(account_manager) = connect_to_exposed_protocol::<AccountManagerMarker>(
        ACCOUNT_MANAGER_MONIKER,
        ACCOUNT_MANAGER_PROTOCOL,
    )
    .await
    {
        println!("Connected to AccountManager protocol supplied by AccountManager");
        return Ok(account_manager);
    }

    // If not then fallback to the deprecated password authenticator implementation.
    let account_manager = connect_to_exposed_protocol::<AccountManagerMarker>(
        PASSWORD_AUTHENTICATOR_MONIKER,
        PASSWORD_AUTHENTICATOR_PROTOCOL,
    )
    .await
    .context("Failed to connect to AccountManager")?;
    println!("Connected to AccountManager protocol supplied by PasswordAuthenticator");
    Ok(account_manager)
}

#[fuchsia::main(logging_tags = ["identity"])]
async fn main() -> Result<(), Error> {
    let command: args::Command = argh::from_env();

    let account_manager = connect_to_account_manager().await?;
    perform_command(command, &account_manager).await
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        anyhow::anyhow,
        args::*,
        fidl::endpoints::create_proxy_and_stream,
        fidl_fuchsia_identity_account::{AccountManagerRequest, AccountManagerRequestStream},
        fuchsia_async as fasync,
        futures::TryStreamExt as _,
        std::sync::Arc,
    };

    /// Defines how the fake server should handle a single `AccountManagerRequest`.
    trait RequestHandler: Send + Sync {
        fn handle(&self, req: AccountManagerRequest) -> Result<(), Error>;
    }

    /// Spawns a task to handle requests from an AccountManagerStream, running the supplied
    /// sequence of request handlers on the sequence of incoming requests.
    fn handle_stream(
        mut stream: AccountManagerRequestStream,
        handlers: Vec<Arc<dyn RequestHandler>>,
    ) {
        fasync::Task::spawn(async move {
            let mut handler_it = handlers.into_iter();
            loop {
                match (stream.try_next().await.unwrap(), handler_it.next()) {
                    (Some(req), Some(handler)) => handler.handle(req).unwrap(),
                    (None, Some(_)) => {
                        panic!("Stream closed before sending all expected requests.")
                    }
                    (Some(_), None) => panic!("Stream did not close after all expected requests."),
                    (None, None) => break,
                }
            }
        })
        .detach();
    }

    struct GetAccountIdsHandler {
        response: Vec<u64>,
    }

    impl RequestHandler for GetAccountIdsHandler {
        fn handle(&self, req: AccountManagerRequest) -> Result<(), Error> {
            match req {
                AccountManagerRequest::GetAccountIds { responder } => {
                    responder.send(&self.response).context("Failed to send response")
                }
                _ => Err(anyhow!("Did not expect {:?}", req)),
            }
        }
    }

    struct GetAccountMetadataHandler {
        id: u64,
        response_name: &'static str,
    }

    impl RequestHandler for GetAccountMetadataHandler {
        fn handle(&self, req: AccountManagerRequest) -> Result<(), Error> {
            match req {
                AccountManagerRequest::GetAccountMetadata { id, responder } => {
                    if id != self.id {
                        Err(anyhow!("Received GetAccountMetadata for {}, expected {}", id, self.id))
                    } else {
                        responder
                            .send(&mut Ok(AccountMetadata {
                                name: Some(self.response_name.to_string()),
                                ..Default::default()
                            }))
                            .context("Failed to send response")
                    }
                }
                _ => Err(anyhow!("Did not expect {:?}", req)),
            }
        }
    }

    struct RemoveAccountHandler {
        id: u64,
    }

    impl RequestHandler for RemoveAccountHandler {
        fn handle(&self, req: AccountManagerRequest) -> Result<(), Error> {
            match req {
                AccountManagerRequest::RemoveAccount { id, responder } => {
                    if id != self.id {
                        Err(anyhow!("Received RemoveAccount for {}, expected {}", id, self.id))
                    } else {
                        responder.send(&mut Ok(())).context("Failed to send response")
                    }
                }
                _ => Err(anyhow!("Did not expect {:?}", req)),
            }
        }
    }

    #[fuchsia::test]
    async fn test_list_no_accounts() {
        let command = Command { subcommand: Subcommand::List(List {}) };
        let (proxy, stream) = create_proxy_and_stream::<AccountManagerMarker>().unwrap();
        handle_stream(stream, vec![Arc::new(GetAccountIdsHandler { response: vec![] })]);

        assert!(perform_command(command, &proxy).await.is_ok());
    }

    #[fuchsia::test]
    async fn test_list_multiple_accounts() {
        let command = Command { subcommand: Subcommand::List(List {}) };
        let (proxy, stream) = create_proxy_and_stream::<AccountManagerMarker>().unwrap();
        handle_stream(
            stream,
            vec![
                Arc::new(GetAccountIdsHandler { response: vec![4, 3] }),
                Arc::new(GetAccountMetadataHandler { id: 4, response_name: "tim" }),
                Arc::new(GetAccountMetadataHandler { id: 3, response_name: "tam" }),
            ],
        );

        assert!(perform_command(command, &proxy).await.is_ok());
    }

    #[fuchsia::test]
    async fn test_remove_all_no_accounts() {
        let command = Command { subcommand: Subcommand::RemoveAll(RemoveAll {}) };
        let (proxy, stream) = create_proxy_and_stream::<AccountManagerMarker>().unwrap();
        handle_stream(stream, vec![Arc::new(GetAccountIdsHandler { response: vec![] })]);

        assert!(perform_command(command, &proxy).await.is_ok());
    }

    #[fuchsia::test]
    async fn test_remove_all_multiple_accounts() {
        let command = Command { subcommand: Subcommand::RemoveAll(RemoveAll {}) };
        let (proxy, stream) = create_proxy_and_stream::<AccountManagerMarker>().unwrap();
        handle_stream(
            stream,
            vec![
                Arc::new(GetAccountIdsHandler { response: vec![3, 2, 1] }),
                Arc::new(RemoveAccountHandler { id: 3 }),
                Arc::new(RemoveAccountHandler { id: 2 }),
                Arc::new(RemoveAccountHandler { id: 1 }),
            ],
        );

        assert!(perform_command(command, &proxy).await.is_ok());
    }
}
