// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::realm::get_all_instances,
    anyhow::{bail, Result},
    fidl_fuchsia_sys2 as fsys,
    moniker::{AbsoluteMoniker, AbsoluteMonikerBase},
};

/// Retrieves a list of CML instance monikers that will match a given string query.
///
/// The string query can be a partial match on the following properties:
/// * component moniker
/// * component URL
/// * component instance ID
// TODO(https://fxbug.dev/114806): `ffx component show` should use this method to get
// component monikers when CMX support has been deprecated.
pub async fn get_cml_monikers_from_query(
    query: &str,
    realm_query: &fsys::RealmQueryProxy,
) -> Result<Vec<AbsoluteMoniker>> {
    // Special-case the root moniker since it will substring match every moniker
    // below.
    if let Ok(moniker) = AbsoluteMoniker::parse_str(&query) {
        if moniker.is_root() {
            return Ok(vec![moniker.clone()]);
        }
    }

    let instances = get_all_instances(realm_query).await?;

    // Try and find instances that contain the query in any of the identifiers
    // (moniker, URL, instance ID).
    let mut monikers: Vec<AbsoluteMoniker> = instances
        .into_iter()
        .filter(|i| {
            let url_match = i.url.contains(&query);
            let moniker_match = i.moniker.to_string().contains(&query);
            let id_match = i.instance_id.as_ref().map_or(false, |id| id.contains(&query));
            url_match || moniker_match || id_match
        })
        .map(|i| i.moniker)
        .collect();

    // For stability guarantees, sort the moniker list
    monikers.sort();

    // If the query is an exact-match of any of the results, return that
    // result only.
    if let Ok(moniker) = AbsoluteMoniker::parse_str(&query) {
        if monikers.contains(&moniker) {
            return Ok(vec![moniker]);
        }
    }

    Ok(monikers)
}

/// Retrieves exactly one CML instance moniker that will match a given string query.
///
/// The string query can be a partial match on the following properties:
/// * component moniker
/// * component URL
/// * component instance ID
///
/// If more than one instance matches the query, an error is thrown.
/// If no instance matches the query, an error is thrown.
pub async fn get_cml_moniker_from_query(
    query: &str,
    realm_query: &fsys::RealmQueryProxy,
) -> Result<AbsoluteMoniker> {
    // Get all instance monikers that match the query and ensure there is only one.
    let mut monikers = get_cml_monikers_from_query(&query, &realm_query).await?;
    if monikers.len() > 1 {
        let monikers: Vec<String> = monikers.into_iter().map(|m| m.to_string()).collect();
        let monikers = monikers.join("\n");
        bail!("The query {:?} matches more than one component instance:\n{}\n\nTo avoid ambiguity, use one of the above monikers instead.", query, monikers);
    }
    if monikers.is_empty() {
        bail!("No matching component instance found for query {:?}.", query);
    }
    let moniker = monikers.remove(0);
    Ok(moniker)
}

#[cfg(test)]
mod tests {
    use {super::*, crate::test_utils::serve_realm_query_instances, moniker::AbsoluteMonikerBase};

    fn setup_fake_realm_query() -> fsys::RealmQueryProxy {
        setup_fake_realm_query_with_entries(vec![
            ("/core/foo", "#meta/1bar.cm", "123456"),
            ("/core/boo", "#meta/2bar.cm", "456789"),
        ])
    }

