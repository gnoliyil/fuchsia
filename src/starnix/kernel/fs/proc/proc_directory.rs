// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::pid_directory::*;
use super::sysctl::*;

use crate::auth::FsCred;
use crate::fs::buffers::{InputBuffer, OutputBuffer};
use crate::fs::*;
use crate::logging::{log_error, not_implemented};
use crate::task::*;
use crate::types::*;
use fuchsia_component::client::connect_channel_to_protocol;
use fuchsia_zircon as zx;
use once_cell::sync::OnceCell;

use maplit::btreemap;
use std::collections::BTreeMap;
use std::sync::Arc;
use std::sync::Weak;
use std::time::SystemTime;

/// `ProcDirectory` represents the top-level directory in `procfs`.
///
/// It contains, for example, a directory for each running task, named after the task's pid.
///
/// It also contains a special symlink, `self`, which targets the task directory for the task
/// that reads the symlink.
pub struct ProcDirectory {
    /// The kernel that this directory is associated with. This is used to populate the
    /// directory contents on-demand.
    kernel: Weak<Kernel>,

    /// A map that stores all the nodes that aren't task directories.
    nodes: BTreeMap<&'static FsStr, FsNodeHandle>,
}

impl ProcDirectory {
    /// Returns a new `ProcDirectory` exposing information about `kernel`.
    pub fn new(fs: &FileSystemHandle, kernel: Weak<Kernel>) -> Arc<ProcDirectory> {
        // TODO: Move somewhere where it can be shared with other consumers.
        let kernel_stats = Arc::new(KernelStatsStore::default());

        let nodes = btreemap! {
            &b"cmdline"[..] => {
                let cmdline = kernel.upgrade().unwrap().cmdline.clone();
                fs.create_node(BytesFile::new_node(cmdline), mode!(IFREG, 0o444), FsCred::root())
            },
            &b"self"[..] => SelfSymlink::new_node(fs),
            &b"thread-self"[..] => ThreadSelfSymlink::new_node(fs),
            &b"meminfo"[..] =>
                fs.create_node(MeminfoFile::new_node(&kernel_stats), mode!(IFREG, 0o444), FsCred::root()),
            // Fake kmsg as being empty.
            &b"kmsg"[..] =>
                fs.create_node(SimpleFileNode::new(|| Ok(ProcKmsgFile)), mode!(IFREG, 0o100), FsCred::root()),
            &b"mounts"[..] => MountsSymlink::new_node(fs),
            // File must exist to pass the CgroupsAvailable check, which is a little bit optional
            // for init but not optional for a lot of the system!
            &b"cgroups"[..] => fs.create_node(BytesFile::new_node(vec![]), mode!(IFREG, 0o444), FsCred::root()),
            &b"stat"[..] =>  fs.create_node(StatFile::new_node(&kernel_stats), mode!(IFREG, 0o444), FsCred::root()),
            &b"sys"[..] => sysctl_directory(fs),
            &b"pressure"[..] => pressure_directory(fs),
            &b"net"[..] => net_directory(fs),
            &b"uptime"[..] =>
                fs.create_node(UptimeFile::new_node(&kernel_stats), mode!(IFREG, 0o444), FsCred::root()),
            &b"loadavg"[..] =>
                fs.create_node(LoadavgFile::new_node(&kernel), mode!(IFREG, 0o444), FsCred::root()),
        };

        Arc::new(ProcDirectory { kernel, nodes })
    }
}

impl FsNodeOps for Arc<ProcDirectory> {
    fn create_file_ops(
        &self,
        _node: &FsNode,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        Ok(Box::new(self.clone()))
    }

    fn lookup(
        &self,
        node: &FsNode,
        _current_task: &CurrentTask,
        name: &FsStr,
    ) -> Result<FsNodeHandle, Errno> {
        match self.nodes.get(name) {
            Some(node) => Ok(Arc::clone(node)),
            None => {
                let pid_string = std::str::from_utf8(name).map_err(|_| errno!(ENOENT))?;
                let pid = pid_string.parse::<pid_t>().map_err(|_| errno!(ENOENT))?;
                if let Some(task) = self.kernel.upgrade().unwrap().pids.read().get_task(pid) {
                    Ok(pid_directory(&node.fs(), &task))
                } else {
                    error!(ENOENT)
                }
            }
        }
    }
}

impl FileOps for Arc<ProcDirectory> {
    fileops_impl_directory!();

    fn seek(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        current_offset: off_t,
        new_offset: off_t,
        whence: SeekOrigin,
    ) -> Result<off_t, Errno> {
        unbounded_seek(current_offset, new_offset, whence)
    }

