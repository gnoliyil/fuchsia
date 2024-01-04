// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::blob::BlobOpenError;
use super::bootfs::AdditionalBootConfigurationError;
use super::bootfs::BootfsPackageError;
use super::bootfs::BootfsPackageIndexError;
use super::bootfs::ComponentManagerConfigurationError;
use cm_rust::CapabilityDecl;
use cm_rust::ComponentDecl;
use dyn_clone::clone_trait_object;
use dyn_clone::DynClone;
use fuchsia_url as furl;
use std::cmp;
use std::collections::HashMap;
use std::fmt;
use std::fmt::Debug;
use std::fmt::Display;
use std::hash;
use std::io::Read;
use std::io::Seek;
use std::path;
use thiserror::Error;

/// The type of readable, seekable objects returned by API interfaces.
pub trait ReaderSeeker: Read + Seek {}

impl<RS: Read + Seek> ReaderSeeker for RS {}

/// The type of paths on API interfaces.
pub trait Path: AsRef<path::Path> + DynClone {}

impl<P: AsRef<path::Path> + DynClone> Path for P {}

clone_trait_object!(Path);

impl Debug for dyn Path {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.as_ref().fmt(f)
    }
}

impl Display for dyn Path {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.as_ref().fmt(f)
    }
}

impl PartialEq for dyn Path {
    fn eq(&self, other: &dyn Path) -> bool {
        // Compare `dyn Path` instances as `std::path::Path` references.
        self.as_ref() == other.as_ref()
    }
}

impl Eq for dyn Path {}

impl hash::Hash for dyn Path {
    fn hash<H: hash::Hasher>(&self, state: &mut H) {
        self.as_ref().hash(state)
    }
}

/// Instance of the scrutiny framework backed by a particular set of artifacts.
pub trait Scrutiny {
    /// Accessor for high-level data about the Fuchsia system composition.
    fn system(&self) -> Box<dyn System>;

    /// Accessor for the system's component manager configuration.
    fn component_manager(&self) -> Box<dyn ComponentManager>;

    /// Accessor for high-level information about this [`Scrutiny`] instance's data sources.
    fn data_sources(&self) -> Box<dyn Iterator<Item = Box<dyn DataSource>>>;

    /// Iterate over all blobs from all system data sources.
    fn blobs(&self) -> Box<dyn Iterator<Item = Box<dyn Blob>>>;

    /// Iterate over all packages from all system data sources.
    fn packages(&self) -> Box<dyn Iterator<Item = Box<dyn Package>>>;

    /// Iterate over all component manifests in the system.
    fn component_manifests(&self) -> Box<dyn Iterator<Item = ComponentDecl>>;

    /// Iterate over all component instances in the system. Note that a component instance is a
    /// component situated at a particular point in the system's component tree.
    fn component_instances(&self) -> Box<dyn Iterator<Item = Box<dyn ComponentInstance>>>;
}

/// High-level metadata about the system inspected by a [`Scrutiny`] instance.
pub trait System: DynClone {
    /// The kind of Fuchsia system under inspection.
    fn variant(&self) -> SystemVariant;

    /// The build directory associated with the system build.
    fn build_dir(&self) -> Box<dyn Path>;

    /// Accessor for the system's Zircon Boot Image (ZBI).
    fn zbi(&self) -> Box<dyn Zbi>;

    /// Accessor for the system's update package.
    fn update_package(&self) -> Box<dyn UpdatePackage>;

    /// Accessor for the system's kernel command-line flags.
    fn kernel_flags(&self) -> Box<dyn KernelFlags>;

    /// Accessor for the system's Verified Boot Metadata (vbmeta).
    fn vb_meta(&self) -> Box<dyn VbMeta>;
}

clone_trait_object!(System);

/// The kind of Fuchsia system that should be inspected by a [`Scrutiny`] instance. The variant determines a
/// variety of strategies used to locate system artifacts. For example, the location of the Zircon Boot Image
/// (ZBI) in the Update Package is different for `Main` and `Recovery` systems.
#[derive(Clone)]
pub enum SystemVariant {
    /// The usual system layout that would be installed on the "A/B" device partitions.
    Main,
    /// The recovery system layout that would be installed on a "recovery" device partition.
    Recovery,
}

// TODO(https://fxbug.dev/112121): This is over-fitted to the "inspect bootfs" use case, and should probably be in terms of
// the various types of ZBI sections.

/// Model of the system's Zircon Boot Image (ZBI) used for Zircon kernel to userspace bootstrapping
/// (userboot). See https://fuchsia.dev/fuchsia-src/concepts/process/userboot for details.
pub trait Zbi: DynClone {
    /// Accessor for bootfs.
    fn bootfs(&self) -> Result<Box<dyn Bootfs>, ZbiError>;
}

clone_trait_object!(Zbi);

