#include "graphics/host_gpu/renderer/performanceDiagnostics.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <map>
#include <tuple>
#include <vector>

namespace Libs::Graphics::PerformanceDiagnostics {
namespace {
void ReadDelayEnvironment();
void PollDelayControl();

constexpr auto Count = static_cast<unsigned>(Metric::Count);
constexpr std::array<const char*, Count> Names {
	"release_mem", "interrupt_flush", "label_flush", "processor_flush", "global_barrier",
	"renderer_submit", "presenter_submit", "queue_lock_ns", "queue_submit_ns",
	"semaphore_wait", "semaphore_wait_ns", "finish", "label_published", "flip_completed",
	"shader_compile", "shader_compile_ns", "pipeline_wait_ns",
	"buffer_readback_bytes", "image_readback_bytes", "buffer_upload_bytes",
	"acquire_ns", "present_ns", "title_update_ns",
	"cpu_readback_bytes", "cpu_writeback_bytes", "gpu_readback_bytes", "gpu_writeback_bytes",
	"metadata_readback_bytes", "gc_readback_bytes", "unknown_readback_bytes",
	"read_requested_bytes", "read_window_bytes", "clean_page_bypass",
	"cpu_read_requests", "cpu_write_requests", "gpu_read_requests", "gpu_write_requests", "metadata_read_requests",
	"gc_dirty_buffers", "cpu_read_sync_ns", "gpu_read_sync_ns",
	"gpu_semaphore_wait_ns", "completion_semaphore_wait_ns", "other_semaphore_wait_ns",
	"release_no_data", "release_dword", "release_qword", "release_timestamp", "release_gds",
	"read_dispatch_ns", "read_prepare_ns", "read_gpu_wait_ns", "read_publish_wait_ns",
	"read_download_requests", "read_no_download_requests",
	"read_fresh_done", "read_fresh_pending", "read_tail_ticks", "read_inline", "read_inline_ns",
	"submit_label", "submit_processor", "submit_flip", "submit_drain", "submit_finish",
	"submit_shutdown", "submit_other",
	"gpu_idle_ns", "gpu_command_ns", "gpu_process_ns",
	"buffer_create", "buffer_create_bytes", "buffer_join", "buffer_join_bytes", "buffer_delete",
	"vma_buffer_create", "vma_buffer_create_ns", "dedicated_alloc", "bda_table_upload", "image_create",
	"tiler_scratch_alloc",
	"label_interrupt", "label_gpu_waited", "label_clock", "label_other", "label_flushed_by_class",
	"cat_pm4_ns", "cat_draw_ns", "cat_dispatch_ns", "cat_bindings_ns", "cat_buffer_cache_ns", "cat_image_cache_ns", "cat_tiler_ns", "cat_pipeline_ns", "cat_wait_ns", "cat_submit_ns", "cat_gc_ns", "cat_command_ns", "cat_pm4_calls", "cat_draw_calls", "cat_dispatch_calls", "cat_bindings_calls", "cat_buffer_cache_calls", "cat_image_cache_calls", "cat_tiler_calls", "cat_pipeline_calls", "cat_wait_calls", "cat_submit_calls", "cat_gc_calls", "cat_command_calls"
};
static_assert(Names.back() != nullptr, "every Metric needs a CSV name, in enum order");

constexpr auto SiteCount  = static_cast<unsigned>(WaitSite::Count);
constexpr auto FieldCount = static_cast<unsigned>(WaitField::Count);
constexpr auto WaitColumns = SiteCount * FieldCount;
constexpr std::array<const char*, SiteCount> SiteNames {
	"unknown", "readback_cpu_read", "readback_cpu_write", "readback_gpu_read", "readback_gpu_write",
	"metadata", "finish_gds", "finish_processor", "finish_unmap", "finish_compute_recovery",
	"finish_collect_faults", "shutdown", "gc_dirty_download", "fault_buffer", "ring_download",
	"ring_stream", "ring_staging", "ring_device", "texture_download", "swapchain_frame",
	"priority_ops", "publish_wait", "readback_dispatch", "command_sync", "gpu_label_blocked",
	"videoout_slot", "videoout_flip", "videoout_vblank"
};
static_assert(SiteNames.back() != nullptr, "every WaitSite needs a CSV name, in enum order");
constexpr std::array<const char*, FieldCount> FieldNames {"calls", "blocking", "ns", "lag_ticks",
                                                          "drains"};

thread_local ThreadRole g_thread_role = ThreadRole::Other;
thread_local WaitSite g_wait_site = WaitSite::Unknown;
thread_local SubmitReason g_submit_reason = SubmitReason::Other;
thread_local ReadbackSource g_readback_source = ReadbackSource::Unknown;
thread_local ReadbackRecord* g_readback_record = nullptr;

// Each shard has one writer. Relaxed atomic stores allow the sampler to read it without races
// or a contended global fetch_add on every PM4 packet. Counters are never reset.
struct alignas(64) Shard {
	std::array<std::atomic<uint64_t>, Count> counters {};
	std::array<std::atomic<uint64_t>, WaitColumns> waits {};
};

struct State {
	std::mutex mutex;
	std::condition_variable wake;
	std::vector<std::unique_ptr<Shard>> shards;
	std::array<uint64_t, Count> previous {};
	std::array<uint64_t, WaitColumns> previous_waits {};
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point last_present {};
	FILE* file = nullptr;
	FILE* readback_file = nullptr;
	uint64_t readback_rows = 0;
	bool stopping = false;
	uint64_t presentations = 0;
	std::thread sampler;

