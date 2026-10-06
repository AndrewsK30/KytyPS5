#pragma once

#include <chrono>
#include <cstdint>

namespace Libs::Graphics::PerformanceDiagnostics {

enum class Metric : unsigned {
	ReleaseMem, InterruptFlush, LabelFlush, ProcessorFlush, GlobalBarrier,
	RendererSubmit, PresenterSubmit, QueueLockNs, QueueSubmitNs,
	SemaphoreWait, SemaphoreWaitNs, Finish, LabelPublished, FlipCompleted,
	ShaderCompile, ShaderCompileNs, PipelineWaitNs,
	BufferReadbackBytes, ImageReadbackBytes, BufferUploadBytes,
	AcquireNs, PresentNs, TitleUpdateNs,
	CpuReadbackBytes, CpuWritebackBytes, GpuReadbackBytes, GpuWritebackBytes,
	MetadataReadbackBytes, GcReadbackBytes, UnknownReadbackBytes,
	ReadRequestedBytes, ReadWindowBytes, CleanPageBypass,
	CpuReadRequests, CpuWriteRequests, GpuReadRequests, GpuWriteRequests, MetadataReadRequests,
	GcDirtyBuffers, CpuReadSyncNs, GpuReadSyncNs,
	GpuSemaphoreWaitNs, CompletionSemaphoreWaitNs, OtherSemaphoreWaitNs,
	ReleaseNoData, ReleaseDword, ReleaseQword, ReleaseTimestamp, ReleaseGds,
	ReadDispatchNs, ReadPrepareNs, ReadGpuWaitNs, ReadPublishWaitNs,
	ReadDownloadRequests, ReadNoDownloadRequests,
	ReadFreshDone, ReadFreshPending, ReadTailTicks, ReadInline, ReadInlineNs,
	SubmitLabel, SubmitProcessor, SubmitFlip, SubmitDrain, SubmitFinish, SubmitShutdown, SubmitOther,
	GpuIdleNs, GpuCommandNs, GpuProcessNs,
	BufferCreate, BufferCreateBytes, BufferJoin, BufferJoinBytes, BufferDelete,
	VmaBufferCreate, VmaBufferCreateNs, DedicatedAlloc, BdaTableUpload, ImageCreate, TilerScratchAlloc,
	LabelInterrupt, LabelGpuWaited, LabelClock, LabelOther, LabelFlushedByClass,
	CatPm4Ns, CatDrawNs, CatDispatchNs, CatBindingsNs, CatBufferCacheNs, CatImageCacheNs, CatTilerNs, CatPipelineNs, CatWaitNs, CatSubmitNs, CatGcNs, CatCommandNs, CatMaterializeNs,
	CatPm4Calls, CatDrawCalls, CatDispatchCalls, CatBindingsCalls, CatBufferCacheCalls, CatImageCacheCalls, CatTilerCalls, CatPipelineCalls, CatWaitCalls, CatSubmitCalls, CatGcCalls, CatCommandCalls, CatMaterializeCalls,
	ProtectCalls, GuestFaultRead, GuestFaultWrite,
	ImportedBuffers, ImportedBufferBytes, ImportUploadSkippedBytes, ImportDownloadSkipped, FaultHandlerNs, ProtectNs,
	Count
};

enum class ThreadRole { Other, Gpu, Completion };

// Call site of a blocking wait. Unknown semaphore waits fall back to the readback source tag.
enum class WaitSite : unsigned {
	Unknown, ReadbackCpuRead, ReadbackCpuWrite, ReadbackGpuRead, ReadbackGpuWrite, Metadata,
	FinishGds, FinishProcessor, FinishUnmap, FinishComputeRecovery, FinishCollectFaults, Shutdown,
	GcDirtyDownload, FaultBuffer, RingDownload, RingStream, RingStaging, RingDevice,
	TextureDownload, SwapchainFrame, PriorityOps, PublishWait, ReadbackDispatch, CommandSync,
	GpuLabelBlocked, VideoOutSlot, VideoOutFlip, VideoOutVblank,
	Count
};
// Per site: all calls (instant included), calls that blocked, blocked ns, GPU lag in ticks at
// the start of a blocking semaphore wait, and waits that first had to submit the open buffer.
enum class WaitField : unsigned { Calls, Blocking, Ns, LagTicks, Drains, Count };
enum class SubmitReason { Other, Label, Processor, Flip, Drain, Finish, Shutdown };

// Exclusive GPU-thread time by category: a nested scope's time is charged to the inner category
// only, so the categories add up to the time inside Pm4 and Command. Other threads are ignored.
enum class Category : unsigned {
	Pm4, Draw, Dispatch, Bindings, BufferCache, ImageCache, Tiler, Pipeline, Wait, Submit, Gc, Command, Materialize,
	Count
};
void EnterCategory(Category category);
void LeaveCategory();
class CategoryScope {
public:
	explicit CategoryScope(Category category) { EnterCategory(category); }
	~CategoryScope() { LeaveCategory(); }
	CategoryScope(const CategoryScope&) = delete;
	CategoryScope& operator=(const CategoryScope&) = delete;
};

const char* WaitSiteName(WaitSite site);
WaitSite CurrentWaitSite();
// Resolves Unknown through the current readback source.
WaitSite EffectiveWaitSite();
void SetWaitSite(WaitSite site);
void RecordWait(WaitSite site, bool blocking, uint64_t ns, uint64_t lag_ticks = 0);
void RecordDrain(WaitSite site);
uint64_t WaitCounter(WaitSite site, WaitField field);
SubmitReason CurrentSubmitReason();
void SetSubmitReason(SubmitReason reason);
void RecordSubmit(SubmitReason reason);

class WaitSiteScope {
public:
	explicit WaitSiteScope(WaitSite site): m_previous(CurrentWaitSite()) { SetWaitSite(site); }
	~WaitSiteScope() { SetWaitSite(m_previous); }
	WaitSiteScope(const WaitSiteScope&) = delete;
	WaitSiteScope& operator=(const WaitSiteScope&) = delete;
private:
	WaitSite m_previous;
};

class SubmitReasonScope {
public:
	explicit SubmitReasonScope(SubmitReason reason): m_previous(CurrentSubmitReason()) {
		SetSubmitReason(reason);
	}
	~SubmitReasonScope() { SetSubmitReason(m_previous); }
	SubmitReasonScope(const SubmitReasonScope&) = delete;
	SubmitReasonScope& operator=(const SubmitReasonScope&) = delete;
private:
	SubmitReason m_previous;
};
enum class ReadbackSource { Unknown, CpuRead, CpuWrite, GpuRead, GpuWrite, Metadata, Gc };

void SetThreadRole(ThreadRole role);
Metric SemaphoreWaitMetric();
ReadbackSource CurrentReadbackSource();
void SetReadbackSource(ReadbackSource source);
void ReadbackBytes(uint64_t bytes);

// Bounded 1-in-64 sampling per requesting thread; addresses only, never guest contents.
struct ReadbackRecord {
	ReadbackSource source = ReadbackSource::Unknown;
	uint64_t start_ns = 0, address = 0, size = 0;
	uint64_t buffer_address = 0, buffer_size = 0, window_address = 0, window_size = 0;
	uint64_t tick_begin = 0, tick_end = 0;
	uint64_t dispatch_ns = 0, prepare_ns = 0, gpu_wait_ns = 0, publish_wait_ns = 0, total_ns = 0;
	uint64_t copies = 0, payload_bytes = 0, packed_bytes = 0, copy_begin = 0, copy_end = 0;
};
uint64_t NowNs();
bool SampleReadback();
void WriteReadback(const ReadbackRecord& record);
void RecordDownload(uint64_t address, uint64_t size, uint64_t packed_bytes);
bool HasReadbackRecord();
class ReadbackRecordScope {
public:
	explicit ReadbackRecordScope(ReadbackRecord* record);
	~ReadbackRecordScope();
	ReadbackRecordScope(const ReadbackRecordScope&) = delete;
	ReadbackRecordScope& operator=(const ReadbackRecordScope&) = delete;
private:
	ReadbackRecord* m_previous;
};

class ReadbackScope {
public:
	explicit ReadbackScope(ReadbackSource source): m_previous(CurrentReadbackSource()) {
		SetReadbackSource(source);
	}
	~ReadbackScope() { SetReadbackSource(m_previous); }
	ReadbackScope(const ReadbackScope&) = delete;
	ReadbackScope& operator=(const ReadbackScope&) = delete;
private:
	ReadbackSource m_previous;
};

bool Enabled();
void Add(Metric metric, uint64_t value = 1);
void Presented();

// Timings measure host wall time, not GPU execution. No GPU queries or waits are added.
class Scope {
public:
	explicit Scope(Metric metric, Metric mirror = Metric::Count)
	    : m_metric(metric), m_mirror(mirror), m_enabled(Enabled()) {
		if (m_enabled) { m_start = std::chrono::steady_clock::now(); }
	}
	~Scope() {
		if (m_enabled) {
			const auto ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			    std::chrono::steady_clock::now() - m_start).count());
			Add(m_metric, ns);
			if (m_mirror != Metric::Count) { Add(m_mirror, ns); }
		}
	}
	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;