    fn setup_fake_realm_query_with_entries(
        entries: Vec<(&str, &str, &str)>,
    ) -> fsys::RealmQueryProxy {
        let instances = entries
            .iter()
            .map(|(moniker, url, instance_id)| fsys::Instance {
                moniker: Some(moniker.to_string()),
                url: Some(url.to_string()),
                instance_id: Some(instance_id.to_string()),
                resolved_info: None,
                ..Default::default()
            })
            .collect::<Vec<_>>();
        serve_realm_query_instances(instances)
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_get_cml_monikers_from_query_exact_match_and_prefixes() {
        let realm_query = setup_fake_realm_query_with_entries(vec![
            ("/", "#meta/1.cm", "1"),
            ("/core", "#meta/2.cm", "2"),
            ("/core:one", "#meta/3.cm", "3"),
            ("/core:one/child", "#meta/4.cm", "4"),
        ]);

        assert_eq!(
            get_cml_monikers_from_query("/", &realm_query).await.unwrap(),
            vec![AbsoluteMoniker::parse_str("/").unwrap()]
        );

        assert_eq!(
            get_cml_monikers_from_query("/core", &realm_query).await.unwrap(),
            vec![AbsoluteMoniker::parse_str("/core").unwrap()]
        );

        assert_eq!(
            get_cml_monikers_from_query("/core:one", &realm_query).await.unwrap(),
            vec![AbsoluteMoniker::parse_str("/core:one").unwrap()]
        );

        assert_eq!(
            get_cml_monikers_from_query("/core:o", &realm_query).await.unwrap(),
            vec![
                AbsoluteMoniker::parse_str("/core:one").unwrap(),
                AbsoluteMoniker::parse_str("/core:one/child").unwrap(),
            ]
        );
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_get_cml_monikers_from_query_moniker_more_than_1() {
        let realm_query = setup_fake_realm_query();
        let results = get_cml_monikers_from_query("core", &realm_query).await.unwrap();
        assert_eq!(
            results,
            vec![
                AbsoluteMoniker::parse_str("/core/boo").unwrap(),
                AbsoluteMoniker::parse_str("/core/foo").unwrap()
            ]
        );
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_get_cml_monikers_from_query_moniker_exactly_1() {
        let realm_query = setup_fake_realm_query();
        let results = get_cml_monikers_from_query("foo", &realm_query).await.unwrap();
        assert_eq!(results, vec![AbsoluteMoniker::parse_str("/core/foo").unwrap()]);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_get_cml_monikers_from_query_url_more_than_1() {
        let realm_query = setup_fake_realm_query();
        let results = get_cml_monikers_from_query("bar.cm", &realm_query).await.unwrap();
        assert_eq!(
            results,
            vec![
                AbsoluteMoniker::parse_str("/core/boo").unwrap(),
                AbsoluteMoniker::parse_str("/core/foo").unwrap()
            ]
        );
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_get_cml_monikers_from_query_url_exactly_1() {
        let realm_query = setup_fake_realm_query();
        let results = get_cml_monikers_from_query("2bar.cm", &realm_query).await.unwrap();
        assert_eq!(results, vec![AbsoluteMoniker::parse_str("/core/boo").unwrap()]);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_get_cml_monikers_from_query_id_more_than_1() {
        let realm_query = setup_fake_realm_query();
        let results = get_cml_monikers_from_query("456", &realm_query).await.unwrap();
        assert_eq!(
            results,
            vec![
                AbsoluteMoniker::parse_str("/core/boo").unwrap(),
                AbsoluteMoniker::parse_str("/core/foo").unwrap()
            ]
        );
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_get_cml_monikers_from_query_id_exactly_1() {
        let realm_query = setup_fake_realm_query();
        let results = get_cml_monikers_from_query("123", &realm_query).await.unwrap();
        assert_eq!(results, vec![AbsoluteMoniker::parse_str("/core/foo").unwrap()]);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_get_cml_monikers_from_query_no_results() {
        let realm_query = setup_fake_realm_query();
        let results = get_cml_monikers_from_query("qwerty", &realm_query).await.unwrap();
        assert_eq!(results.len(), 0);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_get_cml_moniker_from_query_no_match() {
        let realm_query = setup_fake_realm_query();
        get_cml_moniker_from_query("qwerty", &realm_query).await.unwrap_err();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_get_cml_moniker_from_query_multiple_match() {
        let realm_query = setup_fake_realm_query();
        get_cml_moniker_from_query("bar.cm", &realm_query).await.unwrap_err();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_get_cml_moniker_from_query_moniker_single_match() {
        let realm_query = setup_fake_realm_query();
        let moniker = get_cml_moniker_from_query("foo", &realm_query).await.unwrap();
        assert_eq!(moniker, AbsoluteMoniker::parse_str("/core/foo").unwrap());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_get_cml_moniker_from_url_moniker_single_match() {
        let realm_query = setup_fake_realm_query();
        let moniker = get_cml_moniker_from_query("2bar.cm", &realm_query).await.unwrap();
        assert_eq!(moniker, AbsoluteMoniker::parse_str("/core/boo").unwrap());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_get_cml_moniker_from_url_id_single_match() {
        let realm_query = setup_fake_realm_query();
        let moniker = get_cml_moniker_from_query("123", &realm_query).await.unwrap();
        assert_eq!(moniker, AbsoluteMoniker::parse_str("/core/foo").unwrap());
    }
}
