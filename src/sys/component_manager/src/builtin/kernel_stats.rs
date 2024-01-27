// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::builtin::capability::BuiltinCapability,
    ::routing::capability_source::InternalCapability,
    anyhow::Error,
    async_trait::async_trait,
    cm_rust::CapabilityName,
    fidl_fuchsia_kernel as fkernel,
    fuchsia_async::DurationExt as _,
    fuchsia_zircon::{self as zx, Resource},
    futures::prelude::*,
    lazy_static::lazy_static,
    std::sync::Arc,
};

lazy_static! {
    static ref KERNEL_STATS_CAPABILITY_NAME: CapabilityName = "fuchsia.kernel.Stats".into();
}

/// An implementation of the `fuchsia.kernel.Stats` protocol.
pub struct KernelStats {
    resource: Resource,
}

impl KernelStats {
    /// `resource` must be the info resource.
    pub fn new(resource: Resource) -> Arc<Self> {
        Arc::new(Self { resource })
    }
}

#[async_trait]
impl BuiltinCapability for KernelStats {
    const NAME: &'static str = "KernelStats";
    type Marker = fkernel::StatsMarker;

    async fn serve(self: Arc<Self>, mut stream: fkernel::StatsRequestStream) -> Result<(), Error> {
        while let Some(stats_request) = stream.try_next().await? {
            match stats_request {
                fkernel::StatsRequest::GetMemoryStats { responder } => {
                    let mem_stats = &self.resource.mem_stats()?;
                    let stats = fkernel::MemoryStats {
                        total_bytes: Some(mem_stats.total_bytes),
                        free_bytes: Some(mem_stats.free_bytes),
                        wired_bytes: Some(mem_stats.wired_bytes),
                        total_heap_bytes: Some(mem_stats.total_heap_bytes),
                        free_heap_bytes: Some(mem_stats.free_heap_bytes),
                        vmo_bytes: Some(mem_stats.vmo_bytes),
                        mmu_overhead_bytes: Some(mem_stats.mmu_overhead_bytes),
                        ipc_bytes: Some(mem_stats.ipc_bytes),
                        other_bytes: Some(mem_stats.other_bytes),
                        ..Default::default()
                    };
                    responder.send(&stats)?;
                }
                fkernel::StatsRequest::GetMemoryStatsExtended { responder } => {
                    let mem_stats_extended = &self.resource.mem_stats_extended()?;
                    let stats = fkernel::MemoryStatsExtended {
                        total_bytes: Some(mem_stats_extended.total_bytes),
                        free_bytes: Some(mem_stats_extended.free_bytes),
                        wired_bytes: Some(mem_stats_extended.wired_bytes),
                        total_heap_bytes: Some(mem_stats_extended.total_heap_bytes),
                        free_heap_bytes: Some(mem_stats_extended.free_heap_bytes),
                        vmo_bytes: Some(mem_stats_extended.vmo_bytes),
                        vmo_pager_total_bytes: Some(mem_stats_extended.vmo_pager_total_bytes),
                        vmo_pager_newest_bytes: Some(mem_stats_extended.vmo_pager_newest_bytes),
                        vmo_pager_oldest_bytes: Some(mem_stats_extended.vmo_pager_oldest_bytes),
                        vmo_discardable_locked_bytes: Some(
                            mem_stats_extended.vmo_discardable_locked_bytes,
                        ),
                        vmo_discardable_unlocked_bytes: Some(
                            mem_stats_extended.vmo_discardable_unlocked_bytes,
                        ),
                        mmu_overhead_bytes: Some(mem_stats_extended.mmu_overhead_bytes),
                        ipc_bytes: Some(mem_stats_extended.ipc_bytes),
                        other_bytes: Some(mem_stats_extended.other_bytes),
                        ..Default::default()
                    };
                    responder.send(&stats)?;
                }
                fkernel::StatsRequest::GetCpuStats { responder } => {
                    let cpu_stats = &self.resource.cpu_stats()?;
                    let mut per_cpu_stats: Vec<fkernel::PerCpuStats> =
                        Vec::with_capacity(cpu_stats.len());
                    for cpu_stat in cpu_stats.iter() {
                        per_cpu_stats.push(fkernel::PerCpuStats {
                            cpu_number: Some(cpu_stat.cpu_number),
                            flags: Some(cpu_stat.flags),
                            idle_time: Some(cpu_stat.idle_time),
                            reschedules: Some(cpu_stat.reschedules),
                            context_switches: Some(cpu_stat.context_switches),
                            irq_preempts: Some(cpu_stat.irq_preempts),
                            yields: Some(cpu_stat.yields),
                            ints: Some(cpu_stat.ints),
                            timer_ints: Some(cpu_stat.timer_ints),
                            timers: Some(cpu_stat.timers),
                            page_faults: Some(cpu_stat.page_faults),
                            exceptions: Some(cpu_stat.exceptions),
                            syscalls: Some(cpu_stat.syscalls),
                            reschedule_ipis: Some(cpu_stat.reschedule_ipis),
                            generic_ipis: Some(cpu_stat.generic_ipis),
                            ..Default::default()
                        });
                    }
                    let mut stats = fkernel::CpuStats {
                        actual_num_cpus: per_cpu_stats.len() as u64,
                        per_cpu_stats: Some(per_cpu_stats),
                    };
                    responder.send(&mut stats)?;
                }
                fkernel::StatsRequest::GetCpuLoad { duration, responder } => {
                    if duration <= 0 {
                        return Err(anyhow::anyhow!("Duration must be greater than 0"));
                    }

                    // Record `start_time` before the first stats query, and `end_time` *after* the
                    // second stats query completes. This ensures the "total time" (`end_time` -
                    // `start_time`) will never be less than the duration spanned by `start_stats`
                    // to `end_stats`, which would be invalid.
                    let start_time = fuchsia_async::Time::now();
                    let start_stats = self.resource.cpu_stats()?;
                    fuchsia_async::Timer::new(zx::Duration::from_nanos(duration).after_now()).await;
                    let end_stats = self.resource.cpu_stats()?;
                    let end_time = fuchsia_async::Time::now();

                    let loads = calculate_cpu_loads(start_time, start_stats, end_time, end_stats);
                    responder.send(&loads)?;
                }
            }
        }
        Ok(())
    }

