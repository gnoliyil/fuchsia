// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::error;
use std::fmt::Debug;
use std::fmt::Display;
use std::hash;
use std::io::Read;
use std::io::Seek;
use std::path::Path;
use std::path::PathBuf;

/// Instance of the scrutiny framework backed by a particular set of artifacts.
pub trait Scrutiny {
    /// Concrete type used for accessing blobs.
    type Blob: Blob;

    /// Concrete type used for accessing Fuchsa packages.
    type Package: Package;

    /// Concrete type used for resolving packages.
    type PackageResolver: PackageResolver;

    /// Concrete type used for modelling components.
    type Component: Component;

    /// Concrete type used for resolving components.
    type ComponentResolver: ComponentResolver;

    /// Concrete type used for modelling capabilities defined in component manifests.
    type ComponentCapability: ComponentCapability;

    /// Concrete type used for modelling data sources.
    type DataSource: DataSource;

    /// Concrete type used for modelling instances of components at particular locations in the
    /// component tree.
    type ComponentInstance: ComponentInstance;

    /// Concrete type used for modelling capabilities routed through a component instance.
    type ComponentInstanceCapability: ComponentInstanceCapability;

    /// Concrete type for high-level data about the Fuchsia system composition.
    type System: System<Blob = Self::Blob, Package = Self::Package>;

    /// Concrete type for the system's component manager configuration.
    type ComponentManager: ComponentManager;

    /// Accessor for high-level data about the Fuchsia system composition.
    fn system(&self) -> Self::System;

    /// Accessor for the system's component manager configuration.
    fn component_manager(&self) -> Self::ComponentManager;

    /// Accessor for high-level information about this [`Scrutiny`] instance's data sources.
    fn data_sources(&self) -> Box<dyn Iterator<Item = Self::DataSource>>;

    /// Iterate over all blobs from all system data sources.
    fn blobs(&self) -> Box<dyn Iterator<Item = Self::Blob>>;

    /// Iterate over all packages from all system data sources.
    fn packages(&self) -> Box<dyn Iterator<Item = Self::Package>>;

    /// Iterate over all package resolvers in the system.
    fn package_resolvers(&self) -> Box<dyn Iterator<Item = Self::PackageResolver>>;

    /// Iterate over all components in the system.
    fn components(&self) -> Box<dyn Iterator<Item = Self::Component>>;

    /// Iterate over all component resolvers in the system.
    fn component_resolvers(&self) -> Box<dyn Iterator<Item = Self::ComponentResolver>>;

    /// Iterate over all component's capabilities in the system.
    fn component_capabilities(&self) -> Box<dyn Iterator<Item = Self::ComponentCapability>>;

    /// Iterate over all component instances in the system. Note that a component instance is a
    /// component situated at a particular point in the system's component tree.
    fn component_instances(&self) -> Box<dyn Iterator<Item = Self::ComponentInstance>>;

    /// Iterate over all capabilities of component instances in the system.
    fn component_instance_capabilities(
        &self,
    ) -> Box<dyn Iterator<Item = Self::ComponentInstanceCapability>>;
}

/// High-level metadata about the system inspected by a [`Scrutiny`] instance.
pub trait System {
    /// Concrete type used for describing a path to a data source.
    type DataSourcePath: AsRef<Path>;

    /// Concrete type used for describing the system's Zircon Boot Image (ZBI).
    type Zbi: Zbi;

    /// Concrete type used for accessing blobs in the system.
    type Blob: Blob;

    /// Concrete type used for accessing Fuchsa packages.
    type Package: Package;

    /// Concrete type used for accessing kernel command-line flags.
    type KernelFlags: KernelFlags;

    /// Concrete type used for accessing the system's Verified Boot Metadata (vbmeta).
    type VbMeta: VbMeta;

    /// Concrete type for accessing the system's device configuration file.
    type DevMgrConfiguration: DevMgrConfiguration;

    /// Concrete type for the system's component manager configuration.
    type ComponentManagerConfiguration: ComponentManagerConfiguration;

    /// The build directory associated with the system build.
    fn build_dir(&self) -> Self::DataSourcePath;

    /// Accessor for the system's Zircon Boot Image (ZBI).
    fn zbi(&self) -> Self::Zbi;

    /// Accessor for the system's update package.
    fn update_package(&self) -> Self::Package;

    /// Accessor for the system's kernel command-line flags.
    fn kernel_flags(&self) -> Self::KernelFlags;

    /// Accessor for the system's Verified Boot Metadata (vbmeta).
    fn vb_meta(&self) -> Self::VbMeta;

    /// Accessor for the system's device configuration file.
    fn devmgr_configuration(&self) -> Self::DevMgrConfiguration;

    /// Accessor for the system's component manager configuration.
    fn component_manager_configuration(&self) -> Self::ComponentManagerConfiguration;
}

// TODO(fxbug.dev/112121): This is over-fitted to the "inspect bootfs" use case, and should probably be in terms of
// the various types of ZBI sections.

/// Model of the system's Zircon Boot Image (ZBI) used for Zircon kernel to userspace bootstrapping
/// (userboot). See https://fuchsia.dev/fuchsia-src/concepts/process/userboot for details.
pub trait Zbi {
    /// Concrete type used to refer to paths in the bootfs embedded in the ZBI.
    type BootfsPath: AsRef<Path>;

    /// Concrete type used for accessing blobs stored in blootfs.
    type Blob: Blob;

    /// Iterate over (path, contents) pairs of files in this ZBI's bootfs. See
    /// https://fuchsia.dev/fuchsia-src/concepts/process/userboot#bootfs for details.
    fn bootfs(&self) -> Box<dyn Iterator<Item = (Self::BootfsPath, Self::Blob)>>;
}