#[derive(Debug, Error)]
pub enum ZbiError {
    #[error("failed to hash blobfs blob at bootfs path {bootfs_path:?}: {error}")]
    Hash { bootfs_path: Box<dyn Path>, error: std::io::Error },
    #[error("expected to find exactly 1 bootfs section in zbi, but found {num_sections}")]
    BootfsSections { num_sections: usize },
    #[error("failed to parse bootfs image in zbi at path {path:?}: {error}")]
    ParseBootfs { path: Box<dyn Path>, error: anyhow::Error },
}

/// Model of the read-only boot filesystem, containing files needed in early boot.
/// The filesystem is a list of filenames together with, for each filename, the size and
/// offset of the file.
/// See https://fuchsia.dev/fuchsia-src/concepts/process/userboot#bootfs for details.
pub trait Bootfs: DynClone {
    /// Iterator over all contents of bootfs by path, with each file delivered as a `Blob`
    /// implementation.
    ///
    /// Some files are serialized instances of particular data types; for example,
    /// configuration files or packages. This iterator provides those files in raw format
    /// together with all other contents of bootfs, but other `Bootfs` trait methods may
    /// provide an object-oriented view of the same files (after parsing and validating
    /// the raw data). For example, the component manager configuration file is provided
    /// in raw form via `content_blobs()`, and as an implementation of the
    /// `ComponentManagerConfiguration` API via `compoennt_manager_configuration()`.
    fn content_blobs(&self) -> Box<dyn Iterator<Item = (Box<dyn Path>, Box<dyn Blob>)>>;

    /// Accessor for the system's additional boot configuration file.
    fn additional_boot_configuration(
        &self,
    ) -> Result<Box<dyn AdditionalBootConfiguration>, BootfsError>;

    /// Accessor for the system's component manager configuration.
    fn component_manager_configuration(
        &self,
    ) -> Result<Box<dyn ComponentManagerConfiguration>, BootfsError>;

    /// Iterate over all packages in bootfs.
    fn packages(&self) -> Result<Box<dyn Iterator<Item = Box<dyn Package>>>, BootfsError>;
}

clone_trait_object!(Bootfs);