	State() {
		ReadDelayEnvironment();
		file = std::fopen(std::getenv("KYTY_PERF_CSV"), "w");
		if (file == nullptr) {
			std::fprintf(stderr, "Performance diagnostics: cannot open KYTY_PERF_CSV\n");
			return;
		}
		std::fprintf(file, "row_type,elapsed_ns,presentation_id,present_interval_ns");
		for (const auto* name: Names) { std::fprintf(file, ",%s", name); }
		for (const auto* site: SiteNames) {
			for (const auto* field: FieldNames) { std::fprintf(file, ",wait_%s_%s", site, field); }
		}
		std::fprintf(file, "\n");
		std::fflush(file);
		const auto* readback_path = std::getenv("KYTY_PERF_READBACK_CSV");
		if (readback_path != nullptr && readback_path[0] != '\0') {
			readback_file = std::fopen(readback_path, "w");
			if (readback_file != nullptr) {
				std::fprintf(readback_file, "elapsed_ns,source,address,size,buffer_address,buffer_size,window_address,window_size,tick_begin,tick_end,dispatch_ns,prepare_ns,gpu_wait_ns,publish_wait_ns,total_ns,copies,payload_bytes,packed_bytes,copy_begin,copy_end\n");
				std::fflush(readback_file);
			}
		}
		sampler = std::thread([this] {
			std::unique_lock lock(mutex);
			unsigned tick = 0;
			while (!wake.wait_for(lock, std::chrono::milliseconds(100), [this] { return stopping; })) {
				PollDelayControl();
				if (++tick % 10 != 0) { continue; }
				Write("sample", 0);
				WriteDispatchFile();
				WriteLabelFile();
				std::fflush(file);
				if (readback_file != nullptr) { std::fflush(readback_file); }
			}
		});
	}
	~State() {
		{
			std::lock_guard lock(mutex);
			stopping = true;
		}
		wake.notify_all();
		if (sampler.joinable()) { sampler.join(); }
		if (file != nullptr) {
			Write("final", 0);
			std::fclose(file);
		}
		if (readback_file != nullptr) { std::fclose(readback_file); }
	}
	// Caller holds mutex except after all sampler activity has stopped at shutdown.
	void Write(const char* type, uint64_t interval) {
		if (file == nullptr) { return; }
		std::array<uint64_t, Count> total {};
		std::array<uint64_t, WaitColumns> total_waits {};
		for (const auto& shard: shards) {
			for (unsigned i = 0; i < Count; ++i) {
				total[i] += shard->counters[i].load(std::memory_order_relaxed);
			}
			for (unsigned i = 0; i < WaitColumns; ++i) {
				total_waits[i] += shard->waits[i].load(std::memory_order_relaxed);
			}
		}
		const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
		    std::chrono::steady_clock::now() - start).count();
		std::fprintf(file, "%s,%llu,%llu,%llu", type, static_cast<unsigned long long>(elapsed),
		             static_cast<unsigned long long>(presentations), static_cast<unsigned long long>(interval));
		for (unsigned i = 0; i < Count; ++i) {
			std::fprintf(file, ",%llu", static_cast<unsigned long long>(total[i] - previous[i]));
		}
		for (unsigned i = 0; i < WaitColumns; ++i) {
			std::fprintf(file, ",%llu", static_cast<unsigned long long>(total_waits[i] - previous_waits[i]));
		}
		std::fprintf(file, "\n");
		previous = total;
		previous_waits = total_waits;
	}
};

State& GetState() {
	static State state;
	return state;
}

Shard& LocalShard() {
	static thread_local Shard* shard = [] {
		auto& state = GetState();
		auto created = std::make_unique<Shard>();
		auto* result = created.get();
		std::lock_guard lock(state.mutex);
		state.shards.push_back(std::move(created));
		return result;
	}();
	return *shard;
}

} // namespace

uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool SampleReadback() {
	static const bool enabled = [] {
		const auto* path = std::getenv("KYTY_PERF_READBACK_CSV");
		return path != nullptr && path[0] != '\0' && Enabled();
	}();
	if (!enabled) { return false; }
	static thread_local uint64_t requests = 0;
	return (++requests % 64) == 1;
}

ReadbackRecordScope::ReadbackRecordScope(ReadbackRecord* record): m_previous(g_readback_record) {
	g_readback_record = record;
}
ReadbackRecordScope::~ReadbackRecordScope() { g_readback_record = m_previous; }
bool HasReadbackRecord() { return g_readback_record != nullptr; }

void RecordDownload(uint64_t address, uint64_t size, uint64_t packed_bytes) {
	if (g_readback_record == nullptr) { return; }
	auto& record = *g_readback_record;
	if (record.copies == 0 || address < record.copy_begin) { record.copy_begin = address; }
	if (address + size > record.copy_end) { record.copy_end = address + size; }
	++record.copies;
	record.payload_bytes += size;
	record.packed_bytes += packed_bytes;
}

void WriteReadback(const ReadbackRecord& record) {
	auto& state = GetState();
	std::lock_guard lock(state.mutex);
	if (state.readback_file == nullptr || state.readback_rows >= 20000) { return; }
	++state.readback_rows;
	const auto start_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	    state.start.time_since_epoch()).count());
	// A caller can start timing before the diagnostic singleton has been initialized.
	const auto elapsed = record.start_ns > start_ns ? record.start_ns - start_ns : 0;
	std::fprintf(state.readback_file, "%llu,%u", static_cast<unsigned long long>(elapsed),
	             static_cast<unsigned>(record.source));
	for (const auto value: {record.address, record.size, record.buffer_address, record.buffer_size,
	                       record.window_address, record.window_size, record.tick_begin, record.tick_end,
	                       record.dispatch_ns, record.prepare_ns, record.gpu_wait_ns, record.publish_wait_ns,
	                       record.total_ns, record.copies, record.payload_bytes, record.packed_bytes,
	                       record.copy_begin, record.copy_end}) {
		std::fprintf(state.readback_file, ",%llu", static_cast<unsigned long long>(value));
	}
	std::fprintf(state.readback_file, "\n");
}

void SetThreadRole(ThreadRole role) { g_thread_role = role; }

Metric SemaphoreWaitMetric() {
	switch (g_thread_role) {
		case ThreadRole::Gpu: return Metric::GpuSemaphoreWaitNs;
		case ThreadRole::Completion: return Metric::CompletionSemaphoreWaitNs;
		default: return Metric::OtherSemaphoreWaitNs;
	}
}

const char* WaitSiteName(WaitSite site) {
	const auto index = static_cast<unsigned>(site);
	return index < SiteCount ? SiteNames[index] : "invalid";
}

WaitSite CurrentWaitSite() { return g_wait_site; }
void SetWaitSite(WaitSite site) { g_wait_site = site; }

WaitSite EffectiveWaitSite() {
	if (g_wait_site != WaitSite::Unknown) { return g_wait_site; }
	switch (g_readback_source) {
		case ReadbackSource::CpuRead: return WaitSite::ReadbackCpuRead;
		case ReadbackSource::CpuWrite: return WaitSite::ReadbackCpuWrite;
		case ReadbackSource::GpuRead: return WaitSite::ReadbackGpuRead;
		case ReadbackSource::GpuWrite: return WaitSite::ReadbackGpuWrite;
		case ReadbackSource::Metadata: return WaitSite::Metadata;
		case ReadbackSource::Gc: return WaitSite::GcDirtyDownload;
		default: return WaitSite::Unknown;
	}
}

