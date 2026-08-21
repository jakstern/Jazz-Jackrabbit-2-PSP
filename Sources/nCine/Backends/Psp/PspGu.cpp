#include "PspGu.h"
#include "PspDiagnostics.h"
#include "PspHardware.h"
#include "PspVram.h"

#include <cstddef>
#include <cstdlib>
#include <malloc.h>

#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>

namespace nCine
{
	namespace
	{
		constexpr int ScreenWidth = 480;
		constexpr int ScreenHeight = 272;
		constexpr int BufferWidth = 512;

		// Framebuffer PSM only - texture storage (sceGuTexMode), CLUTs and vertex colours are separate GE
		// registers and stay at full precision. 5650 rather than 8888 halves the eDRAM these buffers occupy
		// (1088 -> 544 KiB of 2 MiB, which is what leaves room for the VRAM pool) and halves blend
		// read/modify/write bandwidth. Safe only because nothing reads the framebuffer back (no screenshot,
		// no sceGuCopyImage, no render-to-texture) and blending never uses a destination-alpha factor, which
		// 5650 could not supply. Cost is banding, hidden by the dither matrix below.
		constexpr int FrameBufferFormat = GU_PSM_5650;
		using FrameBufferPixel = std::uint16_t;
		// Offset of the second buffer. Derived from FrameBufferPixel so it tracks the format: at the wrong
		// pixel size the buffers either overlap or strand eDRAM.
		constexpr std::size_t FrameBufferSize = BufferWidth * ScreenHeight * sizeof(FrameBufferPixel);

		// Nothing in pspsdk bounds-checks sceGuGetMemory: a frame that outgrows this list writes into the next
		// heap block. The budget in PspEmitter.cpp derives from this value and is what prevents that.
		// Measured peak on hardware is 93,904 bytes of vertex data for a busy gameplay frame (~6 KB for menus);
		// the GE command stream shares the buffer, so call it ~110 KB. 256 KiB is >2x headroom.
		constexpr std::size_t DisplayListBytes = 256 * 1024;
		void* g_displayList = nullptr;
		bool g_initialized = false;
		// True between sceGuStart and sceGuFinish. NOT the same as g_frameOpen: PspGuBeginFrame starts the list,
		// clears and runs PspEmitResetFrameState (which frees textures) before setting g_frameOpen, so gating the
		// fence on g_frameOpen would leave that window unprotected.
		bool g_listOpen = false;
		bool g_frameOpen = false;
		bool g_framePending = false;
		bool g_pendingFrameComplete = false;
		void (*g_systemUtilityUpdate)() = nullptr;
		bool g_restoreAfterSystemUtility = false;

		// Ordered 4x4 Bayer dither: signed 4-bit offsets applied before the GE truncates to 5/6/5. Free in
		// hardware, and what makes the 16-bit framebuffer viable for stacked alpha and fades.
		void ApplyDitherState()
		{
			ScePspIMatrix4 dither = {
				{ -4,  0, -3,  1 },
				{  2, -2,  3, -1 },
				{ -3,  1, -4,  0 },
				{  3, -1,  2, -2 }
			};
			sceGuSetDither(&dither);
			sceGuEnable(GU_DITHER);
		}
	}

	// Capacity of the display list; the emitter budgets against it.
	std::size_t PspGuDisplayListBytes()
	{
		return DisplayListBytes;
	}

	void PspGuInit()
	{
		if (g_initialized) return;
		const std::size_t listBytes = DisplayListBytes;
		g_displayList = memalign(64, listBytes);
		if (g_displayList == nullptr) return;
		sceGuInit();
		sceGuStart(GU_DIRECT, g_displayList);
		g_listOpen = true;
		// pspsdk remembers this PSM and derives the sceDisplaySetFrameBuf format from it on swap, so the
		// display side follows the draw side automatically. Both take VRAM-relative offsets, not pointers.
		sceGuDrawBuffer(FrameBufferFormat, reinterpret_cast<void*>(0), BufferWidth);
		sceGuDispBuffer(ScreenWidth, ScreenHeight, reinterpret_cast<void*>(FrameBufferSize), BufferWidth);
		// The framebuffers are all that sits at the bottom of eDRAM; the rest becomes tileset page slots.
		// At 5650 that is 1,540,096 bytes = 5 whole 256 KiB slots.
		PspVramInit(2 * FrameBufferSize);
		sceGuOffset(2048 - ScreenWidth / 2, 2048 - ScreenHeight / 2);
		sceGuViewport(2048, 2048, ScreenWidth, ScreenHeight);
		sceGuScissor(0, 0, ScreenWidth, ScreenHeight);
		sceGuEnable(GU_SCISSOR_TEST);
		sceGuDisable(GU_DEPTH_TEST);
		sceGuEnable(GU_BLEND);
		sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
		// Drop fully transparent texels before blending - the menu's stacked full-screen layers are mostly
		// those, and they would otherwise cost framebuffer read/modify/write bandwidth.
		sceGuAlphaFunc(GU_GREATER, 0, 0xff);
		sceGuEnable(GU_ALPHA_TEST);
		ApplyDitherState();
		sceGuFinish();
		g_listOpen = false;
		sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
		sceDisplayWaitVblankStart();
		sceGuDisplay(GU_TRUE);
		g_initialized = true;
	}

