#include "PspAssetPack.h"
#include "PspTextureStreamer.h"
#include "PspHardware.h"

#include <cstdlib>
#include <cstring>

namespace nCine
{
	PspAssetPack& PspAssetPack::Get()
	{
		static PspAssetPack instance;
		return instance;
	}

	void PspAssetPack::Reset()
	{
		// Stop the reader first - it must not touch its descriptor while the pack is torn down.
		PspTextureStreamer::Get().Stop();
		if (file_ != nullptr) std::fclose(file_);
		std::free(entries_);
		std::free(pages_);
		file_ = nullptr;
		entries_ = nullptr;
		pages_ = nullptr;
		header_ = {};
		triedLoad_ = false;
		reopenAfterResume_ = false;
		path_[0] = '\0';
	}

	bool PspAssetPack::IsCompatibleOnDisk(const char* cachePath)
	{
		char path[512];
		std::snprintf(path, sizeof(path), "%s/texture.pak", cachePath);
		std::FILE* f = std::fopen(path, "rb");
		if (f == nullptr) return false;
		PspGpu::PackHeader h {};
		const bool ok = (std::fread(&h, sizeof(h), 1, f) == 1 && h.magic == PspGpu::PackMagic && h.version == PspGpu::PackVersion);
		std::fclose(f);
		return ok;
	}

	bool PspAssetPack::EnsureLoaded(const char* cachePath)
	{
		if (triedLoad_) return file_ != nullptr;
		triedLoad_ = true;

		std::snprintf(path_, sizeof(path_), "%s/texture.pak", cachePath);
		std::FILE* f = std::fopen(path_, "rb");
		if (f == nullptr) return false;

		PspGpu::PackHeader h {};
		if (std::fread(&h, sizeof(h), 1, f) != 1 || h.magic != PspGpu::PackMagic || h.version != PspGpu::PackVersion) {
			std::fclose(f);
			return false;
		}

		// Only the tables are held in RAM; pixel data stays on disk.
		const std::size_t entriesBytes = (std::size_t)h.entryCount * sizeof(PspGpu::TexEntry);
		auto* entries = static_cast<PspGpu::TexEntry*>(std::malloc(entriesBytes));
		std::fseek(f, (long)h.entriesOffset, SEEK_SET);
		if (entries == nullptr || (h.entryCount > 0 && std::fread(entries, sizeof(PspGpu::TexEntry), h.entryCount, f) != h.entryCount)) {
			std::free(entries); std::fclose(f); return false;
		}

		std::uint32_t totalPages = 0;
		for (std::uint32_t i = 0; i < h.entryCount; i++) {
			totalPages += (std::uint32_t)entries[i].pagesX * entries[i].pagesY;
		}
		const std::size_t pagesBytes = (std::size_t)totalPages * sizeof(PspGpu::TexPage);
		auto* pages = static_cast<PspGpu::TexPage*>(std::malloc(pagesBytes == 0 ? 1 : pagesBytes));
		const long pagesOffset = (long)h.entriesOffset + (long)entriesBytes;
		std::fseek(f, pagesOffset, SEEK_SET);
		if (pages == nullptr || (totalPages > 0 && std::fread(pages, sizeof(PspGpu::TexPage), totalPages, f) != totalPages)) {
			std::free(entries); std::free(pages); std::fclose(f); return false;
		}

		file_ = f;
		header_ = h;
		entries_ = entries;
		pages_ = pages;
		// PSP-2000/3000 keep textures eager-resident; only the PSP-1000 needs reads off the render thread.
		if (PspIsLowMemoryModel()) {
			PspTextureStreamer::Get().Start(path_);
		}
		return true;
	}

	void PspAssetPack::Suspend()
	{
		if (reopenAfterResume_) return;
		reopenAfterResume_ = (file_ != nullptr);
		if (!reopenAfterResume_) return;

		// Stop first: its raw descriptor and PspAssetPack's FILE refer to the same Memory Stick file but are
		// independently positioned and may both be inside an I/O call.
		PspTextureStreamer::Get().Stop();
		std::fclose(file_);
		file_ = nullptr;
	}

