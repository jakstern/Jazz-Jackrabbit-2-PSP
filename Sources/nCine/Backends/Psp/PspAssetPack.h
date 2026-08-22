#pragma once

#include "PspGpuFormat.h"
#include <cstdint>
#include <cstdio>

// Device-side reader for the host-baked PSP-GU asset pack (Tools/psp-bake -> Cache/PspTextures.pack).
//
// The pack holds every tileset and is never loaded whole: only the header and entry/page tables live in RAM,
// pixels and CLUTs are read from the file on bind. Entries are keyed by asset-name hash (PspGpu::NameHash),
// which the engine knows before decoding anything, so a hit skips the whole decode/atlas/convert path.
namespace nCine
{
	class PspAssetPack
	{
	public:
		static PspAssetPack& Get();
		void Reset();

		// Header-only check of "<cachePath>/texture.pak" against PackMagic/PackVersion. The first-launch gate uses
		// it so a format bump forces a re-bake rather than loading a stale pack.
		static bool IsCompatibleOnDisk(const char* cachePath);

		// Open the pack and read its tables; loads once, returns whether a valid pack is open.
		bool EnsureLoaded(const char* cachePath);
		// Close device-backed handles before standby and reopen them afterwards without discarding the tables
		// referenced by live textures. Resume remains retryable if Memory Stick access is not ready yet.
		void Suspend();
		bool Resume();
		bool IsLoaded() const { return file_ != nullptr; }

		// Find a baked texture by asset-name hash, or nullptr.
		const PspGpu::TexEntry* Find(std::uint64_t key) const;

		// Paged entries: one page's pixels (PageStride*PageStride*bpp bytes).
		bool ReadPage(const PspGpu::TexEntry& entry, int pageIndex, void* dst, std::size_t dstSize);

		// Sprite entries: the whole non-paged pixel blob (stride*strideH*bpp bytes).
		bool ReadData(const PspGpu::TexEntry& entry, void* dst, std::size_t dstSize);

		// The entry's own 256-entry CLUT (1 KiB). Fails unless flagged TextureFlagOwnClut - any other entry's
		// stored CLUT is a stale snapshot of the game palette and the device must bind the live one.
		bool ReadOwnClut(const PspGpu::TexEntry& entry, void* dst, std::size_t dstSize);

		// A sprite's collision mask, bit-packed (see MaskPackedBytes).
		bool ReadMask(const PspGpu::TexEntry& entry, void* dst, std::size_t dstSize);

		// A sprite's .aura header bytes (entry.metaSize).
		bool ReadMeta(const PspGpu::TexEntry& entry, void* dst, std::size_t dstSize);

		// Page metadata (offset within the atlas + valid size) for entry's page.
		const PspGpu::TexPage* Page(const PspGpu::TexEntry& entry, int pageIndex) const;

	private:
		std::FILE* file_ = nullptr;
		bool triedLoad_ = false;
		bool reopenAfterResume_ = false;
		char path_[512]{};
		PspGpu::PackHeader header_ {};
		PspGpu::TexEntry* entries_ = nullptr;
		PspGpu::TexPage* pages_ = nullptr;
	};
}