#[derive(Debug, Error)]
pub enum BootfsError {
    #[error("failed to instantiate additional boot configuration: {0}")]
    AdditionalBootConfiguration(#[from] AdditionalBootConfigurationError),
    #[error("failed to instantiate component manager configuration: {0}")]
    ComponentManagerConfiguration(#[from] ComponentManagerConfigurationError),
    #[error("failed to read bootfs package index: {0}")]
    PackageIndex(#[from] BootfsPackageIndexError),
    #[error("failed to instantiate bootfs package: {0}")]
    Packages(#[from] BootfsPackageError),
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

// TODO(https://fxbug.dev/112121): What should this API look like?

/// Model of the Verified Boot Metadata (vbmeta).
pub trait VbMeta {}

/// Additional boot configuration file key/value pairs. This configuration file is passed to the
/// component manager, and is combined with configuration set in [`KernelFlags`] and
/// [`VbMeta`] to determine various configuration parameters for booting the Fuchsia system on the
/// device.
///
/// See https://fuchsia.dev/fuchsia-src/reference/kernel/kernel_cmdline for more details about
/// how the configuration is used, and see https://fuchsia.dev/fuchsia-src/gen/boot-options for
/// the set of valid options.
pub trait AdditionalBootConfiguration {
    /// Get the value associated with `key`, or `None` if the key does not exist in in the
    /// underlying device configuration file.
    fn get(&self, key: &str) -> Option<&str>;

    /// Iterate over all key/value pairs specified on this instance.
    fn iter(&self) -> Box<dyn Iterator<Item = (String, String)>>;
}

/// Metadata about the component manager on a system.
pub trait ComponentManager {
    /// Accessor for this component manager's configuration.
    fn configuration(&self) -> Box<dyn ComponentManagerConfiguration>;

    /// Capabilities the system provides to the component manager.
    fn namespace_capabilities(&self) -> Box<dyn Iterator<Item = CapabilityDecl>>;

    /// Capabilities the component manager provides to all components that it manages.
    fn builtin_capabilities(&self) -> Box<dyn Iterator<Item = CapabilityDecl>>;
}

// TODO(https://fxbug.dev/112121): What should this API look like? This is just a starting point to
// get something plumbed through from the data source.

/// Model of the component manager configuration. This configuration file controls various
/// aspects of component manager's execution, including capability routing policy and
/// logging behavior.
///
/// For details about the role of component manager
/// in the system, see https://fuchsia.dev/fuchsia-src/concepts/components/v2/component_manager.
///
/// See //sdk/fidl/fuchsia.component.internal/config.fidl for the (internal) FIDL format that
/// should back implementations of this trait.
pub trait ComponentManagerConfiguration {
    /// Whether Component Manager will run in debug mode.
    /// See //sdk/fidl/fuchsia.component.internal/config.fidl for details.
    fn debug(&self) -> bool;
}

/// Model of a data source that a [`Scrutiny`] instance is using as a source of truth about the
/// underlying system. This type is used for interrogating where a Fuchsia abstraction such as a
/// blob, package, or component came from (in terms of host filesystem artifacts). This is useful,
/// for example, in constructing error messages related to failed operations over a Fuchsia
/// abstraction.
pub trait DataSource: Debug + DynClone {
    /// The kind of artifact that this data source represents.
    fn kind(&self) -> DataSourceKind;

    /// The parent data source in the case of nested data sources. For example, this may refer to an
    /// FVM volume that contains a blobfs archive.
    fn parent(&self) -> Option<Box<dyn DataSource>>;

    /// Children data sources in the case of nested data sources.
    fn children(&self) -> Box<dyn Iterator<Item = Box<dyn DataSource>>>;

    /// The local path to this data source. Generally only applicable to data sources that have no
    /// parent.
    fn path(&self) -> Option<Box<dyn Path>>;

    /// The version of the underlying format of the data source.
    fn version(&self) -> DataSourceVersion;
}

clone_trait_object!(DataSource);

/// Computes equality of data exposed by two [`DataSource`] objects and their descendants.
fn descendants_eq(a: &dyn DataSource, b: &dyn DataSource) -> bool {
    if !data_source_shallow_eq(a, b) {
        return false;
    }

    let a_children: Vec<_> = a.children().collect();
    let b_children: Vec<_> = b.children().collect();
    if a_children.len() != b_children.len() {
        return false;
    }
    for (a_child, b_child) in a_children.into_iter().zip(b_children.into_iter()) {
        if !descendants_eq(a_child.as_ref(), b_child.as_ref()) {
            return false;
        }
    }

    true
}

/// Computes equality of [`DataSource`] ancestor objects of `a` and `b`, not including `a` and `b`
/// themselves.
fn ancestors_eq(a: &dyn DataSource, b: &dyn DataSource) -> bool {
    match (a.parent(), b.parent()) {
        (None, None) => true,
        (Some(_), None) | (None, Some(_)) => false,
        (Some(a_parent), Some(b_parent)) => {
            if !data_source_shallow_eq(a_parent.as_ref(), b_parent.as_ref()) {
                false
            } else {
                ancestors_eq(a_parent.as_ref(), b_parent.as_ref())
            }
        }
    }
}

/// Computes whether `a` and `b` appear at the same position among sibling [`DataSource`] objects in
/// their associated trees of data sources.
fn data_source_child_placement_eq(a: &dyn DataSource, b: &dyn DataSource) -> bool {
    match (a.parent(), b.parent()) {
        (None, None) => true,
        (Some(_), None) | (None, Some(_)) => false,
        (Some(a_parent), Some(b_parent)) => {
            let a_indices: Vec<_> = a_parent
                .children()
                .enumerate()
                .filter(|(_, child)| data_source_shallow_eq(a, child.as_ref()))
                .map(|(index, _)| index)
                .collect();
            if a_indices.len() != 1 {
                return false;
            }
            let b_indices: Vec<_> = b_parent
                .children()
                .enumerate()
                .filter(|(_, child)| data_source_shallow_eq(b, child.as_ref()))
                .map(|(index, _)| index)
                .collect();
            if b_indices.len() != 1 {
                return false;
            }

            a_indices[0] == b_indices[0]
        }
    }
}

/// Computes equality of all non-parent/child pointer data exposed by two [`DataSource`] objects.
fn data_source_shallow_eq(a: &dyn DataSource, b: &dyn DataSource) -> bool {
    if a.kind() != b.kind() || a.version() != b.version() {
        return false;
    }
    match (a.path(), b.path()) {
        (None, Some(_)) | (Some(_), None) => false,
        (None, None) => true,
        (Some(a_path), Some(b_path)) => {
            let a_ref: &std::path::Path = a_path.as_ref().as_ref();
            let b_ref: &std::path::Path = b_path.as_ref().as_ref();
            a_ref == b_ref
        }
    }
}

impl PartialEq for dyn DataSource {
    fn eq(&self, other: &dyn DataSource) -> bool {
        // To be deemed equal, `self` and `other` must define identical subtrees, have the same
        // ancestor data sources, and appear at the same position among their sibling data sources.
        descendants_eq(self, other)
            && ancestors_eq(self, other)
            && data_source_child_placement_eq(self, other)
    }
}

impl Eq for dyn DataSource {}

/// Kinds of artifacts that may constitute a source of truth for a [`Scrutiny`] instance reasoning
/// about a built Fuchsia system.
#[derive(Clone, Debug, Eq, Hash, PartialEq)]
pub enum DataSourceKind {
    /// A product bundle directory that contains various artifacts at known paths within the
    /// directory.
    ProductBundle,
    /// A TUF repository containing metadata and blobs.
    TufRepository,
    /// A blobfs archive (typically named "blob.blk" in Fuchsia builds). For details about blobfs
    /// itself, see https://fuchsia.dev/fuchsia-src/concepts/filesystems/blobfs.
    BlobfsArchive,
    /// A directory of blob files that are named after their Fuchsia merkle root hashes.
    BlobDirectory,
    /// A Fuchsia package. See https://fuchsia.dev/fuchsia-src/concepts/packages/package for
    /// details.
    Package,
    /// An update package that designates a set of packages that constitute an over-the-air (OTA)
    /// system software update. See https://fuchsia.dev/fuchsia-src/concepts/packages/update_pkg
    /// for details.
    UpdatePackage,
    /// A Fuchsia Volume Manager (FVM) filesystem volume file. See
    /// https://fuchsia.dev/fuchsia-src/concepts/filesystems/filesystems#fvm for details.
    FvmVolume,
    /// A Zircon Boot Image (ZBI) file.
    Zbi,

    /// Multiple kinds are associated with underlying data source(s).
    Multiple(Vec<DataSourceKind>),

    // TODO(https://fxbug.dev/112121): Are there other data sources to consume?
    /// An artifact that was passed to a [`Scrutiny`] instance, but either its kind was not
    /// recognized, or the artifact was not a well-formed instance of the kind passed in.
    Unknown,
}

// TODO(https://fxbug.dev/112121): What varieties of versioning do formats need?

/// A version identifier associated with an artifact or unit of software used to interpret
/// artifacts.
#[derive(Clone, Debug, Eq, Hash, PartialEq)]
pub enum DataSourceVersion {
    /// Either no version information is available, or the information was malformed.
    Unknown,
}

/// A content-addressed file.
pub trait Blob {
    /// Accessor for the hash (i.e., content-addressed identity) of this file.
    fn hash(&self) -> Box<dyn Hash>;

    /// Gets a readable and seekable file content access API.
    ///
    /// # Panics
    ///
    /// Some blob sources may not support concurrent invocations of `Blob::reader_seeker`.
    fn reader_seeker(&self) -> Result<Box<dyn ReaderSeeker>, BlobError>;

    /// Iterate over the data sources that provide this blob.
    fn data_sources(&self) -> Box<dyn Iterator<Item = Box<dyn DataSource>>>;
}

/// An error encountered executing [`Blob::reader_seeker`].
#[derive(Debug, Error)]
pub enum BlobError {
    #[error("error converting path to string: {path}, from source: {data_sources:?}")]
    Path { path: String, data_sources: Vec<Box<dyn DataSource>> },
    #[error("error reading from blob: {hash}, from directory: {directory}: {io_error_string}")]
    Io { hash: Box<dyn Hash>, directory: Box<dyn Path>, io_error_string: String },
    #[error("error opening blob: {error}")]
    // Note: Blob hash reported by `BlobOpenError`, and omitted here to avoid duplication.
    Open { error: BlobOpenError },
    #[error("error reading meta file {meta_file_path:?}={meta_file_hash} from data sources {data_sources:?}: {error}")]
    Far {
        meta_file_path: Box<dyn Path>,
        meta_file_hash: Box<dyn Hash>,
        data_sources: Vec<Box<dyn DataSource>>,
        error: fuchsia_archive::Error,
    },
}

/// A type that can be reduced to a byte sequence, such as a hash digest.
pub trait AsBytes: Display + Debug {
    fn as_bytes(&self) -> &[u8];
}

impl AsBytes for fuchsia_url::Hash {
    fn as_bytes(&self) -> &[u8] {
        self.as_bytes()
    }
}

impl PartialEq for dyn AsBytes {
    fn eq(&self, other: &dyn AsBytes) -> bool {
        self.as_bytes() == other.as_bytes()
    }
}

impl Eq for dyn AsBytes {}

impl dyn AsBytes {
    fn compare(&self, other: &dyn AsBytes) -> cmp::Ordering {
        self.as_bytes().cmp(other.as_bytes())
    }
}

impl PartialOrd for dyn AsBytes {
    fn partial_cmp(&self, other: &dyn AsBytes) -> Option<cmp::Ordering> {
        Some(self.compare(other))
    }
}

impl Ord for dyn AsBytes {
    fn cmp(&self, other: &dyn AsBytes) -> cmp::Ordering {
        self.compare(other)
    }
}

impl hash::Hash for dyn AsBytes {
    fn hash<H>(&self, state: &mut H)
    where
        H: hash::Hasher,
    {
        state.write(self.as_bytes())
    }
}

/// A content-address of a sequence of bytes. In most production cases, this is a Fuchsia merkle
/// root; see https://fuchsia.dev/fuchsia-src/concepts/packages/merkleroot for details.
pub trait Hash: AsBytes + DynClone + Send + Sync {}

impl<H: AsBytes + DynClone + Send + Sync> Hash for H {}

clone_trait_object!(Hash);

impl dyn Hash {
    fn compare(&self, other: &dyn Hash) -> cmp::Ordering {
        self.as_bytes().cmp(other.as_bytes())
    }
}

impl PartialOrd for dyn Hash {
    fn partial_cmp(&self, other: &Self) -> Option<cmp::Ordering> {
        Some(self.compare(other))
    }
}

impl Ord for dyn Hash {
    fn cmp(&self, other: &Self) -> cmp::Ordering {
        self.compare(other)
    }
}

impl PartialEq for dyn Hash {
    fn eq(&self, other: &dyn Hash) -> bool {
        self.as_bytes().eq(other.as_bytes())
    }
}

impl Eq for dyn Hash {}

impl hash::Hash for dyn Hash {
    fn hash<H>(&self, state: &mut H)
    where
        H: hash::Hasher,
    {
        state.write(self.as_bytes())
    }
}

/// Model of a Fuchsia package. See https://fuchsia.dev/fuchsia-src/concepts/packages/package for
/// details.
pub trait Package: DynClone {
    /// Get the content-addressed hash of the package's "meta.far" file.
    fn hash(&self) -> Box<dyn Hash>;

    /// Gets the package's "meta/package" file.
    fn meta_package(&self) -> Box<dyn MetaPackage>;

    /// Gets the package's "meta/contents" file.
    fn meta_contents(&self) -> Box<dyn MetaContents>;

    /// Constructs iterator over blobs designated in the "meta/contents" of the package.
    fn content_blobs(&self) -> Box<dyn Iterator<Item = (Box<dyn Path>, Box<dyn Blob>)>>;

    /// Constructs iterator over files in the package's "meta.far" file. This includes, but is not
    /// limited to "meta/package" and "meta/contents" which have their own structured
    /// APIs.
    fn meta_blobs(&self) -> Box<dyn Iterator<Item = (Box<dyn Path>, Box<dyn Blob>)>>;

    /// Constructs iterator over blobs that appear to be component manifests.
    fn component_manifests(
        &self,
    ) -> Result<Box<dyn Iterator<Item = (Box<dyn Path>, ComponentDecl)>>, PackageComponentsError>;
}

impl PartialEq for dyn Package {
    fn eq(&self, other: &Self) -> bool {
        self.hash() == other.hash()
    }
}

impl Eq for dyn Package {}

impl Debug for dyn Package {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("Package(")?;
        fmt::Debug::fmt(&self.hash(), f)?;
        f.write_str(")")?;
        Ok(())
    }
}

clone_trait_object!(Package);

#[derive(Debug, Error)]
pub enum PackageComponentsError {
    #[error("failed open blob for parsing as component: {0}")]
    Open(#[from] BlobError),
    #[error("failed read blob for parsing as component: {0}")]
    Read(#[from] std::io::Error),
}

// TODO(https://fxbug.dev/112121): Define API consistent with fuchsia_pkg::MetaPackage.

/// Model of a Fuchsia package's "meta/package" file. See
/// https://fuchsia.dev/fuchsia-src/concepts/packages/package#structure-of-a-package for details.
pub trait MetaPackage {
    fn name(&self) -> &fuchsia_url::PackageName;

    fn variant(&self) -> &fuchsia_url::PackageVariant;
}

impl PartialEq for dyn MetaPackage {
    fn eq(&self, other: &Self) -> bool {
        self.name() == other.name() && self.variant() == other.variant()
    }
}

impl Eq for dyn MetaPackage {}

// TODO(https://fxbug.dev/112121): Expose data related to fuchsia_pkg::MetaPackage.
impl Debug for dyn MetaPackage {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("MetaPackage")
    }
}

// TODO(https://fxbug.dev/112121): Define API consistent with fuchsia_pkg::MetaContents.

/// Model of a Fuchsia package's "meta/contents" file. See
/// https://fuchsia.dev/fuchsia-src/concepts/packages/package#structure-of-a-package for details.
pub trait MetaContents {
    /// Returns an iterator over all path -> content hash mappings stored in this "meta/contents"
    /// file.
    fn contents(&self) -> Box<dyn Iterator<Item = (Box<dyn Path>, Box<dyn Hash>)>>;
}

impl PartialEq for dyn MetaContents {
    fn eq(&self, other: &Self) -> bool {
        self.contents().collect::<HashMap<_, _>>() == other.contents().collect::<HashMap<_, _>>()
    }
}

impl Debug for dyn MetaContents {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for (path, hash) in self.contents() {
            f.write_fmt(format_args!("{:?} => {:?}\n", path, hash))?;
        }
        Ok(())
    }
}

pub trait UpdatePackage: Package {
    /// Returns a borrowed listing of package URLs described by this update package.
    fn packages(&self) -> &Vec<furl::PinnedAbsolutePackageUrl>;
}

clone_trait_object!(UpdatePackage);

/// Model for a package resolution strategy. See
/// https://fuchsia.dev/fuchsia-src/get-started/learn/intro/packages#hosting_and_serving_packages
/// for details.
pub trait PackageResolver {
    /// Resolve a package URL to a content-addressed identity (hash).
    fn resolve(&self, url: PackageResolverUrl) -> Option<Box<dyn Hash>>;

    /// Iterate over the variety of package URLs that the resolver would resolve to the package
    /// identity given by `hash`.
    fn aliases(&self, hash: Box<dyn Hash>) -> Box<dyn Iterator<Item = PackageResolverUrl>>;
}

/// The variety of URLs that [`PackageResolver`] can resolve to package hashes.
// TODO(https://fxbug.dev/112121): Define varieties of URL that PackageResolver supports, choose types
// for those URLs, and have each of these variants wrap a representation of a URL.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum PackageResolverUrl {
    // A URL identifying a package in bootfs.
    Boot(furl::boot_url::BootUrl),
    // A URL identifying a package in a repository.
    Package(furl::PackageUrl),
}

impl PackageResolverUrl {
    pub fn parse(url: &str) -> Result<Self, PackageResolverUrlParseError> {
        match furl::PackageUrl::parse(url) {
            Ok(package_url) => Ok(Self::Package(package_url)),
            Err(package_error) => match furl::boot_url::BootUrl::parse(url) {
                Ok(boot_url) => {
                    if boot_url.resource().is_some() {
                        Err(PackageResolverUrlParseError::BootWithResource)
                    } else {
                        Ok(Self::Boot(boot_url))
                    }
                }
                Err(boot_error) => {
                    Err(PackageResolverUrlParseError::InvalidFormat { package_error, boot_error })
                }
            },
        }
    }
}

#[derive(Debug, Error)]
pub enum PackageResolverUrlParseError {
    #[error("failed to parse as package url: {package_error}; failed to parse as boot url: {boot_error}")]
    InvalidFormat { package_error: furl::errors::ParseError, boot_error: furl::errors::ParseError },
    #[error("boot url that refers to package contains resource")]
    BootWithResource,
}

/// Model for a component resolution strategy. See
/// https://fuchsia.dev/fuchsia-src/concepts/components/v2/capabilities/resolvers for details.
pub trait ComponentResolver {
    /// Resolve a component URL to a content-addressed identity (hash).
    fn resolve(&self, url: ComponentResolverUrl) -> Option<Box<dyn Hash>>;

    /// Iterate over the variety of component URLs that the resolver would resolve to the package
    /// identity given by `hash`.
    fn aliases(&self, hash: Box<dyn Hash>) -> Box<dyn Iterator<Item = ComponentResolverUrl>>;
}

/// The variety of URLs that [`ComponentResolver`] can resolve to component hashes.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ComponentResolverUrl {
    Resource(ResourcePath),
    // A URL identifying a component file in bootfs.
    Boot(furl::boot_url::BootUrl),
    // A URL identifying a component in a package.
    Component(furl::ComponentUrl),
}

impl ComponentResolverUrl {
    pub fn parse(url: &str) -> Result<Self, ComponentResolverUrlParseError> {
        match furl::ComponentUrl::parse(url) {
            Ok(component_url) => Ok(Self::Component(component_url)),
            Err(component_error) => match furl::boot_url::BootUrl::parse(url) {
                Ok(boot_url) => {
                    if boot_url.resource().is_none() {
                        Err(ComponentResolverUrlParseError::BootWithoutResource)
                    } else {
                        Ok(Self::Boot(boot_url))
                    }
                }
                Err(boot_error) => {
                    let mut chars = url.chars();
                    if let Some(first_char) = chars.next() {
                        if first_char == '#' {
                            let resource_path_str = chars.as_str();
                            match furl::validate_resource_path(resource_path_str) {
                                Ok(()) => {
                                    Ok(Self::Resource(ResourcePath(resource_path_str.to_string())))
                                }
                                Err(resource_error) => {
                                    Err(ComponentResolverUrlParseError::ComponentBootAndResource {
                                        component_error,
                                        boot_error,
                                        resource_error,
                                    })
                                }
                            }
                        } else {
                            Err(ComponentResolverUrlParseError::ComponentAndBoot {
                                component_error,
                                boot_error,
                            })
                        }
                    } else {
                        Err(ComponentResolverUrlParseError::ComponentAndBoot {
                            component_error,
                            boot_error,
                        })
                    }
                }
            },
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ResourcePath(String);

#[derive(Debug, Error)]
pub enum ComponentResolverUrlParseError {
    #[error("failed to parse as component url: {component_error}; failed to parse as boot url: {boot_error}; url is not a resource-only URL")]
    ComponentAndBoot {
        component_error: furl::errors::ParseError,
        boot_error: furl::errors::ParseError,
    },
    #[error("failed to parse as component url: {component_error}; failed to parse as boot url: {boot_error}; failed to parse as resource path in resource-only URL: {resource_error}")]
    ComponentBootAndResource {
        component_error: furl::errors::ParseError,
        boot_error: furl::errors::ParseError,
        resource_error: furl::errors::ResourcePathError,
    },
    #[error("boot url that refers to component contains no resource")]
    BootWithoutResource,
}

/// Model of a component instance that appears at a particular location in a component tree. See
/// https://fuchsia.dev/fuchsia-src/concepts/components/v2/topology#component-instances for details.
pub trait ComponentInstance {
    /// Accessor for the component instance identity, or "moniker".
    fn moniker(&self) -> Box<dyn Moniker>;

    /// Accessor for the environment passed to the component instance in the context of the
    /// component tree.
    fn environment(&self) -> Box<dyn Environment>;

    /// Accessor for the underlying component.
    fn component_manifest(&self) -> ComponentDecl;

    /// Accessor for the parent component instance.
    fn parent(&self) -> Box<dyn ComponentInstance>;

    /// Iterate over the children directly under this component in the component tree.
    fn children(&self) -> Box<dyn Iterator<Item = Box<dyn ComponentInstance>>>;

    /// Iterate over the full set of descendants under this component in the component tree.
    fn descendants(&self) -> Box<dyn Iterator<Item = Box<dyn ComponentInstance>>>;

    /// Iterate over the full set of ancestors above this component in the component tree.
    fn ancestors(&self) -> Box<dyn Iterator<Item = Box<dyn ComponentInstance>>>;
}

// TODO(https://fxbug.dev/112121): Define API compatible with moniker::Moniker.

/// Model of a component instance moniker, the instance's identifier in the context of
/// the component tree constructed by the underlying [`Scrutiny`] instance. See
/// https://fuchsia.dev/fuchsia-src/concepts/components/v2/identifiers#monikers for details.
pub trait Moniker {}

// TODO(https://fxbug.dev/112121): Define API compatible with notion of bound environment in a component tree.

/// Model of a component instance environment that is realized in the context of the component
/// the component tree constructed by the underlying [`Scrutiny`] instance. See
/// https://fuchsia.dev/fuchsia-src/concepts/components/v2/environments for details.
pub trait Environment {}

#[cfg(test)]
mod tests {
    use super::super::data_source as ds;
    use super::ComponentResolverUrl;
    use super::DataSource;
    use super::DataSourceKind;
    use super::DataSourceVersion;
    use super::PackageResolverUrl;
    use super::Path;

    #[fuchsia::test]
    fn test_data_source_eq() {
        fn expect(a: Box<dyn DataSource>, b: Box<dyn DataSource>, eq: bool) {
            if eq {
                assert_eq!(&a, &b);
            } else {
                assert_ne!(&a, &b);
            }
        }

        fn new_data_source(
            kind: DataSourceKind,
            path: Option<Box<dyn Path>>,
            version: DataSourceVersion,
        ) -> ds::DataSource {
            ds::DataSource::new(ds::DataSourceInfo::new(kind, path, version))
        }

        fn reference_tree() -> ds::DataSource {
            let mut root = new_data_source(
                DataSourceKind::ProductBundle,
                Some(Box::new("/pb")),
                DataSourceVersion::Unknown,
            );

            let mut child_1 =
                new_data_source(DataSourceKind::TufRepository, None, DataSourceVersion::Unknown);
            root.add_child(child_1.clone());
            root.add_child(new_data_source(
                DataSourceKind::FvmVolume,
                Some(Box::new("/pb/fvm.blk")),
                DataSourceVersion::Unknown,
            ));
            child_1.add_child(new_data_source(
                DataSourceKind::BlobDirectory,
                Some(Box::new("/pb/test.fuchsia.com/blobs")),
                DataSourceVersion::Unknown,
            ));
            root
        }

        fn diff_child2_kind_tree() -> ds::DataSource {
            let mut root = new_data_source(
                DataSourceKind::ProductBundle,
                Some(Box::new("/pb")),
                DataSourceVersion::Unknown,
            );

            let mut child_1 =
                new_data_source(DataSourceKind::TufRepository, None, DataSourceVersion::Unknown);
            root.add_child(child_1.clone());
            root.add_child(new_data_source(
                // Divergence: FVM misclassified as blobfs volume.
                DataSourceKind::BlobfsArchive,
                Some(Box::new("/pb/fvm.blk")),
                DataSourceVersion::Unknown,
            ));
            child_1.add_child(new_data_source(
                DataSourceKind::BlobDirectory,
                Some(Box::new("/pb/test.fuchsia.com/blobs")),
                DataSourceVersion::Unknown,
            ));
            root
        }

        fn diff_repo_blobs_path_tree() -> ds::DataSource {
            let mut root = new_data_source(
                DataSourceKind::ProductBundle,
                Some(Box::new("/pb")),
                DataSourceVersion::Unknown,
            );

            let mut child_1 =
                new_data_source(DataSourceKind::TufRepository, None, DataSourceVersion::Unknown);
            root.add_child(child_1.clone());
            root.add_child(new_data_source(
                DataSourceKind::FvmVolume,
                Some(Box::new("/pb/fvm.blk")),
                DataSourceVersion::Unknown,
            ));
            child_1.add_child(new_data_source(
                DataSourceKind::BlobDirectory,
                // Divergence: Different path to repository blobs.
                Some(Box::new("/pb/test.fuchsia.com/test_blobs")),
                DataSourceVersion::Unknown,
            ));
            root
        }

        fn diff_missing_grandchild_tree() -> ds::DataSource {
            let mut root = new_data_source(
                DataSourceKind::ProductBundle,
                Some(Box::new("/pb")),
                DataSourceVersion::Unknown,
            );

            let child_1 =
                new_data_source(DataSourceKind::TufRepository, None, DataSourceVersion::Unknown);
            root.add_child(child_1.clone());
            root.add_child(new_data_source(
                DataSourceKind::FvmVolume,
                Some(Box::new("/pb/fvm.blk")),
                DataSourceVersion::Unknown,
            ));
            // Divergence: No grandchild under repository.
            root
        }

        fn diff_extra_root_tree() -> ds::DataSource {
            // Divergence: `reference_tree()` is a subtree under an additional root node.
            let mut extra_root =
                new_data_source(DataSourceKind::Unknown, None, DataSourceVersion::Unknown);
            let mut root = new_data_source(
                DataSourceKind::ProductBundle,
                Some(Box::new("/pb")),
                DataSourceVersion::Unknown,
            );
            extra_root.add_child(root.clone());

            let mut child_1 =
                new_data_source(DataSourceKind::TufRepository, None, DataSourceVersion::Unknown);
            root.add_child(child_1.clone());
            root.add_child(new_data_source(
                DataSourceKind::FvmVolume,
                Some(Box::new("/pb/fvm.blk")),
                DataSourceVersion::Unknown,
            ));
            child_1.add_child(new_data_source(
                DataSourceKind::BlobDirectory,
                Some(Box::new("/pb/test.fuchsia.com/blobs")),
                DataSourceVersion::Unknown,
            ));
            extra_root
        }

        fn diff_extra_descendant_tree() -> ds::DataSource {
            let mut root = new_data_source(
                DataSourceKind::ProductBundle,
                Some(Box::new("/pb")),
                DataSourceVersion::Unknown,
            );

            let mut child_1 =
                new_data_source(DataSourceKind::TufRepository, None, DataSourceVersion::Unknown);
            root.add_child(child_1.clone());
            root.add_child(new_data_source(
                DataSourceKind::FvmVolume,
                Some(Box::new("/pb/fvm.blk")),
                DataSourceVersion::Unknown,
            ));
            let mut grandchild_1 = new_data_source(
                DataSourceKind::BlobDirectory,
                Some(Box::new("/pb/test.fuchsia.com/blobs")),
                DataSourceVersion::Unknown,
            );
            child_1.add_child(grandchild_1.clone());
            // Divergence: `grandchild_1` has a child.
            grandchild_1.add_child(new_data_source(
                DataSourceKind::Unknown,
                None,
                DataSourceVersion::Unknown,
            ));
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
        let reference: Box<dyn DataSource> = Box::new(reference_tree());
        let extra_root = diff_extra_root_tree();
        let almost_same_as_reference = extra_root.children().next().expect("extra root has child");

        // Check that all node attributes are equal, but that the extra ancestor causes a deep
        // equality check to fail.
        assert_eq!(reference.kind(), almost_same_as_reference.kind());
        assert_eq!(reference.version(), almost_same_as_reference.version());
        assert_eq!(
            &reference.path().expect("root path"),
            &almost_same_as_reference.path().expect("extra root child path")
        );
        expect(reference, almost_same_as_reference, false);
    }

    #[fuchsia::test]
    fn test_parse_package_url() {
        PackageResolverUrl::parse("fuchsia-pkg://test.fuchsia.com/some_pkg")
            .expect("fully qualified package URL");
        PackageResolverUrl::parse("/some_pkg").expect("relative package URL");
        PackageResolverUrl::parse("fuchsia-boot:///some/pkg").expect("boot URL without resource");
        PackageResolverUrl::parse("fuchsia-pkg://test.fuchsia.com/some_pkg#meta/resource")
            .expect_err("package URL with resource");
        PackageResolverUrl::parse("fuchsia-boot:///some/pkg#meta/resource")
            .expect_err("boot URL with resource");
        PackageResolverUrl::parse("#meta/resource").expect_err("resource-only URL");
    }

    #[fuchsia::test]
    fn test_parse_component_url() {
        ComponentResolverUrl::parse("fuchsia-pkg://test.fuchsia.com/some_pkg")
            .expect_err("fully qualified package URL");
        ComponentResolverUrl::parse("/some_pkg").expect_err("relative package URL");
        ComponentResolverUrl::parse("fuchsia-boot:///some/pkg")
            .expect_err("boot URL without resource");
        ComponentResolverUrl::parse("fuchsia-pkg://test.fuchsia.com/some_pkg#meta/resource")
            .expect("package URL with resource");
        ComponentResolverUrl::parse("fuchsia-boot:///some/pkg#meta/resource")
            .expect("boot URL with resource");
        ComponentResolverUrl::parse("#meta/resource").expect("resource-only URL");
    }
}