namespace {
void AddWait(WaitSite site, WaitField field, uint64_t value) {
	const auto index = static_cast<unsigned>(site) * FieldCount + static_cast<unsigned>(field);
	if (index >= WaitColumns) { return; }
	auto& counter = LocalShard().waits[index];
	counter.store(counter.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
}
} // namespace

void RecordWait(WaitSite site, bool blocking, uint64_t ns, uint64_t lag_ticks) {
	if (!Enabled()) { return; }
	AddWait(site, WaitField::Calls, 1);
	if (!blocking) { return; }
	AddWait(site, WaitField::Blocking, 1);
	AddWait(site, WaitField::Ns, ns);
	AddWait(site, WaitField::LagTicks, lag_ticks);
}

void RecordDrain(WaitSite site) {
	if (!Enabled()) { return; }
	AddWait(site, WaitField::Drains, 1);
}

uint64_t WaitCounter(WaitSite site, WaitField field) {
	if (!Enabled()) { return 0; }
	const auto index = static_cast<unsigned>(site) * FieldCount + static_cast<unsigned>(field);
	if (index >= WaitColumns) { return 0; }
	auto& state = GetState();
	std::lock_guard lock(state.mutex);
	uint64_t total = 0;
	for (const auto& shard: state.shards) { total += shard->waits[index].load(std::memory_order_relaxed); }
	return total;
}

namespace {
constexpr auto CategoryCount = static_cast<unsigned>(Category::Count);
struct CategoryState {
	std::array<Category, 16> stack {};
	unsigned depth = 0;
	std::chrono::steady_clock::time_point last {};
};
thread_local CategoryState g_categories;

// Causal probe: a fixed busy delay at the entry of a GPU-thread category. A frame-time slope near 1
// per injected ms means the category is on the critical path; near 0 means saving it gains nothing.
// KYTY_PERF_DELAY_<CATEGORY>_NS sets it at start; KYTY_PERF_CONTROL names a file with lines
// "delay_<category>_ns=N" that is polled every 100 ms, so A/B can alternate inside one process.
std::array<std::atomic<uint64_t>, CategoryCount> g_delay_ns {};
const char* const CategoryKeys[] = {"pm4", "draw", "dispatch", "bindings", "buffer_cache", "image_cache",
                                    "tiler", "pipeline", "wait", "submit", "gc", "command"};
static_assert(std::size(CategoryKeys) == CategoryCount, "every Category needs a delay key");
std::atomic<bool> g_delay_any {false};

void SetDelay(unsigned index, uint64_t ns) {
	g_delay_ns[index].store(ns, std::memory_order_relaxed);
	bool any = false;
	for (const auto& value: g_delay_ns) { any = any || value.load(std::memory_order_relaxed) != 0; }
	g_delay_any.store(any, std::memory_order_relaxed);
}

void ReadDelayEnvironment() {
	for (unsigned i = 0; i < CategoryCount; ++i) {
		std::string name = "KYTY_PERF_DELAY_";
		for (const char* c = CategoryKeys[i]; *c != '\0'; ++c) { name += static_cast<char>(std::toupper(*c)); }
		name += "_NS";
		if (const char* value = std::getenv(name.c_str()); value != nullptr) { SetDelay(i, std::strtoull(value, nullptr, 10)); }
	}
}

void PollDelayControl() {
	const char* path = std::getenv("KYTY_PERF_CONTROL");
	if (path == nullptr || path[0] == '\0') { return; }
	std::ifstream in(path);
	if (!in) { return; }
	std::array<uint64_t, CategoryCount> wanted {};
	std::string line;
	while (std::getline(in, line)) {
		for (unsigned i = 0; i < CategoryCount; ++i) {
			const std::string key = std::string("delay_") + CategoryKeys[i] + "_ns=";
			if (line.rfind(key, 0) == 0) { wanted[i] = std::strtoull(line.c_str() + key.size(), nullptr, 10); }
		}
	}
	for (unsigned i = 0; i < CategoryCount; ++i) {
		if (g_delay_ns[i].load(std::memory_order_relaxed) != wanted[i]) { SetDelay(i, wanted[i]); }
	}
}

void SpinDelay(Category category) {
	const auto ns = g_delay_ns[static_cast<unsigned>(category)].load(std::memory_order_relaxed);
	if (ns == 0) { return; }
	const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);
	while (std::chrono::steady_clock::now() < until) { }
}

void ChargeCategory(Category category, std::chrono::steady_clock::time_point now) {
	const auto ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	    now - g_categories.last).count());
	Add(static_cast<Metric>(static_cast<unsigned>(Metric::CatPm4Ns) + static_cast<unsigned>(category)), ns);
}
} // namespace

