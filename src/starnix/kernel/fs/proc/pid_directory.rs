// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use once_cell::sync::Lazy;
use regex::Regex;
use std::sync::Arc;

use crate::fs::buffers::{InputBuffer, OutputBuffer};
use crate::fs::*;
use crate::lock::Mutex;
use crate::mm::{MemoryAccessor, ProcMapsFile, ProcSmapsFile, ProcStatFile, ProcStatusFile};
use crate::selinux::selinux_proc_attrs;
use crate::task::{CurrentTask, Task, ThreadGroup};
use crate::types::*;

/// Creates an [`FsNode`] that represents the `/proc/<pid>` directory for `task`.
pub fn pid_directory(fs: &FileSystemHandle, task: &Arc<Task>) -> Arc<FsNode> {
    let mut dir = static_directory_builder_with_common_task_entries(fs, task);
    dir.entry_creds(task.as_fscred());
    dir.entry(
        b"task",
        TaskListDirectory { thread_group: task.thread_group.clone() },
        mode!(IFDIR, 0o777),
    );
    dir.build()
}

/// Creates an [`FsNode`] that represents the `/proc/<pid>/task/<tid>` directory for `task`.
fn tid_directory(fs: &FileSystemHandle, task: &Arc<Task>) -> Arc<FsNode> {
    static_directory_builder_with_common_task_entries(fs, task).build()
}

/// Creates a [`StaticDirectoryBuilder`] and pre-populates it with files that are present in both
/// `/pid/<pid>` and `/pid/<pid>/task/<tid>`.
fn static_directory_builder_with_common_task_entries<'a>(
    fs: &'a FileSystemHandle,
    task: &Arc<Task>,
) -> StaticDirectoryBuilder<'a> {
    let mut dir = StaticDirectoryBuilder::new(fs);
    dir.entry_creds(task.as_fscred());
    dir.entry(b"exe", ExeSymlink::new(task), mode!(IFLNK, 0o777));
    dir.entry(b"fd", FdDirectory::new(task), mode!(IFDIR, 0o777));
    dir.entry(b"fdinfo", FdInfoDirectory::new(task), mode!(IFDIR, 0o777));
    dir.entry(b"limits", LimitsFile::new_node(task), mode!(IFREG, 0o444));
    dir.entry(b"maps", ProcMapsFile::new_node(task), mode!(IFREG, 0o444));
    dir.entry(b"smaps", ProcSmapsFile::new_node(task), mode!(IFREG, 0o444));
    dir.entry(b"stat", ProcStatFile::new_node(task), mode!(IFREG, 0o444));
    dir.entry(b"status", ProcStatusFile::new_node(task), mode!(IFREG, 0o444));
    dir.entry(b"cmdline", CmdlineFile::new_node(task), mode!(IFREG, 0o444));
    dir.entry(b"environ", EnvironFile::new_node(task), mode!(IFREG, 0o444));
    dir.entry(b"auxv", AuxvFile::new_node(task), mode!(IFREG, 0o444));
    dir.entry(b"comm", CommFile::new_node(task), mode!(IFREG, 0o444));
    dir.subdir(b"attr", 0o555, |dir| {
        dir.entry_creds(task.as_fscred());
        dir.dir_creds(task.as_fscred());
        selinux_proc_attrs(task, dir);
    });
    dir.entry(b"ns", NsDirectory { task: task.clone() }, mode!(IFDIR, 0o777));
    dir.entry(b"mountinfo", ProcMountinfoFile::new_node(task), mode!(IFREG, 0o444));
    dir.dir_creds(task.as_fscred());
    dir
}

/// `FdDirectory` implements the directory listing operations for a `proc/<pid>/fd` directory.
///
/// Reading the directory returns a list of all the currently open file descriptors for the
/// associated task.
struct FdDirectory {
    task: Arc<Task>,
}

impl FdDirectory {
    fn new(task: &Arc<Task>) -> Self {
        Self { task: Arc::clone(task) }
    }
}

impl FsNodeOps for FdDirectory {
    fs_node_impl_dir_readonly!();

    fn create_file_ops(
        &self,
        _node: &FsNode,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        Ok(VecDirectory::new_file(fds_to_directory_entries(self.task.files.get_all_fds())))
    }

    fn lookup(
        &self,
        node: &FsNode,
        _current_task: &CurrentTask,
        name: &FsStr,
    ) -> Result<Arc<FsNode>, Errno> {
        let fd = FdNumber::from_fs_str(name).map_err(|_| errno!(ENOENT))?;
        // Make sure that the file descriptor exists before creating the node.
        let _ = self.task.files.get(fd).map_err(|_| errno!(ENOENT))?;
        Ok(node.fs().create_node(
            FdSymlink::new(self.task.clone(), fd),
            FdSymlink::file_mode(),
            self.task.as_fscred(),
        ))
    }
}

