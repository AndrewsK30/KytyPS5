#include "graphics/host_gpu/renderer/performanceDiagnostics.h"

#include <chrono>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
	using namespace Libs::Graphics::PerformanceDiagnostics;
	if (argc != 2) { return 2; }
	const bool disabled = std::strcmp(argv[1], "--off") == 0;
	const auto trace_path = std::string(argv[1]) + ".readbacks.csv";
#ifdef _WIN32
	_putenv_s("KYTY_PERF_CSV", disabled ? "" : argv[1]);
	_putenv_s("KYTY_PERF_READBACK_CSV", disabled ? "" : trace_path.c_str());
#else
	setenv("KYTY_PERF_CSV", disabled ? "" : argv[1], 1);
	setenv("KYTY_PERF_READBACK_CSV", disabled ? "" : trace_path.c_str(), 1);
#endif
	if (disabled) {
		Add(Metric::ReleaseMem);
		Presented();
		return Enabled() || SampleReadback() ? 1 : 0;
	}
	std::vector<std::thread> workers;
	const std::array<ReadbackSource, 4> sources { ReadbackSource::CpuRead, ReadbackSource::GpuRead,
	                                           ReadbackSource::Metadata, ReadbackSource::Gc };
	for (int t = 0; t < 4; ++t) {
		// Capture origin on the calling thread, as ReadMemory does before SendCommandSync.
		ReadbackScope caller_source(sources[t]);
		const auto source = CurrentReadbackSource();
		workers.emplace_back([source] {
			ReadbackScope worker_source(source);
			unsigned sampled = 0;
			for (int i = 0; i < 10000; ++i) {
				Add(Metric::ReleaseMem);
				Add(Metric::BufferReadbackBytes, 64);
				ReadbackBytes(64);
				if (SampleReadback()) {
					++sampled;
					ReadbackRecord record;
					record.start_ns = NowNs();
					record.source = source;
					{
						ReadbackRecordScope trace(&record);
						RecordDownload(0x1000, 4, 64);
						{ ReadbackRecordScope nested(nullptr); RecordDownload(0x2000, 128, 128); }
						RecordDownload(0x1080, 8, 64);
						if (record.copies != 2 || record.payload_bytes != 12 ||
						    record.packed_bytes != 128 || record.copy_begin != 0x1000 ||
						    record.copy_end != 0x1088) { std::abort(); }
					}
					if (HasReadbackRecord()) { std::abort(); }
					WriteReadback(record);
				}
			}
			if (sampled != 157) { std::abort(); }
			{
				ReadbackScope nested(ReadbackSource::CpuWrite);
				if (CurrentReadbackSource() != ReadbackSource::CpuWrite) { std::abort(); }
			}
			if (CurrentReadbackSource() != source) { std::abort(); }
		});
	}
	for (auto& thread: workers) { thread.join(); }
	// Wait sites: per-thread nested tags, readback fallback, instant vs blocking, concurrent writers.
	constexpr std::array<WaitSite, 4> sites { WaitSite::GcDirtyDownload, WaitSite::RingDownload,
	                                         WaitSite::FinishGds, WaitSite::SwapchainFrame };
	std::vector<std::thread> waiters;
	for (const auto site: sites) {
		waiters.emplace_back([site] {
			WaitSiteScope scope(site);
			for (int i = 0; i < 1000; ++i) {
				RecordWait(EffectiveWaitSite(), (i % 4) == 0, 10, 3);
				if ((i % 10) == 0) { RecordDrain(EffectiveWaitSite()); }
				RecordSubmit((i % 2) == 0 ? SubmitReason::Label : SubmitReason::Drain);
			}
			{
				WaitSiteScope nested(WaitSite::PublishWait);
				if (CurrentWaitSite() != WaitSite::PublishWait) { std::abort(); }
			}
			if (CurrentWaitSite() != site) { std::abort(); }
		});
	}
	for (auto& thread: waiters) { thread.join(); }
	if (CurrentWaitSite() != WaitSite::Unknown || EffectiveWaitSite() != WaitSite::Unknown) { return 7; }
	{
		ReadbackScope source(ReadbackSource::CpuWrite);
		if (EffectiveWaitSite() != WaitSite::ReadbackCpuWrite) { return 7; }
		WaitSiteScope explicit_site(WaitSite::RingDownload);
		if (EffectiveWaitSite() != WaitSite::RingDownload) { return 7; }
	}
	{ WaitTimer instant(WaitSite::CommandSync, false); }
	{
		WaitTimer blocked(WaitSite::CommandSync);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	for (const auto site: sites) {
		if (WaitCounter(site, WaitField::Calls) != 1000 || WaitCounter(site, WaitField::Blocking) != 250 ||
		    WaitCounter(site, WaitField::Ns) != 2500 || WaitCounter(site, WaitField::LagTicks) != 750 ||
		    WaitCounter(site, WaitField::Drains) != 100) { return 8; }
	}
	if (WaitCounter(WaitSite::CommandSync, WaitField::Calls) != 2 ||
	    WaitCounter(WaitSite::CommandSync, WaitField::Blocking) != 1 ||
	    WaitCounter(WaitSite::CommandSync, WaitField::Ns) < 1000000) { return 8; }
	if (std::strcmp(WaitSiteName(WaitSite::GpuLabelBlocked), "gpu_label_blocked") != 0 ||
	    std::strcmp(WaitSiteName(WaitSite::VideoOutVblank), "videoout_vblank") != 0) { return 8; }
	ReadbackRecord bounded;
	bounded.start_ns = NowNs();
	bounded.source = ReadbackSource::CpuRead;
	bounded.copies = 2; bounded.payload_bytes = 12; bounded.packed_bytes = 128;
	bounded.copy_begin = 0x1000; bounded.copy_end = 0x1088;
	for (int i = 0; i < 20001; ++i) { WriteReadback(bounded); }
	if (CurrentReadbackSource() != ReadbackSource::Unknown) { return 4; }
	SetThreadRole(ThreadRole::Completion);
	if (SemaphoreWaitMetric() != Metric::CompletionSemaphoreWaitNs) { return 4; }
	SetThreadRole(ThreadRole::Gpu);
	{
		Scope timing(Metric::SemaphoreWaitNs, SemaphoreWaitMetric());
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	// Categories are exclusive on the GPU thread: the nested 40 ms is charged to Draw only.
	{
		CategoryScope pm4(Category::Pm4);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		{
			CategoryScope draw(Category::Draw);
			std::this_thread::sleep_for(std::chrono::milliseconds(40));
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	// A thread that is not the GPU thread is ignored.
	std::thread([] {
		CategoryScope command(Category::Command);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}).join();
	Presented();
	std::this_thread::sleep_for(std::chrono::milliseconds(2));
	Presented();
	// Sampling must continue without another present and flush the completed rows to disk.
	std::this_thread::sleep_for(std::chrono::milliseconds(1200));
	std::ifstream input(argv[1]);
	std::string line;
	std::getline(input, line);
	uint64_t releases = 0, bytes = 0;
	std::array<uint64_t, static_cast<unsigned>(Metric::Count)> totals {};
	constexpr unsigned wait_columns =
	    static_cast<unsigned>(WaitSite::Count) * static_cast<unsigned>(WaitField::Count);
	const unsigned wait_base = 4 + static_cast<unsigned>(Metric::Count);
	uint64_t ring_calls = 0;
	int presents = 0, samples = 0;
	while (std::getline(input, line)) {
		std::stringstream stream(line);
		std::vector<std::string> columns;
		std::string column;
		while (std::getline(stream, column, ',')) { columns.push_back(column); }
		if (columns.size() != wait_base + wait_columns) { return 3; }
		ring_calls += std::stoull(columns[wait_base +
		    static_cast<unsigned>(WaitSite::RingDownload) * static_cast<unsigned>(WaitField::Count) +
		    static_cast<unsigned>(WaitField::Calls)]);
		for (unsigned i = 0; i < totals.size(); ++i) { totals[i] += std::stoull(columns[4 + i]); }
		releases += std::stoull(columns[4 + static_cast<unsigned>(Metric::ReleaseMem)]);
		bytes += std::stoull(columns[4 + static_cast<unsigned>(Metric::BufferReadbackBytes)]);
		if (columns[0] == "present") { ++presents; }
		if (columns[0] == "sample") { ++samples; }
	}
	if (releases != 40000 || bytes != 2560000 || presents != 2 || samples < 1) {
		std::fprintf(stderr, "Unexpected totals: releases=%llu bytes=%llu presents=%d samples=%d\n",
		             static_cast<unsigned long long>(releases), static_cast<unsigned long long>(bytes),
		             presents, samples);
		return 1;
	}
	for (const auto metric: {Metric::CpuReadbackBytes, Metric::GpuReadbackBytes,
	                         Metric::MetadataReadbackBytes, Metric::GcReadbackBytes}) {
		if (totals[static_cast<unsigned>(metric)] != 640000) { return 5; }
	}
	const auto pm4_ns = totals[static_cast<unsigned>(Metric::CatPm4Ns)];
	const auto draw_ns = totals[static_cast<unsigned>(Metric::CatDrawNs)];
	if (totals[static_cast<unsigned>(Metric::CatPm4Calls)] != 1 || totals[static_cast<unsigned>(Metric::CatDrawCalls)] != 1 ||
	    totals[static_cast<unsigned>(Metric::CatCommandCalls)] != 0 || pm4_ns < 9000000 || pm4_ns > 35000000 ||
	    draw_ns < 39000000) { return 10; }
	if (ring_calls != 1000 || totals[static_cast<unsigned>(Metric::SubmitLabel)] != 2000 ||
	    totals[static_cast<unsigned>(Metric::SubmitDrain)] != 2000) { return 9; }
	if (totals[static_cast<unsigned>(Metric::UnknownReadbackBytes)] != 0 ||
	    totals[static_cast<unsigned>(Metric::GpuSemaphoreWaitNs)] == 0 ||
	    totals[static_cast<unsigned>(Metric::GpuSemaphoreWaitNs)] !=
	        totals[static_cast<unsigned>(Metric::SemaphoreWaitNs)]) { return 5; }
	std::ifstream trace_input(trace_path);
	std::getline(trace_input, line);
	unsigned rows = 0;
	while (std::getline(trace_input, line)) {
		std::stringstream stream(line);
		std::vector<std::string> columns;
		std::string column;
		while (std::getline(stream, column, ',')) { columns.push_back(column); }
		if (columns.size() != 20 || std::stoull(columns[15]) != 2 ||
		    std::stoull(columns[16]) != 12 || std::stoull(columns[17]) != 128 ||
		    std::stoull(columns[18]) != 0x1000 || std::stoull(columns[19]) != 0x1088) { return 6; }
		++rows;
	}
	if (rows != 20000) { return 6; }
	std::puts("Diagnostics concurrency, scoped attribution, bounded samples, wait roles, wait sites, submit reasons and heartbeat: PASS");
	return 0;
}
