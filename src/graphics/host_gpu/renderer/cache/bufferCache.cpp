#include "graphics/host_gpu/renderer/cache/bufferCache.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/performanceDiagnostics.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <chrono>
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;

} // namespace

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging_buffer.Size());
		const auto offset = m_staging_buffer.Copy(bytes, chunk, 4);
		buffer.CopyFrom(m_scheduler.Current(), m_staging_buffer, offset, buffer.Offset(address),
		                chunk, vk::AccessFlagBits::eHostWrite);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

void BufferCache::Register(BufferId id) {
	ChangeRegister<true>(id);
}

void BufferCache::Unregister(BufferId id) {
	ChangeRegister<false>(id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId id) {
	auto& buffer = m_slot_buffers[id];
	PageTable::PageRange pages {};
	EXIT_IF(!(GuestRange {buffer.CpuAddress(), buffer.Size()}.Valid()) ||
	        !PageTable::TryGetPageRange(buffer.CpuAddress(), buffer.Size(), pages));
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		if constexpr (insert) {
			m_page_table[page] = id;
		} else {
			m_page_table[page] = {};
		}
	}
	const auto size_pages = pages.last_exclusive - pages.first;
	const auto table_offset = PageIndex(buffer.CpuAddress()) * sizeof(vk::DeviceAddress);
	if constexpr (insert) {
		const auto [it, inserted] = m_buffers.emplace(buffer.CpuAddress(), id);
		(void)it;
		EXIT_IF(!inserted);
		m_total_used_memory += buffer.Size();
		g_cpu_dirty_epoch.fetch_add(1, std::memory_order_release);
		buffer.lru_id = m_lru_cache.Insert(id, LruClock());
		std::vector<vk::DeviceAddress> addresses;
		addresses.reserve(size_pages);
		for (uint64_t i = 0; i < size_pages; ++i) {
			addresses.push_back(buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS));
		}
		PerformanceDiagnostics::Add(PerformanceDiagnostics::Metric::BdaTableUpload);
		WriteDataBuffer(m_bda_pagetable_buffer, table_offset,
		                addresses.data(), addresses.size() * sizeof(vk::DeviceAddress));
	} else {
		const auto found = m_buffers.find(buffer.CpuAddress());
		EXIT_IF(found == m_buffers.end() || found->second != id);
		m_buffers.erase(found);
		EXIT_IF(buffer.Size() > m_total_used_memory);
		m_total_used_memory -= buffer.Size();
		m_lru_cache.Free(buffer.lru_id);
		m_bda_pagetable_buffer.Fill(table_offset,
		                            size_pages * sizeof(vk::DeviceAddress), 0);
		buffer.is_deleted = true;
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
	if (!buffer.is_deleted) {
		m_lru_cache.Touch(buffer.lru_id, LruClock());
	}
}

void BufferCache::DeleteBuffer(BufferId id) {
	if (IsBufferInvalid(id)) {
		return;
	}
	PerformanceDiagnostics::Add(PerformanceDiagnostics::Metric::BufferDelete);
	Unregister(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

bool BufferCache::DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	// One reservation cannot exceed the download ring, so a larger range goes in ring-sized
	// windows; a window that does not fit drains the ring before it is mapped.
	const auto capacity = m_download_buffer.Size();
	bool       any      = false;
	for (uint64_t offset = 0; offset < size; offset += capacity) {
		any |= DownloadBufferWindow(buffer, vaddr + offset, std::min(capacity, size - offset));
	}
	return any;
}

bool BufferCache::DownloadBufferWindow(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	bool                        inline_ok      = true;
	const auto                  buffer_address = buffer.CpuAddress();
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "buffer download");
		    std::unique_lock lock(m_dirty_ranges_mutex);
		    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    copies.emplace_back(start - buffer_address, total_size, end - start);
			    // Keep packed ranges on separate cache lines, as in shadPS4.
			    total_size += Common::AlignUp(end - start, 64);
			    // An older deferred download of these bytes could still land after an inline one.
		    if (m_downloading_ranges.Intersects(start, end - start)) { inline_ok = false; }
		    m_downloading_ranges.Add(start, end - start);
		    });
		    m_gpu_modified_ranges.Subtract(address, bytes);
	    });
	if (copies.empty()) {
		return false;
	}
	const auto capacity = m_download_buffer.Size();
	for (size_t first = 0; first < copies.size();) {
		const auto base       = copies[first].dstOffset;
		auto       last       = first;
		uint64_t   batch_size = 0;
		while (last < copies.size()) {
			const auto end = copies[last].dstOffset - base + Common::AlignUp(copies[last].size, 64);
			if (end > capacity) {
				break;
			}
			batch_size = end;
			last++;
		}
		EXIT_IF(last == first);
		std::vector<vk::BufferCopy> batch(copies.begin() + static_cast<std::ptrdiff_t>(first),
		                                  copies.begin() + static_cast<std::ptrdiff_t>(last));
		for (auto& copy: batch) {
			copy.dstOffset -= base;
		}
		DownloadBufferCopies(buffer, std::move(batch), batch_size, inline_ok);
		first = last;
	}
	return true;
}

