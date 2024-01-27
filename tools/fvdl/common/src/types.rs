// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::args::StartCommand;
use crate::graphic_utils::get_default_graphics;
use crate::images::Images;
use crate::tools::Tools;
use anyhow::{anyhow, Context, Result};
use errors::ffx_bail;
use ffx_config::sdk::{Sdk, SdkVersion};
use home::home_dir;
use mockall::automock;
use std::convert::From;
use std::env;
use std::fmt;
use std::fs::{create_dir, read_dir, File};
use std::io::{BufRead, BufReader};
use std::os::unix;
use std::path::PathBuf;

pub fn read_env_path(var: &str) -> Result<PathBuf> {
    env::var_os(var)
        .map(PathBuf::from)
        .ok_or(anyhow!("{} is not a valid environment variable", var))
}

#[automock]
pub trait FuchsiaPaths {
    fn find_fuchsia_root(&mut self) -> Result<PathBuf>;
    fn find_fuchsia_build_dir(&mut self) -> Result<PathBuf>;
    fn get_tool_path(&mut self, name: &str) -> Result<PathBuf>;
    fn get_image_path<'a>(&mut self, names: Vec<&'a str>, image_type: &str) -> Result<PathBuf>;
}

pub struct InTreePaths {
    pub root_dir: Option<PathBuf>,
    pub build_dir: Option<PathBuf>,
}

impl FuchsiaPaths for InTreePaths {
    /// Walks the current execution path and its parent directories to find the path
    /// that contains .jiri_root directory.
    fn find_fuchsia_root(&mut self) -> Result<PathBuf> {
        if self.root_dir.is_none() {
            for ancester in std::env::current_exe()?.ancestors() {
                if let Ok(entries) = read_dir(ancester) {
                    for entry in entries {
                        if let Ok(entry) = entry {
                            if entry.path().ends_with(".jiri_root") {
                                self.root_dir.replace(ancester.to_path_buf());
                                println!(
                                    "[fvdl] Found Fuchsia root directory {:?}",
                                    self.root_dir.as_ref().unwrap().display()
                                );
                                return Ok(ancester.to_path_buf());
                            }
                        }
                    }
                }
            }
        }
        self.root_dir
            .as_ref()
            .map(|c| c.clone())
            .ok_or(anyhow!("Cannot locate Fuchsia binaries.\n\
            SDK users, make sure you include the --sdk flag and run the program from inside the Fuchsia SDK directory.\n\
            Non-SDK users, make sure you're running the program from inside the Fuchsia source directory tree."))
    }

    fn find_fuchsia_build_dir(&mut self) -> Result<PathBuf> {
        if self.build_dir.is_none() {
            match read_env_path("FUCHSIA_BUILD_DIR") {
                Ok(val) => self.build_dir.replace(val),
                _ => {
                    let root = self.find_fuchsia_root()?;
                    let build_dir = File::open(root.join(".fx-build-dir"))?;
                    let build_dir_file = BufReader::new(build_dir);
                    let dir = build_dir_file
                        .lines()
                        .nth(0)
                        .ok_or(anyhow!("cannot read file {:?}", root.join(".fx-build-dir")))?;
                    self.build_dir.replace(root.join(dir?))
                }
            };
        }
        self.build_dir
            .as_ref()
            .map(|c| c.clone())
            .ok_or(anyhow!("Cannot read path info from build_dir {:?}", self.build_dir))
    }

    fn get_tool_path(&mut self, name: &str) -> Result<PathBuf> {
        let build_dir = self.find_fuchsia_build_dir()?;
        let tools = Tools::from_build_dir(build_dir.clone())?;
        tools.find_path(name).map(|p| build_dir.join(p))
    }

    fn get_image_path<'a>(&mut self, names: Vec<&'a str>, image_type: &str) -> Result<PathBuf> {
        let build_dir = self.find_fuchsia_build_dir()?;
        let images = Images::from_build_dir(build_dir.clone())?;
        images.find_path(names, image_type).map(|p| build_dir.join(p))
    }
}