void EnterCategory(Category category) {
	if (!Enabled() || g_thread_role != ThreadRole::Gpu || g_categories.depth >= g_categories.stack.size()) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (g_categories.depth != 0) { ChargeCategory(g_categories.stack[g_categories.depth - 1], now); }
	g_categories.stack[g_categories.depth++] = category;
	g_categories.last = now;
	Add(static_cast<Metric>(static_cast<unsigned>(Metric::CatPm4Calls) + static_cast<unsigned>(category)));
	if (g_delay_any.load(std::memory_order_relaxed)) { SpinDelay(category); }
}

void LeaveCategory() {
	if (!Enabled() || g_thread_role != ThreadRole::Gpu || g_categories.depth == 0) { return; }
	const auto now = std::chrono::steady_clock::now();
	ChargeCategory(g_categories.stack[--g_categories.depth], now);
	g_categories.last = now;
}

namespace {
struct DispatchAggregate { uint64_t count = 0, ns = 0; };
std::mutex g_dispatch_mutex;
std::map<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint32_t>, DispatchAggregate> g_dispatches;
const char* DispatchPath() {
	static const char* path = std::getenv("KYTY_PERF_DISPATCH_CSV");
	return path != nullptr && path[0] != '\0' && Enabled() ? path : nullptr;
}
} // namespace

DispatchScope::DispatchScope(uint64_t shader, uint32_t x, uint32_t y, uint32_t z)
    : m_shader(shader), m_x(x), m_y(y), m_z(z), m_enabled(DispatchPath() != nullptr) {
	if (m_enabled) { m_start = std::chrono::steady_clock::now(); }
}

DispatchScope::~DispatchScope() {
	if (!m_enabled) { return; }
	const auto ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	    std::chrono::steady_clock::now() - m_start).count());
	std::lock_guard lock(g_dispatch_mutex);
	auto& aggregate = g_dispatches[{m_shader, m_x, m_y, m_z, m_kind}];
	++aggregate.count;
	aggregate.ns += ns;
}

void WriteDispatchFile() {
	const auto* path = DispatchPath();
	if (path == nullptr) { return; }
	std::lock_guard lock(g_dispatch_mutex);
	if (g_dispatches.empty()) { return; }
	FILE* file = std::fopen(path, "w");
	if (file == nullptr) { return; }
	std::fprintf(file, "shader,groups_x,groups_y,groups_z,kind,count,total_ns\n");
	for (const auto& [key, value]: g_dispatches) {
		std::fprintf(file, "0x%016llx,%u,%u,%u,%u,%llu,%llu\n",
		             static_cast<unsigned long long>(std::get<0>(key)), std::get<1>(key), std::get<2>(key),
		             std::get<3>(key), std::get<4>(key), static_cast<unsigned long long>(value.count),
		             static_cast<unsigned long long>(value.ns));
	}
	std::fclose(file);
}

namespace {
struct LabelAggregate { uint64_t suspends = 0, pending = 0, blocked_ns = 0, reference = 0; uint32_t function = 0, queue = 0; };
std::mutex g_label_mutex;
std::map<uint64_t, LabelAggregate> g_labels;
std::map<uint64_t, uint32_t> g_label_writers;
uint64_t g_last_label = 0;
const char* LabelPath() {
	static const char* path = std::getenv("KYTY_PERF_LABEL_CSV");
	return path != nullptr && path[0] != '\0' && Enabled() ? path : nullptr;
}
} // namespace

void RecordLabelSuspend(uint64_t address, uint32_t queue, bool gpu_pending, uint64_t reference, uint32_t function) {
	if (LabelPath() == nullptr) { return; }
	std::lock_guard lock(g_label_mutex);
	auto& aggregate = g_labels[address];
	++aggregate.suspends;
	if (gpu_pending) { ++aggregate.pending; }
	aggregate.reference = reference;
	aggregate.function  = function;
	aggregate.queue     = queue;
	g_last_label        = address;
}