void BufferCache::DownloadBufferCopies(Buffer& buffer, std::vector<vk::BufferCopy> copies,
                                       uint64_t total_size, bool inline_ok) {
	PerformanceDiagnostics::Add(PerformanceDiagnostics::Metric::BufferReadbackBytes, total_size);
	PerformanceDiagnostics::ReadbackBytes(total_size);
	const auto buffer_address = buffer.CpuAddress();
	if (PerformanceDiagnostics::HasReadbackRecord()) {
		for (const auto& copy: copies) {
			PerformanceDiagnostics::RecordDownload(buffer_address + copy.srcOffset, copy.size,
			                                      Common::AlignUp(copy.size, 64));
		}
	}
	auto [mapped, offset]     = m_download_buffer.Map(total_size, 64);
	std::unique_ptr<Buffer> temporary;
	if (mapped == nullptr) {
		temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		                                     vk::BufferUsageFlagBits::eTransferDst, total_size);
		mapped = temporary->Mapped().data();
	} else {
		m_download_buffer.Commit();
	}
	const auto& download = temporary ? *temporary : m_download_buffer;
	for (auto& copy: copies) {
		copy.dstOffset += offset;
	}
	if (inline_ok && TryDownloadInline(buffer, copies, temporary ? *temporary : m_download_buffer, mapped, offset, total_size)) {
		++m_inline_downloads;
		return;
	}
	++m_deferred_downloads;

	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto              native = command.Handle();
	vk::BufferMemoryBarrier before {};
	before.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = buffer.Handle();
	before.offset              = 0;
	before.size                = buffer.Size();
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &before, 0,
	                       nullptr);
	native.copyBuffer(buffer.Handle(), download.Handle(),
	                  static_cast<uint32_t>(copies.size()), copies.data());

	auto after          = before;
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	after.buffer        = download.Handle();
	after.offset        = offset;
	after.size          = total_size;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands |
	                           vk::PipelineStageFlagBits::eHost,
	                       {}, 0, nullptr, 1, &after, 0, nullptr);
	m_scheduler.DeferPriorityOperation([this, mapped, offset, total_size, buffer_address,
	                                    copies = std::move(copies), owner = std::move(temporary)] {
		(owner ? *owner : m_download_buffer).Invalidate(offset, total_size);
		for (const auto& copy: copies) {
			Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
			                                      mapped + (copy.dstOffset - offset), copy.size);
		}
		std::unique_lock lock(m_dirty_ranges_mutex);
		for (const auto& copy: copies) {
			m_downloading_ranges.Subtract(buffer_address + copy.srcOffset, copy.size);
		}
	});
}

// KYTY_READBACK_QUEUE=1: a guest readback whose GPU writes are all finished copies on the second queue
// and publishes before returning, instead of recording the copy behind the main queue's pending work.
static bool InlineReadbackEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_READBACK_QUEUE");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

