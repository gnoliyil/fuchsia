// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        common::AccountLifetime,
        inspect,
        interaction_lock_state::InteractionLockState,
        persona::{Persona, PersonaContext},
        storage_lock_request,
        stored_account::StoredAccount,
    },
    account_common::{AccountManagerError, FidlPersonaId, PersonaId},
    anyhow::Error,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_identity_account::{
        AccountRequest, AccountRequestStream, AuthState, AuthStateSummary,
        AuthTargetRegisterAuthListenerRequest, Error as ApiError, Lifetime, PersonaMarker,
    },
    fidl_fuchsia_io as fio,
    fuchsia_inspect::{Node, NumericProperty},
    futures::{lock::Mutex, prelude::*},
    identity_common::{cancel_or, TaskGroup, TaskGroupCancel},
    std::{
        fs,
        sync::Arc,
        time::{Duration, SystemTime},
    },
    storage_manager::StorageManager,
    tracing::{error, info, warn},
};

/// The default directory on the filesystem that we return to all clients. Returning a subdirectory
/// rather than the root provides scope to store private account data on the encrypted filesystem
/// that FIDL clients cannot access, and to potentially serve different directories to different
/// clients in the future.
const DEFAULT_DIR: &str = "default";

/// When evaluating whether or not an account is 'unlocked' vs. 'recently
/// authenticated', the threshold to consider.
const RECENCY_THRESHOLD: &Duration = &Duration::from_secs(30);

/// The context that a particular request to an Account should be executed in, capturing
/// information that was supplied upon creation of the channel.
pub struct AccountContext {
    // Note: The per-channel account context is currently empty. It was needed in the past for
    // authentication UI and is likely to be needed again in the future as the API expands.
}

/// Information about the Account that this AccountHandler instance is responsible for.
///
/// This state is only available once the Handler has been initialized to a particular account via
/// the AccountHandlerControl channel.
pub struct Account<SM> {
    /// Lifetime for this account.
    lifetime: Arc<AccountLifetime>,

    /// The default persona for this account.
    default_persona: Arc<Persona>,

    /// Collection of tasks that are using this instance.
    task_group: TaskGroup,

    /// A Sender of a storage lock request.
    storage_lock_request_sender: storage_lock_request::Sender,

    /// Helper for outputting account information via fuchsia_inspect.
    inspect: inspect::Account,

    /// The StorageManager used by the owning account handler.
    storage_manager: Arc<Mutex<SM>>,

    /// Whether or not the account is currently interaction-locked.
    interaction_lock_state: Arc<Mutex<InteractionLockState>>,
    // TODO(jsankey): Once the system and API surface can support more than a single persona, add
    // additional state here to store these personae. This will most likely be a hashmap from
    // PersonaId to Persona struct, and changing default_persona from a struct to an ID.
}

