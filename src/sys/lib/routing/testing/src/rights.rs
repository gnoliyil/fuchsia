// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{CheckUse, ExpectedResult, RoutingTestModel, RoutingTestModelBuilder},
    cm_rust::*,
    cm_rust_testing::{ComponentDeclBuilder, DirectoryDeclBuilder},
    fidl_fuchsia_io as fio, fuchsia_zircon_status as zx_status,
    std::{convert::TryFrom, marker::PhantomData},
};

pub struct CommonRightsTest<T: RoutingTestModelBuilder> {
    builder: PhantomData<T>,
}

impl<T: RoutingTestModelBuilder> CommonRightsTest<T> {
    pub fn new() -> Self {
        Self { builder: PhantomData }
    }

    pub async fn test_offer_increasing_rights(&self) {
        let components = vec![
            (
                "a",
                ComponentDeclBuilder::new()
                    .offer(OfferDecl::Directory(OfferDirectoryDecl {
                        source: OfferSource::static_child("b".to_string()),
                        source_name: "bar_data".parse().unwrap(),
                        target_name: "baz_data".parse().unwrap(),
                        target: OfferTarget::static_child("c".to_string()),
                        rights: Some(fio::RW_STAR_DIR),
                        subdir: None,
                        dependency_type: DependencyType::Strong,
                        availability: Availability::Required,
                    }))
                    .add_lazy_child("b")
                    .add_lazy_child("c")
                    .build(),
            ),
            (
                "b",
                ComponentDeclBuilder::new()
                    .directory(
                        DirectoryDeclBuilder::new("foo_data").rights(fio::RW_STAR_DIR).build(),
                    )
                    .expose(ExposeDecl::Directory(ExposeDirectoryDecl {
                        source: ExposeSource::Self_,
                        source_name: "foo_data".parse().unwrap(),
                        target_name: "bar_data".parse().unwrap(),
                        target: ExposeTarget::Parent,
                        rights: Some(fio::RW_STAR_DIR),
                        subdir: None,
                        availability: cm_rust::Availability::Required,
                    }))
                    .build(),
            ),
            (
                "c",
                ComponentDeclBuilder::new()
                    .use_(UseDecl::Directory(UseDirectoryDecl {
                        source: UseSource::Parent,
                        source_name: "baz_data".parse().unwrap(),
                        target_path: CapabilityPath::try_from("/data/hippo").unwrap(),
                        rights: fio::R_STAR_DIR,
                        subdir: None,
                        dependency_type: DependencyType::Strong,
                        availability: Availability::Required,
                    }))
                    .build(),
            ),
        ];
        let model = T::new("a", components).build().await;
        model
            .check_use(
                vec!["c"].try_into().unwrap(),
                CheckUse::default_directory(ExpectedResult::Ok),
            )
            .await;
    }

    pub async fn test_offer_incompatible_rights(&self) {
        let components = vec![
            (
                "a",
                ComponentDeclBuilder::new()
                    .offer(OfferDecl::Directory(OfferDirectoryDecl {
                        source: OfferSource::static_child("b".to_string()),
                        source_name: "bar_data".parse().unwrap(),
                        target_name: "baz_data".parse().unwrap(),
                        target: OfferTarget::static_child("c".to_string()),
                        rights: Some(fio::W_STAR_DIR),
                        subdir: None,
                        dependency_type: DependencyType::Strong,
                        availability: Availability::Required,
                    }))
                    .add_lazy_child("b")
                    .add_lazy_child("c")
                    .build(),
            ),
            (
                "b",
                ComponentDeclBuilder::new()
                    .directory(
                        DirectoryDeclBuilder::new("foo_data").rights(fio::RW_STAR_DIR).build(),
                    )
                    .expose(ExposeDecl::Directory(ExposeDirectoryDecl {
                        source: ExposeSource::Self_,
                        source_name: "foo_data".parse().unwrap(),
                        target_name: "bar_data".parse().unwrap(),
                        target: ExposeTarget::Parent,
                        rights: Some(fio::RW_STAR_DIR),
                        subdir: None,
                        availability: cm_rust::Availability::Required,
                    }))
                    .build(),
            ),
            (
                "c",
                ComponentDeclBuilder::new()
                    .use_(UseDecl::Directory(UseDirectoryDecl {
                        source: UseSource::Parent,
                        source_name: "baz_data".parse().unwrap(),
                        target_path: CapabilityPath::try_from("/data/hippo").unwrap(),
                        rights: fio::R_STAR_DIR,
                        subdir: None,
                        dependency_type: DependencyType::Strong,
                        availability: Availability::Required,
                    }))
                    .build(),
            ),
        ];
        let model = T::new("a", components).build().await;
        model
            .check_use(
                vec!["c"].try_into().unwrap(),
                CheckUse::default_directory(ExpectedResult::Err(zx_status::Status::UNAVAILABLE)),
            )
            .await;
    }