/// Returns GN SDK tools directory. This assumes that fvdl is located in
/// <sdk_root>/tools/[x64|arm64]/fvdl, and will return path to <sdk_root>/tools/[x64|arm64]
pub fn get_fuchsia_sdk_tools_dir() -> Result<PathBuf> {
    Ok(std::env::current_exe()?
        .parent()
        .ok_or(anyhow!("Cannot get parent path to 'fvdl'."))?
        .to_path_buf())
}

/// Returns GN SDK tools directory. This assumes that fvdl is located in
/// <sdk_root>/tools/[x64|arm64]/fvdl, and will return path to <sdk_root>
pub fn get_fuchsia_sdk_dir() -> Result<PathBuf> {
    Ok(get_fuchsia_sdk_tools_dir()? // ex: <sdk_root>/tools/x64/
        .parent() // ex: <sdk_root>/tools/
        .ok_or(anyhow!("Cannot get parent path to 'tools' directory."))?
        .parent() // ex: <sdk_root>
        .ok_or(anyhow!("Cannot get path to sdk root."))?
        .to_path_buf())
}

/// Returns either the path specified in the environment variable FUCHSIA_SDK_DATA_DIR or
/// $HOME/.fuchsia
pub fn get_sdk_data_dir() -> Result<PathBuf> {
    let sdk_data_dir = match read_env_path("FUCHSIA_SDK_DATA_DIR") {
        Ok(dir) => dir,
        _ => {
            let default = home_dir().unwrap_or_default().join(".fuchsia");
            if !default.exists() {
                create_dir(&default)?;
            }
            default
        }
    };
    Ok(sdk_data_dir)
}

/// Reads sdk version from manifest.json.
/// This method assumes that user is invoking the binary from GN SDK, not in fuchsia repo.
/// TODO(fxb/69689) Use ffx::config to obtain host_tools location.
pub fn get_sdk_version_from_manifest() -> Result<String> {
    let sdk = Sdk::from_sdk_dir(&get_fuchsia_sdk_dir()?)?;
    match sdk.get_version() {
        SdkVersion::Version(v) => Ok(v.to_string()),
        SdkVersion::InTree => ffx_bail!("This should only be used in SDK"),
        SdkVersion::Unknown => ffx_bail!("Cannot determine SDK version"),
    }
}

#[derive(Clone)]
pub struct ImageFiles {
    pub amber_files: Option<PathBuf>,
    pub build_args: Option<PathBuf>,
    pub disk_image: Option<PathBuf>,
    pub kernel: PathBuf,
    pub zbi: PathBuf,
}

impl fmt::Debug for ImageFiles {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f, "[fvdl] image package {:?}", self.amber_files)?;
        writeln!(f, "[fvdl] image build_args {:?}", self.build_args)?;
        writeln!(f, "[fvdl] image disk_image {:?}", self.disk_image)?;
        writeln!(f, "[fvdl] image kernel {:?}", self.kernel)?;
        write!(f, "[fvdl] image zbi {:?}", self.zbi)
    }
}

impl ImageFiles {
    /// Initialize fuchsia image and package files for in-tree usage.
    ///
    /// First checks for environment variable FUCHSIA_BUILD_DIR. If not specify looks into
    /// <repo_root>/.fx-build-dir. For example ~/fuchsia/.fx-build-dir
    pub fn from_tree_env(f: &mut impl FuchsiaPaths) -> Result<Self> {
        let fuchsia_build_dir = f.find_fuchsia_build_dir()?;
        println!("[fvdl] Using fuchsia build dir: {:?}", fuchsia_build_dir.display());

        Ok(Self {
            amber_files: {
                let f = fuchsia_build_dir.join("amber-files");
                if f.exists() {
                    Some(f)
                } else {
                    None
                }
            },
            build_args: f.get_image_path(vec!["buildargs"], "gn").ok(),
            disk_image: f
                .get_image_path(vec!["storage-full", "storage-sparse", "fvm.fastboot"], "blk")
                .or(f.get_image_path(vec!["storage-full"], "fxfs-blk"))
                .ok(),
            kernel: f.get_image_path(vec!["qemu-kernel"], "kernel")?,
            zbi: f.get_image_path(vec!["zircon-a"], "zbi")?,
        })
    }

