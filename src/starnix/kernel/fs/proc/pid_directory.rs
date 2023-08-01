// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fuchsia_zircon as zx;
use itertools::Itertools;
use once_cell::sync::Lazy;
use regex::Regex;
use std::{borrow::Cow, ffi::CString, sync::Arc};

use crate::{
    fs::{
        buffers::{InputBuffer, OutputBuffer},
        *,
    },
    mm::{MemoryAccessor, MemoryAccessorExt, ProcMapsFile, ProcSmapsFile, PAGE_SIZE},
    selinux::selinux_proc_attrs,
    task::{CurrentTask, Task, TaskPersistentInfo, TaskStateCode, ThreadGroup},
    types::*,
};

/// Creates an [`FsNode`] that represents the `/proc/<pid>` directory for `task`.
pub fn pid_directory(fs: &FileSystemHandle, task: &TempRef<'_, Task>) -> Arc<FsNode> {
    let mut dir =
        static_directory_builder_with_common_task_entries(fs, task, StatsScope::ThreadGroup);
    dir.entry_creds(task.as_fscred());
    dir.entry(
        b"task",
        TaskListDirectory { thread_group: task.thread_group.clone() },
        mode!(IFDIR, 0o777),
    );
    dir.build()
}

/// Creates an [`FsNode`] that represents the `/proc/<pid>/task/<tid>` directory for `task`.
fn tid_directory(fs: &FileSystemHandle, task: &TempRef<'_, Task>) -> Arc<FsNode> {
    static_directory_builder_with_common_task_entries(fs, task, StatsScope::Task).build()
}

/// Creates a [`StaticDirectoryBuilder`] and pre-populates it with files that are present in both
/// `/pid/<pid>` and `/pid/<pid>/task/<tid>`.
fn static_directory_builder_with_common_task_entries<'a>(
    fs: &'a FileSystemHandle,
    task: &TempRef<'_, Task>,
    scope: StatsScope,
) -> StaticDirectoryBuilder<'a> {
    let mut dir = StaticDirectoryBuilder::new(fs);
    dir.entry_creds(task.as_fscred());
    dir.entry(
        b"cwd",
        CallbackSymlinkNode::new({
            let task = WeakRef::from(task);
            move || Ok(SymlinkTarget::Node(Task::from_weak(&task)?.fs().cwd()))
        }),
        mode!(IFLNK, 0o777),
    );
    dir.entry(
        b"exe",
        CallbackSymlinkNode::new({
            let task = WeakRef::from(task);
            move || {
                if let Some(node) = Task::from_weak(&task)?.mm.executable_node() {
                    Ok(SymlinkTarget::Node(node))
                } else {
                    error!(ENOENT)
                }
            }
        }),
        mode!(IFLNK, 0o777),
    );
    dir.entry(b"fd", FdDirectory::new(task.into()), mode!(IFDIR, 0o777));
    dir.entry(b"fdinfo", FdInfoDirectory::new(task.into()), mode!(IFDIR, 0o777));
    dir.entry(b"io", IoFile::new_node(task.into()), mode!(IFREG, 0o444));
    dir.entry(b"limits", LimitsFile::new_node(task.into()), mode!(IFREG, 0o444));
    dir.entry(b"maps", ProcMapsFile::new_node(task.into()), mode!(IFREG, 0o444));
    dir.entry(b"mem", MemFile::new_node(task.into()), mode!(IFREG, 0o600));
    dir.entry(
        b"root",
        CallbackSymlinkNode::new({
            let task = WeakRef::from(task);
            move || Ok(SymlinkTarget::Node(Task::from_weak(&task)?.fs().root()))
        }),
        mode!(IFLNK, 0o777),
    );
    dir.entry(b"smaps", ProcSmapsFile::new_node(task.into()), mode!(IFREG, 0o444));
    dir.entry(b"stat", StatFile::new_node(task.into(), scope), mode!(IFREG, 0o444));
    dir.entry(b"statm", StatmFile::new_node(task.into()), mode!(IFREG, 0o444));
    dir.entry(
        b"status",
        StatusFile::new_node(task.into(), task.persistent_info.clone()),
        mode!(IFREG, 0o444),
    );
    dir.entry(b"cmdline", CmdlineFile::new_node(task.into()), mode!(IFREG, 0o444));
    dir.entry(b"environ", EnvironFile::new_node(task.into()), mode!(IFREG, 0o444));
    dir.entry(b"auxv", AuxvFile::new_node(task.into()), mode!(IFREG, 0o444));
    dir.entry(
        b"comm",
        CommFile::new_node(task.into(), task.persistent_info.clone()),
        mode!(IFREG, 0o644),
    );
    dir.subdir(b"attr", 0o555, |dir| {
        dir.entry_creds(task.as_fscred());
        dir.dir_creds(task.as_fscred());
        selinux_proc_attrs(task, dir);
    });
    dir.entry(b"ns", NsDirectory { task: task.into() }, mode!(IFDIR, 0o777));
    dir.entry(b"mountinfo", ProcMountinfoFile::new_node(task.into()), mode!(IFREG, 0o444));
    dir.entry(b"mounts", ProcMountsFile::new_node(task.into()), mode!(IFREG, 0o444));
    dir.entry(b"oom_adj", OomAdjFile::new_node(task.into()), mode!(IFREG, 0o744));
    dir.entry(b"oom_score", OomScoreFile::new_node(task.into()), mode!(IFREG, 0o444));
    dir.entry(b"oom_score_adj", OomScoreAdjFile::new_node(task.into()), mode!(IFREG, 0o744));
    dir.dir_creds(task.as_fscred());
    dir
}