/// Kernel command-line flags. See https://fuchsia.dev/fuchsia-src/reference/kernel/kernel_cmdline
/// for details.
pub trait KernelFlags {
    /// Get the kernel command-line flag named `key`, or `None` if the flag does not exist in among
    /// this instance of kernel command-line flags.
    fn get(&self, key: &str) -> Option<&str>;

    /// Iterate over all kernel command-line flags specified on this instance.
    fn iter(&self) -> Box<dyn Iterator<Item = (String, String)>>;
}

// TODO(fxbug.dev/112121): What should this API look like?

/// Model of the Verified Boot Metadata (vbmeta).
pub trait VbMeta {}

/// Device manager configuration file key/value pairs. This configuration file is passed to the
/// device manager during early boot, and is combined with configuration set in [`KernelFlags`] and
/// [`VbMeta`] to determine various configuration parameters for booting the Fuchsia system on the
/// device.
pub trait DevMgrConfiguration {
    /// Get the value associated with `key`, or `None` if the key does not exist in in the
    /// underlying device configuration file.
    fn get(&self, key: &str) -> Option<&str>;

    /// Iterate over all key/value pairs specified on this instance.
    fn iter(&self) -> Box<dyn Iterator<Item = (String, String)>>;
}

/// Metadata about the component manager on a system.
pub trait ComponentManager {
    /// Concrete component manager configuration type.
    type ComponentManagerConfiguration: ComponentManagerConfiguration;

    /// Concrete type for capabilities bound to this component manager.
    type ComponentCapability: ComponentCapability;

    /// Accessor for this component manager's configuration.
    fn configuration(&self) -> Self::ComponentManagerConfiguration;

    /// Capabilities the system provides to the component manager.
    fn namespace_capabilities(&self) -> Box<dyn Iterator<Item = Self::ComponentCapability>>;

    /// Capabilities the component manager provides to all components that it manages.
    fn builtin_capabilities(&self) -> Box<dyn Iterator<Item = Self::ComponentCapability>>;
}

// TODO(fxbug.dev/112121): What should this API look like?

/// Model of the component manager configuration. For details about the role of component manager
/// in the system, see https://fuchsia.dev/fuchsia-src/concepts/components/v2/component_manager.
pub trait ComponentManagerConfiguration {}

/// Model of a data source that a [`Scrutiny`] instance is using as a source of truth about the
/// underlying system. This type is used for interrogating where a Fuchsia abstraction such as a
/// blob, package, or component came from (in terms of host filesystem artifacts). This is useful,
/// for example, in constructing error messages related to failed operations over a Fuchsia
/// abstraction.
pub trait DataSource: Debug {
    /// Concrete type used to refer to the path to this data source, if any.
    type SourcePath: AsRef<Path>;

    /// The kind of artifact that this data source represents.
    fn kind(&self) -> DataSourceKind;

    /// The parent data source in the case of nested data sources. For example, this may refer to an
    /// FVM volume that contains a blobfs archive.
    fn parent(&self) -> Option<Box<dyn DataSource<SourcePath = Self::SourcePath>>>;

    /// Children data sources in the case of nested data sources.
    fn children(
        &self,
    ) -> Box<dyn Iterator<Item = Box<dyn DataSource<SourcePath = Self::SourcePath>>>>;

    /// The local path to this data source. Generally only applicable to data sources that have no
    /// parent.
    fn path(&self) -> Option<Self::SourcePath>;

    /// The version of the underlying format of the data source.
    fn version(&self) -> DataSourceVersion;
}

impl<SourcePath: AsRef<Path>> PartialEq for dyn DataSource<SourcePath = SourcePath> {
    fn eq(&self, other: &dyn DataSource<SourcePath = SourcePath>) -> bool {
        // Cope with boxed type returned by `data_source.parent()` using anscestor variables.
        let mut a_anscestor: Option<Box<dyn DataSource<SourcePath = SourcePath>>> = None;
        let mut b_anscestor: Option<Box<dyn DataSource<SourcePath = SourcePath>>> = None;

        // Move `a` and `b` up to the root of the data source tree.
        loop {
            // Inspect either latest anscestor or initial values.
            let a: &dyn DataSource<SourcePath = SourcePath> = match a_anscestor.as_ref() {
                Some(a) => a.as_ref(),
                None => self,
            };
            let b: &dyn DataSource<SourcePath = SourcePath> = match b_anscestor.as_ref() {
                Some(b) => b.as_ref(),
                None => other,
            };

            // Move up anscestor tree, return early, or break at root of tree.
            match (a.parent(), b.parent()) {
                (Some(a_parent), Some(b_parent)) => {
                    a_anscestor = Some(a_parent);
                    b_anscestor = Some(b_parent);
                }
                (None, Some(_)) | (Some(_), None) => {
                    return false;
                }
                (None, None) => {
                    break;
                }
            }
        }

        // Directly (shallow) compare root before working entirely in terms of boxed descendants.
        let a = match a_anscestor.as_ref() {
            Some(a) => a.as_ref(),
            None => self,
        };
        let b = match b_anscestor.as_ref() {
            Some(b) => b.as_ref(),
            None => other,
        };
        if a.kind() != b.kind() || a.version() != b.version() {
            return false;
        }
        match (a.path(), b.path()) {
            (None, Some(_)) | (Some(_), None) => return false,
            (None, None) => {}
            (Some(a_path), Some(b_path)) => {
                if a_path.as_ref() != b_path.as_ref() {
                    return false;
                }
            }
        }

        // Compare a queues of boxed descendants of `a` and `b`.
        let mut a_descendants: Vec<_> = a.children().collect();
        let mut a_idx = 0;
        let mut b_descendants: Vec<_> = b.children().collect();
        let mut b_idx = 0;
        loop {
            // Break end-of-queues (i.e., no more descendants to compare).
            if a_idx >= a_descendants.len() {
                break;
            }
            if b_idx >= b_descendants.len() {
                break;
            }

            // Directly compare current descendants.
            let a = &a_descendants[a_idx];
            let b = &b_descendants[b_idx];
            if a.kind() != b.kind() || a.version() != b.version() {
                return false;
            }
            match (a.path(), b.path()) {
                (None, Some(_)) | (Some(_), None) => return false,
                (None, None) => {}
                (Some(a_path), Some(b_path)) => {
                    if a_path.as_ref() != b_path.as_ref() {
                        return false;
                    }
                }
            }

            // Add children of current descendant to queues.
            a_descendants.extend(a.children());
            b_descendants.extend(b.children());

            // Exit early if descendant counts don't match.
            if a_descendants.len() != b_descendants.len() {
                return false;
            }

            // Advance to next items in parallel queues.
            a_idx += 1;
            b_idx += 1;
        }

        true
    }
}

