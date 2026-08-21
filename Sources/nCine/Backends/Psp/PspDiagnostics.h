#pragma once

#include <cstdint>

namespace nCine
{
	enum class PspDiagnosticsPhase : std::uint8_t
	{
		Begin,
		Update,
		PostUpdate,
		Visit,
		VisitCulling,
		VisitCollect,
		Queue,
		Emit,
		GpuWait,
		VBlank,
		Count
	};

	struct PspDiagnosticsSnapshot
	{
		float totalMs = 0.0f;
		float phaseMs[static_cast<int>(PspDiagnosticsPhase::Count)] {};
		std::uint32_t draws = 0;
		std::uint32_t vertices = 0;
		std::uint32_t culled = 0;
		std::uint32_t textureImages = 0;
		std::uint32_t clutLoads = 0;
		std::uint32_t spriteCommands = 0;
		std::uint32_t meshCommands = 0;
		std::uint32_t meshVertices = 0;
		std::uint32_t batchFlushes = 0;
		std::uint32_t batchedQuads = 0;
		std::uint32_t heapArenaBytes = 0;
		std::uint32_t heapUsedBytes = 0;
		std::uint32_t heapFreeBytes = 0;
		std::uint32_t kernelFreeBytes = 0;
	};

	// Full memory picture, sampled once per second (never per frame) and only when a log sink is registered.
	// All byte fields are raw bytes.
	struct PspMemoryReport
	{
		// newlib heap (mallinfo) + kernel partition
		std::uint32_t heapArena = 0;       // total sbrk'd arena
		std::uint32_t heapUsed = 0;        // mallinfo.uordblks
		std::uint32_t heapFree = 0;        // mallinfo.fordblks (free inside the arena)
		std::uint32_t heapKeepCost = 0;    // mallinfo.keepcost (top block returnable to the OS)
		std::uint32_t heapFreeBlocks = 0;  // mallinfo.ordblks (free-list fragment count)
		std::uint32_t maxFreeBlock = 0;    // sceKernelMaxFreeMemSize (largest single allocatable block)
		std::uint32_t kernelFree = 0;      // sceKernelTotalFreeMemSize (kernel partition, NOT app RAM)
		std::uint32_t heapFreeLow = 0;     // low-water of heapFree since boot
		std::uint32_t maxFreeLow = 0;      // low-water of maxFreeBlock since boot

		// GU texture residency — the unified resident-byte counter
		std::uint32_t texResidentTotal = 0; // sum of the categories below
		std::uint32_t texSpritePixels = 0;  // non-paged sprite pixel blobs
		std::uint32_t texPagePixels = 0;    // resident tileset atlas pages
		std::uint32_t texVariantBytes = 0;  // RG8->RGBA variant bakes + paged rgba8888 scratch
		std::uint16_t texCount = 0;
		std::uint16_t texStreamed = 0;         // lowMemoryStreamed textures
		std::uint16_t texResidentSprites = 0;  // non-paged textures with pixels resident
		std::uint16_t texPagedAtlases = 0;
		std::uint16_t texResidentPages = 0;
		std::uint8_t streamerRunning = 0;      // 1 = async streamer worker up (else all reads are synchronous)

		// GU display list consumption. Nothing in pspsdk bounds-checks sceGuGetMemory, so a frame whose geometry
		// exceeds the list writes vertices and GE commands into the next heap block - highWater must stay under
		// capacity and overflowFrames at zero.
		std::uint32_t guListBytes = 0;
		std::uint32_t guListHighWater = 0;     // peak since boot
		std::uint32_t guListCapacity = 0;
		std::uint32_t guListOverflowFrames = 0;
		std::uint32_t guListDroppedCommands = 0;

		// content-layer footprint (filled by the registered content provider)
		std::uint32_t maskResidentBytes = 0;
		std::uint16_t cachedMetadata = 0;
		std::uint16_t cachedGraphics = 0;
		std::uint16_t cachedSounds = 0;

		// activity since the previous report (per-second deltas)
		std::uint32_t streamReads = 0, streamReadBytes = 0;  // draw-time sprite pixel reads (cache misses)
		std::uint32_t pageReads = 0, pageReadBytes = 0;      // draw-time tileset page reads
		std::uint32_t evictions = 0, evictionBytes = 0;
		std::uint32_t pageEvictions = 0, pageEvictionBytes = 0;
		std::uint32_t evictionFences = 0;                    // GU fences forced by an eviction
		std::uint32_t cacheHits = 0;                         // draw-time binds served from resident memory
		std::uint32_t streamSubmits = 0;                     // background sprite reads started off the render thread
		std::uint32_t streamPending = 0;                     // frames a sprite was skipped waiting for its stream
	};

	// Async writer sink (registered by the EBOOT under -DPSP_DEBUG) and content-memory provider (registered by the
	// game layer; fills the mask*/cached* fields). With no sink, sampling is skipped and only the counter bumps run.
	using PspDiagLogSink = void (*)(const char* line);
	using PspDiagContentProvider = void (*)(PspMemoryReport& report);
	void PspDiagnosticsSetLogSink(PspDiagLogSink sink);
	// One-off marker in the memory-report log; no-op without a sink. Used to bracket the level-decode path.
	void PspDiagnosticsLog(const char* line);
	void PspDiagnosticsSetContentMemoryProvider(PspDiagContentProvider provider);

	// Cheap event counters called from the GU emitter's streaming/eviction hot paths.
	void PspDiagnosticsRecordCacheHit();
	void PspDiagnosticsRecordStreamRead(std::uint32_t bytes);
	void PspDiagnosticsRecordPageRead(std::uint32_t bytes);
	void PspDiagnosticsRecordEviction(std::uint32_t bytes);
	void PspDiagnosticsRecordPageEviction(std::uint32_t bytes);
	void PspDiagnosticsRecordEvictionFence();
	void PspDiagnosticsRecordStreamSubmit();
	void PspDiagnosticsRecordStreamPending();

	// Fills the tex* residency fields by walking the GU texture registry. Defined in PspEmitter.cpp.
	void PspEmitCollectTextureMemory(PspMemoryReport& report);

	void PspDiagnosticsBeginFrame();
	void PspDiagnosticsEndFrame();
	void PspDiagnosticsBeginPhase(PspDiagnosticsPhase phase);
	void PspDiagnosticsEndPhase(PspDiagnosticsPhase phase);
	void PspDiagnosticsRecordDraw(std::uint32_t vertices);
	void PspDiagnosticsRecordCulled();
	void PspDiagnosticsRecordTextureImage();
	void PspDiagnosticsRecordClutLoad();
	void PspDiagnosticsRecordSpriteCommand();
	void PspDiagnosticsRecordMeshCommand(std::uint32_t vertices);
	void PspDiagnosticsRecordBatchFlush(std::uint32_t quads);
	const PspDiagnosticsSnapshot& PspDiagnosticsGetSnapshot();
	bool PspDiagnosticsIsVisible();
	void PspDiagnosticsToggleVisible();
}