    pub async fn test_expose_increasing_rights(&self) {
        let components = vec![
            (
                "a",
                ComponentDeclBuilder::new()
                    .offer(OfferDecl::Directory(OfferDirectoryDecl {
                        source: OfferSource::static_child("b".to_string()),
                        source_name: "bar_data".parse().unwrap(),
                        target_name: "baz_data".parse().unwrap(),
                        target: OfferTarget::static_child("c".to_string()),
                        rights: Some(fio::R_STAR_DIR),
                        subdir: None,
                        dependency_type: DependencyType::Strong,
                        availability: Availability::Required,
                    }))
                    .add_lazy_child("b")
                    .add_lazy_child("c")
                    .build(),
            ),
            (
                "b",
                ComponentDeclBuilder::new()
                    .directory(
                        DirectoryDeclBuilder::new("foo_data").rights(fio::RW_STAR_DIR).build(),
                    )
                    .expose(ExposeDecl::Directory(ExposeDirectoryDecl {
                        source: ExposeSource::Self_,
                        source_name: "foo_data".parse().unwrap(),
                        target_name: "bar_data".parse().unwrap(),
                        target: ExposeTarget::Parent,
                        rights: Some(fio::RW_STAR_DIR),
                        subdir: None,
                        availability: cm_rust::Availability::Required,
                    }))
                    .build(),
            ),
            (
                "c",
                ComponentDeclBuilder::new()
                    .use_(UseDecl::Directory(UseDirectoryDecl {
                        source: UseSource::Parent,
                        source_name: "baz_data".parse().unwrap(),
                        target_path: CapabilityPath::try_from("/data/hippo").unwrap(),
                        rights: fio::R_STAR_DIR,
                        subdir: None,
                        dependency_type: DependencyType::Strong,
                        availability: Availability::Required,
                    }))
                    .build(),
            ),
        ];
        let model = T::new("a", components).build().await;
        model
            .check_use(
                vec!["c"].try_into().unwrap(),
                CheckUse::default_directory(ExpectedResult::Ok),
            )
            .await;
    }

    pub async fn test_expose_incompatible_rights(&self) {
        let components = vec![
            (
                "a",
                ComponentDeclBuilder::new()
                    .offer(OfferDecl::Directory(OfferDirectoryDecl {
                        source: OfferSource::static_child("b".to_string()),
                        source_name: "bar_data".parse().unwrap(),
                        target_name: "baz_data".parse().unwrap(),
                        target: OfferTarget::static_child("c".to_string()),
                        rights: Some(fio::RW_STAR_DIR),
                        subdir: None,
                        dependency_type: DependencyType::Strong,
                        availability: Availability::Required,
                    }))
                    .add_lazy_child("b")
                    .add_lazy_child("c")
                    .build(),
            ),
            (
                "b",
                ComponentDeclBuilder::new()
                    .directory(
                        DirectoryDeclBuilder::new("foo_data").rights(fio::RW_STAR_DIR).build(),
                    )
                    .expose(ExposeDecl::Directory(ExposeDirectoryDecl {
                        source: ExposeSource::Self_,
                        source_name: "foo_data".parse().unwrap(),
                        target_name: "bar_data".parse().unwrap(),
                        target: ExposeTarget::Parent,
                        rights: Some(fio::W_STAR_DIR),
                        subdir: None,
                        availability: cm_rust::Availability::Required,
                    }))
                    .build(),
            ),
            (
                "c",
                ComponentDeclBuilder::new()
                    .use_(UseDecl::Directory(UseDirectoryDecl {
                        source: UseSource::Parent,
                        source_name: "baz_data".parse().unwrap(),
                        target_path: CapabilityPath::try_from("/data/hippo").unwrap(),
                        rights: fio::R_STAR_DIR,
                        subdir: None,
                        dependency_type: DependencyType::Strong,
                        availability: Availability::Required,
                    }))
                    .build(),
            ),
        ];
        let model = T::new("a", components).build().await;
        model
            .check_use(
                vec!["c"].try_into().unwrap(),
                CheckUse::default_directory(ExpectedResult::Err(zx_status::Status::UNAVAILABLE)),
            )
            .await;
    }

    pub async fn test_capability_increasing_rights(&self) {
        let components = vec![
            (
                "a",
                ComponentDeclBuilder::new()
                    .offer(OfferDecl::Directory(OfferDirectoryDecl {
                        source: OfferSource::static_child("b".to_string()),
                        source_name: "bar_data".parse().unwrap(),
                        target_name: "baz_data".parse().unwrap(),
                        target: OfferTarget::static_child("c".to_string()),
                        rights: Some(fio::R_STAR_DIR),
                        subdir: None,
                        dependency_type: DependencyType::Strong,
                        availability: Availability::Required,
                    }))
                    .add_lazy_child("b")
                    .add_lazy_child("c")
                    .build(),
            ),
            (
                "b",
                ComponentDeclBuilder::new()
                    .directory(
                        DirectoryDeclBuilder::new("foo_data").rights(fio::RW_STAR_DIR).build(),
                    )
                    .expose(ExposeDecl::Directory(ExposeDirectoryDecl {
                        source: ExposeSource::Self_,
                        source_name: "foo_data".parse().unwrap(),
                        target_name: "bar_data".parse().unwrap(),
                        target: ExposeTarget::Parent,
                        rights: Some(fio::R_STAR_DIR),
                        subdir: None,
                        availability: cm_rust::Availability::Required,
                    }))
                    .build(),
            ),
            (
                "c",
                ComponentDeclBuilder::new()
                    .use_(UseDecl::Directory(UseDirectoryDecl {
                        source: UseSource::Parent,
                        source_name: "baz_data".parse().unwrap(),
                        target_path: CapabilityPath::try_from("/data/hippo").unwrap(),
                        rights: fio::R_STAR_DIR,
                        subdir: None,
                        dependency_type: DependencyType::Strong,
                        availability: Availability::Required,
                    }))
                    .build(),
            ),
        ];
        let model = T::new("a", components).build().await;
        model
            .check_use(
                vec!["c"].try_into().unwrap(),
                CheckUse::default_directory(ExpectedResult::Ok),
            )
            .await;
    }

