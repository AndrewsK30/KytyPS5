#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include <chrono>
#include <cinttypes>
#include <cstring>

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/performanceDiagnostics.h"

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick) {
	const auto site = PerformanceDiagnostics::Enabled() ? PerformanceDiagnostics::EffectiveWaitSite() :
	                                                      PerformanceDiagnostics::WaitSite::Unknown;
	if (IsFree(tick)) {
		PerformanceDiagnostics::RecordWait(site, false, 0);
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		PerformanceDiagnostics::RecordWait(site, false, 0);
		return;
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	// Ticks submitted but not yet completed when the wait starts: how far the GPU runs behind.
	const auto lag = CurrentTick() - 1 - KnownGpuTick();
	PerformanceDiagnostics::Add(PerformanceDiagnostics::Metric::SemaphoreWait);
	vk::Result result;
	{
		// One zone per wait, named by call site, so Tracy separates the sites without per-packet zones.
		static constexpr tracy::SourceLocationData wait_location {"Wait", TracyFunction, TracyFile,
		                                                          static_cast<uint32_t>(__LINE__),
		                                                          0xC04040};
		const bool        profiling = tracy::ProfilerAvailable();
		tracy::ScopedZone zone(&wait_location, TRACY_CALLSTACK, profiling);
		if (profiling) {
			const char* name = PerformanceDiagnostics::WaitSiteName(site);
			zone.Name(name, std::strlen(name));
		}
		PerformanceDiagnostics::CategoryScope wait_category(PerformanceDiagnostics::Category::Wait);
		const auto start = PerformanceDiagnostics::Enabled() ? std::chrono::steady_clock::now() :
		                                                       std::chrono::steady_clock::time_point {};
		PerformanceDiagnostics::Scope timing(PerformanceDiagnostics::Metric::SemaphoreWaitNs,
		                                    PerformanceDiagnostics::SemaphoreWaitMetric());
		result = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
		if (PerformanceDiagnostics::Enabled()) {
			PerformanceDiagnostics::RecordWait(
			    site, true,
			    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			        std::chrono::steady_clock::now() - start).count()),
			    lag);
		}
	}
	if (result != vk::Result::eSuccess) {
		if (result == vk::Result::eErrorDeviceLost) {
			DumpDeviceLossDiagnostics(m_graphics);
		}
		EXIT("MasterSemaphore: wait for tick %" PRIu64 " failed: %s (gpu tick %" PRIu64 ")\n", tick,
		     vk::to_string(result).c_str(), m_gpu_tick.load(std::memory_order_acquire));
	}
	Refresh();
}

} // namespace Libs::Graphics