impl<SourcePath: AsRef<Path>> Eq for dyn DataSource<SourcePath = SourcePath> {}

/// Kinds of artifacts that may constitute a source of truth for a [`Scrutiny`] instance reasoning
/// about a built Fuchsia system.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum DataSourceKind {
    /// A product bundle directory that contains various artifacts at known paths within the
    /// directory.
    ProductBundle,
    /// A TUF repository containing metadata and blobs.
    TUFRepository,
    /// A blobfs archive (typically named "blob.blk" in Fuchsia builds). For details about blobfs
    /// itself, see https://fuchsia.dev/fuchsia-src/concepts/filesystems/blobfs.
    BlobfsArchive,
    /// A directory of blob files that are named after their Fuchsia merkle root hashes.
    BlobDirectory,
    /// An update package that designates a set of packages that constitute an over-the-air (OTA)
    /// system software update. See https://fuchsia.dev/fuchsia-src/concepts/packages/update_pkg
    /// for details.
    UpdatePackage,
    /// A Fuchsia Volume Manager (FVM) filesystem volume file. See
    /// https://fuchsia.dev/fuchsia-src/concepts/filesystems/filesystems#fvm for details.
    FvmVolume,
    /// A Zircon Boot Image (ZBI) file.
    Zbi,

    // TODO(fxbug.dev/112121): Are there other data sources to consume?
    /// An artifact that was passed to a [`Scrutiny`] instance, but either its kind was not
    /// recognized, or the artifact was not a well-formed instance of the kind passed in.
    Unknown,
}

// TODO(fxbug.dev/112121): What varieties of versioning do formats need?

/// A version identifier associated with an artifact or unit of software used to interpret
/// artifacts.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum DataSourceVersion {
    /// Either no version information is available, or the information was malformed.
    Unknown,
}

/// Boxed production [`DataSource`] type.
pub type BoxedDataSource = Box<dyn DataSource<SourcePath = PathBuf>>;

/// A content-addressed file.
pub trait Blob {
    /// Concrete type for the content-addressed hash used to identify this file. In most production
    /// cases, this is a Fuchsia merkle root; see
    /// https://fuchsia.dev/fuchsia-src/concepts/packages/merkleroot for details.
    type Hash: Hash;

    /// Concrete type for readable and seekable file content access API.
    type ReaderSeeker: Read + Seek;

    /// Concrete type for the data sources that provide this blob.
    type DataSource: DataSource;

    /// Concrete type for errors that may arise from attempting to open this blob for reading.
    type Error: error::Error;

    /// Accessor for the hash (i.e., content-addressed identity) of this file.
    fn hash(&self) -> Self::Hash;

    /// Gets a readable and seekable file content access API.
    ///
    /// # Panics
    ///
    /// Some blob sources may not support concurrent invocations of `Blob::reader_seeker`.
    fn reader_seeker(&self) -> Result<Self::ReaderSeeker, Self::Error>;

    /// Iterate over the data sources that provide this blob.
    fn data_sources(&self) -> Box<dyn Iterator<Item = Self::DataSource>>;
}

/// A content-address of a sequence of bytes. In most production cases, this is a Fuchsia merkle
/// root; see https://fuchsia.dev/fuchsia-src/concepts/packages/merkleroot for details.
pub trait Hash: Clone + Display + Debug + Eq + PartialEq + hash::Hash {}

impl<H: Clone + Display + Debug + Eq + PartialEq + hash::Hash> Hash for H {}

/// Model of a Fuchsia package. See https://fuchsia.dev/fuchsia-src/concepts/packages/package for
/// details.
pub trait Package {
    /// Concrete type for the content-addressed hash used to identify the package "meta.far" file.
    type Hash: Hash;

    /// Concrete type for accessing the package's embedded "meta/package" file.
    type MetaPackage: MetaPackage;

    /// Concrete type for accessing the package's embedded "meta/contents" file.
    type MetaContents: MetaContents;

    /// Concrete type for accessing the package's files' contents.
    type Blob: Blob;

    /// Concrete type for referring to "meta/" package paths.
    type PackagePath: AsRef<Path>;

    /// Concrete type for accessing components in the package.
    type Component: Component;

    /// Get the content-addressed hash of the package's "meta.far" file.
    fn hash(&self) -> Self::Hash;

    /// Gets the package's "meta/package" file.
    fn meta_package(&self) -> Self::MetaPackage;

    /// Gets the package's "meta/contents" file.
    fn meta_contents(&self) -> Self::MetaContents;

    /// Constructs iterator over blobs designated in the "meta/contents" of the package.
    fn content_blobs(&self) -> Box<dyn Iterator<Item = (Self::PackagePath, Self::Blob)>>;

    /// Constructs iterator over files in the package's "meta.far" file. This includes, but is not
    /// limited to "meta/package" and "meta/contents" which have their own structured type access
    /// APIs.
    fn meta_blobs(&self) -> Box<dyn Iterator<Item = (Self::PackagePath, Self::Blob)>>;