bool BufferCache::TryDownloadInline(Buffer& buffer, std::span<const vk::BufferCopy> copies,
                                    Buffer& download, const uint8_t* mapped, uint64_t offset,
                                    uint64_t total_size) {
	if (!InlineReadbackEnabled() || m_graphics.readback_queue == nullptr ||
	    !GuestGpu::IsGpuThread()) {
		return false;
	}
	const auto buffer_address = buffer.CpuAddress();
	// The bytes are final once every GPU write to them and the buffer's own creation (a join copies
	// older contents in that command buffer) is behind the GPU. A tick the scheduler is still
	// recording is never finished, so only the common case (nothing pending) is taken here.
	uint64_t ready = buffer.created_tick;
	for (const auto& copy: copies) {
		ready = std::max(ready, LastGpuWriteTick(buffer_address + copy.srcOffset, copy.size));
	}
	if (ready >= m_scheduler.CurrentTick() || !m_scheduler.IsFree(ready)) {
		return false;
	}
	const auto device = m_graphics.device;
	if (!m_readback_pool) {
		vk::CommandPoolCreateInfo pool {};
		pool.queueFamilyIndex = m_graphics.queue_family;
		pool.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
		if (device.createCommandPool(&pool, nullptr, &m_readback_pool) != vk::Result::eSuccess) {
			m_readback_pool = nullptr;
			return false;
		}
		vk::CommandBufferAllocateInfo allocate {};
		allocate.commandPool        = m_readback_pool;
		allocate.level              = vk::CommandBufferLevel::ePrimary;
		allocate.commandBufferCount = 1;
		vk::FenceCreateInfo fence {};
		if (device.allocateCommandBuffers(&allocate, &m_readback_command) != vk::Result::eSuccess ||
		    device.createFence(&fence, nullptr, &m_readback_fence) != vk::Result::eSuccess) {
			return false;
		}
	}
	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	m_readback_command.reset();
	m_readback_command.begin(begin);
	m_readback_command.copyBuffer(buffer.Handle(), download.Handle(),
	                              static_cast<uint32_t>(copies.size()), copies.data());
	vk::BufferMemoryBarrier after {};
	after.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask       = vk::AccessFlagBits::eHostRead;
	after.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	after.buffer              = download.Handle();
	after.offset              = offset;
	after.size                = total_size;
	m_readback_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                                   vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1, &after, 0,
	                                   nullptr);
	m_readback_command.end();
	vk::SubmitInfo submit {};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &m_readback_command;
	if (m_graphics.readback_queue.submit(1, &submit, m_readback_fence) != vk::Result::eSuccess) {
		return false;
	}
	{
		PerformanceDiagnostics::Scope wait(PerformanceDiagnostics::Metric::ReadInlineNs);
		// The copy is tiny, so polling finishes sooner than the wakeup of a blocking wait; a long
		// one falls back to blocking so a busy GPU does not burn this thread.
		const auto spin_until = std::chrono::steady_clock::now() + std::chrono::microseconds(500);
		while (device.getFenceStatus(m_readback_fence) == vk::Result::eNotReady &&
		       std::chrono::steady_clock::now() < spin_until) {
		}
		(void)device.waitForFences(1, &m_readback_fence, VK_TRUE, UINT64_MAX);
		(void)device.resetFences(1, &m_readback_fence);
	}
	PerformanceDiagnostics::Add(PerformanceDiagnostics::Metric::ReadInline);
	download.Invalidate(offset, total_size);
	for (const auto& copy: copies) {
		Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
		                                      mapped + (copy.dstOffset - offset), copy.size);
	}
	std::unique_lock lock(m_dirty_ranges_mutex);
	for (const auto& copy: copies) {
		m_downloading_ranges.Subtract(buffer_address + copy.srcOffset, copy.size);
	}
	return true;
}
BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             BDA_PAGETABLE_SIZE),
      m_memory_tracker(page_manager),
      m_staging_buffer(graphics, scheduler, MemoryUsage::Upload, 512 * MiB),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, 64 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 64 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_texture_cache(texture_cache) {
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
	SetVulkanObjectNameF(m_graphics.device, m_bda_pagetable_buffer.Handle(),
	                     "BDA Page Table Buffer");
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	SetVulkanObjectNameF(m_graphics.device, GetBuffer(null_id).Handle(), "Kyty.NullBuffer");
	if (!m_graphics.CanReportMemoryUsage()) {
		return;
	}
	constexpr int64_t GiB              = 1024ll * 1024 * 1024;
	constexpr int64_t target_threshold = 8 * GiB;
	const auto        budget =
	    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
	const auto threshold = std::min(budget, target_threshold);
	const auto expected  = std::min(budget - 6 * threshold / 10, budget - GiB);
	const auto critical  = std::min(budget - 2 * threshold / 10, budget - GiB / 2);
	m_trigger_gc_memory  = static_cast<uint64_t>(std::max<int64_t>(expected, GiB));
	m_critical_gc_memory = static_cast<uint64_t>(std::max<int64_t>(critical, 2 * GiB));
}

BufferCache::~BufferCache() {
	if (m_readback_pool) {
		m_graphics.device.destroyFence(m_readback_fence, nullptr);
		m_graphics.device.destroyCommandPool(m_readback_pool, nullptr);
	}
	if (!m_gpu_modified_ranges.Empty()) {
		EXIT("BufferCache: destroyed with pending GPU-modified ranges\n");
	}
	for (const auto& [vaddr, id]: m_buffers) {
		(void)vaddr;
		const auto& buffer = m_slot_buffers[id];
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: destroyed with GPU-modified buffer\n");
		}
	}
	m_buffers.clear();
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid memory-invalidation range\n");
	}
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size] { ReadMemory(vaddr, size, true); });
}

