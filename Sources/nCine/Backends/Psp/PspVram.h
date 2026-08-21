#pragma once

#include <cstddef>

// Fixed-slot pool over the eDRAM left after the framebuffers (~1.47 MiB at 5650). The GE reaches eDRAM over a
// dedicated path instead of contending for the system bus, so anything it re-reads every frame is cheaper here.
//
// Uniform slots rather than an allocator: every tileset page in the baked pack is exactly PspVramSlotBytes, so
// the pool cannot fragment and can take a slot back when a level's tileset is released. Odd sizes are refused,
// not rounded up, so e.g. a paged RGBA8888 atlas stays in the heap instead of eating four slots.
//
// Caller obligations:
//   * Slot pointers are absolute (what sceGuTexImage wants). sceGuDrawBuffer/sceGuDispBuffer are the odd ones
//     out - they take VRAM-RELATIVE offsets - so never pass a slot pointer there unconverted.
//   * sceGeEdramGetAddr returns the CACHED alias, so CPU writes through a slot pointer need a
//     sceKernelDcacheWritebackRange before the GE reads them, exactly as in main RAM.
//   * The GE may still be reading a slot handed out earlier: fence (PspGuWaitForPreviousFrame) before releasing
//     one whose pixels are still bound.
namespace nCine
{
	// One 512x512 T8 tileset page - see PspGpu::PageStride.
	constexpr std::size_t PspVramSlotBytes = 512 * 512;

	// Claims the eDRAM above `reservedBytes` (which must cover everything already at the bottom of VRAM, i.e.
	// both framebuffers) and divides it into whole slots. Called by PspGuInit; re-entry is a no-op.
	void PspVramInit(std::size_t reservedBytes);

	// Takes a free slot, or nullptr when exhausted, uninitialized, or `bytes` is not exactly PspVramSlotBytes.
	// Callers must fall back to the heap on nullptr - VRAM residency is an optimisation, never a requirement.
	void* PspVramAcquireSlot(std::size_t bytes);

	// Returns a slot to the pool. Ignores null and foreign pointers, so heap and VRAM frees can share a path.
	void PspVramReleaseSlot(void* ptr);

	// Whether `ptr` is in the pool - notably so std::free() is never called on VRAM.
	bool PspVramContains(const void* ptr);

	std::size_t PspVramSlotCount();
	std::size_t PspVramSlotsUsed();
	std::size_t PspVramCapacityBytes();
	std::size_t PspVramUsedBytes();
}