    /// When running from SDK (running with --sdk), we will either fetch images from GCS or use cached-image files.
    /// If fetching from GCS, these image files will be ignored by device_launcher.
    /// If using cached images, call update_paths_from_cache() to populate the image file paths.
    pub fn from_sdk_env() -> Result<Self> {
        Ok(Self {
            amber_files: None,
            build_args: None,
            disk_image: None,
            kernel: PathBuf::new(),
            zbi: PathBuf::new(),
        })
    }

    /// Checks that all essential files exist. Note: amber_files, build_args, and disk_image are optional.
    pub fn check(&self) -> Result<()> {
        if let Some(a) = &self.amber_files {
            if !a.exists() {
                ffx_bail!("amber-files at {:?} does not exist", a);
            }
        }
        if let Some(b) = &self.build_args {
            if !b.exists() {
                ffx_bail!("build_args file at {:?} does not exist", b);
            }
        }
        if let Some(f) = &self.disk_image {
            if !f.exists() {
                ffx_bail!("disk_image file at {:?} does not exist", f);
            }
        }

        if !self.kernel.exists() {
            ffx_bail!("kernel file at {:?} does not exist", self.kernel);
        }
        if !self.zbi.exists() {
            ffx_bail!("zbi file at {:?} does not exist", self.zbi);
        }
        Ok(())
    }

    pub fn images_exist(&self) -> bool {
        return self.kernel.exists()
            && self.zbi.exists()
            && self.build_args.as_ref().map_or(true, |b| b.exists())
            && self.amber_files.as_ref().map_or(true, |a| a.exists())
            && self.disk_image.as_ref().map_or(true, |f| f.exists());
    }

    pub fn update_paths_from_cache(&mut self, cache_root: &PathBuf) {
        self.amber_files = Some(cache_root.join("package_archive"));
        self.build_args = Some(cache_root.join("images").join("buildargs"));
        self.disk_image = Some(cache_root.join("images").join("femu-disk"));
        self.kernel = cache_root.join("images").join("femu-kernel");
        self.zbi = cache_root.join("images").join("zircon-a.zbi");
    }

    pub fn update_paths_from_args(&mut self, start_command: &StartCommand) {
        if let Some(image) = &start_command.amber_files {
            self.amber_files = Some(PathBuf::from(image))
        }
        if let Some(image) = &start_command.disk_image {
            self.disk_image = Some(PathBuf::from(image))
        }
        if let Some(image) = &start_command.kernel_image {
            self.kernel = PathBuf::from(image)
        }
        if let Some(image) = &start_command.zbi_image {
            self.zbi = PathBuf::from(image)
        }
    }

    pub fn stage_files(&mut self, dir: &PathBuf) -> Result<()> {
        let vdl_kernel_dest = dir.join("femu_kernel");
        let vdl_kernel_src = self.kernel.as_path();
        unix::fs::symlink(&vdl_kernel_src, &vdl_kernel_dest)?;
        self.kernel = vdl_kernel_dest.to_path_buf();

        if let Some(f) = &self.disk_image {
            let vdl_disk_dest = if f
                .to_str()
                .context(format!("Invalid disk image path {:?}", f))?
                .contains("fvm")
            {
                dir.join("femu_fvm")
            } else {
                dir.join("femu_fxfs")
            };
            let vdl_disk_src = f.as_path();
            unix::fs::symlink(&vdl_disk_src, &vdl_disk_dest)?;
            self.disk_image = Some(vdl_disk_dest.to_path_buf());
        }

        if let Some(f) = &self.build_args {
            let vdl_args_dest = dir.join("femu_buildargs");
            let vdl_args_src = f.as_path();
            unix::fs::symlink(&vdl_args_src, &vdl_args_dest)?;
            self.build_args = Some(vdl_args_dest.to_path_buf());
        }
        Ok(())
    }
}