const NS_ENTRIES: &[&str] = &[
    "cgroup",
    "ipc",
    "mnt",
    "net",
    "pid",
    "pid_for_children",
    "time",
    "time_for_children",
    "user",
    "uts",
];

/// /proc/[pid]/ns directory
struct NsDirectory {
    task: Arc<Task>,
}

impl FsNodeOps for NsDirectory {
    fs_node_impl_dir_readonly!();

    fn create_file_ops(
        &self,
        _node: &FsNode,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        // For each namespace, this contains a link to the current identifier of the given namespace
        // for the current task.
        Ok(VecDirectory::new_file(
            NS_ENTRIES
                .iter()
                .map(|name| VecDirectoryEntry {
                    entry_type: DirectoryEntryType::LNK,
                    name: name.as_bytes().to_vec(),
                    inode: None,
                })
                .collect(),
        ))
    }

    fn lookup(
        &self,
        node: &FsNode,
        _current_task: &CurrentTask,
        name: &FsStr,
    ) -> Result<Arc<FsNode>, Errno> {
        // If name is a given namespace, link to the current identifier of the that namespace for
        // the current task.
        // If name is {namespace}:[id], get a file descriptor for the given namespace.

        let name = String::from_utf8(name.to_vec()).map_err(|_| errno!(ENOENT))?;
        let mut elements = name.split(':');
        let ns = elements.next().expect("name must not be empty");
        // The name doesn't starts with a known namespace.
        if !NS_ENTRIES.contains(&ns) {
            return error!(ENOENT);
        }
        if let Some(id) = elements.next() {
            // The name starts with {namespace}:, check that it matches {namespace}:[id]
            static NS_IDENTIFIER_RE: Lazy<Regex> =
                Lazy::new(|| Regex::new("^\\[[0-9]+\\]$").unwrap());
            if NS_IDENTIFIER_RE.is_match(id) {
                // TODO(qsr): For now, returns an empty file. In the future, this should create a
                // reference to to correct namespace, and ensures it keeps it alive.
                Ok(node.fs().create_node(
                    BytesFile::new_node(vec![]),
                    mode!(IFREG, 0o444),
                    self.task.as_fscred(),
                ))
            } else {
                error!(ENOENT)
            }
        } else {
            // The name is {namespace}, link to the correct one of the current task.
            Ok(node.fs().create_node(
                NsDirectoryEntry { name },
                mode!(IFLNK, 0o7777),
                self.task.as_fscred(),
            ))
        }
    }
}

/// An entry in the ns pseudo directory. For now, all namespace have the identifier 1.
struct NsDirectoryEntry {
    name: String,
}

impl FsNodeOps for NsDirectoryEntry {
    fs_node_impl_symlink!();

    fn readlink(
        &self,
        _node: &FsNode,
        _current_task: &CurrentTask,
    ) -> Result<SymlinkTarget, Errno> {
        Ok(SymlinkTarget::Path(format!("{}:[1]", self.name).as_bytes().to_vec()))
    }
}

/// `FdInfoDirectory` implements the directory listing operations for a `proc/<pid>/fdinfo`
/// directory.
///
/// Reading the directory returns a list of all the currently open file descriptors for the
/// associated task.
struct FdInfoDirectory {
    task: Arc<Task>,
}

impl FdInfoDirectory {
    fn new(task: &Arc<Task>) -> Self {
        Self { task: Arc::clone(task) }
    }
}

impl FsNodeOps for FdInfoDirectory {
    fn create_file_ops(
        &self,
        _node: &FsNode,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        Ok(VecDirectory::new_file(fds_to_directory_entries(self.task.files.get_all_fds())))
    }

    fn lookup(
        &self,
        node: &FsNode,
        _current_task: &CurrentTask,
        name: &FsStr,
    ) -> Result<FsNodeHandle, Errno> {
        let fd = FdNumber::from_fs_str(name).map_err(|_| errno!(ENOENT))?;
        let file = self.task.files.get(fd).map_err(|_| errno!(ENOENT))?;
        let pos = *file.offset.lock();
        let flags = file.flags();
        let data = format!("pos:\t{}flags:\t0{:o}\n", pos, flags.bits()).into_bytes();
        Ok(node.fs().create_node(
            BytesFile::new_node(data),
            mode!(IFREG, 0o444),
            self.task.as_fscred(),
        ))
    }
}

fn fds_to_directory_entries(fds: Vec<FdNumber>) -> Vec<VecDirectoryEntry> {
    fds.into_iter()
        .map(|fd| VecDirectoryEntry {
            entry_type: DirectoryEntryType::DIR,
            name: fd.raw().to_string().into_bytes(),
            inode: None,
        })
        .collect()
}

/// Directory that lists the task IDs (tid) in a process. Located at `/proc/<pid>/task/`.
struct TaskListDirectory {
    thread_group: Arc<ThreadGroup>,
}