    pub async fn test_capability_incompatible_rights(&self) {
        let components = vec![
            (
                "a",
                ComponentDeclBuilder::new()
                    .offer(OfferDecl::Directory(OfferDirectoryDecl {
                        source: OfferSource::static_child("b".to_string()),
                        source_name: "bar_data".parse().unwrap(),
                        target_name: "baz_data".parse().unwrap(),
                        target: OfferTarget::static_child("c".to_string()),
                        rights: Some(fio::RW_STAR_DIR),
                        subdir: None,
                        dependency_type: DependencyType::Strong,
                        availability: Availability::Required,
                    }))
                    .add_lazy_child("b")
                    .add_lazy_child("c")
                    .build(),
            ),
            (
                "b",
                ComponentDeclBuilder::new()
                    .directory(
                        DirectoryDeclBuilder::new("foo_data").rights(fio::W_STAR_DIR).build(),
                    )
                    .expose(ExposeDecl::Directory(ExposeDirectoryDecl {
                        source: ExposeSource::Self_,
                        source_name: "foo_data".parse().unwrap(),
                        target_name: "bar_data".parse().unwrap(),
                        target: ExposeTarget::Parent,
                        rights: Some(fio::RW_STAR_DIR),
                        subdir: None,
                        availability: cm_rust::Availability::Required,
                    }))
                    .build(),
            ),
            (
                "c",
                ComponentDeclBuilder::new()
                    .use_(UseDecl::Directory(UseDirectoryDecl {
                        source: UseSource::Parent,
                        source_name: "baz_data".parse().unwrap(),
                        target_path: CapabilityPath::try_from("/data/hippo").unwrap(),
                        rights: fio::R_STAR_DIR,
                        subdir: None,
                        dependency_type: DependencyType::Strong,
                        availability: Availability::Required,
                    }))
                    .build(),
            ),
        ];
        let model = T::new("a", components).build().await;
        model
            .check_use(
                vec!["c"].try_into().unwrap(),
                CheckUse::default_directory(ExpectedResult::Err(zx_status::Status::UNAVAILABLE)),
            )
            .await;
    }

    ///  component manager's namespace
    ///   |
    ///   a
    ///    \
    ///     b
    ///
    /// a: offers directory /offer_from_cm_namespace/data/foo from realm as bar_data
    /// b: uses directory bar_data as /data/hippo, but the rights don't match
    pub async fn test_offer_from_component_manager_namespace_directory_incompatible_rights(&self) {
        let components = vec![
            (
                "a",
                ComponentDeclBuilder::new()
                    .offer(OfferDecl::Directory(OfferDirectoryDecl {
                        source: OfferSource::Parent,
                        source_name: "foo_data".parse().unwrap(),
                        target_name: "bar_data".parse().unwrap(),
                        target: OfferTarget::static_child("b".to_string()),
                        rights: None,
                        subdir: None,
                        dependency_type: DependencyType::Strong,
                        availability: Availability::Required,
                    }))
                    .add_lazy_child("b")
                    .build(),
            ),
            (
                "b",
                ComponentDeclBuilder::new()
                    .use_(UseDecl::Directory(UseDirectoryDecl {
                        source: UseSource::Parent,
                        source_name: "bar_data".parse().unwrap(),
                        target_path: CapabilityPath::try_from("/data/hippo").unwrap(),
                        rights: fio::R_STAR_DIR,
                        subdir: None,
                        dependency_type: DependencyType::Strong,
                        availability: Availability::Required,
                    }))
                    .build(),
            ),
        ];
        let namespace_capabilities = vec![CapabilityDecl::Directory(
            DirectoryDeclBuilder::new("foo_data")
                .path("/offer_from_cm_namespace/data/foo")
                .rights(fio::W_STAR_DIR)
                .build(),
        )];
        let mut builder = T::new("a", components);
        builder.set_namespace_capabilities(namespace_capabilities);
        let model = builder.build().await;

        model.install_namespace_directory("/offer_from_cm_namespace");
        model
            .check_use(
                vec!["b"].try_into().unwrap(),
                CheckUse::default_directory(ExpectedResult::Err(zx_status::Status::UNAVAILABLE)),
            )
            .await;
    }
}