bool BufferCache::WriteClean(uint64_t vaddr, const void* data, uint64_t size) {
	if (!GuestGpu::IsGpuThread() || size == 0 || !GuestRange {vaddr, size}.Valid() ||
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size)) {
		return false;
	}
	{
		// A download publishes the GPU's older value of these bytes when it lands.
		std::shared_lock lock(m_dirty_ranges_mutex);
		if (m_downloading_ranges.Intersects(vaddr, size)) {
			return false;
		}
	}

	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner == nullptr || !*owner || !m_slot_buffers[*owner].IsInBounds(vaddr, size) ||
	    !Libs::LibKernel::Memory::TryWriteBacking(vaddr, data, size)) {
		return false;
	}
	WriteDataBuffer(m_slot_buffers[*owner], vaddr, data, size);
	if (HasGpuDirtyBytes(vaddr, size)) {
		// Overwritten in full: guest memory holds the value the GPU copy will have.
		std::unique_lock lock(m_dirty_ranges_mutex);
		m_gpu_modified_ranges.Subtract(vaddr, size);
	}
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write) {
	using namespace PerformanceDiagnostics;
	const bool gpu_thread = GuestGpu::IsGpuThread();
	auto source = CurrentReadbackSource();
	if (source == ReadbackSource::Unknown) {
		source = gpu_thread ? (is_write ? ReadbackSource::GpuWrite : ReadbackSource::GpuRead) :
		                      (is_write ? ReadbackSource::CpuWrite : ReadbackSource::CpuRead);
	}
	Metric request_metric;
	switch (source) {
		case ReadbackSource::Metadata: request_metric = Metric::MetadataReadRequests; break;
		case ReadbackSource::GpuRead: request_metric = Metric::GpuReadRequests; break;
		case ReadbackSource::GpuWrite: request_metric = Metric::GpuWriteRequests; break;
		case ReadbackSource::CpuWrite: request_metric = Metric::CpuWriteRequests; break;
		default: request_metric = Metric::CpuReadRequests; break;
	}
	Add(request_metric);
	Add(Metric::ReadRequestedBytes, size);
	Scope timing(gpu_thread ? Metric::GpuReadSyncNs : Metric::CpuReadSyncNs);
	const bool diagnostics = Enabled();
	const bool sampled = SampleReadback();
	ReadbackRecord record;
	record.source = source;
	record.address = vaddr;
	record.size = size;
	const auto start_ns = diagnostics ? NowNs() : 0;
	record.start_ns = start_ns;
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	m_scheduler.Context().GetGpu().SendCommandSync([this, vaddr, size, is_write, source, diagnostics, sampled, start_ns, &record] {
		ReadbackScope attribution(source);
		ReadbackRecordScope trace(sampled ? &record : nullptr);
		const auto entered_ns = diagnostics ? NowNs() : 0;
		if (diagnostics) { Add(Metric::ReadDispatchNs, entered_ns - start_ns); }
		if (sampled) {
			record.dispatch_ns = entered_ns - start_ns;
			record.tick_begin = m_scheduler.CurrentTick();
			record.tick_end = record.tick_begin;
		}
		const auto prepared = [&] {
			if (diagnostics) {
				const auto ns = NowNs() - entered_ns;
				Add(Metric::ReadPrepareNs, ns);
				if (sampled) { record.prepare_ns = ns; }
			}
		};
		if (is_write && !IsRegionRegistered(vaddr, size)) {
			Add(Metric::ReadNoDownloadRequests);
			prepared();
			return;
		}
		auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];
		if (sampled) { record.buffer_address = buffer.CpuAddress(); record.buffer_size = buffer.Size(); }

		// The page is protected as GPU-written, but none of its bytes are waiting for a download:
		// the bytes the GPU wrote are elsewhere in the window, or were downloaded with another
		// page. The page is current, so lift its protection. Downloading the window instead drained
		// the GPU (~30 ms a fault) for guest reads of structs next to the command processor's
		// labels.
		const auto page_begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
		const auto page_end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
		if (m_memory_tracker.IsRegionGpuModified(page_begin, page_end - page_begin) &&
		    !HasGpuDirtyBytes(page_begin, page_end - page_begin)) {
			Add(Metric::CleanPageBypass);
			Add(Metric::ReadNoDownloadRequests);
			m_memory_tracker.UnmarkRegionAsGpuModified(page_begin, page_end - page_begin);
			if (is_write) {
				m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
			}
			prepared();
			return;
		}

		// Coalesce nearby accesses so they share one GPU drain, including pre-write readbacks.
		constexpr uint64_t WindowSize   = 512 * 1024;
		const auto         buffer_begin = buffer.CpuAddress();
		const auto         buffer_end   = buffer_begin + buffer.Size();
		const auto window_begin = std::max(Common::AlignDown(vaddr, WindowSize), buffer_begin);
		const auto window_end = std::min(std::max(window_begin + WindowSize, vaddr + size), buffer_end);
		Add(Metric::ReadWindowBytes, window_end - window_begin);
		if (sampled) { record.window_address = window_begin; record.window_size = window_end - window_begin; }

		const auto deferred_before = m_deferred_downloads;
		const bool downloaded = DownloadBufferMemory(buffer, window_begin, window_end - window_begin);
		const bool completed_inline = downloaded && m_deferred_downloads == deferred_before;
		prepared();
		Add(downloaded ? Metric::ReadDownloadRequests : Metric::ReadNoDownloadRequests);
		if (downloaded && diagnostics) {
			const auto writer = LastGpuWriteTick(window_begin, window_end - window_begin);
			const auto current = m_scheduler.CurrentTick();
			Add(m_scheduler.IsFree(writer) ? Metric::ReadFreshDone : Metric::ReadFreshPending);
			Add(Metric::ReadTailTicks, current > writer ? current - writer : 0);
		}
		// UNSAFE measurement only: KYTY_READBACK_UPPER_BOUND=1 skips the waits so the guest may read stale data.
		static const bool upper_bound = std::getenv("KYTY_READBACK_UPPER_BOUND") != nullptr;
		if (downloaded && !completed_inline) {
			const auto tick = m_scheduler.CurrentTick();
			{
				Scope wait(Metric::ReadGpuWaitNs);
				const auto begin_ns = sampled ? NowNs() : 0;
				if (!upper_bound) { m_scheduler.Wait(tick); }
				if (sampled) { record.gpu_wait_ns = NowNs() - begin_ns; }
			}
			{
				Scope wait(Metric::ReadPublishWaitNs);
				const auto begin_ns = sampled ? NowNs() : 0;
				if (!upper_bound) { m_scheduler.WaitPriorityOperations(tick); }
				if (sampled) { record.publish_wait_ns = NowNs() - begin_ns; }
			}
			m_memory_tracker.UnmarkRegionAsGpuModified(window_begin, window_end - window_begin);
		} else if (downloaded) {
			m_memory_tracker.UnmarkRegionAsGpuModified(window_begin, window_end - window_begin);
		}
		if (is_write) {
			m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		}
		if (sampled) { record.tick_end = m_scheduler.CurrentTick(); }
	});
	if (sampled) { record.total_ns = NowNs() - start_ns; WriteReadback(record); }
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0) {
		return NULL_BUFFER_ID;
	}
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid buffer discovery request\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			return *owner;
		}
	}
	return CreateBuffer(vaddr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(uint64_t vaddr, uint64_t size) {
	static constexpr int      StreamLeapThreshold = 16;
	static constexpr uint64_t StreamLeapSize      = CACHING_PAGESIZE * 128;

	auto       begin      = vaddr;
	auto       end        = vaddr + size;
	const auto find_first = [&](uint64_t address) {
		auto first = m_buffers.lower_bound(address);
		if (first != m_buffers.begin()) {
			const auto  previous = std::prev(first);
			const auto& buffer   = m_slot_buffers[previous->second];
			if (buffer.CpuAddress() + buffer.Size() > address) {
				first = previous;
			}
		}
		return first;
	};
	auto first           = find_first(begin);
	auto last            = first;
	int  stream_score    = 0;
	bool has_stream_leap = false;
	for (; last != m_buffers.end() && last->first < end; ++last) {
		const auto& buffer        = m_slot_buffers[last->second];
		const auto  buffer_begin  = buffer.CpuAddress();
		const auto  buffer_end    = buffer_begin + buffer.Size();
		const bool  expands_left  = buffer_begin < begin;
		const bool  expands_right = buffer_end > end;
		begin                     = std::min(begin, buffer_begin);
		end                       = std::max(end, buffer_end);
		if (!has_stream_leap && (stream_score += buffer.StreamScore()) > StreamLeapThreshold) {
			has_stream_leap = true;
			// Reserve space in the incoming stream's direction of growth.
			// The old buffer extending left of the request predicts growth to the right, and vice versa.
			if (expands_left) {
				end += std::min(StreamLeapSize, (vaddr < LOWER_ADDRESS_SIZE ? LOWER_ADDRESS_SIZE
				                                       : LibKernel::Memory::kExtendedMemoryBase +
				                                             LibKernel::Memory::kExtendedMemorySize) - end);
			}
			if (expands_right) {
				const auto minimum = vaddr < LOWER_ADDRESS_SIZE
				                         ? CACHING_PAGESIZE * 2
				                         : LibKernel::Memory::kExtendedMemoryBase;
				if (begin > minimum) {
					begin -= std::min(StreamLeapSize, begin - minimum);
				}
				first = find_first(begin);
				begin = std::min(begin, first->first);
			}
		}
	}
	return {first, last, begin, end, has_stream_leap};
}

void BufferCache::JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score) {
	auto& new_buffer = m_slot_buffers[new_id];
	auto& overlap    = m_slot_buffers[overlap_id];
	if (accumulate_stream_score) {
		new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
	}
	PerformanceDiagnostics::Add(PerformanceDiagnostics::Metric::BufferJoin);
	PerformanceDiagnostics::Add(PerformanceDiagnostics::Metric::BufferJoinBytes, overlap.Size());
	new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
	                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
	EXIT_IF(m_scheduler.Current().IsInvalid());
	const auto end = Common::AlignUp(vaddr + size, CACHING_PAGESIZE);
	if (vaddr < CACHING_PAGESIZE) {
		// Guest page 0 is never mapped; a request here is a garbage or null descriptor that
		// slipped past the null checks. The memory tracker cannot hold address 0, so name the
		// caller now rather than in the garbage collector later.
		static std::atomic<uint32_t> reported = 0;
		if (reported.fetch_add(1) < 8) {
			LOGF("BufferCache: buffer requested in guest page 0: vaddr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n%s",
			     vaddr, size, Common::HostBacktrace().c_str());
		}
	}
	vaddr = Common::AlignDown(vaddr, CACHING_PAGESIZE);
	size               = end - vaddr;
	const auto overlap = ResolveOverlaps(vaddr, size);

	const auto id = m_slot_buffers.insert(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, overlap.begin,
	    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, overlap.end - overlap.begin);
	m_slot_buffers[id].created_tick = m_scheduler.CurrentTick();
	const auto& buffer = m_slot_buffers[id];
	PerformanceDiagnostics::Add(PerformanceDiagnostics::Metric::BufferCreate);
	PerformanceDiagnostics::Add(PerformanceDiagnostics::Metric::BufferCreateBytes, buffer.Size());
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", overlap.begin,
	                     overlap.end - overlap.begin);
	for (auto it = overlap.first; it != overlap.last;) {
		const auto old_id = (it++)->second;
		JoinOverlap(id, old_id, !overlap.has_stream_leap);
	}
	Register(id);
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size = 0;
	vk::Buffer                  source;
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    copies.emplace_back(total_size, buffer.Offset(address), bytes);
		    total_size += bytes;
	    },
	    [&]() noexcept { source = UploadCopies(buffer, copies, total_size); });
	if (source) {
		auto& command = m_scheduler.Current();
		command.EndRendering();
		const auto native = command.Handle();
		vk::BufferMemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                       vk::AccessFlagBits::eTransferRead |
		                       vk::AccessFlagBits::eTransferWrite;
		before.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer              = buffer.Handle();
		before.offset              = 0;
		before.size                = buffer.Size();
		native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &before, 0, nullptr);
		native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
		                  copies.data());
		auto after          = before;
		after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                       vk::PipelineStageFlagBits::eAllCommands,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
	}
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