impl FsNodeOps for TaskListDirectory {
    fn create_file_ops(
        &self,
        _node: &FsNode,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        Ok(VecDirectory::new_file(
            self.thread_group
                .read()
                .tasks
                .keys()
                .map(|tid| VecDirectoryEntry {
                    entry_type: DirectoryEntryType::DIR,
                    name: tid.to_string().into_bytes(),
                    inode: None,
                })
                .collect(),
        ))
    }

    fn lookup(
        &self,
        node: &FsNode,
        _current_task: &CurrentTask,
        name: &FsStr,
    ) -> Result<Arc<FsNode>, Errno> {
        let tid = std::str::from_utf8(name)
            .map_err(|_| errno!(ENOENT))?
            .parse::<pid_t>()
            .map_err(|_| errno!(ENOENT))?;
        // Make sure the tid belongs to this process.
        if !self.thread_group.read().tasks.contains_key(&tid) {
            return error!(ENOENT);
        }
        let task =
            self.thread_group.kernel.pids.read().get_task(tid).ok_or_else(|| errno!(ENOENT))?;
        Ok(tid_directory(&node.fs(), &task))
    }
}

/// An `ExeSymlink` points to the `executable_node` (the node that contains the task's binary) of
/// the task that reads it.
pub struct ExeSymlink {
    task: Arc<Task>,
}

impl ExeSymlink {
    fn new(task: &Arc<Task>) -> Self {
        Self { task: Arc::clone(task) }
    }
}

impl FsNodeOps for ExeSymlink {
    fs_node_impl_symlink!();

    fn readlink(
        &self,
        _node: &FsNode,
        _current_task: &CurrentTask,
    ) -> Result<SymlinkTarget, Errno> {
        if let Some(node) = self.task.mm.executable_node() {
            Ok(SymlinkTarget::Node(node))
        } else {
            error!(ENOENT)
        }
    }
}

/// `FdSymlink` is a symlink that points to a specific file descriptor in a task.
pub struct FdSymlink {
    /// The file descriptor that this symlink targets.
    fd: FdNumber,

    /// The task that `fd` is to be read from.
    task: Arc<Task>,
}

impl FdSymlink {
    fn new(task: Arc<Task>, fd: FdNumber) -> Self {
        FdSymlink { fd, task }
    }

    fn file_mode() -> FileMode {
        mode!(IFLNK, 0o777)
    }
}

impl FsNodeOps for FdSymlink {
    fs_node_impl_symlink!();

    fn readlink(
        &self,
        _node: &FsNode,
        _current_task: &CurrentTask,
    ) -> Result<SymlinkTarget, Errno> {
        let file = self.task.files.get(self.fd).map_err(|_| errno!(ENOENT))?;
        Ok(SymlinkTarget::Node(file.name.clone()))
    }
}

fn read_at_from_addr<F>(
    seq: &Mutex<SeqFileState<()>>,
    task: &Arc<Task>,
    current_task: &CurrentTask,
    offset: usize,
    data: &mut dyn OutputBuffer,
    get_range: F,
) -> Result<usize, Errno>
where
    F: Fn() -> (UserAddress, UserAddress),
{
    let iter = move |_, sink: &mut SeqFileBuf| {
        let (range_start, range_end) = get_range();
        #[allow(clippy::manual_saturating_arithmetic)]
        let len = range_end.ptr().checked_sub(range_start.ptr()).unwrap_or(0);
        let mut buf = vec![0u8; len];
        let len = task.mm.read_memory_partial(range_start, &mut buf)?;
        sink.write(&buf[..len]);
        Ok(None)
    };
    seq.lock().read_at(current_task, iter, offset, data)
}

/// `CmdlineFile` implements the `FsNodeOps` for a `proc/<pid>/cmdline` file.
struct CmdlineFile {
    /// The task from which the `CmdlineFile` fetches the command line parameters.
    task: Arc<Task>,
    seq: Mutex<SeqFileState<()>>,
}

impl CmdlineFile {
    fn new_node(task: &Arc<Task>) -> impl FsNodeOps {
        let task = Arc::clone(task);
        SimpleFileNode::new(move || {
            Ok(CmdlineFile { task: Arc::clone(&task), seq: Default::default() })
        })
    }
}

impl FileOps for CmdlineFile {
    fileops_impl_seekable!();

    fn read_at(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        offset: usize,
        data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        let task = self.task.clone();
        read_at_from_addr(&self.seq, &self.task, current_task, offset, data, move || {
            let mm_state = task.mm.state.read();
            (mm_state.argv_start, mm_state.argv_end)
        })
    }

    fn write_at(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        _data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        error!(ENOSYS)
    }
}