private:
	Metric m_metric;
	Metric m_mirror;
	bool m_enabled;
	std::chrono::steady_clock::time_point m_start {};
};

// Aggregates compute dispatches by (shader address, group counts, outcome); KYTY_PERF_DISPATCH_CSV.
// Outcome: 0 executed, 1 consumed as a meta clear, 2 consumed as an image clear, 3 no program.
class DispatchScope {
public:
	DispatchScope(uint64_t shader, uint32_t x, uint32_t y, uint32_t z);
	~DispatchScope();
	DispatchScope(const DispatchScope&) = delete;
	DispatchScope& operator=(const DispatchScope&) = delete;
	void SetKind(uint32_t kind) { m_kind = kind; }
private:
	uint64_t m_shader;
	uint32_t m_x, m_y, m_z, m_kind = 0;
	bool m_enabled;
	std::chrono::steady_clock::time_point m_start {};
};
void WriteDispatchFile();
// Per PS5 queue (KYTY_PERF_QUEUE_CSV): PM4 slices processed, GPU-thread time inside them and the exclusive
// category time they contain. The GPU thread sets the queue it is processing; other threads are ignored.
constexpr unsigned MaxQueues = 64;
void SetCurrentQueue(unsigned queue);
void RecordQueueSlice(unsigned queue, uint64_t ns);
void WriteQueueFile();
// Per guest buffer (KYTY_PERF_BUFFER_CSV): how often and how expensively the CPU and GPU hand it data.
enum class BufferEvent : unsigned { Readback, Upload, Wait };
void RecordBufferEvent(uint64_t address, uint64_t size, BufferEvent event, uint64_t bytes, uint64_t ns);
void WriteBufferFile();
// WAIT_REG_MEM suspensions by label address (KYTY_PERF_LABEL_CSV): how often a queue suspends on it, how
// often the awaited value was a GPU-pending label (the GPU has yet to complete its writer), and the time the
// GPU thread then sat idle with every queue blocked, attributed to the address suspended last.
void RecordLabelSuspend(uint64_t address, uint32_t queue, bool gpu_pending, uint64_t reference, uint32_t function);
void RecordLabelBlockedNs(uint64_t ns);
// A PM4 packet on queue writes a label at ddress (RELEASE_MEM, WRITE_DATA); waits on an address with no
// PM4 writer are released by guest CPU threads.
void RecordLabelWriter(uint64_t address, uint32_t queue);
void WriteLabelFile();

// Times a non-semaphore wait (condition variable, binary semaphore) attributed to an explicit site.
// Pass blocking=false when the wait predicate was already satisfied on entry.
class WaitTimer {
public:
	explicit WaitTimer(WaitSite site, bool blocking = true)
	    : m_site(site), m_blocking(blocking), m_enabled(Enabled()) {
		if (m_enabled && m_blocking) {
			EnterCategory(Category::Wait);
			m_start = std::chrono::steady_clock::now();
		}
	}
	~WaitTimer() {
		if (!m_enabled) { return; }
		if (m_blocking) { LeaveCategory(); }
		const auto ns = m_blocking ? static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(
		        std::chrono::steady_clock::now() - m_start).count()) : 0;
		RecordWait(m_site, m_blocking, ns);
	}
	WaitTimer(const WaitTimer&) = delete;
	WaitTimer& operator=(const WaitTimer&) = delete;
private:
	WaitSite m_site;
	bool m_blocking;
	bool m_enabled;
	std::chrono::steady_clock::time_point m_start {};
};

} // namespace Libs::Graphics::PerformanceDiagnostics