// Reads guest memory for an upload. The guest can unmap memory a cached buffer still covers: AMM
// maps streaming textures in blocks, and a texture upload once read one byte past the end of the
// block it had mapped. Such bytes read as zero, like ObtainBufferForImage's direct path.
static void ReadGuestForUpload(uint8_t* destination, uint64_t address, uint64_t size) {
	if (!Libs::LibKernel::Memory::TryReadBacking(address, destination, size) &&
	    !Libs::LibKernel::Memory::TryReadSparseBacking(address, destination, size)) {
		static std::atomic<uint32_t> reported {0};
		if (reported.fetch_add(1) < 16) {
			LOGF("BufferCache: upload of unmapped guest memory 0x%016" PRIx64 " size=0x%" PRIx64
			     " reads as zero\n",
			     address, size);
		}
		std::memset(destination, 0, static_cast<size_t>(size));
	}
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     uint64_t total_size) {
	if (copies.empty()) {
		return nullptr;
	}
	PerformanceDiagnostics::Add(PerformanceDiagnostics::Metric::BufferUploadBytes, total_size);

	auto [mapped, base_offset] = m_staging_buffer.Map(total_size, 4);
	if (mapped != nullptr) {
		for (auto& copy: copies) {
			const auto address = buffer.CpuAddress() + copy.dstOffset;
			ReadGuestForUpload(mapped + copy.srcOffset, address, copy.size);
			copy.srcOffset += base_offset;
		}
		m_staging_buffer.Commit();
		return m_staging_buffer.Handle();
	}

	auto temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Upload, 0,
	                                         vk::BufferUsageFlagBits::eTransferSrc, total_size);
	for (const auto& copy: copies) {
		const auto address = buffer.CpuAddress() + copy.dstOffset;
		ReadGuestForUpload(temporary->Mapped().data() + copy.srcOffset, address, copy.size);
	}
	temporary->Flush(0, total_size);
	const auto handle = temporary->Handle();
	m_scheduler.DeferOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return handle;
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id, bool needs_device_address) {
	PerformanceDiagnostics::CategoryScope category_scope(PerformanceDiagnostics::Category::BufferCache);
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || !GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: buffer request requires a recording command buffer\n");
	}

	if (!is_written && size <= CACHING_PAGESIZE &&
	    (!needs_device_address || m_stream_buffer.HasDeviceAddress()) &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		if (mapped != nullptr) {
			std::memcpy(mapped, reinterpret_cast<const void*>(vaddr), size);
			m_stream_buffer.Commit();
			return {&m_stream_buffer, offset};
		}
	}

	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	(void)SynchronizeBuffer(buffer, vaddr, size, is_written, is_texel_buffer);
	if (is_written) {
		MarkGpuWritten(vaddr, size);
	}
	return {&buffer, buffer.Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferWritten(uint64_t vaddr, uint64_t size,
                                                              uint64_t written_vaddr,
                                                              uint64_t written_size, BufferId id) {
	PerformanceDiagnostics::CategoryScope category_scope(PerformanceDiagnostics::Category::BufferCache);
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || !GuestRange {vaddr, size}.Valid() ||
	    written_vaddr < vaddr || written_size > size || written_vaddr - vaddr > size - written_size) {
		EXIT("BufferCache: invalid written-range buffer request\n");
	}
	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	(void)SynchronizeBuffer(buffer, vaddr, size, false, false);
	if (written_size != 0) {
		(void)SynchronizeBuffer(buffer, written_vaddr, written_size, true, false);
		MarkGpuWritten(written_vaddr, written_size);
	}
	return {&buffer, buffer.Offset(vaddr)};
}