    /// Constructs iterator over blobs that appear to be component manifests.
    fn components(&self) -> Box<dyn Iterator<Item = (Self::PackagePath, Self::Component)>>;
}

// TODO(fxbug.dev/112121): Define API consistent with fuchsia_pkg::MetaPackage.

/// Model of a Fuchsia package's "meta/package" file. See
/// https://fuchsia.dev/fuchsia-src/concepts/packages/package#structure-of-a-package for details.
pub trait MetaPackage {}

// TODO(fxbug.dev/112121): Define API consistent with fuchsia_pkg::MetaContents.

/// Model of a Fuchsia package's "meta/contents" file. See
/// https://fuchsia.dev/fuchsia-src/concepts/packages/package#structure-of-a-package for details.
pub trait MetaContents {
    /// Concrete type for the content-addressed hash used to identify the blobs.
    type Hash: Hash;

    /// Concrete type used for describing a paths to content blobs.
    type EntryPath: AsRef<Path>;

    /// Returns an iterator over all path -> content hash mappings stored in this "meta/contents"
    /// file.
    fn contents(&self) -> Box<dyn Iterator<Item = (Self::EntryPath, Self::Hash)>>;
}

/// Model for a package resolution strategy. See
/// https://fuchsia.dev/fuchsia-src/get-started/learn/intro/packages#hosting_and_serving_packages
/// for details.
pub trait PackageResolver {
    /// Concrete type for the content-addressed hash used to identify packages.
    type Hash: Hash;

    /// Resolve a package URL to a content-addressed identity (hash).
    fn resolve(&self, url: PackageResolverUrl) -> Option<Self::Hash>;

    /// Iterate over the variety of package URLs that the resolver would resolve to the package
    /// identity given by `hash`.
    fn aliases(&self, hash: Self::Hash) -> Box<dyn Iterator<Item = PackageResolverUrl>>;
}

// TODO(fxbug.dev/112121): Define varieties of URL that PackageResolver supports.

/// The variety of URLs that [`PackageResolver`] can resolve to package hashes.
pub enum PackageResolverUrl {}

/// Model for a Fuchsia component. Note that this model is of a component as described by a
/// component manifest, not to be confused with a component _instance_, which is a component
/// situated at a particular point in a runtime component tree. See
/// https://fuchsia.dev/fuchsia-src/concepts/components/v2 for details.
pub trait Component {
    /// Concrete type for describing packages where the component resides.
    type Package: Package;

    /// Concrete type for describing capabilities associated with the component.
    type ComponentCapability: ComponentCapability;

    /// Concrete type for describing instances of the component in a constructed component tree.
    type ComponentInstance: ComponentInstance;

    /// Iterate over the known packages that contain the component.
    fn packages(&self) -> Box<dyn Iterator<Item = Self::Package>>;

    /// Iterate over known child component URLs.
    fn children(&self) -> Box<dyn Iterator<Item = PackageResolverUrl>>;

    /// Iterate over capability that the component uses.
    fn uses(&self) -> Box<dyn Iterator<Item = Self::ComponentCapability>>;

    /// Iterate over capabilities that the component exposes to its parent.
    fn exposes(&self) -> Box<dyn Iterator<Item = Self::ComponentCapability>>;

    /// Iterate over capabilities that the component offers to one or more of its children.
    fn offers(&self) -> Box<dyn Iterator<Item = Self::ComponentCapability>>;

    /// Iterate over capabilities defined by (i.e., originating from) the component.
    fn capabilities(&self) -> Box<dyn Iterator<Item = Self::ComponentCapability>>;

    /// Iterate over instances of the component that appear in the component tree known to the
    /// [`Scrutiny`] instance that underpins this component.
    fn instances(&self) -> Box<dyn Iterator<Item = Self::ComponentInstance>>;
}

/// Model for a component resolution strategy. See
/// https://fuchsia.dev/fuchsia-src/concepts/components/v2/capabilities/resolvers for details.
pub trait ComponentResolver {
    /// Concrete type for the content-addressed hash used to identify components.
    type Hash: Hash;

    /// Resolve a component URL to a content-addressed identity (hash).
    fn resolve(&self, url: ComponentResolverUrl) -> Option<Self::Hash>;

    /// Iterate over the variety of component URLs that the resolver would resolve to the package
    /// identity given by `hash`.
    fn aliases(&self, hash: Self::Hash) -> Box<dyn Iterator<Item = ComponentResolverUrl>>;
}

// TODO(fxbug.dev/112121): Define varieties of URL that ComponentResolver supports.

/// The variety of URLs that [`ComponentResolver`] can resolve to component hashes.
pub enum ComponentResolverUrl {}

/// A capability named in a particular component manifest. See
/// https://fuchsia.dev/fuchsia-src/concepts/components/v2/component_manifests and
/// https://fuchsia.dev/fuchsia-src/concepts/components/v2/capabilities for details.
pub trait ComponentCapability {
    /// Concrete type for the component that names the capability.
    type Component: Component;

    /// Concrete type for the capability name.
    type CapabilityName: ComponentCapabilityName;

    /// Concrete type for the capability path.
    type CapabilityPath: ComponentCapabilityPath;

    /// Accessor for component that names the capability.
    fn component(&self) -> Self::Component;

    /// Accessor for the kind of capability.
    fn kind(&self) -> CapabilityKind;

    /// Accessor for the component-local notion of the capability's source, such as the parent
    /// component or a particular child component. Note that this is different from a
    /// [`ComponentInstance`] notion of source, which refers to the component instance where the
    /// capability originates before being routed around the component tree.
    fn source(&self) -> CapabilitySource;

    /// Accessor for the component-local notion of the capability's destination, such as the parent
    /// component or a particular child component. Note that this is different from a
    /// [`ComponentInstance`] notion of source, which refers to the component instances to which
    /// the capability is routed to, and routed no further.
    fn destination(&self) -> CapabilityDestination;