/// `EnvironFile` implements the `FsNodeOps` for a `proc/<pid>/environ` file.
struct EnvironFile {
    /// The task from which the `EnvironFile` fetches the environment variables.
    task: Arc<Task>,
    seq: Mutex<SeqFileState<()>>,
}

impl EnvironFile {
    fn new_node(task: &Arc<Task>) -> impl FsNodeOps {
        let task = Arc::clone(task);
        SimpleFileNode::new(move || {
            Ok(EnvironFile { task: Arc::clone(&task), seq: Default::default() })
        })
    }
}

impl FileOps for EnvironFile {
    fileops_impl_seekable!();

    fn read_at(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        offset: usize,
        data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        read_at_from_addr(&self.seq, &self.task, current_task, offset, data, move || {
            let mm_state = self.task.mm.state.read();
            (mm_state.environ_start, mm_state.environ_end)
        })
    }

    fn write_at(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        _data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        error!(ENOSYS)
    }
}

/// `AuxvFile` implements the `FsNodeOps` for a `proc/<pid>/environ` file.
struct AuxvFile {
    /// The task from which the `AuxvFile` fetches the AUX vector.
    task: Arc<Task>,
    seq: Mutex<SeqFileState<()>>,
}

impl AuxvFile {
    fn new_node(task: &Arc<Task>) -> impl FsNodeOps {
        let task = Arc::clone(task);
        SimpleFileNode::new(move || {
            Ok(AuxvFile { task: Arc::clone(&task), seq: Default::default() })
        })
    }
}

impl FileOps for AuxvFile {
    fileops_impl_seekable!();

    fn read_at(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        offset: usize,
        data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        read_at_from_addr(&self.seq, &self.task, current_task, offset, data, move || {
            let mm_state = self.task.mm.state.read();
            (mm_state.auxv_start, mm_state.auxv_end)
        })
    }

    fn write_at(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        _data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        error!(ENOSYS)
    }
}

/// `CommFile` implements the `FsNodeOps` for a `proc/<pid>/comm` file.
pub struct CommFile {
    /// The task from which the `CommFile` fetches the command line parameters.
    task: Arc<Task>,
    seq: Mutex<SeqFileState<()>>,
}

impl CommFile {
    fn new_node(task: &Arc<Task>) -> impl FsNodeOps {
        let task = Arc::clone(task);
        SimpleFileNode::new(move || {
            Ok(CommFile { task: Arc::clone(&task), seq: Default::default() })
        })
    }
}

impl FileOps for CommFile {
    fileops_impl_seekable!();

    fn read_at(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        offset: usize,
        data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        let comm = self.task.command();
        let mut seq = self.seq.lock();
        let iter = move |_cursor, sink: &mut SeqFileBuf| {
            sink.write(comm.as_bytes());
            sink.write(b"\n");
            Ok(None)
        };
        seq.read_at(current_task, iter, offset, data)
    }

    fn write_at(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        _data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        error!(ENOSYS)
    }
}

/// `LimitsFile` implements the `FsNodeOps` for a `proc/<pid>/limits` file.
pub struct LimitsFile {
    /// The task from which the `LimitsFile` fetches the limits.
    task: Arc<Task>,
    seq: Mutex<SeqFileState<()>>,
}

impl LimitsFile {
    fn new_node(task: &Arc<Task>) -> impl FsNodeOps {
        let task = Arc::clone(task);
        SimpleFileNode::new(move || {
            Ok(LimitsFile { task: Arc::clone(&task), seq: Default::default() })
        })
    }
}

impl FileOps for LimitsFile {
    fileops_impl_seekable!();

    fn read_at(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        offset: usize,
        data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        let state = self.task.thread_group.read();
        let limits = &state.limits;
        let mut seq = self.seq.lock();
        let iter = move |_cursor, sink: &mut SeqFileBuf| {
            let write_limit = |sink: &mut SeqFileBuf, value| {
                if value == RLIM_INFINITY as u64 {
                    sink.write(format!("{:<20}", "unlimited").as_bytes());
                } else {
                    sink.write(format!("{:<20}", value).as_bytes());
                }
            };
            sink.write(
                format!("{:<25}{:<20}{:<20}{:<10}\n", "Limit", "Soft Limit", "Hard Limit", "Units")
                    .as_bytes(),
            );
            for resource in Resource::ALL {
                let desc = resource.desc();
                let limit = limits.get(resource);
                sink.write(format!("{:<25}", desc.name).as_bytes());
                write_limit(sink, limit.rlim_cur);
                write_limit(sink, limit.rlim_max);
                if !desc.unit.is_empty() {
                    sink.write(format!("{:<10}", desc.unit).as_bytes());
                }
                sink.write(b"\n");
            }
            Ok(None)
        };
        seq.read_at(current_task, iter, offset, data)
    }

    fn write_at(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        _data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        error!(ENOSYS)
    }
}
