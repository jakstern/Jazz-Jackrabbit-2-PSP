#include "PspDiagnostics.h"

#include <pspkernel.h>
#include <pspsysmem.h>
#include <malloc.h>
#include <cstdio>

namespace nCine
{
	namespace
	{
		PspDiagnosticsSnapshot g_current;
		PspDiagnosticsSnapshot g_smoothed;
		std::uint64_t g_frameStart = 0;
		std::uint64_t g_phaseStart[static_cast<int>(PspDiagnosticsPhase::Count)] {};
		bool g_hasSample = false;
		// Toggled at run time from the pause menu, independently of the PSP_DEBUG build option.
		bool g_visible = false;

		struct MemoryActivity {
			std::uint32_t streamReads = 0, streamReadBytes = 0;
			std::uint32_t pageReads = 0, pageReadBytes = 0;
			std::uint32_t evictions = 0, evictionBytes = 0;
			std::uint32_t pageEvictions = 0, pageEvictionBytes = 0;
			std::uint32_t evictionFences = 0, cacheHits = 0;
			std::uint32_t streamSubmits = 0, streamPending = 0;
		};
		MemoryActivity g_activity;
		PspDiagLogSink g_sink = nullptr;
		PspDiagContentProvider g_contentProvider = nullptr;
		std::uint64_t g_lastMemoryUs = 0;
		std::uint32_t g_heapFreeLow = 0xFFFFFFFFu;
		std::uint32_t g_maxFreeLow = 0xFFFFFFFFu;

		std::uint64_t NowUs()
		{
			return sceKernelGetSystemTimeWide();
		}

		float Smooth(float previous, float current)
		{
			return g_hasSample ? previous * 0.9f + current * 0.1f : current;
		}

		void SampleAndEmitMemory()
		{
			const struct mallinfo mi = mallinfo();
			PspMemoryReport r;
			r.heapArena = (std::uint32_t)mi.arena;
			r.heapUsed = (std::uint32_t)mi.uordblks;
			r.heapFree = (std::uint32_t)mi.fordblks;
			r.heapKeepCost = (std::uint32_t)mi.keepcost;
			r.heapFreeBlocks = (std::uint32_t)mi.ordblks;
			r.maxFreeBlock = (std::uint32_t)sceKernelMaxFreeMemSize();
			r.kernelFree = (std::uint32_t)sceKernelTotalFreeMemSize();
			if (r.heapFree < g_heapFreeLow) g_heapFreeLow = r.heapFree;
			if (r.maxFreeBlock < g_maxFreeLow) g_maxFreeLow = r.maxFreeBlock;
			r.heapFreeLow = g_heapFreeLow;
			r.maxFreeLow = g_maxFreeLow;

			PspEmitCollectTextureMemory(r);
			if (g_contentProvider != nullptr) g_contentProvider(r);

			r.streamReads = g_activity.streamReads; r.streamReadBytes = g_activity.streamReadBytes;
			r.pageReads = g_activity.pageReads; r.pageReadBytes = g_activity.pageReadBytes;
			r.evictions = g_activity.evictions; r.evictionBytes = g_activity.evictionBytes;
			r.pageEvictions = g_activity.pageEvictions; r.pageEvictionBytes = g_activity.pageEvictionBytes;
			r.evictionFences = g_activity.evictionFences; r.cacheHits = g_activity.cacheHits;
			r.streamSubmits = g_activity.streamSubmits; r.streamPending = g_activity.streamPending;
			g_activity = MemoryActivity{};

			char line[256];
			std::snprintf(line, sizeof(line),
				"mem.heap arena=%u used=%u free=%u freelow=%u maxfree=%u maxlow=%u kfree=%u frag=%ublk/%ukeep",
				r.heapArena, r.heapUsed, r.heapFree, r.heapFreeLow, r.maxFreeBlock, r.maxFreeLow, r.kernelFree,
				r.heapFreeBlocks, r.heapKeepCost);
			g_sink(line);
			std::snprintf(line, sizeof(line),
				"mem.tex resident=%u sprite=%u pages=%u var=%u | tex=%u streamed=%u rspr=%u atlas=%u rpg=%u strm=%u",
				r.texResidentTotal, r.texSpritePixels, r.texPagePixels, r.texVariantBytes,
				r.texCount, r.texStreamed, r.texResidentSprites, r.texPagedAtlases, r.texResidentPages,
				r.streamerRunning);
			g_sink(line);
			std::snprintf(line, sizeof(line),
				"mem.gu list=%u high=%u cap=%u overflowFrames=%u droppedCmds=%u",
				r.guListBytes, r.guListHighWater, r.guListCapacity, r.guListOverflowFrames,
				r.guListDroppedCommands);
			g_sink(line);
			std::snprintf(line, sizeof(line),
				"mem.gfx mask=%u meta=%u gfx=%u snd=%u | io rd=%u(%uB) pg=%u(%uB) ev=%u(%uB) pgev=%u(%uB) fence=%u hit=%u sub=%u pend=%u",
				r.maskResidentBytes, r.cachedMetadata, r.cachedGraphics, r.cachedSounds,
				r.streamReads, r.streamReadBytes, r.pageReads, r.pageReadBytes,
				r.evictions, r.evictionBytes, r.pageEvictions, r.pageEvictionBytes, r.evictionFences, r.cacheHits,
				r.streamSubmits, r.streamPending);
			g_sink(line);
		}
	}