    /// Accessor for the component-local source name for this capability, if any. Capabilities can
    /// be renamed as they are routed around in the component tree by designating different source
    /// and destination names.
    fn source_name(&self) -> Option<Self::CapabilityName>;

    /// Accessor for the component-local source name for this capability, if any. Capabilities can
    /// be renamed as they are routed around in the component tree by designating different source
    /// and destination names.
    fn destination_name(&self) -> Option<Self::CapabilityName>;

    /// Accessor for the component-local source path for this capability, if any. Capabilities can
    /// be mapped to different path locations as they are routed around the component tree by
    /// designating different source and destination paths.
    fn source_path(&self) -> Option<Self::CapabilityPath>;

    /// Accessor for the component-local destination path for this capability, if any. Capabilities
    /// can be mapped to different path locations as they are routed around the component tree by
    /// designating different source and destination paths.
    fn destination_path(&self) -> Option<Self::CapabilityPath>;
}

/// Various kinds of capabilities that a [`Scrutiny`] instance can reason about. See
/// https://fuchsia.dev/fuchsia-src/concepts/components/v2/capabilities#capability-types for
/// details.
pub enum CapabilityKind {
    // TODO(fxbug.dev/112121): Add kinds of capabilities based on cm capability types.
    /// The kind of capability denoted in the component manfiest is not recognized by the
    /// underlying [`Scrutiny`] instance.
    Unknown,
}

/// The component-local source from which a capability is routed. See
/// https://fuchsia.dev/fuchsia-src/concepts/components/v2/capabilities#routing for details.
pub enum CapabilitySource {
    // TODO(fxbug.dev/112121): Add capability sources based on cm capability types.
    /// The capability source denoted in the component manfiest is not recognized by the
    /// underlying [`Scrutiny`] instance.
    Unknown,
}

/// The component-local destination to which a capability is routed. See
/// https://fuchsia.dev/fuchsia-src/concepts/components/v2/capabilities#routing for details.
pub enum CapabilityDestination {
    // TODO(fxbug.dev/112121): Add capability destinations based on cm capability types.
    /// The capability destination denoted in the component manfiest is not recognized by the
    /// underlying [`Scrutiny`] instance.
    Unknown,
}

/// The name of a capability in the context of its component manifest.
pub trait ComponentCapabilityName {
    /// Concrete type of the capability with this name.
    type ComponentCapability: ComponentCapability;

    /// Accessor for the capability that uses this name.
    fn component(&self) -> Self::ComponentCapability;
}

/// The path associated with a capability in the context of its component manifest.
pub trait ComponentCapabilityPath {
    /// Concrete type of the capability with this path.
    type ComponentCapability: ComponentCapability;

    /// Accessor for the capability that uses this name.
    fn component(&self) -> Self::ComponentCapability;
}

/// Model of a component instance that appears at a particular location in a component tree. See
/// https://fuchsia.dev/fuchsia-src/concepts/components/v2/topology#component-instances for details.
pub trait ComponentInstance {
    /// Concrete type for the identity of the component instance location in the component tree.
    type Moniker: Moniker;

    /// Concrete type for the environment passed to the component instance in the context of the
    /// component tree.
    type Environment: Environment;

    /// Concrete type for the underlying component.
    type Component: Component;

    /// Concrete type for the capababilities that this component instance refers to.
    type ComponentInstanceCapability: ComponentInstanceCapability;

    /// Accessor for the component instance identity, or "moniker".
    fn moniker(&self) -> Self::Moniker;

    /// Accessor for the environment passed to the component instance in the context of the
    /// component tree.
    fn environment(&self) -> Self::Environment;

    /// Accessor for the underlying component.
    fn component(&self) -> Self::Component;

    /// Accessor for the parent component instance.
    fn parent(
        &self,
    ) -> Box<
        dyn ComponentInstance<
            Moniker = Self::Moniker,
            Environment = Self::Environment,
            Component = Self::Component,
            ComponentInstanceCapability = Self::ComponentInstanceCapability,
        >,
    >;

    /// Iterate over the children directly under this component in the component tree.
    fn children(
        &self,
    ) -> Box<
        dyn Iterator<
            Item = Box<
                dyn ComponentInstance<
                    Moniker = Self::Moniker,
                    Environment = Self::Environment,
                    Component = Self::Component,
                    ComponentInstanceCapability = Self::ComponentInstanceCapability,
                >,
            >,
        >,
    >;

    /// Iterate over the full set of descendants under this component in the component tree.
    fn descendants(
        &self,
    ) -> Box<
        dyn Iterator<
            Item = Box<
                dyn ComponentInstance<
                    Moniker = Self::Moniker,
                    Environment = Self::Environment,
                    Component = Self::Component,
                    ComponentInstanceCapability = Self::ComponentInstanceCapability,
                >,
            >,
        >,
    >;

    /// Iterate over the full set of ancestors above this component in the component tree.
    fn ancestors(
        &self,
    ) -> Box<
        dyn Iterator<
            Item = Box<
                dyn ComponentInstance<
                    Moniker = Self::Moniker,
                    Environment = Self::Environment,
                    Component = Self::Component,
                    ComponentInstanceCapability = Self::ComponentInstanceCapability,
                >,
            >,
        >,
    >;

    /// Iterate over capabilities that the component instance uses.
    fn uses(&self) -> Box<dyn Iterator<Item = Self::ComponentInstanceCapability>>;

    /// Iterate over capabilities that the component instance exposes to its parent.
    fn exposes(&self) -> Box<dyn Iterator<Item = Self::ComponentInstanceCapability>>;

    /// Iterate over capabilities that the component instance offers to one or more of its children.
    fn offers(&self) -> Box<dyn Iterator<Item = Self::ComponentInstanceCapability>>;