#[derive(Clone)]
pub struct SSHKeys {
    pub authorized_keys: PathBuf,
    pub private_key: PathBuf,
}

impl fmt::Debug for SSHKeys {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f, "[fvdl] private_key {:?}", self.private_key)?;
        write!(f, "[fvdl] authorized_keys {:?}", self.authorized_keys)
    }
}

impl SSHKeys {
    /// Initialize SSH key files for in-tree usage.
    ///
    /// Requires the environment variable FUCHSIA_BUILD_DIR to be specified.
    pub fn from_tree_env(f: &mut impl FuchsiaPaths) -> Result<Self> {
        let ssh_file = File::open(f.find_fuchsia_root()?.join(".fx-ssh-path"))?;
        let ssh_file = BufReader::new(ssh_file);
        let mut lines = ssh_file.lines();

        let private_key = PathBuf::from(lines.next().unwrap()?);
        let authorized_keys = PathBuf::from(lines.next().unwrap()?);
        Ok(Self { authorized_keys: authorized_keys, private_key: private_key })
    }

    /// Initialize SSH key files for GN SDK usage.
    ///
    /// Requires SSH keys to have been generated and stored in $HOME/.ssh/...
    pub fn from_sdk_env() -> Result<Self> {
        Ok(Self {
            authorized_keys: home_dir().unwrap_or_default().join(".ssh/fuchsia_authorized_keys"),
            private_key: home_dir().unwrap_or_default().join(".ssh/fuchsia_ed25519"),
        })
    }

    pub fn check(&self) -> Result<()> {
        if !self.private_key.exists() {
            ffx_bail!("private_key file at {:?} does not exist", self.private_key);
        }
        if !self.authorized_keys.exists() {
            ffx_bail!("authorized_keys file at {:?} does not exist", self.authorized_keys);
        }
        Ok(())
    }

    pub fn update_paths_from_args(&mut self, start_command: &StartCommand) {
        if let Some(path) = &start_command.ssh {
            let ssh_path = PathBuf::from(path);
            self.authorized_keys = ssh_path.join("fuchsia_authorized_keys").to_path_buf();
            self.private_key = ssh_path.join("fuchsia_ed25519").to_path_buf();
        }
    }

    pub fn stage_files(&mut self, dir: &PathBuf) -> Result<()> {
        let vdl_priv_key_dest = dir.join("id_ed25519");
        let vdl_priv_key_src = self.private_key.as_path();
        unix::fs::symlink(&vdl_priv_key_src, &vdl_priv_key_dest)?;
        self.private_key = vdl_priv_key_dest.to_path_buf();

        let vdl_authorized_keys_dest = dir.join("fuchsia_authorized_keys");
        let vdl_authorized_keys_src = self.authorized_keys.as_path();
        unix::fs::symlink(&vdl_authorized_keys_src, &vdl_authorized_keys_dest)?;
        self.authorized_keys = vdl_authorized_keys_dest.to_path_buf();

        Ok(())
    }
}

#[derive(Debug)]
pub struct VDLArgs {
    pub headless: bool,
    pub tuntap: bool,
    pub enable_grpcwebproxy: bool,
    pub enable_hidpi_scaling: bool,
    pub grpcwebproxy_port: String,
    pub upscript: String,
    pub start_package_server: bool,
    pub packages_to_serve: String,
    pub device_proto: String,
    pub gpu: String,
    pub gcs_bucket: String,
    pub gcs_image_archive: String,
    pub sdk_version: String,
    pub cache_root: PathBuf,
    pub extra_kerel_args: String,
    pub amber_unpack_root: String,
    pub package_server_port: String,
    pub acceleration: bool,
    pub image_architecture: String,
    pub isolated_ffx_config_path: String,
}

