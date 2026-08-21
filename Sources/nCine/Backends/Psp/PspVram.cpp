#include "PspVram.h"

#include <cstdint>

#include <pspge.h>

namespace nCine
{
	namespace
	{
		// 2 MiB of eDRAM / 256 KiB per slot, i.e. the count with no framebuffer reserved at all.
		// PspVramInit computes the real one.
		constexpr int MaxSlots = 8;

		std::uint8_t* g_regionBase = nullptr;   // absolute
		std::size_t g_regionBytes = 0;
		int g_slotCount = 0;
		bool g_slotUsed[MaxSlots] = {};
		bool g_initialized = false;
	}

	void PspVramInit(std::size_t reservedBytes)
	{
		if (g_initialized) return;
		auto* edram = static_cast<std::uint8_t*>(sceGeEdramGetAddr());
		if (edram == nullptr) return;
		const std::size_t total = sceGeEdramGetSize();
		if (reservedBytes >= total) return;

		// Today's reservation is 16-byte aligned by construction (512 * 272 * bpp); don't depend on whatever
		// gets reserved next also being so.
		const std::size_t alignedReserve = (reservedBytes + 15u) & ~static_cast<std::size_t>(15u);
		if (alignedReserve >= total) return;

		const std::size_t usable = total - alignedReserve;
		int count = static_cast<int>(usable / PspVramSlotBytes);
		if (count > MaxSlots) count = MaxSlots;
		if (count <= 0) return; // no whole slot fits: leave the pool inert, every acquire fails to the heap

		g_regionBase = edram + alignedReserve;
		g_slotCount = count;
		g_regionBytes = static_cast<std::size_t>(count) * PspVramSlotBytes;
		for (int i = 0; i < MaxSlots; ++i) g_slotUsed[i] = false;
		g_initialized = true;
	}

	void* PspVramAcquireSlot(std::size_t bytes)
	{
		// Exact size only: rounding up wastes a whole 256 KiB slot, and anything larger would overrun the next.
		if (!g_initialized || bytes != PspVramSlotBytes) return nullptr;
		for (int i = 0; i < g_slotCount; ++i) {
			if (!g_slotUsed[i]) {
				g_slotUsed[i] = true;
				return g_regionBase + static_cast<std::size_t>(i) * PspVramSlotBytes;
			}
		}
		return nullptr;
	}

	void PspVramReleaseSlot(void* ptr)
	{
		if (!PspVramContains(ptr)) return;
		const std::size_t offset = static_cast<std::size_t>(static_cast<std::uint8_t*>(ptr) - g_regionBase);
		// Anything off a slot boundary is an interior pointer; freeing its slot would release storage still in use.
		if ((offset % PspVramSlotBytes) != 0) return;
		g_slotUsed[offset / PspVramSlotBytes] = false;
	}

	bool PspVramContains(const void* ptr)
	{
		if (!g_initialized || ptr == nullptr) return false;
		const auto* p = static_cast<const std::uint8_t*>(ptr);
		return (p >= g_regionBase && p < g_regionBase + g_regionBytes);
	}

	std::size_t PspVramSlotCount()
	{
		return static_cast<std::size_t>(g_slotCount);
	}

	std::size_t PspVramSlotsUsed()
	{
		std::size_t used = 0;
		for (int i = 0; i < g_slotCount; ++i) {
			if (g_slotUsed[i]) ++used;
		}
		return used;
	}

	std::size_t PspVramCapacityBytes()
	{
		return g_regionBytes;
	}

	std::size_t PspVramUsedBytes()
	{
		return PspVramSlotsUsed() * PspVramSlotBytes;
	}
}