    fn readdir(
        &self,
        file: &FileObject,
        current_task: &CurrentTask,
        sink: &mut dyn DirentSink,
    ) -> Result<(), Errno> {
        emit_dotdot(file, sink)?;

        // Iterate through all the named entries (i.e., non "task directories") and add them to
        // the sink. Subtract 2 from the offset, to account for `.` and `..`.
        for (name, node) in self.nodes.iter().skip((sink.offset() - 2) as usize) {
            sink.add(
                node.inode_num,
                sink.offset() + 1,
                DirectoryEntryType::from_mode(node.info().mode),
                name,
            )?;
        }

        // Add 2 to the number of non-"task directories", to account for `.` and `..`.
        let pid_offset = (self.nodes.len() + 2) as i32;

        // Adjust the offset to account for the other nodes in the directory.
        let adjusted_offset = (sink.offset() - pid_offset as i64) as usize;
        // Sort the pids, to keep the traversal order consistent.
        let mut pids = current_task.thread_group.kernel.pids.read().process_ids();
        pids.sort();

        // The adjusted offset is used to figure out which task directories are to be listed.
        if let Some(start) = pids.iter().position(|pid| *pid as usize >= adjusted_offset) {
            for pid in &pids[start..] {
                // TODO: Figure out if this inode number is fine, given the content of the task
                // directories.
                let inode_num = file.fs.next_inode_num();
                let name = format!("{pid}");

                // The + 1 is to set the offset to the next possible pid for subsequent reads.
                let next_offset = (*pid + pid_offset + 1) as i64;
                sink.add(inode_num, next_offset, DirectoryEntryType::DIR, name.as_bytes())?;
            }
        }

        Ok(())
    }
}

struct ProcKmsgFile;

impl FileOps for ProcKmsgFile {
    fileops_impl_seekless!();

    fn wait_async(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        waiter: &Waiter,
        _events: FdEvents,
        _handler: EventHandler,
    ) -> Option<WaitCanceler> {
        Some(waiter.fake_wait())
    }

    fn query_events(&self, _current_task: &CurrentTask) -> FdEvents {
        FdEvents::empty()
    }

    fn read(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        _offset: usize,
        _data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        not_implemented!("ProcKmsgFile.read() is stubbed.");
        Waiter::new().wait_until(current_task, zx::Time::INFINITE)?;
        error!(EAGAIN)
    }

    fn write(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        _data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        error!(EIO)
    }
}

/// A node that represents a symlink to `proc/<pid>` where <pid> is the pid of the task that
/// reads the `proc/self` symlink.
struct SelfSymlink;

impl SelfSymlink {
    fn new_node(fs: &FileSystemHandle) -> FsNodeHandle {
        fs.create_node(Self, mode!(IFLNK, 0o777), FsCred::root())
    }
}

impl FsNodeOps for SelfSymlink {
    fs_node_impl_symlink!();

    fn readlink(&self, _node: &FsNode, current_task: &CurrentTask) -> Result<SymlinkTarget, Errno> {
        Ok(SymlinkTarget::Path(current_task.get_pid().to_string().into_bytes()))
    }
}

/// A node that represents a symlink to `proc/<pid>/task/<tid>` where <pid> and <tid> are derived
/// from the task reading the symlink.
struct ThreadSelfSymlink;

impl ThreadSelfSymlink {
    fn new_node(fs: &FileSystemHandle) -> FsNodeHandle {
        fs.create_node(Self, mode!(IFLNK, 0o777), FsCred::root())
    }
}

impl FsNodeOps for ThreadSelfSymlink {
    fs_node_impl_symlink!();

    fn readlink(&self, _node: &FsNode, current_task: &CurrentTask) -> Result<SymlinkTarget, Errno> {
        Ok(SymlinkTarget::Path(
            format!("{}/task/{}", current_task.get_pid(), current_task.get_tid()).into_bytes(),
        ))
    }
}

/// A node that represents a link to `self/mounts`.
struct MountsSymlink;

impl MountsSymlink {
    fn new_node(fs: &FileSystemHandle) -> FsNodeHandle {
        fs.create_node(Self, mode!(IFLNK, 0o777), FsCred::root())
    }
}

impl FsNodeOps for MountsSymlink {
    fs_node_impl_symlink!();

    fn readlink(
        &self,
        _node: &FsNode,
        _current_task: &CurrentTask,
    ) -> Result<SymlinkTarget, Errno> {
        Ok(SymlinkTarget::Path(b"self/mounts".to_vec()))
    }
}