void BufferCache::MarkGpuWritten(uint64_t vaddr, uint64_t size) {
	std::unique_lock lock(m_dirty_ranges_mutex);
	m_gpu_modified_ranges.Add(vaddr, size);
	auto& slot = m_gpu_writes[m_gpu_write_next];
	m_gpu_write_floor = std::max(m_gpu_write_floor, slot.tick);
	slot = {vaddr, vaddr + size, m_scheduler.CurrentTick()};
	m_gpu_write_next = (m_gpu_write_next + 1) % m_gpu_writes.size();
}

uint64_t BufferCache::LastGpuWriteTick(uint64_t vaddr, uint64_t size) const {
	std::shared_lock lock(m_dirty_ranges_mutex);
	auto tick = m_gpu_write_floor;
	for (const auto& write: m_gpu_writes) {
		if (write.tick > tick && write.begin < vaddr + size && vaddr < write.end) { tick = write.tick; }
	}
	return tick;
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	PerformanceDiagnostics::CategoryScope category_scope(PerformanceDiagnostics::Category::BufferCache);
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid image source\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			TouchBuffer(buffer);
			(void)SynchronizeBuffer(buffer, vaddr, size, false, false);
			return {&buffer, buffer.Offset(vaddr)};
		}
	}
	if (IsRegionGpuModified(vaddr, size)) {
		return ObtainBuffer(vaddr, size, false, false);
	}

	auto [staging, stage_offset] = m_staging_buffer.Map(size, 16);
	if (staging == nullptr) {
		EXIT("BufferCache: staging reservation failed for guest image\n");
	}
	if (!Libs::LibKernel::Memory::TryReadSparseBacking(vaddr, staging, size)) {
		std::memset(staging, 0, static_cast<size_t>(size));
	}
	m_staging_buffer.Commit();
	return {&m_staging_buffer, stage_offset};
}

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
	if ((vaddr & 3u) != 0 || size == 0 || (size & 3u) != 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: fill range must be dword aligned\n");
	}
	if (is_gds) {
		if (vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - vaddr) {
			EXIT("BufferCache: GDS fill range is out of bounds\n");
		}
		m_gds_buffer.Fill(vaddr, size, value);
		return;
	}
	if (vaddr == 0) {
		EXIT("BufferCache: invalid fill memory address\n");
	}
	(void)m_texture_cache.ClearMeta(vaddr);
	if (!IsRegionGpuModified(vaddr, size)) {
		// Access the guest mapping so write faults invalidate cached buffers and images.
		auto* destination = reinterpret_cast<uint32_t*>(vaddr);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true);
	dst->Fill(dst_offset, size, value);
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	const bool dst_memory = !dst_gds;
	const bool src_memory = !src_gds;
	if ((dst_memory && dst_vaddr == 0) || (src_memory && src_vaddr == 0) || size == 0 ||
	    ((dst_gds || src_gds) && ((dst_vaddr | src_vaddr | size) & 3u) != 0) ||
	    size > UINT64_MAX - dst_vaddr || size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) ||
	    (dst_gds && (dst_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - dst_vaddr)) ||
	    (src_gds && (src_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - src_vaddr))) {
		EXIT("BufferCache: invalid copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " size=0x%016" PRIx64 " src_gds=%d dst_gds=%d\n",
		     src_vaddr, dst_vaddr, size, static_cast<int>(src_gds), static_cast<int>(dst_gds));
	}
	if (src_memory && dst_memory && !IsRegionGpuModified(dst_vaddr, size) &&
	    !IsRegionGpuModified(src_vaddr, size) && !m_texture_cache.FindImageFromRange(src_vaddr, size)) {
		std::memcpy(reinterpret_cast<void*>(dst_vaddr), reinterpret_cast<const void*>(src_vaddr),
		            size);
		return;
	}

	auto& command = m_scheduler.Current();
	if (dst_memory) {
		m_texture_cache.InvalidateMemoryFromGPU(dst_vaddr, size);
	}
	const auto src_id      = src_memory ? FindBuffer(src_vaddr, size) : BufferId {};
	const auto dst_id      = dst_memory ? FindBuffer(dst_vaddr, size) : BufferId {};
	auto [src, src_offset] = src_memory ? ObtainBuffer(src_vaddr, size, false, true, src_id)
	                                    : std::pair {&m_gds_buffer, src_vaddr};
	auto [dst, dst_offset] = dst_memory ? ObtainBuffer(dst_vaddr, size, true, true, dst_id)
	                                    : std::pair {&m_gds_buffer, dst_vaddr};
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid registered-region query\n");
	}
	// Cached buffers are ordered and non-overlapping. The last buffer beginning before the query
	// end is therefore the only possible intersection.
	const auto candidate = m_buffers.lower_bound(vaddr + size);
	if (candidate == m_buffers.begin()) {
		return false;
	}
	const auto& [address, id] = *std::prev(candidate);
	return address + m_slot_buffers[id].Size() > vaddr;
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Intersects(vaddr, size);
}