    /// Iterate over capabilities defined by (i.e., originating from) the component instance.
    fn capabilities(&self) -> Box<dyn Iterator<Item = Self::ComponentInstanceCapability>>;
}

// TODO(fxbug.dev/112121): Define API compatible with moniker::AbsoluteMoniker.

/// Model of a component instance moniker, the instance's identifier in the context of
/// the component tree constructed by the underlying [`Scrutiny`] instance. See
/// https://fuchsia.dev/fuchsia-src/concepts/components/v2/identifiers#monikers for details.
pub trait Moniker {}

// TODO(fxbug.dev/112121): Define API compatible with notion of bound environment in a component tree.

/// Model of a component instance environment that is realized in the context of the component
/// the component tree constructed by the underlying [`Scrutiny`] instance. See
/// https://fuchsia.dev/fuchsia-src/concepts/components/v2/environments for details.
pub trait Environment {}

/// A capability named by a component instance in the context of the component tree constructed
/// by the underlying [`Scrutiny`] instance. See
/// https://fuchsia.dev/fuchsia-src/concepts/components/v2/capabilities#routing for details.
pub trait ComponentInstanceCapability {
    /// Concrete type for the component capability for which this instance is a special case.
    type ComponentCapability: ComponentCapability;

    /// Concrete type for the component instance that names this capability.
    type ComponentInstance: ComponentInstance;

    /// Accessor for the component capability for which this instance is a special case.
    fn component_capability(&self) -> Self::ComponentCapability;

    /// Accessor for the component instance that names this capability.
    fn component_instance(&self) -> Self::ComponentInstance;

    /// The source where this capability originates.
    fn source(
        &self,
    ) -> Box<
        dyn ComponentInstanceCapability<
            ComponentCapability = Self::ComponentCapability,
            ComponentInstance = Self::ComponentInstance,
        >,
    >;

    /// Iterate over the component instance capabilities that constitute the capability route from
    /// its origin to this component instance capability (inclusive).
    fn source_path(
        &self,
    ) -> Box<
        dyn Iterator<
            Item = Box<
                dyn ComponentInstanceCapability<
                    ComponentCapability = Self::ComponentCapability,
                    ComponentInstance = Self::ComponentInstance,
                >,
            >,
        >,
    >;

    /// Iterate over the component instance capabilities that constitute all routes from this
    /// component instance capability to destinations that route no further (inclusive).
    fn destination_paths(
        &self,
    ) -> Box<
        dyn Iterator<
            Item = Box<
                dyn Iterator<
                    Item = Box<
                        dyn ComponentInstanceCapability<
                            ComponentCapability = Self::ComponentCapability,
                            ComponentInstance = Self::ComponentInstance,
                        >,
                    >,
                >,
            >,
        >,
    >;

    /// Iterate over the component instance capabilities that constitute all routes from this
    /// component instance capability's source to all destinations that route no further
    /// (inclusive).
    fn all_paths(
        &self,
    ) -> Box<
        dyn Iterator<
            Item = Box<
                dyn Iterator<
                    Item = Box<
                        dyn ComponentInstanceCapability<
                            ComponentCapability = Self::ComponentCapability,
                            ComponentInstance = Self::ComponentInstance,
                        >,
                    >,
                >,
            >,
        >,
    >;
}

#[cfg(test)]
mod tests {
    use super::DataSource;
    use super::DataSourceKind;
    use super::DataSourceVersion;
    use std::cell::RefCell;
    use std::fmt::Debug;
    use std::marker::PhantomData;
    use std::path::Path;
    use std::rc::Rc;
    use std::rc::Weak;

    trait MutableDataSource: DataSource
    where
        Self::SourcePath: AsRef<Path> + Debug,
    {
        fn add_child(&mut self, child: SharedDataSource<Self::SourcePath>);
    }

    #[derive(Clone, Debug)]
    struct SharedDataSource<SourcePath: AsRef<Path> + Debug>(
        Rc<RefCell<dyn MutableDataSource<SourcePath = SourcePath>>>,
        PhantomData<SourcePath>,
    );

    impl<SourcePath: AsRef<Path> + Debug> SharedDataSource<SourcePath> {
        fn new<MDS: MutableDataSource<SourcePath = SourcePath> + 'static>(
            data_source: MDS,
        ) -> Self {
            let data_source: Rc<RefCell<dyn MutableDataSource<SourcePath = SourcePath>>> =
                Rc::new(RefCell::new(data_source));
            Self(data_source, PhantomData)
        }

        fn from_raw(
            data_source: Rc<RefCell<dyn MutableDataSource<SourcePath = SourcePath>>>,
        ) -> Self {
            Self(data_source, PhantomData)
        }