/// Creates the /proc/pressure directory. https://docs.kernel.org/accounting/psi.html
fn pressure_directory(fs: &FileSystemHandle) -> FsNodeHandle {
    let mut dir = StaticDirectoryBuilder::new(fs);
    dir.entry(b"memory", PressureFile::new_node(), mode!(IFREG, 0o666));
    dir.build()
}

struct PressureFileSource;
impl DynamicFileSource for PressureFileSource {
    fn generate(&self, sink: &mut DynamicFileBuf) -> Result<(), Errno> {
        writeln!(sink, "some avg10={:.2} avg60={:.2} avg300={:.2} total={}", 0, 0, 0, 0)?;
        writeln!(sink, "full avg10={:.2} avg60={:.2} avg300={:.2} total={}", 0, 0, 0, 0)?;
        Ok(())
    }
}

struct PressureFile(DynamicFile<PressureFileSource>);
impl PressureFile {
    pub fn new_node() -> impl FsNodeOps {
        SimpleFileNode::new(move || Ok(Self(DynamicFile::new(PressureFileSource {}))))
    }
}

impl FileOps for PressureFile {
    fileops_impl_delegate_read_and_seek!(self, self.0);

    /// Pressure notifications are configured by writing to the file.
    fn write(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        // Ignore the request for now.
        not_implemented!("pressure notification setup");
        Ok(data.drain())
    }

    fn wait_async(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        waiter: &Waiter,
        _events: FdEvents,
        _handler: EventHandler,
    ) -> Option<WaitCanceler> {
        Some(waiter.fake_wait())
    }

    fn query_events(&self, _current_task: &CurrentTask) -> FdEvents {
        FdEvents::empty()
    }
}

#[derive(Default)]
struct KernelStatsStore(OnceCell<fidl_fuchsia_kernel::StatsSynchronousProxy>);

impl KernelStatsStore {
    pub fn get(&self) -> &fidl_fuchsia_kernel::StatsSynchronousProxy {
        self.0.get_or_init(|| {
            let (client_end, server_end) = zx::Channel::create();
            connect_channel_to_protocol::<fidl_fuchsia_kernel::StatsMarker>(server_end)
                .expect("Failed to connect to fuchsia.kernel.Stats.");
            fidl_fuchsia_kernel::StatsSynchronousProxy::new(client_end)
        })
    }
}

#[derive(Clone)]
struct MeminfoFile {
    kernel_stats: Arc<KernelStatsStore>,
}
impl MeminfoFile {
    pub fn new_node(kernel_stats: &Arc<KernelStatsStore>) -> impl FsNodeOps {
        DynamicFile::new_node(Self { kernel_stats: kernel_stats.clone() })
    }
}
impl DynamicFileSource for MeminfoFile {
    fn generate(&self, sink: &mut DynamicFileBuf) -> Result<(), Errno> {
        let memstats =
            self.kernel_stats.get().get_memory_stats_extended(zx::Time::INFINITE).map_err(|e| {
                log_error!("FIDL error getting memory stats: {e}");
                errno!(EIO)
            })?;
        writeln!(sink, "MemTotal:\t {} kB", memstats.total_bytes.unwrap_or_default() / 1024)?;
        writeln!(sink, "MemFree:\t {} kB", memstats.free_bytes.unwrap_or_default() / 1024)?;
        writeln!(
            sink,
            "MemAvailable:\t {} kB",
            (memstats.free_bytes.unwrap_or_default()
                + memstats.vmo_discardable_unlocked_bytes.unwrap_or_default())
                / 1024
        )?;
        Ok(())
    }
}

#[derive(Clone)]
struct UptimeFile {
    kernel_stats: Arc<KernelStatsStore>,
}

impl UptimeFile {
    pub fn new_node(kernel_stats: &Arc<KernelStatsStore>) -> impl FsNodeOps {
        DynamicFile::new_node(Self { kernel_stats: kernel_stats.clone() })
    }
}

impl DynamicFileSource for UptimeFile {
    fn generate(&self, sink: &mut DynamicFileBuf) -> Result<(), Errno> {
        let uptime = (zx::Time::get_monotonic() - zx::Time::ZERO).into_seconds_f64();

        // Fetch CPU stats from `fuchsia.kernel.Stats` to calculate idle time.
        let cpu_stats =
            self.kernel_stats.get().get_cpu_stats(zx::Time::INFINITE).map_err(|_| errno!(EIO))?;
        let per_cpu_stats = cpu_stats.per_cpu_stats.unwrap_or(vec![]);
        let idle_time = per_cpu_stats.iter().map(|s| s.idle_time.unwrap_or(0)).sum();
        let idle_time = zx::Duration::from_nanos(idle_time).into_seconds_f64();

        writeln!(sink, "{:.2} {:.2}", uptime, idle_time)?;

        Ok(())
    }
}