/// `FdDirectory` implements the directory listing operations for a `proc/<pid>/fd` directory.
///
/// Reading the directory returns a list of all the currently open file descriptors for the
/// associated task.
struct FdDirectory {
    task: WeakRef<Task>,
}

impl FdDirectory {
    fn new(task: WeakRef<Task>) -> Self {
        Self { task }
    }
}

impl FsNodeOps for FdDirectory {
    fs_node_impl_dir_readonly!();

    fn create_file_ops(
        &self,
        _node: &FsNode,
        _current_task: &CurrentTask,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        Ok(VecDirectory::new_file(fds_to_directory_entries(
            Task::from_weak(&self.task)?.files.get_all_fds(),
        )))
    }

    fn lookup(
        &self,
        node: &FsNode,
        _current_task: &CurrentTask,
        name: &FsStr,
    ) -> Result<Arc<FsNode>, Errno> {
        let fd = FdNumber::from_fs_str(name).map_err(|_| errno!(ENOENT))?;
        let task = Task::from_weak(&self.task)?;
        // Make sure that the file descriptor exists before creating the node.
        let _ = task.files.get(fd).map_err(|_| errno!(ENOENT))?;
        let task_reference = self.task.clone();
        Ok(node.fs().create_node(
            CallbackSymlinkNode::new(move || {
                let task = Task::from_weak(&task_reference)?;
                //Task::try_from(&task_reference)?;
                let file = task.files.get(fd).map_err(|_| errno!(ENOENT))?;
                Ok(SymlinkTarget::Node(file.name.clone()))
            }),
            FsNodeInfo::new_factory(mode!(IFLNK, 0o777), task.as_fscred()),
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
    task: WeakRef<Task>,
}

impl FsNodeOps for NsDirectory {
    fs_node_impl_dir_readonly!();

    fn create_file_ops(
        &self,
        _node: &FsNode,
        _current_task: &CurrentTask,
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
        current_task: &CurrentTask,
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

        let task = Task::from_weak(&self.task)?;
        if let Some(id) = elements.next() {
            // The name starts with {namespace}:, check that it matches {namespace}:[id]
            static NS_IDENTIFIER_RE: Lazy<Regex> =
                Lazy::new(|| Regex::new("^\\[[0-9]+\\]$").unwrap());
            if !NS_IDENTIFIER_RE.is_match(id) {
                return error!(ENOENT);
            }
            let node_info = FsNodeInfo::new_factory(mode!(IFREG, 0o444), task.as_fscred());

            Ok(match ns {
                "mnt" => node.fs().create_node(current_task.task.fs().namespace(), node_info),
                _ => {
                    // TODO(https://fxbug.dev/76946) support other kinds of namespaces
                    node.fs().create_node(BytesFile::new_node(vec![]), node_info)
                }
            })
        } else {
            // The name is {namespace}, link to the correct one of the current task.
            Ok(node.fs().create_node(
                CallbackSymlinkNode::new(move || {
                    // For now, all namespace have the identifier 1.
                    Ok(SymlinkTarget::Path(format!("{}:[1]", name).as_bytes().to_vec()))
                }),
                FsNodeInfo::new_factory(mode!(IFLNK, 0o7777), task.as_fscred()),
            ))
        }
    }
}

/// `FdInfoDirectory` implements the directory listing operations for a `proc/<pid>/fdinfo`
/// directory.
///
/// Reading the directory returns a list of all the currently open file descriptors for the
/// associated task.
struct FdInfoDirectory {
    task: WeakRef<Task>,
}

impl FdInfoDirectory {
    fn new(task: WeakRef<Task>) -> Self {
        Self { task }
    }
}

impl FsNodeOps for FdInfoDirectory {
    fn create_file_ops(
        &self,
        _node: &FsNode,
        _current_task: &CurrentTask,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        Ok(VecDirectory::new_file(fds_to_directory_entries(
            Task::from_weak(&self.task)?.files.get_all_fds(),
        )))
    }

    fn lookup(
        &self,
        node: &FsNode,
        _current_task: &CurrentTask,
        name: &FsStr,
    ) -> Result<FsNodeHandle, Errno> {
        let task = Task::from_weak(&self.task)?;
        let fd = FdNumber::from_fs_str(name).map_err(|_| errno!(ENOENT))?;
        let file = task.files.get(fd).map_err(|_| errno!(ENOENT))?;
        let pos = *file.offset.lock();
        let flags = file.flags();
        let data = format!("pos:\t{}flags:\t0{:o}\n", pos, flags.bits()).into_bytes();
        Ok(node.fs().create_node(
            BytesFile::new_node(data),
            FsNodeInfo::new_factory(mode!(IFREG, 0o444), task.as_fscred()),
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
        _current_task: &CurrentTask,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        Ok(VecDirectory::new_file(
            self.thread_group
                .read()
                .task_ids()
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
        if !self.thread_group.read().contains_task(tid) {
            return error!(ENOENT);
        }
        let pid_state = self.thread_group.kernel.pids.read();
        let weak_task = pid_state.get_task(tid);
        let task = weak_task.upgrade().ok_or_else(|| errno!(ENOENT))?;
        Ok(tid_directory(&node.fs(), &task))
    }
}

fn fill_buf_from_addr_range(
    task: &Task,
    range_start: UserAddress,
    range_end: UserAddress,
    sink: &mut DynamicFileBuf,
) -> Result<(), Errno> {
    #[allow(clippy::manual_saturating_arithmetic)]
    let len = range_end.ptr().checked_sub(range_start.ptr()).unwrap_or(0);
    let buf = task.mm.read_memory_partial_to_vec(range_start, len)?;
    sink.write(&buf[..]);
    Ok(())
}

/// `CmdlineFile` implements `proc/<pid>/cmdline` file.
#[derive(Clone)]
pub struct CmdlineFile(WeakRef<Task>);
impl CmdlineFile {
    pub fn new_node(task: WeakRef<Task>) -> impl FsNodeOps {
        DynamicFile::new_node(Self(task))
    }
}
impl DynamicFileSource for CmdlineFile {
    fn generate(&self, sink: &mut DynamicFileBuf) -> Result<(), Errno> {
        // Opened cmdline file should still be functional once the task is a zombie.
        let task = if let Some(task) = self.0.upgrade() {
            task
        } else {
            return Ok(());
        };
        let (start, end) = {
            let mm_state = task.mm.state.read();
            (mm_state.argv_start, mm_state.argv_end)
        };
        fill_buf_from_addr_range(&task, start, end, sink)
    }
}

/// `EnvironFile` implements `proc/<pid>/environ` file.
#[derive(Clone)]
pub struct EnvironFile(WeakRef<Task>);
impl EnvironFile {
    pub fn new_node(task: WeakRef<Task>) -> impl FsNodeOps {
        DynamicFile::new_node(Self(task))
    }
}
impl DynamicFileSource for EnvironFile {
    fn generate(&self, sink: &mut DynamicFileBuf) -> Result<(), Errno> {
        let task = Task::from_weak(&self.0)?;
        let (start, end) = {
            let mm_state = task.mm.state.read();
            (mm_state.environ_start, mm_state.environ_end)
        };
        fill_buf_from_addr_range(&task, start, end, sink)
    }
}

/// `AuxvFile` implements `proc/<pid>/auxv` file.
#[derive(Clone)]
pub struct AuxvFile(WeakRef<Task>);
impl AuxvFile {
    pub fn new_node(task: WeakRef<Task>) -> impl FsNodeOps {
        DynamicFile::new_node(Self(task))
    }
}
impl DynamicFileSource for AuxvFile {
    fn generate(&self, sink: &mut DynamicFileBuf) -> Result<(), Errno> {
        let task = Task::from_weak(&self.0)?;
        let (start, end) = {
            let mm_state = task.mm.state.read();
            (mm_state.auxv_start, mm_state.auxv_end)
        };
        fill_buf_from_addr_range(&task, start, end, sink)
    }
}

/// `CommFile` implements `proc/<pid>/comm` file.
pub struct CommFile {
    task: WeakRef<Task>,
    dynamic_file: DynamicFile<CommFileSource>,
}
impl CommFile {
    pub fn new_node(task: WeakRef<Task>, info: TaskPersistentInfo) -> impl FsNodeOps {
        SimpleFileNode::new(move || {
            Ok(CommFile {
                task: task.clone(),
                dynamic_file: DynamicFile::new(CommFileSource(info.clone())),
            })
        })
    }
}

impl FileOps for CommFile {
    fileops_impl_delegate_read_and_seek!(self, self.dynamic_file);

    fn write(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        _offset: usize,
        data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        let task = Task::from_weak(&self.task)?;
        if !Arc::ptr_eq(&task.thread_group, &current_task.thread_group) {
            return error!(EINVAL);
        }
        // What happens if userspace writes to this file in multiple syscalls? We need more
        // detailed tests to see when the data is actually committed back to the task.
        let bytes = data.read_all()?;
        let command =
            CString::new(bytes.iter().copied().take_while(|c| *c != b'\0').collect::<Vec<_>>())
                .unwrap();
        task.set_command_name(command);
        Ok(bytes.len())
    }
}

#[derive(Clone)]
pub struct CommFileSource(TaskPersistentInfo);
impl DynamicFileSource for CommFileSource {
    fn generate(&self, sink: &mut DynamicFileBuf) -> Result<(), Errno> {
        sink.write(self.0.lock().command().as_bytes());
        sink.write(b"\n");
        Ok(())
    }
}

/// `IoFile` implements `proc/<pid>/io` file.
#[derive(Clone)]
pub struct IoFile(WeakRef<Task>);
impl IoFile {
    pub fn new_node(task: WeakRef<Task>) -> impl FsNodeOps {
        DynamicFile::new_node(Self(task))
    }
}
impl DynamicFileSource for IoFile {
    fn generate(&self, sink: &mut DynamicFileBuf) -> Result<(), Errno> {
        // TODO: Keep track of these stats and report them in this file.
        sink.write(b"rchar: 0\n");
        sink.write(b"wchar: 0\n");
        sink.write(b"syscr: 0\n");
        sink.write(b"syscw: 0\n");
        sink.write(b"read_bytes: 0\n");
        sink.write(b"write_bytes: 0\n");
        sink.write(b"cancelled_write_bytes: 0\n");
        Ok(())
    }
}

/// `LimitsFile` implements `proc/<pid>/limits` file.
#[derive(Clone)]
pub struct LimitsFile(WeakRef<Task>);
impl LimitsFile {
    pub fn new_node(task: WeakRef<Task>) -> impl FsNodeOps {
        DynamicFile::new_node(Self(task))
    }
}
impl DynamicFileSource for LimitsFile {
    fn generate(&self, sink: &mut DynamicFileBuf) -> Result<(), Errno> {
        let task = Task::from_weak(&self.0)?;
        let limits = task.thread_group.limits.lock();

        let write_limit = |sink: &mut DynamicFileBuf, value| {
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
        Ok(())
    }
}

/// `MemFile` implements `proc/<pid>/mem` file.
#[derive(Clone)]
pub struct MemFile(WeakRef<Task>);
impl MemFile {
    pub fn new_node(task: WeakRef<Task>) -> impl FsNodeOps {
        SimpleFileNode::new(move || Ok(Self(task.clone())))
    }
}

impl FileOps for MemFile {
    fn is_seekable(&self) -> bool {
        true
    }

    fn seek(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        current_offset: off_t,
        target: SeekTarget,
    ) -> Result<off_t, Errno> {
        default_seek(current_offset, target, |_| error!(EINVAL))
    }

    fn read(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        offset: usize,
        data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        let task = if let Some(task) = self.0.upgrade() {
            task
        } else {
            return Ok(0);
        };
        match task.state_code() {
            TaskStateCode::Zombie => Ok(0),
            TaskStateCode::Running | TaskStateCode::Sleeping => {
                let mut addr = UserAddress::default() + offset;
                data.write_each(&mut |bytes| {
                    let actual = task
                        .mm
                        .read_memory_partial_to_slice(addr, bytes)
                        .map_err(|_| errno!(EIO))?;
                    addr += actual;
                    Ok(actual)
                })
            }
        }
    }

    fn write(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        offset: usize,
        data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        let task = Task::from_weak(&self.0)?;
        match task.state_code() {
            TaskStateCode::Zombie => Ok(0),
            TaskStateCode::Running | TaskStateCode::Sleeping => {
                let addr = UserAddress::default() + offset;
                let mut written = 0;
                let result = data.peek_each(&mut |bytes| {
                    let actual = task
                        .mm
                        .write_memory_partial(addr + written, bytes)
                        .map_err(|_| errno!(EIO))?;
                    written += actual;
                    Ok(actual)
                });
                data.advance(written)?;
                result
            }
        }
    }
}

#[derive(Copy, Clone, Eq, PartialEq)]
pub enum StatsScope {
    Task,
    ThreadGroup,
}

#[derive(Clone)]
pub struct StatFile {
    task: WeakRef<Task>,
    scope: StatsScope,
}

impl StatFile {
    pub fn new_node(task: WeakRef<Task>, scope: StatsScope) -> impl FsNodeOps {
        DynamicFile::new_node(Self { task, scope })
    }
}
impl DynamicFileSource for StatFile {
    fn generate(&self, sink: &mut DynamicFileBuf) -> Result<(), Errno> {
        let task = Task::from_weak(&self.task)?;
        let command = task.command();
        let command = command.as_c_str().to_str().unwrap_or("unknown");
        let mut stats = [0u64; 48];
        {
            let thread_group = task.thread_group.read();
            stats[0] = thread_group.get_ppid() as u64;
            stats[1] = thread_group.process_group.leader as u64;
            stats[2] = thread_group.process_group.session.leader as u64;

            // TTY device ID.
            {
                let session = thread_group.process_group.session.read();
                stats[3] = session
                    .controlling_terminal
                    .as_ref()
                    .map(|t| t.terminal.device().bits())
                    .unwrap_or(0);
            }

            stats[12] =
                duration_to_scheduler_clock(thread_group.children_time_stats.user_time) as u64;
            stats[13] =
                duration_to_scheduler_clock(thread_group.children_time_stats.system_time) as u64;

            stats[16] = thread_group.tasks_count() as u64;
        }

        let time_stats = match self.scope {
            StatsScope::Task => task.time_stats(),
            StatsScope::ThreadGroup => task.thread_group.time_stats(),
        };
        stats[10] = duration_to_scheduler_clock(time_stats.user_time) as u64;
        stats[11] = duration_to_scheduler_clock(time_stats.system_time) as u64;

        let info = task.thread_group.process.info().map_err(|_| errno!(EIO))?;
        stats[18] =
            duration_to_scheduler_clock(zx::Time::from_nanos(info.start_time) - zx::Time::ZERO)
                as u64;

        let mem_stats = task.mm.get_stats().map_err(|_| errno!(EIO))?;
        let page_size = *PAGE_SIZE as usize;
        stats[19] = mem_stats.vm_size as u64;
        stats[20] = (mem_stats.vm_rss / page_size) as u64;
        stats[21] = task.thread_group.limits.lock().get(Resource::RSS).rlim_max;

        {
            let mm_state = task.mm.state.read();
            stats[24] = mm_state.stack_start.ptr() as u64;
            stats[44] = mm_state.argv_start.ptr() as u64;
            stats[45] = mm_state.argv_end.ptr() as u64;
            stats[46] = mm_state.environ_start.ptr() as u64;
            stats[47] = mm_state.environ_end.ptr() as u64;
        }
        let stat_str = stats.map(|n| n.to_string()).join(" ");

        writeln!(
            sink,
            "{} ({}) {} {}",
            task.get_pid(),
            command,
            task.state_code().code_char(),
            stat_str
        )?;

        Ok(())
    }
}

#[derive(Clone)]
pub struct StatmFile(WeakRef<Task>);
impl StatmFile {
    pub fn new_node(task: WeakRef<Task>) -> impl FsNodeOps {
        DynamicFile::new_node(Self(task))
    }
}
impl DynamicFileSource for StatmFile {
    fn generate(&self, sink: &mut DynamicFileBuf) -> Result<(), Errno> {
        let mem_stats = Task::from_weak(&self.0)?.mm.get_stats().map_err(|_| errno!(EIO))?;
        let page_size = *PAGE_SIZE as usize;

        // 5th and 7th fields are deprecated and should be set to 0.
        writeln!(
            sink,
            "{} {} {} {} 0 {} 0",
            mem_stats.vm_size / page_size,
            mem_stats.vm_rss / page_size,
            mem_stats.rss_shared / page_size,
            mem_stats.vm_exe / page_size,
            (mem_stats.vm_data + mem_stats.vm_stack) / page_size
        )?;
        Ok(())
    }
}

#[derive(Clone)]
pub struct StatusFile(WeakRef<Task>, TaskPersistentInfo);
impl StatusFile {
    pub fn new_node(task: WeakRef<Task>, info: TaskPersistentInfo) -> impl FsNodeOps {
        DynamicFile::new_node(Self(task, info))
    }
}
impl DynamicFileSource for StatusFile {
    fn generate(&self, sink: &mut DynamicFileBuf) -> Result<(), Errno> {
        let task = &self.0.upgrade();
        let info = self.1.lock().clone();

        write!(sink, "Name:\t")?;
        sink.write(info.command().as_bytes());
        writeln!(sink)?;

        if let Some(task) = task {
            writeln!(sink, "Umask:\t0{:03o}", task.fs().umask().bits())?;
        }

        let state_code =
            if let Some(task) = task { task.state_code() } else { TaskStateCode::Zombie };
        writeln!(sink, "State:\t{} ({})", state_code.code_char(), state_code.name())?;

        writeln!(sink, "Tgid:\t{}", info.pid())?;
        writeln!(sink, "Pid:\t{}", info.tid())?;
        let (ppid, threads) = if let Some(task) = task {
            let task_group = task.thread_group.read();
            (task_group.get_ppid(), task_group.tasks_count())
        } else {
            // TODO(fxbug.dev/129993): The data is incorrect, and requires keeping information for zombie processes.
            (1, 1)
        };
        writeln!(sink, "PPid:\t{}", ppid)?;

        // TODO(tbodt): the fourth one is supposed to be fsuid, but we haven't implemented fsuid.
        let creds = info.creds();
        writeln!(sink, "Uid:\t{}\t{}\t{}\t{}", creds.uid, creds.euid, creds.saved_uid, creds.euid)?;
        writeln!(sink, "Gid:\t{}\t{}\t{}\t{}", creds.gid, creds.egid, creds.saved_gid, creds.egid)?;
        writeln!(sink, "Groups:\t{}", creds.groups.iter().map(|n| n.to_string()).join(" "))?;

        if let Some(task) = task {
            let mem_stats = task.mm.get_stats().map_err(|_| errno!(EIO))?;
            writeln!(sink, "VmSize:\t{} kB", mem_stats.vm_size / 1024)?;
            writeln!(sink, "VmRSS:\t{} kB", mem_stats.vm_rss / 1024)?;
            writeln!(sink, "RssAnon:\t{} kB", mem_stats.rss_anonymous / 1024)?;
            writeln!(sink, "RssFile:\t{} kB", mem_stats.rss_file / 1024)?;
            writeln!(sink, "RssShmem:\t{} kB", mem_stats.rss_shared / 1024)?;
            writeln!(sink, "VmData:\t{} kB", mem_stats.vm_data / 1024)?;
            writeln!(sink, "VmStk:\t{} kB", mem_stats.vm_stack / 1024)?;
            writeln!(sink, "VmExe:\t{} kB", mem_stats.vm_exe / 1024)?;
        }

        // There should be at least on thread in Zombie processes.
        writeln!(sink, "Threads:\t{}", std::cmp::max(1, threads))?;

        Ok(())
    }
}

struct OomScoreFile(WeakRef<Task>);

impl OomScoreFile {
    fn new_node(task: WeakRef<Task>) -> impl FsNodeOps {
        BytesFile::new_node(Self(task))
    }
}

impl BytesFileOps for OomScoreFile {
    fn read(&self, _current_task: &CurrentTask) -> Result<Cow<'_, [u8]>, Errno> {
        let _task = Task::from_weak(&self.0)?;
        // TODO: Compute this score from the amount of memory used by the task.
        // See https://man7.org/linux/man-pages/man5/proc.5.html for the algorithm.
        Ok(serialize_i32_file(0).into())
    }
}

// Redefine these constants as i32 to avoid conversions below.
const OOM_ADJUST_MAX: i32 = uapi::OOM_ADJUST_MAX as i32;
const OOM_SCORE_ADJ_MAX: i32 = uapi::OOM_SCORE_ADJ_MAX as i32;

struct OomAdjFile(WeakRef<Task>);
impl OomAdjFile {
    fn new_node(task: WeakRef<Task>) -> impl FsNodeOps {
        BytesFile::new_node(Self(task))
    }
}

impl BytesFileOps for OomAdjFile {
    fn write(&self, current_task: &CurrentTask, data: Vec<u8>) -> Result<(), Errno> {
        let value = parse_i32_file(&data)?;
        let oom_score_adj = if value == OOM_DISABLE {
            OOM_SCORE_ADJ_MIN
        } else {
            if !(OOM_ADJUST_MIN..=OOM_ADJUST_MAX).contains(&value) {
                return error!(EINVAL);
            }
            let fraction = (value - OOM_ADJUST_MIN) / (OOM_ADJUST_MAX - OOM_ADJUST_MIN);
            fraction * (OOM_SCORE_ADJ_MAX - OOM_SCORE_ADJ_MIN) + OOM_SCORE_ADJ_MIN
        };
        if !current_task.creds().has_capability(CAP_SYS_RESOURCE) {
            return error!(EPERM);
        }
        let task = Task::from_weak(&self.0)?;
        task.write().oom_score_adj = oom_score_adj;
        Ok(())
    }

    fn read(&self, _current_task: &CurrentTask) -> Result<Cow<'_, [u8]>, Errno> {
        let task = Task::from_weak(&self.0)?;
        let oom_score_adj = task.read().oom_score_adj;
        let oom_adj = if oom_score_adj == OOM_SCORE_ADJ_MIN {
            OOM_DISABLE
        } else {
            let fraction =
                (oom_score_adj - OOM_SCORE_ADJ_MIN) / (OOM_SCORE_ADJ_MAX - OOM_SCORE_ADJ_MIN);
            fraction * (OOM_ADJUST_MAX - OOM_ADJUST_MIN) + OOM_ADJUST_MIN
        };
        Ok(serialize_i32_file(oom_adj).into())
    }
}

struct OomScoreAdjFile(WeakRef<Task>);

impl OomScoreAdjFile {
    fn new_node(task: WeakRef<Task>) -> impl FsNodeOps {
        BytesFile::new_node(Self(task))
    }
}

impl BytesFileOps for OomScoreAdjFile {
    fn write(&self, current_task: &CurrentTask, data: Vec<u8>) -> Result<(), Errno> {
        let value = parse_i32_file(&data)?;
        if !(OOM_SCORE_ADJ_MIN..=OOM_SCORE_ADJ_MAX).contains(&value) {
            return error!(EINVAL);
        }
        if !current_task.creds().has_capability(CAP_SYS_RESOURCE) {
            return error!(EPERM);
        }
        let task = Task::from_weak(&self.0)?;
        task.write().oom_score_adj = value;
        Ok(())
    }

    fn read(&self, _current_task: &CurrentTask) -> Result<Cow<'_, [u8]>, Errno> {
        let task = Task::from_weak(&self.0)?;
        let oom_score_adj = task.read().oom_score_adj;
        Ok(serialize_i32_file(oom_score_adj).into())
    }
}