	void PspDiagnosticsBeginFrame()
	{
		g_current = {};
		for (std::uint64_t& start : g_phaseStart) start = 0;
		g_frameStart = NowUs();
	}

	void PspDiagnosticsEndFrame()
	{
		if (g_frameStart == 0) return;
		g_current.totalMs = float(NowUs() - g_frameStart) * 0.001f;
		g_smoothed.totalMs = Smooth(g_smoothed.totalMs, g_current.totalMs);
		for (int i = 0; i < static_cast<int>(PspDiagnosticsPhase::Count); ++i) {
			g_smoothed.phaseMs[i] = Smooth(g_smoothed.phaseMs[i], g_current.phaseMs[i]);
		}
		g_smoothed.draws = g_current.draws;
		g_smoothed.vertices = g_current.vertices;
		g_smoothed.culled = g_current.culled;
		g_smoothed.textureImages = g_current.textureImages;
		g_smoothed.clutLoads = g_current.clutLoads;
		g_smoothed.spriteCommands = g_current.spriteCommands;
		g_smoothed.meshCommands = g_current.meshCommands;
		g_smoothed.meshVertices = g_current.meshVertices;
		g_smoothed.batchFlushes = g_current.batchFlushes;
		g_smoothed.batchedQuads = g_current.batchedQuads;
		const struct mallinfo heap = mallinfo();
		g_smoothed.heapArenaBytes = heap.arena;
		g_smoothed.heapUsedBytes = heap.uordblks;
		g_smoothed.heapFreeBytes = heap.fordblks;
		// Outside newlib's reserved user heap - NOT the application's free RAM, and must not be shown as such.
		g_smoothed.kernelFreeBytes = sceKernelTotalFreeMemSize();
		g_hasSample = true;
		g_frameStart = 0;

		// Once-per-second memory report; without a sink the texture-registry walk never runs on the frame thread.
		if (g_sink != nullptr) {
			const std::uint64_t now = NowUs();
			if (g_lastMemoryUs == 0) {
				g_lastMemoryUs = now;
			} else if (now - g_lastMemoryUs >= 1000000) {
				g_lastMemoryUs = now;
				SampleAndEmitMemory();
			}
		}
	}

	void PspDiagnosticsBeginPhase(PspDiagnosticsPhase phase)
	{
		g_phaseStart[static_cast<int>(phase)] = NowUs();
	}

	void PspDiagnosticsEndPhase(PspDiagnosticsPhase phase)
	{
		const int index = static_cast<int>(phase);
		if (g_phaseStart[index] == 0) return;
		g_current.phaseMs[index] += float(NowUs() - g_phaseStart[index]) * 0.001f;
		g_phaseStart[index] = 0;
	}

	void PspDiagnosticsRecordDraw(std::uint32_t vertices)
	{
		g_current.draws++;
		g_current.vertices += vertices;
	}

	void PspDiagnosticsRecordCulled() { g_current.culled++; }
	void PspDiagnosticsRecordTextureImage() { g_current.textureImages++; }
	void PspDiagnosticsRecordClutLoad() { g_current.clutLoads++; }
	void PspDiagnosticsRecordSpriteCommand() { g_current.spriteCommands++; }
	void PspDiagnosticsRecordMeshCommand(std::uint32_t vertices)
	{
		g_current.meshCommands++;
		g_current.meshVertices += vertices;
	}
	void PspDiagnosticsRecordBatchFlush(std::uint32_t quads)
	{
		g_current.batchFlushes++;
		g_current.batchedQuads += quads;
	}
	const PspDiagnosticsSnapshot& PspDiagnosticsGetSnapshot() { return g_smoothed; }
	bool PspDiagnosticsIsVisible() { return g_visible; }
	void PspDiagnosticsToggleVisible() { g_visible = !g_visible; }

	void PspDiagnosticsSetLogSink(PspDiagLogSink sink) { g_sink = sink; }
	void PspDiagnosticsLog(const char* line) { if (g_sink != nullptr) g_sink(line); }
	void PspDiagnosticsSetContentMemoryProvider(PspDiagContentProvider provider) { g_contentProvider = provider; }
	void PspDiagnosticsRecordCacheHit() { g_activity.cacheHits++; }
	void PspDiagnosticsRecordStreamRead(std::uint32_t bytes) { g_activity.streamReads++; g_activity.streamReadBytes += bytes; }
	void PspDiagnosticsRecordPageRead(std::uint32_t bytes) { g_activity.pageReads++; g_activity.pageReadBytes += bytes; }
	void PspDiagnosticsRecordEviction(std::uint32_t bytes) { g_activity.evictions++; g_activity.evictionBytes += bytes; }
	void PspDiagnosticsRecordPageEviction(std::uint32_t bytes) { g_activity.pageEvictions++; g_activity.pageEvictionBytes += bytes; }
	void PspDiagnosticsRecordEvictionFence() { g_activity.evictionFences++; }
	void PspDiagnosticsRecordStreamSubmit() { g_activity.streamSubmits++; }
	void PspDiagnosticsRecordStreamPending() { g_activity.streamPending++; }
}