#[derive(Clone)]
struct StatFile {
    kernel_stats: Arc<KernelStatsStore>,
}
impl StatFile {
    pub fn new_node(kernel_stats: &Arc<KernelStatsStore>) -> impl FsNodeOps {
        DynamicFile::new_node(Self { kernel_stats: kernel_stats.clone() })
    }
}
impl DynamicFileSource for StatFile {
    fn generate(&self, sink: &mut DynamicFileBuf) -> Result<(), Errno> {
        let uptime = zx::Time::get_monotonic() - zx::Time::ZERO;

        let cpu_stats =
            self.kernel_stats.get().get_cpu_stats(zx::Time::INFINITE).map_err(|_| errno!(EIO))?;

        // Number of values reported per CPU. See `get_cpu_stats_row` below for the list of values.
        const NUM_CPU_STATS: usize = 10;

        let get_cpu_stats_row = |cpu_stats: &fidl_fuchsia_kernel::PerCpuStats| {
            let idle = zx::Duration::from_nanos(cpu_stats.idle_time.unwrap_or(0));

            // Assume that all non-idle time is spent in user mode.
            let user = uptime - idle;

            // Zircon currently reports only number of various interrupts, but not the time spent
            // handling them. Return zeros.
            let nice: u64 = 0;
            let system: u64 = 0;
            let iowait: u64 = 0;
            let irq: u64 = 0;
            let softirq: u64 = 0;
            let steal: u64 = 0;
            let quest: u64 = 0;
            let quest_nice: u64 = 0;

            [
                duration_to_scheduler_clock(user) as u64,
                nice,
                system,
                duration_to_scheduler_clock(idle) as u64,
                iowait,
                irq,
                softirq,
                steal,
                quest,
                quest_nice,
            ]
        };
        let per_cpu_stats = cpu_stats.per_cpu_stats.unwrap_or(vec![]);
        let mut cpu_total_row = [0u64; NUM_CPU_STATS];
        for row in per_cpu_stats.iter().map(get_cpu_stats_row) {
            for (i, value) in row.iter().enumerate() {
                cpu_total_row[i] += value
            }
        }

        writeln!(sink, "cpu {}", cpu_total_row.map(|n| n.to_string()).join(" "))?;
        for (i, row) in per_cpu_stats.iter().map(get_cpu_stats_row).enumerate() {
            writeln!(sink, "cpu{} {}", i, row.map(|n| n.to_string()).join(" "))?;
        }

        let context_switches: u64 =
            per_cpu_stats.iter().map(|s| s.context_switches.unwrap_or(0)).sum();
        writeln!(sink, "ctxt {}", context_switches)?;

        let num_interrupts: u64 = per_cpu_stats.iter().map(|s| s.ints.unwrap_or(0)).sum();
        writeln!(sink, "intr {}", num_interrupts)?;

        let epoch_time = zx::Duration::from(
            SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).unwrap_or_default(),
        );
        let boot_time_epoch = epoch_time - uptime;
        writeln!(sink, "btime {}", boot_time_epoch.into_seconds())?;

        Ok(())
    }
}

#[derive(Clone)]
struct LoadavgFile(Weak<Kernel>);
impl LoadavgFile {
    pub fn new_node(kernel: &Weak<Kernel>) -> impl FsNodeOps {
        DynamicFile::new_node(Self(kernel.clone()))
    }
}
impl DynamicFileSource for LoadavgFile {
    fn generate(&self, sink: &mut DynamicFileBuf) -> Result<(), Errno> {
        let (runnable_tasks, existing_tasks, last_pid) = {
            let kernel = self.0.upgrade().ok_or(errno!(EIO))?;
            let pid_table = kernel.pids.read();

            let curr_tids = pid_table.task_ids();
            let mut runnable_tasks = 0;
            for pid in &curr_tids {
                if let Some(task) = pid_table.get_task(*pid) {
                    if task.state_code() == TaskStateCode::Running {
                        runnable_tasks += 1;
                    }
                };
            }

            let existing_tasks = pid_table.process_ids().len() + curr_tids.len();
            (runnable_tasks, existing_tasks, pid_table.last_pid())
        };

        // TODO: Collect and report load stats.
        writeln!(sink, "0.50 0.50 0.50 {}/{} {}", runnable_tasks, existing_tasks, last_pid)?;
        Ok(())
    }
}