	// All defined in PspEmitter.cpp.
	void PspEmitResetFrameState();
	void PspEmitFlushBatches();
	void PspEmitInvalidateGuState();

	void PspGuWaitForPreviousFrame()
	{
		// A list still being built has ALREADY been handed to the GE - sceGuStart(GU_DIRECT) enqueues it and
		// pspsdk advances the stall address per draw - so the hardware may be reading vertices or texture
		// memory out of it right now. Callers fence to make a free safe, so mid-list this must genuinely
		// drain: finish, sync, restart in the same buffer. Returning early here silently breaks every
		// "fence before we free" in the emitter (g_framePending is only set in PspGuEndFrame).
		if (g_listOpen) {
			sceGuFinish();
			sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
			sceGuStart(GU_DIRECT, g_displayList);
			// GE registers survive across lists, but the emitter's shadow of them describes the retired list
			// and the buffer has been rewound.
			PspEmitInvalidateGuState();
			return;
		}
		if (!g_framePending || g_pendingFrameComplete) return;
		sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
		g_pendingFrameComplete = true;
	}

	void PspGuPrepareFrame()
	{
		if (!g_framePending) return;
		if (!g_pendingFrameComplete) {
			PspDiagnosticsBeginPhase(PspDiagnosticsPhase::GpuWait);
			PspGuWaitForPreviousFrame();
			PspDiagnosticsEndPhase(PspDiagnosticsPhase::GpuWait);
		}
		PspDiagnosticsBeginPhase(PspDiagnosticsPhase::VBlank);
		sceDisplayWaitVblankStart();
		PspDiagnosticsEndPhase(PspDiagnosticsPhase::VBlank);
		sceGuSwapBuffers();
		g_framePending = false;
		g_pendingFrameComplete = false;
	}

	void PspGuSetSystemUtilityUpdate(void (*callback)())
	{
		g_systemUtilityUpdate = callback;
	}

	void PspGuNotifySystemUtilityFinished()
	{
		g_restoreAfterSystemUtility = true;
	}

	void PspGuBeginFrame(std::uint32_t clearColor)
	{
		if (!g_initialized || g_frameOpen) return;
		PspGuPrepareFrame();
		sceGuStart(GU_DIRECT, g_displayList);
		g_listOpen = true;
		if (g_restoreAfterSystemUtility) {
			// A visible system utility owns the GU and may change any persistent register, so re-establish
			// everything the emitter assumes before taking game draws again.
			sceGuOffset(2048 - ScreenWidth / 2, 2048 - ScreenHeight / 2);
			sceGuViewport(2048, 2048, ScreenWidth, ScreenHeight);
			sceGuScissor(0, 0, ScreenWidth, ScreenHeight);
			sceGuEnable(GU_SCISSOR_TEST);
			sceGuDisable(GU_DEPTH_TEST);
			sceGuEnable(GU_BLEND);
			sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
			sceGuAlphaFunc(GU_GREATER, 0, 0xff);
			sceGuEnable(GU_ALPHA_TEST);
			ApplyDitherState();
			g_restoreAfterSystemUtility = false;
		}
		sceGuClearColor(clearColor);
		sceGuClear(GU_COLOR_BUFFER_BIT);
		PspEmitResetFrameState();
		g_frameOpen = true;
	}

	void PspGuEndFrame()
	{
		if (!g_frameOpen) return;
		PspEmitFlushBatches();
		sceGuFinish();
		g_listOpen = false;
		g_frameOpen = false;
		// Ordering from the SDK utility samples: finish the game list, let the utility draw, then present the
		// combined framebuffer. The pipelined path stays untouched when no dialog is up.
		if (g_systemUtilityUpdate != nullptr) {
			sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
			g_systemUtilityUpdate();
		}
		g_framePending = true;
		g_pendingFrameComplete = false;
	}

	bool PspGuIsInitialized()
	{
		return g_initialized;
	}
}