        fn downgrade(self) -> ParentDataSource<SourcePath> {
            ParentDataSource::from_raw(Rc::downgrade(&self.0))
        }
    }

    impl<SourcePath: AsRef<Path> + Debug> DataSource for SharedDataSource<SourcePath> {
        type SourcePath = SourcePath;

        fn kind(&self) -> DataSourceKind {
            self.0.borrow().kind()
        }

        fn parent(&self) -> Option<Box<dyn DataSource<SourcePath = Self::SourcePath>>> {
            self.0.borrow().parent()
        }

        fn children(
            &self,
        ) -> Box<dyn Iterator<Item = Box<dyn DataSource<SourcePath = Self::SourcePath>>>> {
            self.0.borrow().children()
        }

        fn path(&self) -> Option<Self::SourcePath> {
            self.0.borrow().path()
        }

        fn version(&self) -> DataSourceVersion {
            self.0.borrow().version()
        }
    }

    impl<SourcePath: AsRef<Path> + Debug> MutableDataSource for SharedDataSource<SourcePath> {
        fn add_child(&mut self, child: SharedDataSource<Self::SourcePath>) {
            self.0.borrow_mut().add_child(child);
        }
    }

    #[derive(Clone, Debug)]
    struct ParentDataSource<SourcePath: AsRef<Path> + Debug>(
        Weak<RefCell<dyn MutableDataSource<SourcePath = SourcePath>>>,
        PhantomData<SourcePath>,
    );

    impl<SourcePath: AsRef<Path> + Debug> ParentDataSource<SourcePath> {
        fn from_raw(
            data_source: Weak<RefCell<dyn MutableDataSource<SourcePath = SourcePath>>>,
        ) -> Self {
            Self(data_source, PhantomData)
        }

        fn upgrade(self) -> Option<SharedDataSource<SourcePath>> {
            self.0.upgrade().map(SharedDataSource::from_raw)
        }
    }

    #[derive(Debug)]
    struct DataSourceRoot {
        kind: DataSourceKind,
        version: DataSourceVersion,
        path: Option<&'static str>,
        children: Vec<SharedDataSource<&'static str>>,
    }

    impl DataSourceRoot {
        fn new(
            kind: DataSourceKind,
            version: DataSourceVersion,
            path: Option<&'static str>,
        ) -> SharedDataSource<&'static str> {
            SharedDataSource::<&'static str>::new(Self { kind, version, path, children: vec![] })
        }
    }

    impl DataSource for DataSourceRoot {
        type SourcePath = &'static str;

        fn kind(&self) -> DataSourceKind {
            self.kind.clone()
        }

        fn parent(&self) -> Option<Box<dyn DataSource<SourcePath = Self::SourcePath>>> {
            None
        }

        fn children(
            &self,
        ) -> Box<dyn Iterator<Item = Box<dyn DataSource<SourcePath = Self::SourcePath>>>> {
            Box::new(self.children.clone().into_iter().map(|child| {
                let child: Box<dyn DataSource<SourcePath = Self::SourcePath>> = Box::new(child);
                child
            }))
        }

        fn path(&self) -> Option<Self::SourcePath> {
            self.path.clone()
        }

        fn version(&self) -> DataSourceVersion {
            self.version.clone()
        }
    }

    impl MutableDataSource for DataSourceRoot {
        fn add_child(&mut self, child: SharedDataSource<Self::SourcePath>) {
            self.children.push(child);
        }
    }

    #[derive(Debug)]
    struct DataSourceChild {
        parent: ParentDataSource<&'static str>,
        kind: DataSourceKind,
        version: DataSourceVersion,
        path: Option<&'static str>,
        children: Vec<SharedDataSource<&'static str>>,
    }

    impl DataSourceChild {
        fn new(
            mut parent: SharedDataSource<&'static str>,
            kind: DataSourceKind,
            version: DataSourceVersion,
            path: Option<&'static str>,
        ) -> SharedDataSource<&'static str> {
            let data_source_child =
                Self { parent: parent.clone().downgrade(), kind, version, path, children: vec![] };
            let shared_data_source = SharedDataSource::<&'static str>::new(data_source_child);
            parent.add_child(shared_data_source.clone());
            shared_data_source
        }
    }

    impl DataSource for DataSourceChild {
        type SourcePath = &'static str;

        fn kind(&self) -> DataSourceKind {
            self.kind.clone()
        }

        fn parent(&self) -> Option<Box<dyn DataSource<SourcePath = Self::SourcePath>>> {
            let parent =
                self.parent.clone().upgrade().expect("DataSourceChild's parent not dropped");
            let parent: Box<dyn DataSource<SourcePath = Self::SourcePath>> = Box::new(parent);
            Some(parent)
        }

        fn children(
            &self,
        ) -> Box<dyn Iterator<Item = Box<dyn DataSource<SourcePath = Self::SourcePath>>>> {
            Box::new(self.children.clone().into_iter().map(|child| {
                let child: Box<dyn DataSource<SourcePath = Self::SourcePath>> = Box::new(child);
                child
            }))
        }

        fn path(&self) -> Option<Self::SourcePath> {
            self.path.clone()
        }

        fn version(&self) -> DataSourceVersion {
            self.version.clone()
        }
    }

    impl MutableDataSource for DataSourceChild {
        fn add_child(&mut self, child: SharedDataSource<Self::SourcePath>) {
            self.children.push(child);
        }
    }

    #[fuchsia::test]
    fn test_data_source_eq() {
        fn expect(
            a: Box<dyn DataSource<SourcePath = &'static str>>,
            b: Box<dyn DataSource<SourcePath = &'static str>>,
            eq: bool,
        ) {
            if eq {
                assert_eq!(&a, &b);
            } else {
                assert_ne!(&a, &b);
            }

            let mut a_children = a.children();
            let mut b_children = b.children();
            loop {
                match (a_children.next(), b_children.next()) {
                    (Some(a_child), Some(b_child)) => expect(a_child, b_child, eq),
                    _ => break,
                }
            }
        }

        fn reference_tree() -> SharedDataSource<&'static str> {
            let root = DataSourceRoot::new(
                DataSourceKind::ProductBundle,
                DataSourceVersion::Unknown,
                Some("/pb"),
            );
            let child_1 = DataSourceChild::new(
                root.clone(),
                DataSourceKind::TUFRepository,
                DataSourceVersion::Unknown,
                None,
            );
            let _child_2 = DataSourceChild::new(
                root.clone(),
                DataSourceKind::FvmVolume,
                DataSourceVersion::Unknown,
                Some("/pb/fvm.blk"),
            );
            let _grandchild_1 = DataSourceChild::new(
                child_1.clone(),
                DataSourceKind::BlobDirectory,
                DataSourceVersion::Unknown,
                Some("/pb/test.fuchsia.com/blobs"),
            );
            root
        }

        fn diff_child2_kind_tree() -> SharedDataSource<&'static str> {
            let root = DataSourceRoot::new(
                DataSourceKind::ProductBundle,
                DataSourceVersion::Unknown,
                Some("/pb"),
            );
            let child_1 = DataSourceChild::new(
                root.clone(),
                DataSourceKind::TUFRepository,
                DataSourceVersion::Unknown,
                None,
            );
            let _child_2 = DataSourceChild::new(
                root.clone(),
                // Divergence: FVM misclassified as blobfs volume.
                DataSourceKind::BlobfsArchive,
                DataSourceVersion::Unknown,
                Some("/pb/fvm.blk"),
            );
            let _grandchild_1 = DataSourceChild::new(
                child_1.clone(),
                DataSourceKind::BlobDirectory,
                DataSourceVersion::Unknown,
                Some("/pb/test.fuchsia.com/blobs"),
            );
            root
        }

        fn diff_repo_blobs_path_tree() -> SharedDataSource<&'static str> {
            let root = DataSourceRoot::new(
                DataSourceKind::ProductBundle,
                DataSourceVersion::Unknown,
                Some("/pb"),
            );
            let child_1 = DataSourceChild::new(
                root.clone(),
                DataSourceKind::TUFRepository,
                DataSourceVersion::Unknown,
                None,
            );
            let _child_2 = DataSourceChild::new(
                root.clone(),
                DataSourceKind::FvmVolume,
                DataSourceVersion::Unknown,
                Some("/pb/fvm.blk"),
            );
            let _grandchild_1 = DataSourceChild::new(
                child_1.clone(),
                DataSourceKind::BlobDirectory,
                DataSourceVersion::Unknown,
                // Divergence: Different path to repository blobs.
                Some("/pb/test.fuchsia.com/test_blobs"),
            );
            root
        }

        fn diff_missing_grandchild_tree() -> SharedDataSource<&'static str> {
            let root = DataSourceRoot::new(
                DataSourceKind::ProductBundle,
                DataSourceVersion::Unknown,
                Some("/pb"),
            );
            let _child_1 = DataSourceChild::new(
                root.clone(),
                DataSourceKind::TUFRepository,
                DataSourceVersion::Unknown,
                None,
            );
            let _child_2 = DataSourceChild::new(
                root.clone(),
                DataSourceKind::FvmVolume,
                DataSourceVersion::Unknown,
                Some("/pb/fvm.blk"),
            );
            // Divergence: No grandchild under repository.
            root
        }

        fn diff_extra_root_tree() -> SharedDataSource<&'static str> {
            // Divergence: `reference_tree()` is a subtree under an additional root node.
            let extra_root =
                DataSourceRoot::new(DataSourceKind::Unknown, DataSourceVersion::Unknown, None);
            let root = DataSourceChild::new(
                extra_root.clone(),
                DataSourceKind::ProductBundle,
                DataSourceVersion::Unknown,
                Some("/pb"),
            );
            let child_1 = DataSourceChild::new(
                root.clone(),
                DataSourceKind::TUFRepository,
                DataSourceVersion::Unknown,
                None,
            );
            let _child_2 = DataSourceChild::new(
                root.clone(),
                DataSourceKind::FvmVolume,
                DataSourceVersion::Unknown,
                Some("/pb/fvm.blk"),
            );
            let _grandchild_1 = DataSourceChild::new(
                child_1.clone(),
                DataSourceKind::BlobDirectory,
                DataSourceVersion::Unknown,
                Some("/pb/test.fuchsia.com/blobs"),
            );
            extra_root
        }

        fn diff_extra_descendant_tree() -> SharedDataSource<&'static str> {
            let root = DataSourceRoot::new(
                DataSourceKind::ProductBundle,
                DataSourceVersion::Unknown,
                Some("/pb"),
            );
            let child_1 = DataSourceChild::new(
                root.clone(),
                DataSourceKind::TUFRepository,
                DataSourceVersion::Unknown,
                None,
            );
            let _child_2 = DataSourceChild::new(
                root.clone(),
                DataSourceKind::FvmVolume,
                DataSourceVersion::Unknown,
                Some("/pb/fvm.blk"),
            );
            let grandchild_1 = DataSourceChild::new(
                child_1.clone(),
                DataSourceKind::BlobDirectory,
                DataSourceVersion::Unknown,
                Some("/pb/test.fuchsia.com/blobs"),
            );
            // Divergence: `grandchild_1` has a child.
            let _great_grandchild_1 = DataSourceChild::new(
                grandchild_1.clone(),
                DataSourceKind::Unknown,
                DataSourceVersion::Unknown,
                None,
            );
            root
        }

        expect(Box::new(reference_tree()), Box::new(reference_tree()), true);
        expect(Box::new(reference_tree()), Box::new(diff_child2_kind_tree()), false);
        expect(Box::new(reference_tree()), Box::new(diff_repo_blobs_path_tree()), false);
        expect(Box::new(reference_tree()), Box::new(diff_missing_grandchild_tree()), false);
        expect(Box::new(reference_tree()), Box::new(diff_extra_descendant_tree()), false);

        // The subtree rooted in the first (and only) child of `diff_extra_root_tree` is identical
        // to `reference_tree`, but should not be treated as equal due to the additional parent
        // above.
        let reference: Box<dyn DataSource<SourcePath = &'static str>> = Box::new(reference_tree());
        let extra_root = diff_extra_root_tree();
        let almost_same_as_reference = extra_root.children().next().unwrap();

        // Sanity check: Shallowly compare `reference` and `almost_same_as_reference`.
        assert_eq!(reference.kind(), almost_same_as_reference.kind());
        assert_eq!(reference.version(), almost_same_as_reference.version());
        assert_eq!(reference.path().unwrap(), almost_same_as_reference.path().unwrap());

        expect(reference, almost_same_as_reference, false);
    }
}