    fn matches_routed_capability(&self, capability: &InternalCapability) -> bool {
        capability.matches_protocol(&KERNEL_STATS_CAPABILITY_NAME)
    }
}

/// Uses start / end times and corresponding PerCpuStats to calculate and return a vector of per-CPU
/// load values as floats in the range 0.0 - 100.0.
fn calculate_cpu_loads(
    start_time: fuchsia_async::Time,
    start_stats: Vec<zx::PerCpuStats>,
    end_time: fuchsia_async::Time,
    end_stats: Vec<zx::PerCpuStats>,
) -> Vec<f32> {
    let elapsed_time = (end_time - start_time).into_nanos();
    start_stats
        .iter()
        .zip(end_stats.iter())
        .map(|(start, end)| {
            let busy_time = elapsed_time - (end.idle_time - start.idle_time);
            let load_pct = busy_time as f64 / elapsed_time as f64 * 100.0;
            load_pct as f32
        })
        .collect::<Vec<f32>>()
}

#[cfg(test)]
mod tests {
    use {
        super::*, fidl_fuchsia_boot as fboot, fuchsia_async as fasync,
        fuchsia_component::client::connect_to_protocol, zx::DurationNum as _,
    };

    async fn get_root_resource() -> Result<Resource, Error> {
        let root_resource_provider = connect_to_protocol::<fboot::RootResourceMarker>()?;
        let root_resource_handle = root_resource_provider.get().await?;
        Ok(Resource::from(root_resource_handle))
    }

    enum OnError {
        Panic,
        Ignore,
    }

    async fn serve_kernel_stats(on_error: OnError) -> Result<fkernel::StatsProxy, Error> {
        let root_resource = get_root_resource().await?;

        let (proxy, stream) = fidl::endpoints::create_proxy_and_stream::<fkernel::StatsMarker>()?;
        fasync::Task::local(KernelStats::new(root_resource).serve(stream).unwrap_or_else(
            move |e| match on_error {
                OnError::Panic => panic!("Error while serving kernel stats: {}", e),
                _ => {}
            },
        ))
        .detach();
        Ok(proxy)
    }