void RecordLabelWriter(uint64_t address, uint32_t queue) {
	if (LabelPath() == nullptr) { return; }
	std::lock_guard lock(g_label_mutex);
	g_label_writers[address & ~7ull] = queue;
}

void RecordLabelBlockedNs(uint64_t ns) {
	if (LabelPath() == nullptr) { return; }
	std::lock_guard lock(g_label_mutex);
	g_labels[g_last_label].blocked_ns += ns;
}

void WriteLabelFile() {
	const auto* path = LabelPath();
	if (path == nullptr) { return; }
	std::lock_guard lock(g_label_mutex);
	if (g_labels.empty()) { return; }
	FILE* file = std::fopen(path, "w");
	if (file == nullptr) { return; }
	std::fprintf(file, "address,queue,function,reference,suspends,gpu_pending,blocked_ns,pm4_writer_queue\n");
	for (const auto& [address, value]: g_labels) {
		const auto writer = g_label_writers.find(address & ~7ull);
		const long long writer_queue = writer == g_label_writers.end() ? -1ll : static_cast<long long>(writer->second);
		std::fprintf(file, "0x%016llx,%u,%u,0x%llx,%llu,%llu,%llu,%lld\n", static_cast<unsigned long long>(address),
		             value.queue, value.function, static_cast<unsigned long long>(value.reference),
		             static_cast<unsigned long long>(value.suspends), static_cast<unsigned long long>(value.pending),
		             static_cast<unsigned long long>(value.blocked_ns), writer_queue);
	}
	std::fclose(file);
}

SubmitReason CurrentSubmitReason() { return g_submit_reason; }
void SetSubmitReason(SubmitReason reason) { g_submit_reason = reason; }

void RecordSubmit(SubmitReason reason) {
	Metric metric;
	switch (reason) {
		case SubmitReason::Label: metric = Metric::SubmitLabel; break;
		case SubmitReason::Processor: metric = Metric::SubmitProcessor; break;
		case SubmitReason::Flip: metric = Metric::SubmitFlip; break;
		case SubmitReason::Drain: metric = Metric::SubmitDrain; break;
		case SubmitReason::Finish: metric = Metric::SubmitFinish; break;
		case SubmitReason::Shutdown: metric = Metric::SubmitShutdown; break;
		default: metric = Metric::SubmitOther; break;
	}
	Add(metric);
}

ReadbackSource CurrentReadbackSource() { return g_readback_source; }
void SetReadbackSource(ReadbackSource source) { g_readback_source = source; }

void ReadbackBytes(uint64_t bytes) {
	Metric metric;
	switch (g_readback_source) {
		case ReadbackSource::CpuRead: metric = Metric::CpuReadbackBytes; break;
		case ReadbackSource::CpuWrite: metric = Metric::CpuWritebackBytes; break;
		case ReadbackSource::GpuRead: metric = Metric::GpuReadbackBytes; break;
		case ReadbackSource::GpuWrite: metric = Metric::GpuWritebackBytes; break;
		case ReadbackSource::Metadata: metric = Metric::MetadataReadbackBytes; break;
		case ReadbackSource::Gc: metric = Metric::GcReadbackBytes; break;
		default: metric = Metric::UnknownReadbackBytes; break;
	}
	Add(metric, bytes);
}

bool Enabled() {
	static const bool enabled = [] {
		const auto* path = std::getenv("KYTY_PERF_CSV");
		return path != nullptr && path[0] != '\0';
	}();
	return enabled;
}

void Add(Metric metric, uint64_t value) {
	if (!Enabled()) { return; }
	auto& counter = LocalShard().counters[static_cast<unsigned>(metric)];
	counter.store(counter.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
}

void Presented() {
	if (!Enabled()) { return; }
	auto& state = GetState();
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard lock(state.mutex);
	const auto interval = state.last_present.time_since_epoch().count() == 0 ? 0 :
	    std::chrono::duration_cast<std::chrono::nanoseconds>(now - state.last_present).count();
	state.last_present = now;
	++state.presentations;
	state.Write("present", static_cast<uint64_t>(interval));
}

} // namespace Libs::Graphics::PerformanceDiagnostics