	bool PspAssetPack::Resume()
	{
		if (!reopenAfterResume_) return true;
		if (path_[0] == '\0') return false;

		std::FILE* reopened = std::fopen(path_, "rb");
		if (reopened == nullptr) return false;
		PspGpu::PackHeader reopenedHeader{};
		if (std::fread(&reopenedHeader, sizeof(reopenedHeader), 1, reopened) != 1 ||
			reopenedHeader.magic != header_.magic || reopenedHeader.version != header_.version ||
			reopenedHeader.entryCount != header_.entryCount || reopenedHeader.entriesOffset != header_.entriesOffset ||
			reopenedHeader.blobOffset != header_.blobOffset || reopenedHeader.blobSize != header_.blobSize) {
			std::fclose(reopened);
			return false;
		}
		file_ = reopened;
		if (PspIsLowMemoryModel()) PspTextureStreamer::Get().Start(path_);
		reopenAfterResume_ = false;
		return true;
	}

	const PspGpu::TexEntry* PspAssetPack::Find(std::uint64_t key) const
	{
		if (entries_ == nullptr) return nullptr;
		for (std::uint32_t i = 0; i < header_.entryCount; i++) {
			if (entries_[i].key == key) return &entries_[i];
		}
		return nullptr;
	}

	const PspGpu::TexPage* PspAssetPack::Page(const PspGpu::TexEntry& entry, int pageIndex) const
	{
		if (pages_ == nullptr || pageIndex < 0 || pageIndex >= entry.pagesX * entry.pagesY) return nullptr;
		return &pages_[entry.firstPage + pageIndex];
	}

	bool PspAssetPack::ReadPage(const PspGpu::TexEntry& entry, int pageIndex, void* dst, std::size_t dstSize)
	{
		const PspGpu::TexPage* p = Page(entry, pageIndex);
		if (p == nullptr || file_ == nullptr) return false;
		const std::size_t bytes = (std::size_t)PspGpu::PageStride * PspGpu::PageStride * PspGpu::BytesPerTexel((PspGpu::PixelFormat)entry.format);
		if (dstSize < bytes) return false;
		std::fseek(file_, (long)p->dataOffset, SEEK_SET);
		return std::fread(dst, 1, bytes, file_) == bytes;
	}

	bool PspAssetPack::ReadData(const PspGpu::TexEntry& entry, void* dst, std::size_t dstSize)
	{
		if (file_ == nullptr) return false;
		const std::size_t bytes = (std::size_t)entry.stride * entry.strideH * PspGpu::BytesPerTexel((PspGpu::PixelFormat)entry.format);
		if (dstSize < bytes) return false;
		std::fseek(file_, (long)entry.dataOffset, SEEK_SET);
		return std::fread(dst, 1, bytes, file_) == bytes;
	}

	bool PspAssetPack::ReadOwnClut(const PspGpu::TexEntry& entry, void* dst, std::size_t dstSize)
	{
		if (file_ == nullptr || entry.clutOffset == 0xFFFFFFFFu) return false;
		if ((entry.flags & PspGpu::TextureFlagOwnClut) == 0) return false;
		constexpr std::size_t bytes = 256 * sizeof(std::uint32_t);
		if (dstSize < bytes) return false;
		std::fseek(file_, (long)entry.clutOffset, SEEK_SET);
		return std::fread(dst, 1, bytes, file_) == bytes;
	}

	bool PspAssetPack::ReadMask(const PspGpu::TexEntry& entry, void* dst, std::size_t dstSize)
	{
		if (file_ == nullptr || entry.maskOffset == 0xFFFFFFFFu) return false;
		// Bit-packed at 1 bit/texel since pack v5.
		const std::size_t bytes = PspGpu::MaskPackedBytes((std::size_t)entry.width * entry.height);
		if (dstSize < bytes) return false;
		std::fseek(file_, (long)entry.maskOffset, SEEK_SET);
		return std::fread(dst, 1, bytes, file_) == bytes;
	}

	bool PspAssetPack::ReadMeta(const PspGpu::TexEntry& entry, void* dst, std::size_t dstSize)
	{
		if (file_ == nullptr || entry.metaOffset == 0xFFFFFFFFu || entry.metaSize == 0) return false;
		if (dstSize < entry.metaSize) return false;
		std::fseek(file_, (long)entry.metaOffset, SEEK_SET);
		return std::fread(dst, 1, entry.metaSize, file_) == entry.metaSize;
	}
}