impl<SM> Account<SM>
where
    SM: StorageManager,
{
    /// Manually construct an account object, shouldn't normally be called directly.
    async fn new(
        persona_id: PersonaId,
        lifetime: AccountLifetime,
        storage_lock_request_sender: storage_lock_request::Sender,
        storage_manager: Arc<Mutex<SM>>,
        inspect_parent: &Node,
    ) -> Result<Account<SM>, AccountManagerError> {
        let task_group = TaskGroup::new();
        let default_persona_task_group = task_group
            .create_child()
            .await
            .map_err(|_| AccountManagerError::new(ApiError::RemovalInProgress))?;
        let lifetime = Arc::new(lifetime);
        let account_inspect = inspect::Account::new(inspect_parent);
        Ok(Self {
            lifetime: Arc::clone(&lifetime),
            default_persona: Arc::new(Persona::new(
                persona_id,
                lifetime,
                default_persona_task_group,
                inspect_parent,
            )),
            task_group,
            storage_lock_request_sender,
            inspect: account_inspect,
            storage_manager,
            interaction_lock_state: Arc::new(Mutex::new(InteractionLockState::Unlocked {
                last_authentication_time: SystemTime::now(),
            })),
        })
    }

    /// Creates a new system account and, if it is persistent, stores it on disk.
    pub async fn create(
        lifetime: AccountLifetime,
        storage_manager: Arc<Mutex<SM>>,
        storage_lock_request_sender: storage_lock_request::Sender,
        inspect_parent: &Node,
    ) -> Result<Account<SM>, AccountManagerError> {
        let persona_id = PersonaId::new(rand::random::<u64>());
        if let AccountLifetime::Persistent { ref account_dir } = lifetime {
            if StoredAccount::path(account_dir).exists() {
                warn!("Attempting to create account twice");
                return Err(AccountManagerError::new(ApiError::Internal));
            }
            let stored_account = StoredAccount::new(persona_id);
            stored_account.save(account_dir)?;
        }
        Self::new(
            persona_id,
            lifetime,
            storage_lock_request_sender,
            storage_manager,
            inspect_parent,
        )
        .await
    }

    /// Loads an existing system account from disk.
    pub async fn load(
        lifetime: AccountLifetime,
        storage_manager: Arc<Mutex<SM>>,
        storage_lock_request_sender: storage_lock_request::Sender,
        inspect_parent: &Node,
    ) -> Result<Account<SM>, AccountManagerError> {
        let account_dir = match lifetime {
            AccountLifetime::Persistent { ref account_dir } => account_dir,
            AccountLifetime::Ephemeral => {
                warn!(concat!(
                    "Attempting to load an ephemeral account from disk. This is not a ",
                    "supported operation."
                ));
                return Err(AccountManagerError::new(ApiError::Internal));
            }
        };
        let stored_account = StoredAccount::load(account_dir)?;
        let persona_id = *stored_account.get_default_persona_id();
        Self::new(
            persona_id,
            lifetime,
            storage_lock_request_sender,
            storage_manager,
            inspect_parent,
        )
        .await
    }

    /// Removes the account from disk or returns the account and the error.
    pub fn remove(self) -> Result<(), (Self, AccountManagerError)> {
        self.remove_inner().map_err(|err| (self, err))
    }

    /// Removes the account from disk.
    fn remove_inner(&self) -> Result<(), AccountManagerError> {
        match self.lifetime.as_ref() {
            AccountLifetime::Ephemeral => Ok(()),
            AccountLifetime::Persistent { account_dir } => {
                let to_remove = StoredAccount::path(&account_dir.clone());
                fs::remove_file(to_remove).map_err(|err| {
                    warn!("Failed to delete account doc: {:?}", err);
                    AccountManagerError::new(ApiError::Resource).with_cause(err)
                })
            }
        }
    }

    /// Returns a task group which can be used to spawn and cancel tasks that use this instance.
    pub fn task_group(&self) -> &TaskGroup {
        &self.task_group
    }

    /// Asynchronously handles the supplied stream of `AccountRequest` messages.
    pub async fn handle_requests_from_stream<'a>(
        &'a self,
        context: &'a AccountContext,
        mut stream: AccountRequestStream,
        cancel: TaskGroupCancel,
    ) -> Result<(), Error> {
        self.inspect.open_client_channels.add(1);
        scopeguard::defer!(self.inspect.open_client_channels.subtract(1));
        while let Some(result) = cancel_or(&cancel, stream.try_next()).await {
            if let Some(request) = result? {
                self.handle_request(context, request).await?;
            } else {
                break;
            }
        }
        Ok(())
    }

    /// Dispatches an `AccountRequest` message to the appropriate handler method
    /// based on its type.
    pub async fn handle_request<'a>(
        &'a self,
        context: &'a AccountContext,
        req: AccountRequest,
    ) -> Result<(), fidl::Error> {
        match req {
            AccountRequest::GetLifetime { responder } => {
                let response = self.get_lifetime();
                responder.send(response)?;
            }
            AccountRequest::GetAuthState { responder } => {
                let mut response = self.get_auth_state().await;
                responder.send(&mut response)?;
            }
            AccountRequest::RegisterAuthListener { payload, responder } => {
                let response = self.register_auth_listener(payload);
                responder.send(response)?;
            }
            AccountRequest::GetPersonaIds { responder } => {
                let response = self.get_persona_ids();
                responder.send(&response)?;
            }
            AccountRequest::GetDefaultPersona { persona, responder } => {
                let response = self.get_default_persona(context, persona).await;
                responder.send(response)?;
            }
            AccountRequest::GetPersona { id, persona, responder } => {
                let response = self.get_persona(context, id.into(), persona).await;
                responder.send(response)?;
            }
            AccountRequest::GetAuthMechanismEnrollments { responder, .. } => {
                responder.send(&mut Err(ApiError::UnsupportedOperation))?;
            }
            AccountRequest::CreateAuthMechanismEnrollment { responder, .. } => {
                responder.send(Err(ApiError::UnsupportedOperation))?;
            }
            AccountRequest::RemoveAuthMechanismEnrollment { responder, .. } => {
                responder.send(Err(ApiError::UnsupportedOperation))?;
            }
            AccountRequest::StorageLock { responder } => {
                let response = self.storage_lock().await;
                responder.send(response)?;
            }
            AccountRequest::InteractionLock { responder } => {
                let response = self.interaction_lock().await;
                responder.send(response)?;
            }
            AccountRequest::GetDataDirectory { data_directory, responder, .. } => {
                let response = self.get_data_directory(data_directory).await;
                responder.send(response)?;
            }
        }
        Ok(())
    }

    fn get_lifetime(&self) -> Lifetime {
        Lifetime::from(self.lifetime.as_ref())
    }

    async fn get_auth_state(&self) -> Result<AuthState, ApiError> {
        let interaction_lock_state = &*self.interaction_lock_state.lock().await;
        let summary = match &interaction_lock_state {
            InteractionLockState::Locked => AuthStateSummary::InteractionLocked,
            InteractionLockState::Unlocked { .. } => {
                if interaction_lock_state.is_recently_authenticated(RECENCY_THRESHOLD) {
                    AuthStateSummary::RecentlyAuthenticated
                } else {
                    AuthStateSummary::Unlocked
                }
            }
        };

        Ok(AuthState { summary: Some(summary), ..Default::default() })
    }

    fn register_auth_listener(
        &self,
        _payload: AuthTargetRegisterAuthListenerRequest,
    ) -> Result<(), ApiError> {
        // TODO(jsankey): Implement this method.
        warn!("RegisterAuthListener not yet implemented");
        Err(ApiError::UnsupportedOperation)
    }

    fn get_persona_ids(&self) -> Vec<FidlPersonaId> {
        vec![(*self.default_persona.id()).into()]
    }

    async fn get_default_persona<'a>(
        &'a self,
        _context: &'a AccountContext,
        persona_server_end: ServerEnd<PersonaMarker>,
    ) -> Result<FidlPersonaId, ApiError> {
        let persona_clone = Arc::clone(&self.default_persona);
        let persona_context = PersonaContext {};
        let stream = persona_server_end.into_stream().map_err(|err| {
            error!("Error opening Persona channel: {:?}", err);
            ApiError::Resource
        })?;
        self.default_persona
            .task_group()
            .spawn(|cancel| async move {
                persona_clone
                    .handle_requests_from_stream(&persona_context, stream, cancel)
                    .await
                    .unwrap_or_else(|e| error!("Error handling Persona channel: {:?}", e))
            })
            .await
            .map_err(|_| ApiError::RemovalInProgress)?;
        Ok((*self.default_persona.id()).into())
    }

    async fn get_persona<'a>(
        &'a self,
        context: &'a AccountContext,
        id: PersonaId,
        persona_server_end: ServerEnd<PersonaMarker>,
    ) -> Result<(), ApiError> {
        if &id == self.default_persona.id() {
            self.get_default_persona(context, persona_server_end).await.map(|_| ())
        } else {
            warn!("Requested persona does not exist {:?}", id);
            Err(ApiError::NotFound)
        }
    }

    async fn storage_lock(&self) -> Result<(), ApiError> {
        match self.storage_lock_request_sender.send().await {
            Err(storage_lock_request::SendError::NotSupported) => {
                info!("Account storage lock failure: unsupported account type");
                Err(ApiError::FailedPrecondition)
            }
            Err(storage_lock_request::SendError::UnattendedReceiver) => {
                warn!("Account storage lock failure: unattended listener");
                Err(ApiError::Internal)
            }
            Err(storage_lock_request::SendError::AlreadySent) => {
                info!("Received account storage lock request while existing request in progress");
                Ok(())
            }
            Ok(()) => {
                info!("Account storage lock succeeded");
                Ok(())
            }
        }
    }

    async fn interaction_lock(&self) -> Result<(), ApiError> {
        self.interaction_lock_state.lock().await.lock();
        Ok(())
    }

    async fn get_data_directory(
        &self,
        data_directory: ServerEnd<fio::DirectoryMarker>,
    ) -> Result<(), ApiError> {
        let storage_manager = self.storage_manager.lock().await;

        let root_dir = storage_manager.get_root_dir().await.map_err(|err| {
            warn!("get_data_directory: error accessing root directory: {:?}", err);
            ApiError::Resource
        })?;

        root_dir
            .open(
                fio::OpenFlags::RIGHT_READABLE
                    | fio::OpenFlags::RIGHT_WRITABLE
                    | fio::OpenFlags::DIRECTORY
                    | fio::OpenFlags::CREATE,
                fio::ModeType::empty(),
                DEFAULT_DIR,
                ServerEnd::new(data_directory.into_channel()),
            )
            .map_err(|err| {
                error!("get_data_directory: couldn't open data dir out of storage: {:?}", err);
                ApiError::Resource
            })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_util::*;
    use assert_matches::assert_matches;
    use fidl::endpoints::create_endpoints;
    use fidl_fuchsia_identity_account::{
        AccountMarker, AccountProxy, AuthChangeGranularity, AuthTargetRegisterAuthListenerRequest,
    };
    use fuchsia_async as fasync;
    use fuchsia_fs::{directory, file};
    use fuchsia_inspect::Inspector;
    use futures::channel::oneshot;
    use storage_manager::{minfs::StorageManager as MinfsStorageManager, Key};
    use typed_builder::TypedBuilder;
    use unittest_util::{make_formatted_account_partition_any_key, MockDiskManager};

    const TEST_ENROLLMENT_ID: u64 = 1337;

    const TEST_KEY: Key = Key::Key256Bit([1; 32]);

    /// Type to hold the common state require during construction of test objects and execution
    /// of a test, including an async executor and a temporary location in the filesystem.
    #[derive(TypedBuilder)]
    struct Test {
        #[builder(default = TempLocation::new())]
        location: TempLocation,
    }

    impl Test {
        fn new() -> Self {
            Self::builder().build()
        }

        async fn create_persistent_account(
            &self,
        ) -> Result<Account<MinfsStorageManager<MockDiskManager>>, AccountManagerError> {
            let inspector = Inspector::default();
            let account_dir = self.location.path.clone();
            Account::create(
                AccountLifetime::Persistent { account_dir },
                make_provisioned_minfs_storage_manager().await,
                storage_lock_request::Sender::NotSupported,
                inspector.root(),
            )
            .await
        }

        async fn create_ephemeral_account(
            &self,
        ) -> Result<Account<MinfsStorageManager<MockDiskManager>>, AccountManagerError> {
            let inspector = Inspector::default();
            Account::create(
                AccountLifetime::Ephemeral,
                make_provisioned_minfs_storage_manager().await,
                storage_lock_request::Sender::NotSupported,
                inspector.root(),
            )
            .await
        }

        async fn load_account(
            &self,
        ) -> Result<Account<MinfsStorageManager<MockDiskManager>>, AccountManagerError> {
            let inspector = Inspector::default();
            Account::load(
                AccountLifetime::Persistent { account_dir: self.location.path.clone() },
                make_provisioned_minfs_storage_manager().await,
                storage_lock_request::Sender::NotSupported,
                inspector.root(),
            )
            .await
        }

        async fn create_persistent_account_with_storage_lock_request(
            &self,
        ) -> Result<
            (Account<MinfsStorageManager<MockDiskManager>>, oneshot::Receiver<()>),
            AccountManagerError,
        > {
            let inspector = Inspector::default();
            let account_dir = self.location.path.clone();
            let (sender, receiver) = storage_lock_request::channel();

            let account = Account::create(
                AccountLifetime::Persistent { account_dir },
                make_provisioned_minfs_storage_manager().await,
                sender,
                inspector.root(),
            )
            .await?;
            Ok((account, receiver))
        }

        async fn run<TestFn, Fut, SM>(&mut self, test_object: Account<SM>, test_fn: TestFn)
        where
            TestFn: FnOnce((AccountProxy, Arc<Mutex<SM>>)) -> Fut,
            Fut: Future<Output = Result<(), Error>>,
            SM: StorageManager + Send + Sync + 'static,
        {
            let (account_client_end, account_server_end) = create_endpoints::<AccountMarker>();
            let account_proxy = account_client_end.into_proxy().unwrap();
            let request_stream = account_server_end.into_stream().unwrap();

            let context = AccountContext {};

            let task_group = TaskGroup::new();
            let storage_manager = Arc::clone(&test_object.storage_manager);

            task_group
                .spawn(|cancel| async move {
                    test_object
                        .handle_requests_from_stream(&context, request_stream, cancel)
                        .await
                        .unwrap_or_else(|err| panic!("Fatal error handling test request: {err:?}"))
                })
                .await
                .expect("Unable to spawn task");
            test_fn((account_proxy, storage_manager)).await.expect("Test function failed.")
        }
    }

    async fn make_provisioned_minfs_storage_manager(
    ) -> Arc<Mutex<MinfsStorageManager<MockDiskManager>>> {
        let storage_manager = MinfsStorageManager::new(
            MockDiskManager::new().with_partition(make_formatted_account_partition_any_key()),
        );
        let () =
            storage_manager.provision(&(&TEST_KEY).try_into().unwrap()).await.expect("provision");
        Arc::new(Mutex::new(storage_manager))
    }

    #[fasync::run_until_stalled(test)]
    async fn test_random_identifiers() {
        let mut test = Test::new();
        // Generating two accounts with the same accountID should lead to two different persona IDs
        let account_1 = test.create_persistent_account().await.unwrap();
        test.location = TempLocation::new();
        let account_2 = test.create_persistent_account().await.unwrap();
        assert_ne!(account_1.default_persona.id(), account_2.default_persona.id());
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_lifetime_ephemeral() {
        let mut test = Test::new();
        test.run(
            test.create_ephemeral_account().await.unwrap(),
            |(proxy, _storage_manager)| async move {
                assert_eq!(proxy.get_lifetime().await?, Lifetime::Ephemeral);
                Ok(())
            },
        )
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_lifetime_persistent() {
        let mut test = Test::new();
        test.run(
            test.create_persistent_account().await.unwrap(),
            |(proxy, _storage_manager)| async move {
                assert_eq!(proxy.get_lifetime().await?, Lifetime::Persistent);
                Ok(())
            },
        )
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_create_and_load() {
        let test = Test::new();
        // Persists the account on disk
        let account_1 = test.create_persistent_account().await.unwrap();
        // Reads from same location
        let account_2 = test.load_account().await.unwrap();

        // Since persona ids are random, we can check that loading worked successfully here
        assert_eq!(account_1.default_persona.id(), account_2.default_persona.id());
    }

    #[fasync::run_until_stalled(test)]
    async fn test_load_non_existing() {
        let test = Test::new();
        assert!(test.load_account().await.is_err()); // Reads from uninitialized location
    }

    /// Attempting to load an ephemeral account fails.
    #[fasync::run_until_stalled(test)]
    async fn test_load_ephemeral() {
        let inspector = Inspector::default();
        assert!(Account::load(
            AccountLifetime::Ephemeral,
            make_provisioned_minfs_storage_manager().await,
            storage_lock_request::Sender::NotSupported,
            inspector.root(),
        )
        .await
        .is_err());
    }

    #[fasync::run_until_stalled(test)]
    async fn test_create_twice() {
        let test = Test::new();
        assert!(test.create_persistent_account().await.is_ok());
        assert!(test.create_persistent_account().await.is_err()); // Tries to write to same dir
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_auth_state_recently_authenticated_to_unlocked() {
        let mut test = Test::new();
        test.run(
            test.create_persistent_account().await.unwrap(),
            |(proxy, _storage_manager)| async move {
                assert_matches!(
                    proxy.get_auth_state().await?,
                    Ok(AuthState { summary: Some(AuthStateSummary::RecentlyAuthenticated), .. })
                );

                // TODO(fxb/110859): Once fxr/790543 lands and we can use
                // structured configuration to reduce the recency threshold,
                // wait a second and then assert that this is ::Unlocked.

                Ok(())
            },
        )
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_interaction_lock() {
        let mut test = Test::new();
        test.run(
            test.create_persistent_account().await.unwrap(),
            |(proxy, _storage_manager)| async move {
                assert_matches!(proxy.interaction_lock().await, Ok(Ok(())));

                assert_matches!(
                    proxy.get_auth_state().await?,
                    Ok(AuthState { summary: Some(AuthStateSummary::InteractionLocked), .. })
                );

                Ok(())
            },
        )
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_register_auth_listener() {
        let mut test = Test::new();
        test.run(
            test.create_persistent_account().await.unwrap(),
            |(proxy, _storage_manager)| async move {
                let (auth_listener_client_end, _) = create_endpoints();
                assert_eq!(
                    proxy
                        .register_auth_listener(AuthTargetRegisterAuthListenerRequest {
                            listener: Some(auth_listener_client_end),
                            initial_state: Some(true),
                            granularity: Some(AuthChangeGranularity {
                                summary_changes: Some(true),
                                ..Default::default()
                            }),
                            ..Default::default()
                        })
                        .await?,
                    Err(ApiError::UnsupportedOperation)
                );
                Ok(())
            },
        )
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_persona_ids() {
        let mut test = Test::new();
        // Note: Persona ID is random. Record the persona_id before starting the test.
        let account = test.create_persistent_account().await.unwrap();
        let persona_id = &account.default_persona.id().clone();

        test.run(account, |(proxy, _storage_manager)| async move {
            let response = proxy.get_persona_ids().await?;
            assert_eq!(response.len(), 1);
            assert_eq!(&PersonaId::new(response[0]), persona_id);
            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_default_persona() {
        let mut test = Test::new();
        // Note: Persona ID is random. Record the persona_id before starting the test.
        let account = test.create_persistent_account().await.unwrap();
        let persona_id = &account.default_persona.id().clone();

        test.run(account, |(account_proxy, _storage_manager)| {
            async move {
                let (persona_client_end, persona_server_end) = create_endpoints();
                let response = account_proxy.get_default_persona(persona_server_end).await?;
                assert_eq!(&PersonaId::from(response.unwrap()), persona_id);

                // The persona channel should now be usable.
                let persona_proxy = persona_client_end.into_proxy().unwrap();
                assert_eq!(
                    persona_proxy.get_auth_state().await?,
                    Err(ApiError::UnsupportedOperation)
                );
                assert_eq!(persona_proxy.get_lifetime().await?, Lifetime::Persistent);

                Ok(())
            }
        })
        .await;
    }

    /// When an ephemeral account is created, its default persona is also ephemeral.
    #[fasync::run_until_stalled(test)]
    async fn test_ephemeral_account_has_ephemeral_persona() {
        let mut test = Test::new();
        let account = test.create_ephemeral_account().await.unwrap();
        test.run(account, |(account_proxy, _storage_manager)| async move {
            let (persona_client_end, persona_server_end) = create_endpoints();
            assert!(account_proxy.get_default_persona(persona_server_end).await?.is_ok());
            let persona_proxy = persona_client_end.into_proxy().unwrap();

            assert_eq!(persona_proxy.get_lifetime().await?, Lifetime::Ephemeral);
            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_persona_by_correct_id() {
        let mut test = Test::new();
        let account = test.create_persistent_account().await.unwrap();
        let persona_id = *account.default_persona.id();

        test.run(account, |(account_proxy, _storage_manager)| {
            async move {
                let (persona_client_end, persona_server_end) = create_endpoints();
                assert!(account_proxy
                    .get_persona(FidlPersonaId::from(persona_id), persona_server_end)
                    .await?
                    .is_ok());

                // The persona channel should now be usable.
                let persona_proxy = persona_client_end.into_proxy().unwrap();
                assert_eq!(
                    persona_proxy.get_auth_state().await?,
                    Err(ApiError::UnsupportedOperation)
                );

                Ok(())
            }
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_persona_by_incorrect_id() {
        let mut test = Test::new();
        let account = test.create_persistent_account().await.unwrap();
        // Note: This fixed value has a 1 - 2^64 probability of not matching the randomly chosen
        // one.
        let wrong_id = PersonaId::new(13);

        test.run(account, |(proxy, _storage_manager)| async move {
            let (_, persona_server_end) = create_endpoints();
            assert_eq!(
                proxy.get_persona(wrong_id.into(), persona_server_end).await?,
                Err(ApiError::NotFound)
            );

            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_auth_mechanisms() {
        let mut test = Test::new();
        test.run(
            test.create_persistent_account().await.unwrap(),
            |(proxy, _storage_manager)| async move {
                assert_eq!(
                    proxy.get_auth_mechanism_enrollments().await?,
                    Err(ApiError::UnsupportedOperation)
                );
                let (_, interaction_server_end) = create_endpoints();
                assert_eq!(
                    proxy.create_auth_mechanism_enrollment(interaction_server_end).await?,
                    Err(ApiError::UnsupportedOperation)
                );
                assert_eq!(
                    proxy.remove_auth_mechanism_enrollment(TEST_ENROLLMENT_ID).await?,
                    Err(ApiError::UnsupportedOperation)
                );
                Ok(())
            },
        )
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_storage_lock() {
        let mut test = Test::new();
        let (account, mut receiver) =
            test.create_persistent_account_with_storage_lock_request().await.unwrap();
        test.run(account, |(proxy, _storage_manager)| async move {
            assert_eq!(receiver.try_recv(), Ok(None));
            assert_eq!(proxy.storage_lock().await?, Ok(()));
            assert_eq!(receiver.await, Ok(()));
            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_storage_lock_not_supported() {
        let mut test = Test::new();
        let account = test.create_persistent_account().await.unwrap();
        test.run(account, |(proxy, _storage_manager)| async move {
            assert_eq!(proxy.storage_lock().await?, Err(ApiError::FailedPrecondition));
            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_storage_lock_unattended_receiver() {
        let mut test = Test::new();
        let (account, receiver) =
            test.create_persistent_account_with_storage_lock_request().await.unwrap();
        std::mem::drop(receiver);
        test.run(account, |(proxy, _storage_manager)| async move {
            assert_eq!(proxy.storage_lock().await?, Err(ApiError::Internal));
            Ok(())
        })
        .await;
    }

    #[fasync::run_until_stalled(test)]
    async fn test_storage_lock_twice() {
        let mut test = Test::new();
        let (account, _receiver) =
            test.create_persistent_account_with_storage_lock_request().await.unwrap();
        test.run(account, |(proxy, _storage_manager)| async move {
            assert_eq!(proxy.storage_lock().await?, Ok(()));
            assert_eq!(proxy.storage_lock().await?, Ok(()));
            Ok(())
        })
        .await;
    }

    async fn is_directory_extant<SM>(storage_manager: Arc<Mutex<SM>>, path: &str) -> bool
    where
        SM: StorageManager,
    {
        // Assert that the directory exists.
        let root_dir = storage_manager.lock().await.get_root_dir().await.expect("get root dir");
        directory::open_directory(&root_dir, path, fio::OpenFlags::empty()).await.is_ok()
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_data_directory_ok_and_usable() {
        let mut test = Test::new();

        let (account, _receiver) =
            test.create_persistent_account_with_storage_lock_request().await.unwrap();

        test.run(account, |(proxy, storage_manager)| async move {
            // Directory does not exist.
            assert!(!is_directory_extant(Arc::clone(&storage_manager), DEFAULT_DIR).await);

            // but then we get a directory,
            let (dir, dir_server_end) = fidl::endpoints::create_proxy().unwrap();
            assert_matches!(proxy.get_data_directory(dir_server_end).await, Ok(Ok(())));

            // So now it does.
            assert!(is_directory_extant(Arc::clone(&storage_manager), DEFAULT_DIR).await);

            // And it is usable -- we can open a file and write some contents to
            // it.
            assert_matches!(
                directory::open_file(
                    &dir,
                    "testfile",
                    fio::OpenFlags::CREATE | fio::OpenFlags::RIGHT_WRITABLE
                ).await,
                Ok(f) => {
                    assert_matches!(file::write(&f, "test").await, Ok(_));
                }
            );

            // If for whatever reason get_data_directory() is called again,
            let (dir_again, dir_server_end) = fidl::endpoints::create_proxy().unwrap();
            assert_matches!(proxy.get_data_directory(dir_server_end).await, Ok(Ok(())));

            // We can open the same file and read the same bytes out of it,
            // thereby proving that the directory was not unexpectedly recreated
            // or deleted.
            assert_matches!(
                directory::open_file(&dir_again, "testfile", fio::OpenFlags::RIGHT_READABLE).await,
                Ok(f) => {
                    let bytes = assert_matches!(file::read(&f).await, Ok(b) => b);
                    assert_eq!(bytes, b"test");
                }
            );

            Ok(())
        })
        .await;
    }
}