impl From<&StartCommand> for VDLArgs {
    fn from(cmd: &StartCommand) -> Self {
        let mut gpu = get_default_graphics();
        if cmd.host_gpu {
            gpu = "host".to_string();
        } else if cmd.software_gpu {
            gpu = "swiftshader_indirect".to_string();
        }

        let mut enable_grpcwebproxy = false;
        let mut grpcwebproxy_port = "0".to_string();

        match cmd.grpcwebproxy {
            Some(port) => {
                enable_grpcwebproxy = true;
                grpcwebproxy_port = format!("{}", port);
            }
            _ => (),
        }
        let gcs_image = cmd.image_name.as_ref().unwrap_or(&String::from("qemu-x64")).to_string();
        let sdk_version = match &cmd.sdk_version {
            Some(version) => version.to_string(),
            None => match get_sdk_version_from_manifest() {
                Ok(version) => version,
                Err(_) => String::from(""),
            },
        };
        let mut cache_path = PathBuf::new();
        if cmd.cache_image {
            cache_path = get_sdk_data_dir().unwrap_or_default().join(&gcs_image).join(&sdk_version);
        }
        VDLArgs {
            headless: cmd.headless,
            tuntap: cmd.tuntap,
            enable_hidpi_scaling: cmd.hidpi_scaling,
            upscript: cmd.upscript.as_ref().unwrap_or(&String::from("")).to_string(),
            start_package_server: cmd.start_package_server,
            packages_to_serve: cmd
                .packages_to_serve
                .as_ref()
                .unwrap_or(&String::from(""))
                .to_string(),
            device_proto: cmd.device_proto.as_ref().unwrap_or(&String::from("")).to_string(),
            gpu: gpu,
            enable_grpcwebproxy: enable_grpcwebproxy,
            grpcwebproxy_port: grpcwebproxy_port,
            gcs_bucket: cmd.gcs_bucket.as_ref().unwrap_or(&String::from("fuchsia")).to_string(),
            gcs_image_archive: gcs_image,
            sdk_version: sdk_version,
            cache_root: cache_path,
            extra_kerel_args: cmd.kernel_args.as_ref().unwrap_or(&String::from("")).to_string(),
            amber_unpack_root: cmd
                .amber_unpack_root
                .as_ref()
                .unwrap_or(&String::from(""))
                .to_string(),
            package_server_port: cmd.package_server_port.as_ref().unwrap_or(&0).to_string(),
            acceleration: !cmd.noacceleration,
            image_architecture: cmd
                .image_architecture
                .as_ref()
                .unwrap_or(&String::from(""))
                .to_string(),
            isolated_ffx_config_path: cmd
                .isolated_ffx_config_path
                .as_ref()
                .unwrap_or(&String::from(""))
                .to_string(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serial_test::serial;
    use std::io::Write;
    use tempfile::Builder;

    #[test]
    fn test_convert_start_cmd_to_vdl() {
        let start_command = &StartCommand {
            tuntap: true,
            upscript: Some("/path/to/upscript".to_string()),
            packages_to_serve: Some("pkg1.far,pkg2.far".to_string()),
            host_gpu: true,
            aemu_version: Some("git_revision:da1cc2ee512714a176f08b8b5fec035994ca305d".to_string()),
            sdk_version: Some("0.20201130.3.1".to_string()),
            image_name: Some("qemu-x64".to_string()),
            package_server_log: Some("/a/b/c/server.log".to_string()),
            envs: Vec::new(),
            cache_image: true,
            isolated_ffx_config_path: Some("/a/b/c/isolated_config.txt".to_string()),
            ..Default::default()
        };
        let vdl_args: VDLArgs = start_command.into();
        assert_eq!(vdl_args.headless, false);
        assert_eq!(vdl_args.tuntap, true);
        assert_eq!(vdl_args.upscript, "/path/to/upscript");
        assert_eq!(vdl_args.packages_to_serve, "pkg1.far,pkg2.far");
        assert_eq!(vdl_args.device_proto, "");
        assert_eq!(vdl_args.gpu, "host");
        assert_eq!(vdl_args.start_package_server, false);
        assert_eq!(vdl_args.acceleration, true);
        assert_eq!(vdl_args.package_server_port, "0");
        assert_eq!(vdl_args.amber_unpack_root, "");
        assert_eq!(vdl_args.isolated_ffx_config_path, "/a/b/c/isolated_config.txt");
        assert!(vdl_args.cache_root.as_path().ends_with("qemu-x64/0.20201130.3.1"));
    }

    #[test]
    #[serial]
    fn test_image_intree_files() -> Result<()> {
        env::remove_var("FUCHSIA_BUILD_DIR");
        let mut mock = MockFuchsiaPaths::new();
        let data = format!(
            "/build/out
",
        );
        let tmp_dir = Builder::new().prefix("fvdl_tests_").tempdir()?;
        let a = tmp_dir.into_path();
        let b = a.join("/build/out");
        File::create(a.join(".fx-build-dir"))?.write_all(data.as_bytes())?;
        mock.expect_find_fuchsia_root().returning(move || Ok(a.clone()));
        mock.expect_find_fuchsia_build_dir().returning(move || Ok(b.clone()));
        mock.expect_get_image_path().returning(|name: Vec<&str>, _: &str| {
            let mut p = PathBuf::from("/build/out");
            p.push(name[0]);
            Ok(p)
        });

        let mut image_files = ImageFiles::from_tree_env(&mut mock)?;
        assert_eq!(image_files.zbi.to_str().unwrap(), "/build/out/zircon-a");
        assert_eq!(image_files.kernel.to_str().unwrap(), "/build/out/qemu-kernel");
        assert_eq!(
            image_files.disk_image.as_ref().unwrap().to_str().unwrap(),
            "/build/out/storage-full"
        );
        assert_eq!(
            image_files.build_args.as_ref().unwrap().to_str().unwrap(),
            "/build/out/buildargs"
        );
        // amber_files are optional and is only specified if the file exists. For unit test
        // the file always does not exit.
        assert_eq!(image_files.amber_files, None);

        image_files.update_paths_from_args(&StartCommand {
            disk_image: Some("/path/to/new_fxfs".to_string()),
            zbi_image: Some("/path/to/new_zbi".to_string()),
            kernel_image: Some("/path/to/new_kernel".to_string()),
            amber_files: Some("/path/to/amber_files".to_string()),
            ..Default::default()
        });
        assert_eq!(image_files.zbi.to_str().unwrap(), "/path/to/new_zbi");
        assert_eq!(image_files.kernel.to_str().unwrap(), "/path/to/new_kernel");
        assert_eq!(image_files.disk_image.as_ref().unwrap().to_str().unwrap(), "/path/to/new_fxfs");
        assert_eq!(
            image_files.amber_files.as_ref().unwrap().to_str().unwrap(),
            "/path/to/amber_files"
        );
        assert_eq!(
            image_files.build_args.as_ref().unwrap().to_str().unwrap(),
            "/build/out/buildargs"
        );
        let tmp_dir = Builder::new().prefix("fvdl_tests_").tempdir()?;

        image_files.stage_files(&tmp_dir.path().to_owned())?;
        assert_eq!(image_files.kernel.to_str(), tmp_dir.path().join("femu_kernel").to_str());
        assert_eq!(
            image_files.disk_image.as_ref().unwrap().to_str(),
            tmp_dir.path().join("femu_fxfs").to_str()
        );
        Ok(())
    }

    #[test]
    #[serial]
    fn test_image_sdk_files() -> Result<()> {
        let mut image_files = ImageFiles::from_sdk_env()?;
        image_files.update_paths_from_args(&StartCommand {
            amber_files: Some("/path/to/amber_files".to_string()),
            disk_image: Some("/path/to/new_fvm".to_string()),
            kernel_image: Some("/path/to/new_kernel".to_string()),
            zbi_image: Some("/path/to/new_zbi".to_string()),
            ..Default::default()
        });
        assert_eq!(
            image_files.amber_files.as_ref().unwrap().to_str().unwrap(),
            "/path/to/amber_files"
        );
        assert_eq!(image_files.build_args, None);
        assert_eq!(image_files.disk_image.as_ref().unwrap().to_str().unwrap(), "/path/to/new_fvm");
        assert_eq!(image_files.kernel.to_str().unwrap(), "/path/to/new_kernel");
        assert_eq!(image_files.zbi.to_str().unwrap(), "/path/to/new_zbi");

        let tmp_dir = Builder::new().prefix("fvdl_tests_").tempdir()?;
        image_files.stage_files(&tmp_dir.path().to_owned())?;
        assert_eq!(image_files.kernel.to_str(), tmp_dir.path().join("femu_kernel").to_str());
        assert_eq!(
            image_files.disk_image.as_ref().unwrap().to_str(),
            tmp_dir.path().join("femu_fvm").to_str()
        );
        assert_eq!(image_files.build_args, None);
        Ok(())
    }

    #[test]
    #[serial]
    fn test_ssh_files() -> Result<()> {
        let mut mock = MockFuchsiaPaths::new();
        let data = format!(
            "/usr/local/home/foo/.ssh/fuchsia_ed25519
/usr/local/home/foo/.ssh/fuchsia_authorized_keys
",
        );
        let tmp_dir = Builder::new().prefix("fvdl_tests_").tempdir()?;
        let a = tmp_dir.into_path();
        File::create(a.join(".fx-ssh-path"))?.write_all(data.as_bytes())?;
        mock.expect_find_fuchsia_root().returning(move || Ok(a.clone()));
        let mut ssh_files = SSHKeys::from_tree_env(&mut mock)?;
        assert_eq!(
            ssh_files.private_key.to_str().unwrap(),
            "/usr/local/home/foo/.ssh/fuchsia_ed25519"
        );
        assert_eq!(
            ssh_files.authorized_keys.to_str().unwrap(),
            "/usr/local/home/foo/.ssh/fuchsia_authorized_keys"
        );
        let tmp_dir = Builder::new().prefix("fvdl_test_ssh_").tempdir()?;
        ssh_files.stage_files(&tmp_dir.path().to_owned())?;
        assert!(ssh_files.private_key.ends_with("id_ed25519"));
        Ok(())
    }

    #[test]
    #[serial]
    fn test_ssh_dir() -> Result<()> {
        let mut ssh_files = SSHKeys::from_sdk_env()?;
        assert_eq!(
            ssh_files.private_key,
            home_dir().unwrap_or_default().join(".ssh/fuchsia_ed25519")
        );
        assert_eq!(
            ssh_files.authorized_keys,
            home_dir().unwrap_or_default().join(".ssh/fuchsia_authorized_keys")
        );

        ssh_files.update_paths_from_args(&StartCommand {
            ssh: Some("/path/to/ssh".to_string()),
            ..Default::default()
        });

        assert_eq!(ssh_files.private_key.to_str().unwrap(), "/path/to/ssh/fuchsia_ed25519");
        assert_eq!(
            ssh_files.authorized_keys.to_str().unwrap(),
            "/path/to/ssh/fuchsia_authorized_keys"
        );
        Ok(())
    }

    #[test]
    #[serial]
    fn test_sdk_data_dir() -> Result<()> {
        let tmp_dir = Builder::new().prefix("fvdl_test_sdk_data_dir_").tempdir()?;
        env::set_var("FUCHSIA_SDK_DATA_DIR", tmp_dir.path());
        let p = get_sdk_data_dir()?;
        assert_eq!(p.to_str(), tmp_dir.path().to_str());
        Ok(())
    }
}