    #[fuchsia::test]
    async fn get_mem_stats() -> Result<(), Error> {
        let kernel_stats_provider = serve_kernel_stats(OnError::Panic).await?;
        let mem_stats = kernel_stats_provider.get_memory_stats().await?;

        assert!(mem_stats.total_bytes.unwrap() > 0);
        assert!(mem_stats.total_heap_bytes.unwrap() > 0);

        Ok(())
    }

    #[fuchsia::test]
    async fn get_mem_stats_extended() -> Result<(), Error> {
        let kernel_stats_provider = serve_kernel_stats(OnError::Panic).await?;
        let mem_stats_extended = kernel_stats_provider.get_memory_stats_extended().await?;

        assert!(mem_stats_extended.total_bytes.unwrap() > 0);
        assert!(mem_stats_extended.total_heap_bytes.unwrap() > 0);

        Ok(())
    }

    #[fuchsia::test]
    async fn get_cpu_stats() -> Result<(), Error> {
        let kernel_stats_provider = serve_kernel_stats(OnError::Panic).await?;
        let cpu_stats = kernel_stats_provider.get_cpu_stats().await?;
        let actual_num_cpus = cpu_stats.actual_num_cpus;
        assert!(actual_num_cpus > 0);
        let per_cpu_stats = cpu_stats.per_cpu_stats.unwrap();

        let mut idle_time_sum = 0;
        let mut syscalls_sum = 0;

        for per_cpu_stat in per_cpu_stats.iter() {
            idle_time_sum += per_cpu_stat.idle_time.unwrap();
            syscalls_sum += per_cpu_stat.syscalls.unwrap();
        }

        assert!(idle_time_sum > 0);
        assert!(syscalls_sum > 0);

        Ok(())
    }

    #[fuchsia::test]
    async fn get_cpu_load_invalid_duration() {
        let kernel_stats_provider = serve_kernel_stats(OnError::Ignore).await.unwrap();

        // The server should close the channel when it receives an invalid argument
        assert_matches::assert_matches!(
            kernel_stats_provider.get_cpu_load(0).await,
            Err(fidl::Error::ClientChannelClosed { .. })
        );
    }

    #[fuchsia::test]
    async fn get_cpu_load() -> Result<(), Error> {
        let kernel_stats_provider = serve_kernel_stats(OnError::Panic).await?;
        let cpu_loads = kernel_stats_provider.get_cpu_load(1.seconds().into_nanos()).await?;

        assert!(
            cpu_loads.iter().all(|l| l > &0.0 && l <= &100.0),
            "Invalid CPU load value (expected range 0.0 - 100.0, received {:?}",
            cpu_loads
        );

        Ok(())
    }

    // Takes a vector of CPU loads and generates the necessary parameters that can be fed into
    // `calculate_cpu_loads` to result in those load calculations.
    fn parameters_for_expected_cpu_loads(
        cpu_loads: Vec<f32>,
    ) -> (fuchsia_async::Time, Vec<zx::PerCpuStats>, fuchsia_async::Time, Vec<zx::PerCpuStats>)
    {
        let start_time = fuchsia_async::Time::from_nanos(0);
        let end_time = fuchsia_async::Time::from_nanos(1000000000);

        let (start_stats, end_stats) = std::iter::repeat(zx::PerCpuStats::default())
            .zip(cpu_loads.into_iter().map(|load| {
                let end_time_f32 = end_time.into_nanos() as f32;
                let idle_time = (end_time_f32 - (load / 100.0 * end_time_f32)) as i64;
                zx::PerCpuStats { idle_time, ..zx::PerCpuStats::default() }
            }))
            .unzip();

        (start_time, start_stats, end_time, end_stats)
    }

    #[fuchsia::test]
    fn test_calculate_cpu_loads() -> Result<(), Error> {
        // CPU0 loaded to 75%
        let (start_time, start_stats, end_time, end_stats) =
            parameters_for_expected_cpu_loads(vec![75.0, 0.0]);
        assert_eq!(
            calculate_cpu_loads(start_time, start_stats, end_time, end_stats),
            vec![75.0, 0.0]
        );

        // CPU1 loaded to 75%
        let (start_time, start_stats, end_time, end_stats) =
            parameters_for_expected_cpu_loads(vec![0.0, 75.0]);
        assert_eq!(
            calculate_cpu_loads(start_time, start_stats, end_time, end_stats),
            vec![0.0, 75.0]
        );

        Ok(())
    }
}
