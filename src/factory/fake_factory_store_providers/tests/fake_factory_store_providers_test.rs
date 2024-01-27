// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]

use {
    anyhow::Error,
    fidl_fuchsia_factory::{
        AlphaFactoryStoreProviderMarker, CastCredentialsFactoryStoreProviderMarker,
        MiscFactoryStoreProviderMarker, PlayReadyFactoryStoreProviderMarker,
        WeaveFactoryStoreProviderMarker, WidevineFactoryStoreProviderMarker,
    },
    fidl_fuchsia_io as fio, fuchsia_async as fasync,
};

macro_rules! connect_to_factory_store_provider {
    ($t:ty) => {{
        let provider = fuchsia_component::client::connect_to_protocol::<$t>()
            .expect("Failed to connect to protocol");

        let (dir_proxy, dir_server) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()?;
        provider.get_factory_store(dir_server).expect("Failed to get factory store");
        dir_proxy
    }};
}

async fn read_file_from_proxy<'a>(
    dir_proxy: &'a fio::DirectoryProxy,
    file_path: &'a str,
) -> Result<Vec<u8>, Error> {
    let file = fuchsia_fs::directory::open_file_no_describe(
        &dir_proxy,
        file_path,
        fuchsia_fs::OpenFlags::RIGHT_READABLE,
    )?;
    fuchsia_fs::file::read(&file).await.map_err(Into::into)
}

async fn assert_file<'a>(
    dir_proxy: &'a fio::DirectoryProxy,
    filename: &'a str,
    expected_contents: &'a [u8],
) -> Result<(), Error> {
    let contents = read_file_from_proxy(&dir_proxy, filename).await?;
    assert_eq!(expected_contents, &contents[..]);
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn read_factory_files_from_cast_credentials_store() -> Result<(), Error> {
    let dir_proxy = connect_to_factory_store_provider!(CastCredentialsFactoryStoreProviderMarker);
    assert_file(&dir_proxy, "txt/cast.txt", "a cast file".as_bytes()).await?;
    assert_file(&dir_proxy, "cast2.bat", "another one (cast)".as_bytes()).await?;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn missing_factory_files_from_cast_credentials_store() -> Result<(), Error> {
    let dir_proxy = connect_to_factory_store_provider!(CastCredentialsFactoryStoreProviderMarker);
    read_file_from_proxy(&dir_proxy, "missing.txt").await.unwrap_err();
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn read_factory_files_from_misc_store() -> Result<(), Error> {
    let dir_proxy = connect_to_factory_store_provider!(MiscFactoryStoreProviderMarker);
    assert_file(&dir_proxy, "misc.bin", "a misc file".as_bytes()).await?;
    assert_file(&dir_proxy, "misc/misc.bin", "misc from another".as_bytes()).await?;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn missing_factory_files_from_misc_store() -> Result<(), Error> {
    let dir_proxy = connect_to_factory_store_provider!(MiscFactoryStoreProviderMarker);
    read_file_from_proxy(&dir_proxy, "missing2").await.unwrap_err();
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn read_factory_files_from_playready_store() -> Result<(), Error> {
    let dir_proxy = connect_to_factory_store_provider!(PlayReadyFactoryStoreProviderMarker);
    assert_file(&dir_proxy, "txt/playready.txt", "a playready file".as_bytes()).await?;
    assert_file(&dir_proxy, "playready.cfg", "another playready".as_bytes()).await?;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn missing_factory_files_from_playready_store() -> Result<(), Error> {
    let dir_proxy = connect_to_factory_store_provider!(PlayReadyFactoryStoreProviderMarker);
    read_file_from_proxy(&dir_proxy, "abc").await.unwrap_err();
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn read_factory_files_from_weave_store() -> Result<(), Error> {
    let dir_proxy = connect_to_factory_store_provider!(WeaveFactoryStoreProviderMarker);
    assert_file(&dir_proxy, "weave.txt", "a weave file".as_bytes()).await?;
    assert_file(&dir_proxy, "weave2.log", "and yet another weave".as_bytes()).await?;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn missing_factory_files_from_weave_store() -> Result<(), Error> {
    let dir_proxy = connect_to_factory_store_provider!(WeaveFactoryStoreProviderMarker);
    read_file_from_proxy(&dir_proxy, "defg").await.unwrap_err();
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn read_factory_files_from_alpha_store() -> Result<(), Error> {
    let dir_proxy = connect_to_factory_store_provider!(AlphaFactoryStoreProviderMarker);
    assert_file(&dir_proxy, "alpha.txt", "an alpha file".as_bytes()).await?;
    assert_file(&dir_proxy, "alpha2.log", "and yet another alpha".as_bytes()).await?;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn missing_factory_files_from_alpha_store() -> Result<(), Error> {
    let dir_proxy = connect_to_factory_store_provider!(AlphaFactoryStoreProviderMarker);
    read_file_from_proxy(&dir_proxy, "missing_alpha.txt").await.unwrap_err();
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn read_factory_files_from_widevine_store() -> Result<(), Error> {
    let dir_proxy = connect_to_factory_store_provider!(WidevineFactoryStoreProviderMarker);
    assert_file(&dir_proxy, "widevine.txt", "a widevine file".as_bytes()).await?;
    assert_file(&dir_proxy, "widevine2.log", "and yet another wv".as_bytes()).await?;
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn missing_factory_files_from_widevine_store() -> Result<(), Error> {
    let dir_proxy = connect_to_factory_store_provider!(WidevineFactoryStoreProviderMarker);
    read_file_from_proxy(&dir_proxy, "defg").await.unwrap_err();
    Ok(())
}