bool BufferCache::IsCleanForConcurrentRead(uint64_t vaddr, uint64_t size) const {
	std::shared_lock lock(m_dirty_ranges_mutex);
	return !m_gpu_modified_ranges.Intersects(vaddr, size) &&
	       !m_downloading_ranges.Intersects(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

uint64_t BufferCache::LruClock() const noexcept {
	return m_graphics.presented_frames.load(std::memory_order_relaxed) + m_gc_tick / 512;
}

void BufferCache::RunGarbageCollector() {
	PerformanceDiagnostics::CategoryScope category_scope(PerformanceDiagnostics::Category::Gc);
	m_gc_tick++;
	const auto clock = LruClock();
	// Pressure is judged by this cache's own bytes. Device-wide usage also counts the images
	// spilled to host memory, which on a 6 GB card keeps it above the critical mark forever
	// and has the collector destroy and recreate every buffer twice a second (a third of
	// the GPU thread's time in the world). KYTY_GC_DEVICE_BYTES=1 restores that policy.
	static const bool device_bytes = std::getenv("KYTY_GC_DEVICE_BYTES") != nullptr;
	if (device_bytes && m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}

	const bool     aggressive = m_total_used_memory >= m_critical_gc_memory;
	// Ages in frames, as in the texture cache: a buffer used this frame or the last is
	// never a candidate, whatever the submission count.
	const uint64_t age        = std::min<uint64_t>(aggressive ? 2 : 4, clock);
	const size_t   limit      = aggressive ? 64 : 32;

	std::vector<BufferId> dirty_buffers;
	size_t                retire_count = 0;
	m_lru_cache.ForEachItemBelow(clock - age, [&](BufferId id) {
		auto& buffer = m_slot_buffers[id];
		EXIT_IF(buffer.is_deleted);
		if (buffer.CpuAddress() == 0) {
			// See CreateBuffer: the tracker rejects address 0, so this one is never collected.
			return false;
		}
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		// An aged GPU-dirty buffer is downloaded and dropped under either policy. Retaining it
		// (the non-aggressive rule before) leaves its GPU-written bytes owning the pages
		// forever, and the images that share those pages are then re-sourced from the
		// buffer's stale contents: the world renders black. Measured 2026-09-21.
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		if (dirty) {
			PerformanceDiagnostics::ReadbackScope attribution(PerformanceDiagnostics::ReadbackSource::Gc);
			PerformanceDiagnostics::Add(PerformanceDiagnostics::Metric::GcDirtyBuffers);
			EXIT_IF(!DownloadBufferMemory(buffer, buffer.CpuAddress(), buffer.Size()));
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
			DeleteBuffer(id);
		}
		return ++retire_count == limit;
	});
	if (dirty_buffers.empty()) {
		return;
	}

	// Publish all queued downloads before releasing their tracked pages and owners.
	const auto completion_tick = m_scheduler.CurrentTick();
	{
		PerformanceDiagnostics::WaitSiteScope site(PerformanceDiagnostics::WaitSite::GcDirtyDownload);
		m_scheduler.Wait(completion_tick);
	}
	m_scheduler.WaitPriorityOperations(completion_tick);
	for (const auto id: dirty_buffers) {
		auto& buffer = m_slot_buffers[id];
		m_memory_tracker.UnmarkRegionAsGpuModified(buffer.CpuAddress(), buffer.Size());
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size()) ||
		    m_gpu_modified_ranges.Intersects(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: garbage collection retained GPU ownership\n");
		}
		m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
		Unregister(id);
		m_slot_buffers.erase(id);
	}
}

void BufferCache::ProcessFaultBuffer() {
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::SynchronizeCpuDirtyBuffersInRange(uint64_t vaddr, uint64_t size) {
	m_memory_tracker.ForEachMaybeCpuDirtyRegion(
	    vaddr, size, [this](uint64_t address, uint64_t bytes) {
		    SynchronizeBuffersInRange(address, bytes);
	    });
}

void BufferCache::SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size) {
	const auto end = vaddr + size;
	auto       it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		auto&      buffer = m_slot_buffers[it->second];
		const auto start  = std::max(buffer.CpuAddress(), vaddr);
		const auto finish = std::min(buffer.CpuAddress() + buffer.Size(), end);
		if (start < finish) {
			(void)SynchronizeBuffer(buffer, start, finish - start, false, false);
		}
	}
}

} // namespace Libs::Graphics
