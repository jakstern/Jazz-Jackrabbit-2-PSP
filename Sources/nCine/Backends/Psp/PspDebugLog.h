#pragma once

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <malloc.h>

// On-device debug log. Appends to "debug.log" next to the EBOOT from a worker thread; compiled to no-ops
// unless -DPSP_DEBUG=ON. Starts before the GU loop, where a hang is otherwise just a black screen.
namespace nCine
{
#if defined(JAZZ2_PSP_DEBUG)
	constexpr std::uint32_t DebugQueueCapacity = 32;
	constexpr std::uint32_t DebugMessageSize = 768;
	struct DebugMessage { char Text[DebugMessageSize]; };
	inline DebugMessage g_debugQueue[DebugQueueCapacity];
	inline std::uint32_t g_debugRead = 0;
	inline std::uint32_t g_debugWrite = 0;
	inline std::uint32_t g_debugCount = 0;
	inline SceUID g_debugMutex = -1;
	inline SceUID g_debugWake = -1;
	inline SceUID g_debugThread = -1;
	inline bool g_debugRunning = false;
	inline void DbgLogImmediate(const char* msg, const char* extra)
	{
		FILE* f = std::fopen("debug.log", "a");
		if (f == nullptr) return;
		std::fputs(msg, f);
		if (extra != nullptr) { std::fputc(' ', f); std::fputs(extra, f); }
		std::fputc('\n', f);
		std::fflush(f);
		std::fclose(f);
	}

	inline int DebugWriterThread(SceSize, void*)
	{
		FILE* f = std::fopen("debug.log", "a");
		while (true) {
			sceKernelWaitSema(g_debugWake, 1, nullptr);
			char text[DebugMessageSize];
			bool hasMessage = false;
			bool running;
			sceKernelWaitSema(g_debugMutex, 1, nullptr);
			if (g_debugCount > 0) {
				std::memcpy(text, g_debugQueue[g_debugRead].Text, sizeof(text));
				g_debugRead = (g_debugRead + 1) % DebugQueueCapacity;
				g_debugCount--;
				hasMessage = true;
			}
			running = g_debugRunning;
			sceKernelSignalSema(g_debugMutex, 1);

			if (!hasMessage) {
				if (!running) break;
				continue;
			}
			if (f != nullptr) {
				std::fputs(text, f);
				std::fputc('\n', f);
				// Close/reopen instead of fflush: the FAT entry (and thus the readable file size) is only
				// committed on close, so after a hard crash the tail would silently look truncated. At a few
				// lines per second, an open/close per line is worth a trustworthy tail.
				std::fclose(f);
				f = std::fopen("debug.log", "a");
			}
		}
		if (f != nullptr) std::fclose(f);
		return 0;
	}

	inline void DbgReset() { FILE* f = std::fopen("debug.log", "w"); if (f) std::fclose(f); }

	inline void DbgStart()
	{
		g_debugMutex = sceKernelCreateSema("Jazz2DebugMutex", 0, 1, 1, nullptr);
		g_debugWake = sceKernelCreateSema("Jazz2DebugWake", 0, 0, DebugQueueCapacity + 1, nullptr);
		if (g_debugMutex < 0 || g_debugWake < 0) goto fail;
		g_debugRunning = true;
		g_debugThread = sceKernelCreateThread("Jazz2Debug", DebugWriterThread, 0x30, 8 * 1024, PSP_THREAD_ATTR_USER, nullptr);
		if (g_debugThread < 0 || sceKernelStartThread(g_debugThread, 0, nullptr) < 0) goto fail;
		return;

	fail:
		g_debugRunning = false;
		if (g_debugThread >= 0) { sceKernelDeleteThread(g_debugThread); g_debugThread = -1; }
		if (g_debugWake >= 0) { sceKernelDeleteSema(g_debugWake); g_debugWake = -1; }
		if (g_debugMutex >= 0) { sceKernelDeleteSema(g_debugMutex); g_debugMutex = -1; }
	}

	inline void DbgStop()
	{
		if (g_debugThread < 0) return;
		sceKernelWaitSema(g_debugMutex, 1, nullptr);
		g_debugRunning = false;
		sceKernelSignalSema(g_debugMutex, 1);
		sceKernelSignalSema(g_debugWake, 1);
		sceKernelWaitThreadEnd(g_debugThread, nullptr);
		sceKernelDeleteThread(g_debugThread);
		sceKernelDeleteSema(g_debugWake);
		sceKernelDeleteSema(g_debugMutex);
		g_debugThread = g_debugWake = g_debugMutex = -1;
	}

	inline void DbgLog(const char* msg, const char* extra = nullptr)
	{
		if (g_debugThread < 0) {
			DbgLogImmediate(msg, extra);
			return;
		}
		sceKernelWaitSema(g_debugMutex, 1, nullptr);
		if (g_debugCount < DebugQueueCapacity) {
			DebugMessage& entry = g_debugQueue[g_debugWrite];
			if (extra != nullptr) std::snprintf(entry.Text, sizeof(entry.Text), "%s %s", msg, extra);
			else std::snprintf(entry.Text, sizeof(entry.Text), "%s", msg);
			g_debugWrite = (g_debugWrite + 1) % DebugQueueCapacity;
			g_debugCount++;
			sceKernelSignalSema(g_debugMutex, 1);
			sceKernelSignalSema(g_debugWake, 1);
		} else {
			sceKernelSignalSema(g_debugMutex, 1);
		}
	}
	inline void DbgHeap(const char* stage)
	{
		const struct mallinfo heap = mallinfo();
		char line[128];
		std::snprintf(line, sizeof(line), "%s heapUsed=%d heapFree=%d kernelFree=%u",
			stage, heap.uordblks, heap.fordblks, (unsigned)sceKernelTotalFreeMemSize());
		DbgLog(line);
	}
#else
	// Release build: call sites stay so they cannot rot, and fold away to nothing.
	inline void DbgReset() {}
	inline void DbgStart() {}
	inline void DbgStop() {}
	inline void DbgLog(const char*, const char* = nullptr) {}
	inline void DbgHeap(const char*) {}
#endif
}
