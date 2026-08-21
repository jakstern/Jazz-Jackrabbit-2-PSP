// Tileset -> PSP-GU pack baker (Stage 2).
//
// Bakes a native `.j2t` diffuse atlas into the form the device binds directly: indexed (GU_PSM_T8), padded,
// split into cell-aligned 512-pages, swizzled, plus the 256-entry CLUT.
//
// The atlas assembly and QOI decode are a VERBATIM port of ContentResolver::BuildTilesetDiffuse /
// ReadImageFromFile / ExpandTileDiffuse, so the baked bytes match what the device would produce at runtime;
// keep them in sync. Tilesets containing any 32-bit tile are skipped and left to the runtime path.

#include "TilesetBake.h"
#include "nCine/Backends/Psp/PspGpuFormat.h"
#include "Jazz2/Compatibility/JJ2Anims.Palettes.h"
#include "nCine/Primitives/Color.h"

#include <IO/FileSystem.h>
#include <IO/Stream.h>
#include <IO/Compression/DeflateStream.h>
#include <jsoncpp/json.h>

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace Death::Containers;
using namespace Death::Containers::Literals;
using namespace Death::IO;
using namespace Death::IO::Compression;
using namespace nCine::PspGpu;

namespace {
	constexpr int TileSize = 32;             // TileSet::DefaultTileSize
	constexpr int ColorsPerPalette = 256;

	// QOI decode, verbatim from ContentResolver::ReadImageFromFile.
	void ReadImageFromFile(Stream& s, std::uint8_t* data, std::int32_t width, std::int32_t height, std::int32_t channelCount)
	{
		typedef union { struct { unsigned char r, g, b, a; } rgba; unsigned int v; } rgba_t;
		#define QOI_OP_INDEX 0x00
		#define QOI_OP_DIFF  0x40
		#define QOI_OP_LUMA  0x80
		#define QOI_OP_RUN   0xc0
		#define QOI_OP_RGB   0xfe
		#define QOI_OP_RGBA  0xff
		#define QOI_MASK_2   0xc0
		#define QOI_COLOR_HASH(C) (C.rgba.r*3 + C.rgba.g*5 + C.rgba.b*7 + C.rgba.a*11)
		rgba_t index[64] { };
		rgba_t px;
		std::int32_t run = 0;
		std::int32_t px_len = width * height * channelCount;
		px.rgba.r = 0; px.rgba.g = 0; px.rgba.b = 0; px.rgba.a = 255;
		for (std::int32_t px_pos = 0; px_pos < px_len; px_pos += channelCount) {
			if (run > 0) {
				run--;
			} else {
				std::int32_t b1 = s.ReadValue<std::uint8_t>();
				if (b1 == QOI_OP_RGB) {
					px.rgba.r = s.ReadValue<std::uint8_t>();
					px.rgba.g = (channelCount >= 2 ? s.ReadValue<std::uint8_t>() : 0);
					px.rgba.b = (channelCount >= 3 ? s.ReadValue<std::uint8_t>() : 0);
				} else if (b1 == QOI_OP_RGBA) {
					px.rgba.r = s.ReadValue<std::uint8_t>();
					px.rgba.g = s.ReadValue<std::uint8_t>();
					px.rgba.b = s.ReadValue<std::uint8_t>();
					px.rgba.a = s.ReadValue<std::uint8_t>();
				} else if ((b1 & QOI_MASK_2) == QOI_OP_INDEX) {
					px = index[b1];
				} else if ((b1 & QOI_MASK_2) == QOI_OP_DIFF) {
					px.rgba.r += ((b1 >> 4) & 0x03) - 2;
					px.rgba.g += ((b1 >> 2) & 0x03) - 2;
					px.rgba.b += (b1 & 0x03) - 2;
				} else if ((b1 & QOI_MASK_2) == QOI_OP_LUMA) {
					std::int32_t b2 = s.ReadValue<std::uint8_t>();
					std::int32_t vg = (b1 & 0x3f) - 32;
					px.rgba.r += vg - 8 + ((b2 >> 4) & 0x0f);
					px.rgba.g += vg;
					px.rgba.b += vg - 8 + (b2 & 0x0f);
				} else if ((b1 & QOI_MASK_2) == QOI_OP_RUN) {
					run = (b1 & 0x3f);
				}
				index[QOI_COLOR_HASH(px) & 63] = px;
			}
			// Must copy exactly channelCount bytes: the destination is tightly packed, so a 32-bit store is
			// only aligned for RGBA and faults on real MIPS hardware for indexed/RG sprites.
			std::memcpy(data + px_pos, &px, (std::size_t)channelCount);
		}
	}

	// Decode QOI in 32-row tile bands so no contiguous full-image buffer (nor a scratch file) is needed:
	// the consumer expands each band straight into the current set of GU pages.
	template<class BandSink>
	bool ReadImageInTileBands(Stream& s, std::int32_t width, std::int32_t height,
		std::int32_t channelCount, BandSink&& consumeBand)
	{
		typedef union { struct { unsigned char r, g, b, a; } rgba; unsigned int v; } rgba_t;
		rgba_t index[64]{};
		rgba_t px;
		std::int32_t run = 0;
		px.rgba.r = 0; px.rgba.g = 0; px.rgba.b = 0; px.rgba.a = 255;
		std::vector<std::uint8_t> band((std::size_t)width * TileSize * channelCount);
		const std::int32_t pixelCount = width * height;
		for (std::int32_t pixel = 0; pixel < pixelCount; ++pixel) {
			if (run > 0) {
				--run;
			} else {
				const std::int32_t b1 = s.ReadValue<std::uint8_t>();
				if (b1 == QOI_OP_RGB) {
					px.rgba.r = s.ReadValue<std::uint8_t>();
					px.rgba.g = (channelCount >= 2 ? s.ReadValue<std::uint8_t>() : 0);
					px.rgba.b = (channelCount >= 3 ? s.ReadValue<std::uint8_t>() : 0);
				} else if (b1 == QOI_OP_RGBA) {
					px.rgba.r = s.ReadValue<std::uint8_t>();
					px.rgba.g = s.ReadValue<std::uint8_t>();
					px.rgba.b = s.ReadValue<std::uint8_t>();
					px.rgba.a = s.ReadValue<std::uint8_t>();
				} else if ((b1 & QOI_MASK_2) == QOI_OP_INDEX) {
					px = index[b1];
				} else if ((b1 & QOI_MASK_2) == QOI_OP_DIFF) {
					px.rgba.r += ((b1 >> 4) & 0x03) - 2;
					px.rgba.g += ((b1 >> 2) & 0x03) - 2;
					px.rgba.b += (b1 & 0x03) - 2;
				} else if ((b1 & QOI_MASK_2) == QOI_OP_LUMA) {
					const std::int32_t b2 = s.ReadValue<std::uint8_t>();
					const std::int32_t vg = (b1 & 0x3f) - 32;
					px.rgba.r += vg - 8 + ((b2 >> 4) & 0x0f);
					px.rgba.g += vg;
					px.rgba.b += vg - 8 + (b2 & 0x0f);
				} else if ((b1 & QOI_MASK_2) == QOI_OP_RUN) {
					run = b1 & 0x3f;
				}
				index[(px.rgba.r * 3 + px.rgba.g * 5 + px.rgba.b * 7 + px.rgba.a * 11) & 63] = px;
			}
			const std::uint8_t* channels = reinterpret_cast<const std::uint8_t*>(&px);
			const std::int32_t x = pixel % width;
			const std::int32_t y = pixel / width;
			std::memcpy(&band[((std::size_t)(y % TileSize) * width + x) * channelCount],
				channels, channelCount);
			if (x == width - 1 && (y % TileSize == TileSize - 1 || y == height - 1)) {
				const int rows = y % TileSize + 1;
				if (!consumeBand(band.data(), y / TileSize, rows)) return false;
			}
		}
		return true;
	}

	// Edge-expand a 32px tile into its 1px padding; verbatim from ExpandTileDiffuse, 1 channel.
	void ExpandTileDiffuse(std::uint8_t* p, std::uint32_t pw, std::uint32_t bpp)
	{
		for (std::uint32_t x = 0; x < TileSize; x++) {
			std::memcpy(&p[(0*pw + (x+1))*bpp], &p[(1*pw + (x+1))*bpp], bpp);
			std::memcpy(&p[((TileSize+1)*pw + (x+1))*bpp], &p[(TileSize*pw + (x+1))*bpp], bpp);
		}
		for (std::uint32_t y = 0; y < TileSize; y++) {
			std::memcpy(&p[((y+1)*pw + 0)*bpp], &p[((y+1)*pw + 1)*bpp], bpp);
			std::memcpy(&p[((y+1)*pw + (TileSize+1))*bpp], &p[((y+1)*pw + TileSize)*bpp], bpp);
		}
		std::memcpy(&p[(0*pw + 0)*bpp], &p[(0*pw + 1)*bpp], bpp);
		std::memcpy(&p[(0*pw + (TileSize+1))*bpp], &p[(0*pw + TileSize)*bpp], bpp);
		std::memcpy(&p[((TileSize+1)*pw + 0)*bpp], &p[((TileSize+1)*pw + 1)*bpp], bpp);
		std::memcpy(&p[((TileSize+1)*pw + (TileSize+1))*bpp], &p[((TileSize+1)*pw + TileSize)*bpp], bpp);
	}
}

// Bake one native .j2t tileset. Returns false (and leaves *out untouched) for non-indexed tilesets.
bool BakeTilesetJ2t(const char* path, const char* name, BakedTexture* out, const char** skipReason)
{
	auto s = FileSystem::Open(path, FileAccess::Read);
	if (s == nullptr || !s->IsValid()) { *skipReason = "cannot open"; return false; }

	std::uint64_t sig1 = s->ReadValueAsLE<std::uint64_t>();
	std::uint16_t sig2 = s->ReadValueAsLE<std::uint16_t>();
	std::uint8_t version = s->ReadValue<std::uint8_t>();
	s->ReadValue<std::uint8_t>(); // flags
	if (sig1 != 0xB8EF8498E2BFBBEF || sig2 != 0x208F || version != 2) { *skipReason = "bad signature"; return false; }

	std::uint8_t channelCount = s->ReadValue<std::uint8_t>();
	std::uint32_t width = s->ReadValueAsLE<std::uint32_t>();
	std::uint32_t height = s->ReadValueAsLE<std::uint32_t>();
	std::uint16_t tileCount = s->ReadValueAsLE<std::uint16_t>();
	std::int32_t compressedSize = s->ReadValueAsLE<std::int32_t>();

	// Compressed block: palette (256 u32), is32bit flags, mask.
	std::uint32_t palette[ColorsPerPalette];
	std::vector<std::uint8_t> is32(( tileCount + 7) / 8);
	{
		DeflateStream uc(*s, compressedSize);
		for (int i = 0; i < ColorsPerPalette; i++) palette[i] = uc.ReadValueAsLE<std::uint32_t>();
		uc.Read(is32.data(), (std::uint32_t)is32.size());
		std::uint32_t maskSize = uc.ReadValueAsLE<std::uint32_t>();
		uc.Seek((std::int64_t)maskSize, SeekOrigin::Current);
	}

	bool indexed = true;
	for (int t = 0; t < tileCount; t++) {
		if ((is32[t / 8] & (1 << (t & 7))) != 0) { indexed = false; break; }
	}
	if (!indexed) { *skipReason = "contains 32-bit tiles (RGBA bake TODO)"; return false; }

	// Raw QOI follows the compressed block. Packed storage plus a three-byte tail guard: the decoder
	// advances by channelCount but stores through a four-byte temporary. Indexed tilesets therefore need
	// ~0.8 MiB here instead of a 3.4 MiB RGBA allocation on PSP-1000.
	std::vector<std::uint8_t> pixels((std::size_t)width * height * channelCount + 3);
	ReadImageFromFile(*s, pixels.data(), (std::int32_t)width, (std::int32_t)height, (std::int32_t)channelCount);

	// Assemble the indexed (R8) padded atlas exactly as BuildTilesetDiffuse does.
	const std::uint32_t tilesPerRow = width / TileSize;
	const std::uint32_t tilesPerCol = height / TileSize;
	const std::uint32_t pw = width + 2 * tilesPerRow;   // paddedWidth
	const std::uint32_t ph = height + 2 * tilesPerCol;  // paddedHeight
	std::vector<std::uint8_t> atlas((std::size_t)pw * ph, 0);

	for (std::uint32_t ty = 0; ty < tilesPerCol; ty++) {
		for (std::uint32_t tx = 0; tx < tilesPerRow; tx++) {
			const std::uint32_t srcX = tx * TileSize, srcY = ty * TileSize;
			const std::uint32_t dstX = tx * (TileSize + 2), dstY = ty * (TileSize + 2);
			std::uint8_t* dstTile = &atlas[(dstY * pw + dstX)];
			for (std::uint32_t y = 0; y < TileSize; y++) {
				for (std::uint32_t x = 0; x < TileSize; x++) {
					const std::uint32_t src = ((srcY + y) * width + (srcX + x)) * channelCount;
					const std::uint32_t dst = (y + 1) * pw + (x + 1);
					const std::uint8_t origIndex = pixels[src];
					const bool transparent = (channelCount >= 4 ? (pixels[src + 3] == 0) : (origIndex == 0));
					dstTile[dst] = (transparent ? 0 : origIndex);
				}
			}
			ExpandTileDiffuse(dstTile, pw, 1);
		}
	}

	// Split into 512-pages and swizzle each, mirroring PspEmitter's BuildPages. Pages advance by PageTexels
	// (15 whole 34px cells), not 512, so no tile ever straddles a page boundary. Keyed by tileset NAME so
	// RequestTileSet can find it without redoing the decode/atlas.
	out->key = NameHash(name);
	out->paged = true;
	out->format = (std::uint8_t)PixelFormat::Indexed8;
	out->swizzled = true;
	out->width = (int)pw; out->height = (int)ph;
	out->clut.assign(palette, palette + ColorsPerPalette);
	out->pagesX = ((int)pw + PageTexels - 1) / PageTexels;
	out->pagesY = ((int)ph + PageTexels - 1) / PageTexels;
	out->pages.clear();
	for (int py = 0; py < out->pagesY; py++) {
		for (int px = 0; px < out->pagesX; px++) {
			BakedTexture::Page page;
			page.ox = px * PageTexels; page.oy = py * PageTexels;
			page.w = std::min((int)pw - page.ox, PageTexels);
			page.h = std::min((int)ph - page.oy, PageTexels);
			std::vector<std::uint8_t> linear((std::size_t)PageStride * PageStride, 0); // T8 -> 1 byte/texel
			for (int y = 0; y < page.h; y++) {
				std::memcpy(&linear[(std::size_t)y * PageStride],
					&atlas[(std::size_t)(page.oy + y) * pw + page.ox], (std::size_t)page.w);
			}
			page.swizzled.assign((std::size_t)PageStride * PageStride, 0);
			// T8: pitch and height are both PageStride, so both are multiples of the swizzle block.
			Swizzle(page.swizzled.data(), linear.data(), PageStride, PageStride);
			out->pages.push_back(std::move(page));
		}
	}
	*skipReason = nullptr;
	return true;
}

// Pack layout, all offsets from file start: PackHeader | TexEntry[] | TexPage[] | blob(CLUTs + pages).
static std::uint32_t Align16(std::uint32_t v) { return (v + 15u) & ~15u; }

bool WriteTexturePack(const char* path, const std::vector<BakedTexture>& textures)
{
	std::uint32_t totalPages = 0;
	for (const auto& t : textures) totalPages += (std::uint32_t)t.pages.size();

	const std::uint32_t headerSize = sizeof(PackHeader);
	const std::uint32_t entriesOffset = headerSize;
	const std::uint32_t pagesOffset = entriesOffset + (std::uint32_t)textures.size() * sizeof(TexEntry);
	const std::uint32_t blobOffset = Align16(pagesOffset + totalPages * sizeof(TexPage));

	std::vector<TexEntry> entries;
	std::vector<TexPage> pages;
	std::vector<std::uint8_t> blob;
	auto blobPos = [&]() -> std::uint32_t { return blobOffset + (std::uint32_t)blob.size(); };
	auto padBlob16 = [&]() { while ((blob.size() & 15u) != 0) blob.push_back(0); };

	std::uint32_t firstPage = 0;
	for (const auto& t : textures) {
		TexEntry e {};
		e.key = t.key;
		e.width = (std::uint16_t)t.width; e.height = (std::uint16_t)t.height;
		e.format = t.format;
		e.flags = t.flags;
		e.swizzled = (t.swizzled ? 1 : 0);
		e.paged = (t.paged ? 1 : 0);
		e.clutOffset = 0xFFFFFFFFu;
		e.maskOffset = 0xFFFFFFFFu;
		e.metaOffset = 0xFFFFFFFFu;
		e.metaSize = 0;

		// A palettised pre-coloured sprite owns a CLUT while staying single, so this cannot be folded into
		// the paged branch below.
		if (!t.paged && !t.clut.empty()) {
			padBlob16();
			e.clutOffset = blobPos();
			const std::uint8_t* clutBytes = reinterpret_cast<const std::uint8_t*>(t.clut.data());
			blob.insert(blob.end(), clutBytes, clutBytes + t.clut.size() * sizeof(std::uint32_t));
		}

		if (t.paged) {
			e.stride = (std::uint16_t)PageStride; e.strideH = (std::uint16_t)PageStride;
			e.pagesX = (std::uint8_t)t.pagesX; e.pagesY = (std::uint8_t)t.pagesY;
			e.firstPage = firstPage;
			padBlob16();
			e.clutOffset = blobPos();
			const std::uint8_t* clutBytes = reinterpret_cast<const std::uint8_t*>(t.clut.data());
			blob.insert(blob.end(), clutBytes, clutBytes + t.clut.size() * sizeof(std::uint32_t));
			for (const auto& pg : t.pages) {
				padBlob16();
				TexPage tp {};
				tp.dataOffset = blobPos();
				tp.ox = (std::int16_t)pg.ox; tp.oy = (std::int16_t)pg.oy;
				tp.w = (std::int16_t)pg.w; tp.h = (std::int16_t)pg.h;
				blob.insert(blob.end(), pg.swizzled.begin(), pg.swizzled.end());
				pages.push_back(tp);
			}
			firstPage += (std::uint32_t)t.pages.size();
		} else {
			e.stride = (std::uint16_t)t.stride; e.strideH = (std::uint16_t)t.strideH;
			padBlob16();
			e.dataOffset = blobPos();
			blob.insert(blob.end(), t.data.begin(), t.data.end());
		}
		// Frame-paged sprites still carry mask/meta; tilesets leave both empty.
		if (!t.mask.empty()) {
			padBlob16();
			e.maskOffset = blobPos();
			blob.insert(blob.end(), t.mask.begin(), t.mask.end());
		}
		if (!t.meta.empty()) {
			padBlob16();
			e.metaOffset = blobPos();
			e.metaSize = (std::uint16_t)t.meta.size();
			blob.insert(blob.end(), t.meta.begin(), t.meta.end());
		}
		entries.push_back(e);
	}

	PackHeader h {};
	h.magic = PackMagic; h.version = PackVersion; h.reserved = 0;
	h.entryCount = (std::uint32_t)entries.size();
	h.entriesOffset = entriesOffset;
	h.blobOffset = blobOffset;
	h.blobSize = (std::uint32_t)blob.size();

	std::FILE* f = std::fopen(path, "wb");
	if (f == nullptr) return false;
	std::fwrite(&h, sizeof(h), 1, f);
	if (!entries.empty()) std::fwrite(entries.data(), sizeof(TexEntry), entries.size(), f);
	if (!pages.empty()) std::fwrite(pages.data(), sizeof(TexPage), pages.size(), f);
	for (std::uint32_t pos = pagesOffset + totalPages * sizeof(TexPage); pos < blobOffset; pos++) std::fputc(0, f);
	if (!blob.empty()) std::fwrite(blob.data(), 1, blob.size(), f);
	std::fclose(f);
	return true;
}

struct TexturePackStream
{
	std::string outputPath;
	std::string spoolPath;
	std::FILE* spool = nullptr;
	std::vector<char> ioBuffer;
	std::vector<TexEntry> entries;
	std::vector<TexPage> pages;
};

TexturePackStream* CreateTexturePackStream(const char* outputPath)
{
	auto* stream = new TexturePackStream;
	stream->outputPath = outputPath;
	stream->spoolPath = stream->outputPath + ".tmp";
	stream->spool = std::fopen(stream->spoolPath.c_str(), "w+b");
	if (stream->spool == nullptr) { delete stream; return nullptr; }
	stream->ioBuffer.resize(64 * 1024);
	std::setvbuf(stream->spool, stream->ioBuffer.data(), _IOFBF, stream->ioBuffer.size());
	// Reserve the header, patch it at completion, and append the small entry/page index after the blob:
	// spooling in final-pack layout avoids rereading and rewriting 40-130 MiB on a real Memory Stick.
	PackHeader placeholder{};
	if (std::fwrite(&placeholder, sizeof(placeholder), 1, stream->spool) != 1) {
		std::fclose(stream->spool);
		std::remove(stream->spoolPath.c_str());
		delete stream;
		return nullptr;
	}
	while ((std::ftell(stream->spool) & 15) != 0) std::fputc(0, stream->spool);
	return stream;
}

bool AppendTexturePack(TexturePackStream* stream, BakedTexture&& t)
{
	if (stream == nullptr || stream->spool == nullptr) return false;
	auto align16 = [&]() {
		long position = std::ftell(stream->spool);
		while ((position++ & 15) != 0) std::fputc(0, stream->spool);
	};
	auto writeBlob = [&](const void* data, std::size_t size, std::uint32_t& offset) {
		align16();
		offset = (std::uint32_t)std::ftell(stream->spool);
		return size == 0 || std::fwrite(data, 1, size, stream->spool) == size;
	};

	TexEntry e{};
	e.key = t.key;
	e.width = (std::uint16_t)t.width; e.height = (std::uint16_t)t.height;
	e.format = t.format; e.flags = t.flags;
	e.swizzled = (t.swizzled ? 1 : 0); e.paged = (t.paged ? 1 : 0);
	e.clutOffset = e.maskOffset = e.metaOffset = 0xFFFFFFFFu;
	e.firstPage = (std::uint32_t)stream->pages.size();
	bool success = true;
	if (t.paged) {
		e.stride = (std::uint16_t)PageStride; e.strideH = (std::uint16_t)PageStride;
		e.pagesX = (std::uint8_t)t.pagesX; e.pagesY = (std::uint8_t)t.pagesY;
		success = writeBlob(t.clut.data(), t.clut.size() * sizeof(std::uint32_t), e.clutOffset);
		for (const auto& page : t.pages) {
			TexPage p{};
			p.ox = (std::int16_t)page.ox; p.oy = (std::int16_t)page.oy;
			p.w = (std::int16_t)page.w; p.h = (std::int16_t)page.h;
			success = success && writeBlob(page.swizzled.data(), page.swizzled.size(), p.dataOffset);
			stream->pages.push_back(p);
		}
	} else {
		e.stride = (std::uint16_t)t.stride; e.strideH = (std::uint16_t)t.strideH;
		// Single, but a palettised pre-coloured sprite still owns a CLUT (see TextureFlagOwnClut).
		if (!t.clut.empty()) success = writeBlob(t.clut.data(), t.clut.size() * sizeof(std::uint32_t), e.clutOffset);
		success = success && writeBlob(t.data.data(), t.data.size(), e.dataOffset);
	}
	if (!t.mask.empty()) success = success && writeBlob(t.mask.data(), t.mask.size(), e.maskOffset);
	if (!t.meta.empty()) {
		e.metaSize = (std::uint16_t)t.meta.size();
		success = success && writeBlob(t.meta.data(), t.meta.size(), e.metaOffset);
	}
	if (success) stream->entries.push_back(e);
	return success && !std::ferror(stream->spool);
}

bool AppendTilesetPixels(TexturePackStream* stream, const char* name, const std::uint32_t* palette,
	const std::uint8_t* pixels, int width, int height, int channelCount, int tileCount,
	BakeLogSink log, void* logContext)
{
	auto trace = [&](const char* message) { if (log != nullptr) log(logContext, message); };
	if (stream == nullptr || stream->spool == nullptr || palette == nullptr || pixels == nullptr ||
		name == nullptr || channelCount != 1 || width <= 0 || height <= 0 || tileCount <= 0) return false;

	const int tilesPerRow = width / TileSize;
	const int tilesPerColumn = height / TileSize;
	const int paddedWidth = width + 2 * tilesPerRow;
	const int paddedHeight = height + 2 * tilesPerColumn;
	const int pagesX = (paddedWidth + PageTexels - 1) / PageTexels;
	const int pagesY = (paddedHeight + PageTexels - 1) / PageTexels;
	auto align16 = [&]() {
		long position = std::ftell(stream->spool);
		while ((position++ & 15) != 0) std::fputc(0, stream->spool);
	};
	auto writeBlob = [&](const void* data, std::size_t size, std::uint32_t& offset) {
		align16();
		offset = (std::uint32_t)std::ftell(stream->spool);
		return size == 0 || std::fwrite(data, 1, size, stream->spool) == size;
	};

	TexEntry entry{};
	entry.key = NameHash(name);
	entry.width = (std::uint16_t)paddedWidth;
	entry.height = (std::uint16_t)paddedHeight;
	entry.stride = entry.strideH = PageStride;
	entry.format = (std::uint8_t)PixelFormat::Indexed8;
	entry.swizzled = entry.paged = 1;
	entry.pagesX = (std::uint8_t)pagesX;
	entry.pagesY = (std::uint8_t)pagesY;
	entry.firstPage = (std::uint32_t)stream->pages.size();
	entry.dataOffset = 0;
	entry.maskOffset = entry.metaOffset = 0xFFFFFFFFu;
	if (!writeBlob(palette, ColorsPerPalette * sizeof(std::uint32_t), entry.clutOffset)) return false;

	const std::size_t pageBytes = (std::size_t)PageStride * PageStride;
	// One 512x512 page at a time: the converter still owns the tile array and its row-major image, so a
	// pagesX-wide band here would only raise the PSP peak heap.
	std::vector<std::uint8_t> pagePixels(pageBytes, 0);
	std::vector<std::uint8_t> swizzleChunk(64 * 1024);
	char traceMessage[160];
	for (int pageY = 0; pageY < pagesY; ++pageY) {
		for (int pageX = 0; pageX < pagesX; ++pageX) {
			std::fill(pagePixels.begin(), pagePixels.end(), 0);
			const int firstTileX = pageX * PageCells;
			const int firstTileY = pageY * PageCells;
			const int lastTileX = std::min(firstTileX + PageCells, tilesPerRow);
			const int lastTileY = std::min(firstTileY + PageCells, tilesPerColumn);
			for (int tileY = firstTileY; tileY < lastTileY; ++tileY) {
				for (int tileX = firstTileX; tileX < lastTileX; ++tileX) {
					std::uint8_t* cell = pagePixels.data() +
						(std::size_t)(tileY - firstTileY) * (TileSize + 2) * PageStride +
						(tileX - firstTileX) * (TileSize + 2);
					for (int y = 0; y < TileSize; ++y) {
						std::memcpy(cell + (std::size_t)(y + 1) * PageStride + 1,
							pixels + (std::size_t)(tileY * TileSize + y) * width + tileX * TileSize,
							TileSize);
					}
					ExpandTileDiffuse(cell, PageStride, 1);
				}
			}
			TexPage page{};
			page.ox = (std::int16_t)(pageX * PageTexels);
			page.oy = (std::int16_t)(pageY * PageTexels);
			page.w = (std::int16_t)std::min(paddedWidth - page.ox, PageTexels);
			page.h = (std::int16_t)std::min(paddedHeight - page.oy, PageTexels);
			align16();
			page.dataOffset = (std::uint32_t)std::ftell(stream->spool);
			const std::uint8_t* linear = pagePixels.data();
			std::size_t chunkSize = 0;
			for (int blockY = 0; blockY < PageStride / 8; ++blockY) {
				for (int blockX = 0; blockX < PageStride / 16; ++blockX) {
					for (int y = 0; y < 8; ++y) {
						std::memcpy(swizzleChunk.data() + chunkSize,
							linear + (std::size_t)(blockY * 8 + y) * PageStride + blockX * 16, 16);
						chunkSize += 16;
						if (chunkSize == swizzleChunk.size()) {
							if (std::fwrite(swizzleChunk.data(), 1, chunkSize, stream->spool) != chunkSize) return false;
							chunkSize = 0;
						}
					}
				}
			}
			if (chunkSize != 0 && std::fwrite(swizzleChunk.data(), 1, chunkSize, stream->spool) != chunkSize)
				return false;
			stream->pages.push_back(page);
			std::snprintf(traceMessage, sizeof(traceMessage), "tileset %s: wrote direct page %d/%d", name,
				pageY * pagesX + pageX + 1, pagesX * pagesY);
			trace(traceMessage);
		}
	}
	stream->entries.push_back(entry);
	return !std::ferror(stream->spool);
}

bool AppendTilesetJ2t(TexturePackStream* stream, const char* path, const char* name, const char** skipReason,
	BakeLogSink log, void* logContext)
{
	auto trace = [&](const char* message) { if (log != nullptr) log(logContext, message); };
	char traceMessage[160];
	std::snprintf(traceMessage, sizeof(traceMessage), "tileset %s: begin", name);
	trace(traceMessage);
	if (skipReason != nullptr) *skipReason = nullptr;
	if (stream == nullptr || stream->spool == nullptr) {
		if (skipReason != nullptr) *skipReason = "pack is not open";
		return false;
	}
	auto source = FileSystem::Open(path, FileAccess::Read);
	if (source == nullptr || !source->IsValid()) {
		if (skipReason != nullptr) *skipReason = "cannot open";
		return false;
	}

	const std::uint64_t sig1 = source->ReadValueAsLE<std::uint64_t>();
	const std::uint16_t sig2 = source->ReadValueAsLE<std::uint16_t>();
	const std::uint8_t version = source->ReadValue<std::uint8_t>();
	source->ReadValue<std::uint8_t>();
	if (sig1 != 0xB8EF8498E2BFBBEF || sig2 != 0x208F || version != 2) {
		if (skipReason != nullptr) *skipReason = "bad signature";
		return false;
	}

	const std::uint8_t channelCount = source->ReadValue<std::uint8_t>();
	const std::uint32_t width = source->ReadValueAsLE<std::uint32_t>();
	const std::uint32_t height = source->ReadValueAsLE<std::uint32_t>();
	const std::uint16_t tileCount = source->ReadValueAsLE<std::uint16_t>();
	const std::int32_t compressedSize = source->ReadValueAsLE<std::int32_t>();
	std::snprintf(traceMessage, sizeof(traceMessage), "tileset %s: header %ux%u channels=%u tiles=%u",
		name, (unsigned)width, (unsigned)height, (unsigned)channelCount, (unsigned)tileCount);
	trace(traceMessage);
	if (channelCount == 0 || width == 0 || height == 0) {
		if (skipReason != nullptr) *skipReason = "bad dimensions";
		return false;
	}

	std::uint32_t palette[ColorsPerPalette];
	std::vector<std::uint8_t> is32((tileCount + 7) / 8);
	{
		DeflateStream uncompressed(*source, compressedSize);
		for (int i = 0; i < ColorsPerPalette; ++i) palette[i] = uncompressed.ReadValueAsLE<std::uint32_t>();
		uncompressed.Read(is32.data(), (std::uint32_t)is32.size());
		const std::uint32_t maskSize = uncompressed.ReadValueAsLE<std::uint32_t>();
		uncompressed.Seek((std::int64_t)maskSize, SeekOrigin::Current);
	}
	for (int tile = 0; tile < tileCount; ++tile) {
		if ((is32[tile / 8] & (1 << (tile & 7))) != 0) {
			if (skipReason != nullptr) *skipReason = "contains 32-bit tiles (RGBA bake TODO)";
			return false;
		}
	}
	// Free before allocating the page band; clear() alone would keep the allocation.
	std::vector<std::uint8_t>().swap(is32);

	const int tilesPerRow = (int)width / TileSize;
	const int tilesPerColumn = (int)height / TileSize;
	const int paddedWidth = (int)width + 2 * tilesPerRow;
	const int paddedHeight = (int)height + 2 * tilesPerColumn;
	const int pagesX = (paddedWidth + PageTexels - 1) / PageTexels;
	const int pagesY = (paddedHeight + PageTexels - 1) / PageTexels;

	auto align16 = [&]() {
		long position = std::ftell(stream->spool);
		while ((position++ & 15) != 0) std::fputc(0, stream->spool);
	};
	auto writeBlob = [&](const void* data, std::size_t size, std::uint32_t& offset) {
		align16();
		offset = (std::uint32_t)std::ftell(stream->spool);
		return size == 0 || std::fwrite(data, 1, size, stream->spool) == size;
	};

	TexEntry entry{};
	entry.key = NameHash(name);
	entry.width = (std::uint16_t)paddedWidth;
	entry.height = (std::uint16_t)paddedHeight;
	entry.stride = entry.strideH = PageStride;
	entry.format = (std::uint8_t)PixelFormat::Indexed8;
	entry.swizzled = entry.paged = 1;
	entry.pagesX = (std::uint8_t)pagesX;
	entry.pagesY = (std::uint8_t)pagesY;
	entry.firstPage = (std::uint32_t)stream->pages.size();
	entry.dataOffset = 0; // unused for paged entries
	entry.maskOffset = entry.metaOffset = 0xFFFFFFFFu;
	if (!writeBlob(palette, sizeof(palette), entry.clutOffset)) {
		if (skipReason != nullptr) *skipReason = "write failed";
		return false;
	}
	trace("tileset: CLUT written; allocating current page band");
	const std::size_t pageBytes = (std::size_t)PageStride * PageStride;
	std::vector<std::uint8_t> pageBand((std::size_t)pagesX * pageBytes, 0);
	std::vector<std::uint8_t> swizzleChunk(64 * 1024);
	trace("tileset: page band allocated; decoding QOI in RAM");
	bool writeFailed = false;
	auto consumeBand = [&](const std::uint8_t* pixels, int tileY, int rows) {
		const int cellY = (tileY % PageCells) * (TileSize + 2);
		for (int row = 0; row < rows; ++row) {
			const int atlasY = cellY + row + 1;
			for (int sourceX = 0; sourceX < (int)width; ++sourceX) {
				const std::size_t sourceOffset = ((std::size_t)row * width + sourceX) * channelCount;
				const std::uint8_t index = pixels[sourceOffset];
				const std::uint8_t value = ((channelCount >= 4 && pixels[sourceOffset + 3] == 0) || index == 0)
					? 0 : index;
				const int inTileX = sourceX % TileSize;
				const int atlasX = (sourceX / TileSize) * (TileSize + 2) + inTileX + 1;
				const int pageX = atlasX / PageTexels;
				const int localX = atlasX % PageTexels;
				std::uint8_t* destination = pageBand.data() + (std::size_t)pageX * pageBytes +
					(std::size_t)atlasY * PageStride + localX;
				*destination = value;
				if (inTileX == 0) destination[-1] = value;
				if (inTileX == TileSize - 1) destination[1] = value;
			}
		}
		// Horizontal padding is already in place, so copying whole rows also fills the four corners.
		for (int pageX = 0; pageX < pagesX; ++pageX) {
			std::uint8_t* pagePixels = pageBand.data() + (std::size_t)pageX * pageBytes;
			std::memcpy(pagePixels + (std::size_t)cellY * PageStride,
				pagePixels + (std::size_t)(cellY + 1) * PageStride, PageStride);
			std::memcpy(pagePixels + (std::size_t)(cellY + TileSize + 1) * PageStride,
				pagePixels + (std::size_t)(cellY + rows) * PageStride, PageStride);
		}

		const bool pageBandComplete = tileY % PageCells == PageCells - 1 || tileY == tilesPerColumn - 1;
		if (!pageBandComplete) return true;
		const int pageY = tileY / PageCells;
		for (int pageX = 0; pageX < pagesX; ++pageX) {
			const int pageNumber = pageY * pagesX + pageX + 1;
			std::snprintf(traceMessage, sizeof(traceMessage), "tileset %s: writing page %d/%d", name,
				pageNumber, pagesX * pagesY);
			trace(traceMessage);
			TexPage page{};
			page.ox = (std::int16_t)(pageX * PageTexels);
			page.oy = (std::int16_t)(pageY * PageTexels);
			page.w = (std::int16_t)std::min(paddedWidth - page.ox, PageTexels);
			page.h = (std::int16_t)std::min(paddedHeight - page.oy, PageTexels);
			align16();
			page.dataOffset = (std::uint32_t)std::ftell(stream->spool);
			const std::uint8_t* linear = pageBand.data() + (std::size_t)pageX * pageBytes;
			std::size_t chunkSize = 0;
			for (int blockY = 0; blockY < PageStride / 8; ++blockY) {
				for (int blockX = 0; blockX < PageStride / 16; ++blockX) {
					for (int y = 0; y < 8; ++y) {
						std::memcpy(swizzleChunk.data() + chunkSize,
							linear + (std::size_t)(blockY * 8 + y) * PageStride + blockX * 16, 16);
						chunkSize += 16;
						if (chunkSize == swizzleChunk.size()) {
							if (std::fwrite(swizzleChunk.data(), 1, chunkSize, stream->spool) != chunkSize) return false;
							chunkSize = 0;
						}
					}
				}
			}
			if (chunkSize != 0 && std::fwrite(swizzleChunk.data(), 1, chunkSize, stream->spool) != chunkSize)
				return false;
			stream->pages.push_back(page);
			std::snprintf(traceMessage, sizeof(traceMessage), "tileset %s: wrote page %d/%d", name,
				pageNumber, pagesX * pagesY);
			trace(traceMessage);
		}
		std::fill(pageBand.begin(), pageBand.end(), 0);
		return true;
	};
	if (!ReadImageInTileBands(*source, (std::int32_t)width, (std::int32_t)height, channelCount, consumeBand))
		writeFailed = true;
	source.reset();
	if (writeFailed || std::ferror(stream->spool)) {
		if (skipReason != nullptr) *skipReason = "QOI decode or page write failed";
		return false;
	}
	stream->entries.push_back(entry);
	std::snprintf(traceMessage, sizeof(traceMessage), "tileset %s: complete", name);
	trace(traceMessage);
	return !std::ferror(stream->spool);
}

bool FinishTexturePack(TexturePackStream* stream, BakeLogSink log, void* logContext)
{
	auto trace = [&](const char* message) { if (log != nullptr) log(logContext, message); };
	if (stream == nullptr || stream->spool == nullptr) return false;
	trace("texture pack: appending entry/page index");
	const std::uint32_t blobOffset = Align16(sizeof(PackHeader));
	const long blobEnd = std::ftell(stream->spool);
	if (blobEnd < (long)blobOffset) return false;
	while ((std::ftell(stream->spool) & 15) != 0) std::fputc(0, stream->spool);
	const std::uint32_t entriesOffset = (std::uint32_t)std::ftell(stream->spool);
	bool success = stream->entries.empty() ||
		std::fwrite(stream->entries.data(), sizeof(TexEntry), stream->entries.size(), stream->spool) == stream->entries.size();
	if (!stream->pages.empty()) success = success &&
		std::fwrite(stream->pages.data(), sizeof(TexPage), stream->pages.size(), stream->spool) == stream->pages.size();

	PackHeader header{};
	header.magic = PackMagic; header.version = PackVersion;
	header.entryCount = (std::uint32_t)stream->entries.size();
	header.entriesOffset = entriesOffset;
	header.blobOffset = blobOffset;
	header.blobSize = (std::uint32_t)(blobEnd - blobOffset);
	// Never seek backwards while newlib still owns the caller-supplied write buffer: on real hardware that
	// faults with the last partial 64 KiB block pending, truncating texture.pak.tmp at a buffer boundary.
	// Close the sequential writer first, then patch the header through a fresh unbuffered handle.
	trace("texture pack: flushing sequential writer");
	success = success && std::fflush(stream->spool) == 0 && !std::ferror(stream->spool);
	std::fclose(stream->spool);
	stream->spool = nullptr;
	if (success) {
		std::FILE* patchFile = std::fopen(stream->spoolPath.c_str(), "r+b");
		if (patchFile == nullptr) success = false;
		else {
			std::setvbuf(patchFile, nullptr, _IONBF, 0);
			success = std::fwrite(&header, sizeof(header), 1, patchFile) == 1 &&
				std::fflush(patchFile) == 0 && !std::ferror(patchFile);
			std::fclose(patchFile);
		}
	}
	trace(success ? "texture pack: header patched; renaming" : "texture pack: header patch failed");
	// FAT/PSP rename does not reliably replace an existing destination, and cache migration can rebuild the
	// pack without the "Regenerate" path having removed the old one.
	if (success && FileSystem::IsReadableFile(stream->outputPath.c_str()) &&
		!FileSystem::RemoveFile(stream->outputPath.c_str())) success = false;
	if (success) success = FileSystem::Move(stream->spoolPath.c_str(), stream->outputPath.c_str());
	if (!success) std::remove(stream->spoolPath.c_str());
	trace(success ? "texture pack: rename complete" : "texture pack: finalization failed");
	return success;
}

void DestroyTexturePackStream(TexturePackStream* stream)
{
	if (stream == nullptr) return;
	if (stream->spool != nullptr) std::fclose(stream->spool);
	std::remove(stream->spoolPath.c_str());
	delete stream;
}

// ======================================================================================================
//  Sprite baking (single textures)
// ======================================================================================================
namespace {
	// Base sprite palette as 0xAABBGGRR (ContentResolver::_palettes row 0), used only to pick R8 vs RG8.
	std::uint32_t BasePaletteAt(int offset)
	{
		constexpr int N = (int)(sizeof(SpritePalette) / sizeof(SpritePalette[0]));
		if (offset < 0 || offset >= N) return 0xFF000000u; // out of base row -> treat as opaque (RG8)
		const auto& c = SpritePalette[offset];
		return ((std::uint32_t)c.A << 24) | ((std::uint32_t)c.B << 16) | ((std::uint32_t)c.G << 8) | (std::uint32_t)c.R;
	}

	// Pad packed pixels to a NextPow2 stride, top-left aligned and zero elsewhere, matching what
	// PspCaptureTexture produces on device. Fills out->stride/strideH/data.
	void PadToPow2(const std::uint8_t* packed, int width, int height, int bpp, BakedTexture* out)
	{
		int stride = NextPow2(width);
		const int strideH = NextPow2(height);
		// The GE requires a row pitch of at least 16 bytes; anything narrower is misread and faults on real
		// hardware. Only tiny sprites hit this (the 5x5 Pepper bullet would bake an 8-byte-wide T8 texture).
		while (stride * bpp < 16) stride <<= 1;
		out->stride = stride; out->strideH = strideH;
		out->data.assign((std::size_t)stride * strideH * bpp, 0);
		for (int y = 0; y < height; y++) {
			std::memcpy(&out->data[(std::size_t)y * stride * bpp], &packed[(std::size_t)y * width * bpp], (std::size_t)width * bpp);
		}
	}

	// Convert a single-texture payload to the PSP's 16-byte x 8-row block layout. Textures too small to form
	// complete blocks stay linear with swizzled=false.
	void SwizzleSprite(BakedTexture* out, int bpp)
	{
		const unsigned int widthBytes = (unsigned int)(out->stride * bpp);
		const unsigned int height = (unsigned int)out->strideH;
		if (!CanSwizzle(widthBytes, height)) return;

		std::vector<std::uint8_t> swizzled(out->data.size());
		Swizzle(swizzled.data(), out->data.data(), widthBytes, height);
		out->data.swap(swizzled);
		out->swizzled = true;
	}

	// Fixed-size .aura header (sig..gunspot) emitted by WriteImageToStream; stored verbatim in the pack
	// because the device parses exactly these bytes to build the sprite's frame metadata.
	constexpr int AuraHeaderSize = 39;

	// Decode one .aura stream into *out, reproducing RequestGraphicsAura + CreateIndexedTexture.
	bool DecodeAura(Stream& s, int paletteOffset, std::uint64_t key, bool upscaleMenuGlow, BakedTexture* out)
	{
		std::uint8_t hdr[AuraHeaderSize];
		if (s.Read(hdr, AuraHeaderSize) != AuraHeaderSize) return false;
		std::uint64_t sig1; std::memcpy(&sig1, hdr, 8);
		std::uint16_t sig2; std::memcpy(&sig2, hdr + 8, 2);
		std::uint8_t version = hdr[10], flags = hdr[11];
		if (sig1 != 0xB8EF8498E2BFBBEF || sig2 != 0x208F || version != 2 || (flags & 0x80) != 0x80) return false;

		std::uint8_t channelCount = hdr[12];
		std::uint32_t fdX; std::memcpy(&fdX, hdr + 13, 4);
		std::uint32_t fdY; std::memcpy(&fdY, hdr + 17, 4);
		std::uint8_t fcX = hdr[21], fcY = hdr[22];

		int width = (int)(fdX * fcX), height = (int)(fdY * fcY);
		if (width <= 0 || height <= 0) return false;
		std::vector<std::uint8_t> pixels((std::size_t)width * height * channelCount + 3);
		ReadImageFromFile(s, pixels.data(), width, height, channelCount); // s is positioned right after the header
		const bool isFireShield = (key == NameHash("Common/shield_fire.aura"));
		const bool isLightningShield = (key == NameHash("Common/shield_lightning.aura"));
		const bool isWaterShield = (key == NameHash("Common/shield_water.aura"));
		if (isFireShield || isLightningShield) {
			return BakeShieldAnimationFromPixels(key, pixels.data(), width, height, channelCount,
				isLightningShield, hdr, out);
		}
		if (isWaterShield) {
			return BakeWaterShieldFromPixels(key, pixels.data(), width, height, channelCount,
				paletteOffset, hdr, out);
		}

		if (upscaleMenuGlow && channelCount == 4) {
			// UI/glow is only 22x12 and the renderer samples nearest. Bake an 8x bilinear copy so wide
			// selection underglows stay smooth without enabling linear filtering for all UI pixel art.
			constexpr int Scale = 8;
			const int sourceWidth = width, sourceHeight = height;
			width *= Scale;
			height *= Scale;
			std::vector<std::uint8_t> scaled((std::size_t)width * height * 4);
			for (int y = 0; y < height; ++y) {
				const float sourceY = (y + 0.5f) / Scale - 0.5f;
				const int y0 = std::max(0, (int)std::floor(sourceY));
				const int y1 = std::min(y0 + 1, sourceHeight - 1);
				const float fy = std::clamp(sourceY - y0, 0.0f, 1.0f);
				for (int x = 0; x < width; ++x) {
					const float sourceX = (x + 0.5f) / Scale - 0.5f;
					const int x0 = std::max(0, (int)std::floor(sourceX));
					const int x1 = std::min(x0 + 1, sourceWidth - 1);
					const float fx = std::clamp(sourceX - x0, 0.0f, 1.0f);
					std::uint8_t* destination = scaled.data() + ((std::size_t)y * width + x) * 4;
					for (int channel = 0; channel < 4; ++channel) {
						const float top = pixels[((std::size_t)y0 * sourceWidth + x0) * 4 + channel] +
							(pixels[((std::size_t)y0 * sourceWidth + x1) * 4 + channel] -
							 pixels[((std::size_t)y0 * sourceWidth + x0) * 4 + channel]) * fx;
						const float bottom = pixels[((std::size_t)y1 * sourceWidth + x0) * 4 + channel] +
							(pixels[((std::size_t)y1 * sourceWidth + x1) * 4 + channel] -
							 pixels[((std::size_t)y1 * sourceWidth + x0) * 4 + channel]) * fx;
						destination[channel] = (std::uint8_t)std::clamp(top + (bottom - top) * fy, 0.0f, 255.0f);
					}
				}
			}
			pixels.swap(scaled);
			fdX *= Scale;
			fdY *= Scale;
			std::memcpy(hdr + 13, &fdX, sizeof(fdX));
			std::memcpy(hdr + 17, &fdY, sizeof(fdY));
		}

		const bool notIndexed = ((flags & 0x01) == 0x01); // palette already applied -> keep RGBA
		const bool includeMask = (flags & 0x02) == 0;
		if (!BakeSpriteFromPixels(key, pixels.data(), width, height, channelCount, notIndexed, paletteOffset,
			(int)fdX, (int)fdY, out, false, includeMask)) return false;
		out->meta.assign(hdr, hdr + AuraHeaderSize);
		return true;
	}

	// Collect {animation asset path -> palette offset} from every .res under `dir`. First occurrence wins,
	// matching the device's offset-independent sprite cache.
	void CollectMetadata(const std::string& dir, std::unordered_map<std::string, int>& outMap)
	{
		using fs = Death::IO::FileSystem;
		for (auto item : fs::Directory(StringView(dir.c_str()), fs::EnumerationOptions::None)) {
			String itemS(item);
			if (fs::DirectoryExists(itemS)) { CollectMetadata(std::string(itemS.data(), itemS.size()), outMap); continue; }
			if (fs::GetExtension(itemS) != "res"_s) continue;
			auto st = fs::Open(itemS, FileAccess::Read);
			if (st == nullptr || !st->IsValid()) continue;
			auto sz = st->GetSize();
			if (sz <= 0) continue;
			std::string buf(sz, '\0');
			st->Read(&buf[0], (std::int32_t)sz);
			Json::CharReaderBuilder b;
			auto reader = std::unique_ptr<Json::CharReader>(b.newCharReader());
			Json::Value doc; std::string errs;
			if (!reader->parse(buf.data(), buf.data() + buf.size(), &doc, &errs)) continue;
			const Json::Value& anims = doc["Animations"];
			if (!anims.isObject()) continue;
			for (auto it = anims.begin(); it != anims.end(); ++it) {
				std::string_view path;
				if ((*it)["Path"].get(path) != Json::SUCCESS || path.empty()) continue;
				std::int64_t off = 0; (*it)["PaletteOffset"].get(off);
				std::string p(path);
				if (outMap.find(p) == outMap.end()) outMap[p] = (int)(off < 0 ? 0 : off);
			}
		}
	}
}

// Losslessly palettise a 4-channel RGBA image, or fail if it needs more than 256 distinct colours. NEVER
// quantises: a caller that gets false must keep the image as RGBA. On success `outClut` is exactly 256
// entries (zero-padded), as sceGuClutLoad requires, and `outIndices` is one byte per texel.
//
// Fully transparent texels all fold onto one index with CLUT entry 0 whatever RGB the source left under
// them; that keeps images padded with several distinct transparent colours inside the 256 budget.
static bool PalettiseRgba(const std::uint8_t* data, int count, std::vector<std::uint32_t>& outClut,
	std::vector<std::uint8_t>& outIndices)
{
	constexpr int MaxColors = 256;
	std::unordered_map<std::uint32_t, std::uint8_t> lookup;
	std::vector<std::uint32_t> colors;
	colors.reserve(MaxColors);
	outIndices.resize((std::size_t)count);

	for (int i = 0; i < count; ++i) {
		std::uint32_t rgba;
		std::memcpy(&rgba, data + (std::size_t)i * 4, sizeof(rgba));
		if ((rgba >> 24) == 0) rgba = 0;
		auto it = lookup.find(rgba);
		if (it == lookup.end()) {
			if ((int)colors.size() >= MaxColors) return false; // too rich to palettise; caller keeps RGBA
			const std::uint8_t index = (std::uint8_t)colors.size();
			colors.push_back(rgba);
			it = lookup.emplace(rgba, index).first;
		}
		outIndices[(std::size_t)i] = it->second;
	}

	outClut.assign(MaxColors, 0u);
	std::copy(colors.begin(), colors.end(), outClut.begin());
	return true;
}

bool BakeSpriteFromPixels(std::uint64_t key, const std::uint8_t* data, int width, int height,
	int channelCount, bool notIndexed, int paletteOffset, int frameWidth, int frameHeight, BakedTexture* out,
	bool classifyAlpha, bool includeMask)
{
	if (width <= 0 || height <= 0) return false;
	out->key = key; out->paged = false; out->swizzled = false;
	out->width = width; out->height = height;
	const int count = width * height;

	// Build the mask only once the padded/swizzled texture is done: holding it while a 512x512 page has both
	// its linear and swizzled copies in flight costs up to 384 KiB of PSP peak. Shields opt out entirely.
	auto finish = [&]() {
		if (!includeMask) {
			out->mask.clear();
			return true;
		}
		// 1 bit/texel, set when coverage exceeds PspGpu::MaskAlphaThreshold.
		out->mask.assign(MaskPackedBytes((std::size_t)count), 0);
		for (int i = 0; i < count; i++) {
			std::uint8_t coverage;
			if (channelCount == 1) coverage = (data[i] != 0 ? 255 : 0);
			else if (channelCount == 2) coverage = data[(std::size_t)i * 2 + 1];
			else coverage = data[(std::size_t)i * channelCount + 3];
			if (coverage > MaskAlphaThreshold) out->mask[i >> 3] |= (std::uint8_t)(1u << (i & 7));
		}
		return true;
	};

	// The GU cannot sample past 512 texels, so oversized atlases are split on animation-frame boundaries:
	// a frame must never straddle two pages.
	auto pageFrames = [&](const std::uint8_t* pixels, int bpp) -> bool {
		if (width <= PageStride && height <= PageStride) return false;
		if (frameWidth <= 0 || frameHeight <= 0) return false;
		// A full-screen source picture is one frame wider than the GE limit, so it has to be cut on a
		// 512-texel boundary instead; the UI renderer submits one source region per page.
		const int pageCellWidth = std::min(frameWidth, PageStride);
		const int pageCellHeight = std::min(frameHeight, PageStride);
		const int frameColumns = (width + pageCellWidth - 1) / pageCellWidth;
		const int frameRows = (height + pageCellHeight - 1) / pageCellHeight;
		const int framesPerPageX = std::max(1, PageStride / pageCellWidth);
		const int framesPerPageY = std::max(1, PageStride / pageCellHeight);
		out->paged = true;
		out->pagesX = (frameColumns + framesPerPageX - 1) / framesPerPageX;
		out->pagesY = (frameRows + framesPerPageY - 1) / framesPerPageY;
		out->stride = PageStride;
		out->strideH = PageStride;
		out->data.clear();
		out->pages.clear();
		out->swizzled = (bpp != 2);
		for (int py = 0; py < out->pagesY; py++) {
			for (int px = 0; px < out->pagesX; px++) {
				BakedTexture::Page page;
				page.ox = px * framesPerPageX * pageCellWidth;
				page.oy = py * framesPerPageY * pageCellHeight;
				page.w = std::min(framesPerPageX * pageCellWidth, width - page.ox);
				page.h = std::min(framesPerPageY * pageCellHeight, height - page.oy);
				std::vector<std::uint8_t> linear((std::size_t)PageStride * PageStride * bpp, 0);
				for (int y = 0; y < page.h; y++) {
					const auto* src = pixels + ((std::size_t)(page.oy + y) * width + page.ox) * bpp;
					auto* dst = linear.data() + (std::size_t)y * PageStride * bpp;
					std::memcpy(dst, src, (std::size_t)page.w * bpp);
				}
				page.swizzled.resize(linear.size());
				if (bpp != 2) Swizzle(page.swizzled.data(), linear.data(), PageStride * bpp, PageStride);
				else page.swizzled.swap(linear); // RG8 is expanded on the device and must remain linear.
				out->pages.push_back(std::move(page));
			}
		}
		return true;
	};

	if (notIndexed) {
		if (channelCount != 4) return false; // pre-coloured sprites are 4-channel RGBA
		if (classifyAlpha) {
			bool binaryAlpha = true;
			bool opaque = true;
			for (int i = 0; i < count; ++i) {
				const std::uint8_t alpha = data[(std::size_t)i * 4 + 3];
				binaryAlpha = binaryAlpha && (alpha == 0 || alpha == 255);
				opaque = opaque && (alpha == 255);
			}
			if (binaryAlpha) out->flags |= nCine::PspGpu::TextureFlagBinaryAlpha;
			if (opaque) out->flags |= nCine::PspGpu::TextureFlagOpaque;
		}

		// Pre-coloured does not mean many-coloured: menu/character splash art is JJ2 pixel art and usually
		// fits 256 RGBA values, so it palettises exactly - same pixels, a quarter of the bytes (1 MiB vs
		// 256 KiB per 512x512 page). The CLUT is the sprite's own, not the game palette's, hence
		// TextureFlagOwnClut. Over 256 colours falls through to RGBA untouched; never quantise here, or
		// smooth artwork bands.
		{
			std::vector<std::uint32_t> clut;
			std::vector<std::uint8_t> indices;
			if (PalettiseRgba(data, count, clut, indices)) {
				out->format = (std::uint8_t)PixelFormat::Indexed8;
				out->flags |= nCine::PspGpu::TextureFlagOwnClut;
				out->clut = std::move(clut);
				if (!pageFrames(indices.data(), 1)) {
					PadToPow2(indices.data(), width, height, 1, out);
					SwizzleSprite(out, 1);
				}
				return finish();
			}
		}

		out->format = (std::uint8_t)PixelFormat::Rgba8888;
		if (!pageFrames(data, 4)) {
			PadToPow2(data, width, height, 4, out);
			// RGBA menu art must stay swizzled - linear RGBA8888 sampling is dramatically slower on the real
			// GE. Sprites that cannot use swizzled RGBA are diverted to the RG8 path before reaching here.
			SwizzleSprite(out, 4);
		}
		return finish();
	}

	// Indexed: reproduce CreateIndexedTexture(data, channelCount, paletteBaseTransparent).
	// Only the custom hue row guarantees a transparent index 0. Gem gradients sit at offsets 128/256/384/512
	// inside two packed rows, so a blanket "offset >= 256 is transparent" rule drops their alpha and turns
	// index 0 into a palette-coloured rectangle whenever the shared atlas is drawn at another gem offset.
	constexpr int Hue180PaletteOffset = 3 * ColorsPerPalette;
	const bool customPaletteBaseTransparent = (paletteOffset == Hue180PaletteOffset);
	const bool paletteBaseTransparent = customPaletteBaseTransparent ||
		(((BasePaletteAt(paletteOffset) >> 24) & 0xFF) == 0);
	if (channelCount == 1 && paletteBaseTransparent) {
		out->format = (std::uint8_t)PixelFormat::Indexed8;
		if (!pageFrames(data, 1)) {
			PadToPow2(data, width, height, 1, out);
			SwizzleSprite(out, 1);
		}
	} else {
		{
			std::vector<std::uint8_t> rg((std::size_t)count * 2);
			if (channelCount == 2) {
				std::memcpy(rg.data(), data, (std::size_t)count * 2);
			} else if (channelCount == 1) {
				for (int i = 0; i < count; i++) { rg[i*2] = data[i]; rg[i*2+1] = (data[i] == 0 ? 0 : 255); }
			} else {
				for (int i = 0; i < count; i++) { rg[i*2] = data[(std::size_t)i*channelCount]; rg[i*2+1] = data[(std::size_t)i*channelCount+3]; }
			}
			out->format = (std::uint8_t)PixelFormat::Rg8;
			if (!pageFrames(rg.data(), 2)) PadToPow2(rg.data(), width, height, 2, out);
			// RG8 has no GU sampler mode and MUST stay linear: the device reads it on the CPU to expand it
			// into a palette-offset-specific RGBA8888 texture.
		}
	}
	return finish();
}

bool BakeShieldAnimationFromPixels(std::uint64_t key, const std::uint8_t* data, int width, int height,
	int channelCount, bool lightning, const std::uint8_t* auraHeader, BakedTexture* out)
{
	if (data == nullptr || auraHeader == nullptr || out == nullptr || width <= 0 || height <= 0 || channelCount != 4)
		return false;

	// 64px rather than the source's 128: the shield is a soft glow drawn at ~64px anyway, and halving it
	// makes each shield ~4x smaller and non-paged (fits one 512-page).
	constexpr int FrameSize = 64;
	constexpr float Pi = 3.14159265358979323846f;
	constexpr float Tau = Pi * 2.0f;
	const int frameCount = (lightning ? 24 : 20);
	const int frameColumns = (lightning ? 6 : 5);
	const int frameRows = 4;
	const int atlasWidth = FrameSize * frameColumns;
	const int atlasHeight = FrameSize * frameRows;
	std::vector<std::uint8_t> atlas((std::size_t)atlasWidth * atlasHeight * 4, 0);

	auto sampleWrapped = [&](float u, float v, bool wrapY) {
		u -= std::floor(u);
		if (wrapY) v -= std::floor(v);
		else v = std::clamp(v, 0.0f, 1.0f);
		const float sourceX = u * (wrapY ? (width - 1) : width);
		const float sourceY = v * (height - 1);
		const int x0 = (int)std::floor(sourceX) % width;
		const int y0 = (int)std::floor(sourceY);
		const int x1 = (x0 + 1) % width;
		const int y1 = (wrapY ? (y0 + 1) % height : std::min(y0 + 1, height - 1));
		const float amountX = sourceX - std::floor(sourceX);
		const float amountY = sourceY - y0;
		auto red = [&](int x, int y) { return (float)data[((std::size_t)y * width + x) * channelCount]; };
		const float top = red(x0, y0) * (1.0f - amountX) + red(x1, y0) * amountX;
		const float bottom = red(x0, y1) * (1.0f - amountX) + red(x1, y1) * amountX;
		return top * (1.0f - amountY) + bottom * amountY;
	};

	for (int frame = 0; frame < frameCount; ++frame) {
		const int frameX = (frame % frameColumns) * FrameSize;
		const int frameY = (frame / frameColumns) * FrameSize;
		const float progress = (float)frame / frameCount;
		const float angle = progress * Tau;
		const float angleCos = std::cos(angle);
		const float angleSin = std::sin(angle);

		for (int y = 0; y < FrameSize; ++y) {
			for (int x = 0; x < FrameSize; ++x) {
				std::uint8_t red = 0, green = 0, blue = 0, alpha = 0;
				if (lightning) {
					const float nx = ((x + 0.5f) * 2.0f / FrameSize) - 1.0f;
					const float ny = ((y + 0.5f) * 2.0f / FrameSize) - 1.0f;
					const float radiusSquared = nx * nx + ny * ny;
					if (radiusSquared <= 0.94f * 0.94f) {
						const float nz = std::sqrt(std::max(0.0f, 1.0f - radiusSquared));
						const float longitude = std::atan2(nx, nz) / Pi;
						const float latitude = std::asin(std::clamp(ny, -1.0f, 1.0f)) / Pi;
						const float centerU = 0.5f + 0.70f * progress;
						const float centerV = 0.5f + 0.20f * std::sin(angle);
						const float noise = sampleWrapped(centerU + 0.30f * longitude,
							centerV + 0.30f * latitude, true);
						if (noise >= 142.0f) {
							const float strength = std::clamp((noise - 142.0f) / 85.0f, 0.0f, 1.0f);
							red = (std::uint8_t)(40.0f + 215.0f * strength);
							green = (std::uint8_t)(220.0f + 35.0f * strength);
							blue = (std::uint8_t)(5.0f + 35.0f * strength);
							alpha = 255;
						}
					}
				} else {
					const float nx = ((x + 0.5f) * 2.0f / FrameSize) - 1.0f;
					const float ny = 1.0f - ((y + 0.5f) * 2.0f / FrameSize);
					const float radiusSquared = nx * nx + ny * ny;
					if (radiusSquared < 1.0f) {
						const float nz = std::sqrt(1.0f - radiusSquared);
						const float radius = std::sqrt(radiusSquared);
						const float rotatedX = angleCos * nx + angleSin * nz;
						const float rotatedZ = -angleSin * nx + angleCos * nz;
						const float u = 0.5f + std::atan2(rotatedZ, rotatedX) / Tau;
						const float v = std::acos(std::clamp(ny, -1.0f, 1.0f)) / Pi;
						const int noise = (int)(sampleWrapped(u, v, false) + 0.5f);
						const float rim = std::clamp((radius - 0.48f) / 0.48f, 0.0f, 1.0f);
						const float threshold = 190.0f - 45.0f * rim;
						if (noise > threshold) {
							const float heat = std::clamp((noise - threshold) / std::max(1.0f, 239.0f - threshold), 0.0f, 1.0f);
							red = 255;
							if (heat < 0.24f) green = 48;
							else if (heat < 0.50f) green = 96;
							else if (heat < 0.73f) green = 158;
							else if (heat < 0.90f) { green = 220; blue = 18; }
							else { green = 255; blue = 150; }
							alpha = 255;
						}
					}
				}

				std::uint8_t* destination = atlas.data() +
					((std::size_t)(frameY + y) * atlasWidth + frameX + x) * 4;
				destination[0] = red;
				destination[1] = green;
				destination[2] = blue;
				destination[3] = alpha;
			}
		}
	}

	if (!BakeSpriteFromPixels(key, atlas.data(), atlasWidth, atlasHeight, 4, true, 0,
		FrameSize, FrameSize, out, true, false)) return false;
	out->meta.assign(auraHeader, auraHeader + 39);
	out->meta[11] |= 0x03; // Pre-coloured RGBA and no collision mask.
	std::uint32_t frameDimension = FrameSize;
	std::uint16_t generatedFrameCount = (std::uint16_t)frameCount;
	std::memcpy(out->meta.data() + 13, &frameDimension, sizeof(frameDimension));
	std::memcpy(out->meta.data() + 17, &frameDimension, sizeof(frameDimension));
	out->meta[21] = (std::uint8_t)frameColumns;
	out->meta[22] = (std::uint8_t)frameRows;
	std::memcpy(out->meta.data() + 23, &generatedFrameCount, sizeof(generatedFrameCount));
	return true;
}

bool BakeWaterShieldFromPixels(std::uint64_t key, const std::uint8_t* data, int width, int height,
	int channelCount, int paletteOffset, const std::uint8_t* auraHeader, BakedTexture* out)
{
	if (data == nullptr || auraHeader == nullptr || out == nullptr || width <= 0 || height <= 0 || channelCount != 4)
		return false;

	std::uint32_t frameWidth, frameHeight;
	std::uint16_t frameCount;
	std::memcpy(&frameWidth, auraHeader + 13, sizeof(frameWidth));
	std::memcpy(&frameHeight, auraHeader + 17, sizeof(frameHeight));
	std::memcpy(&frameCount, auraHeader + 23, sizeof(frameCount));
	const int sourceColumns = auraHeader[21];
	if (frameWidth == 0 || frameHeight == 0 || frameCount == 0 || sourceColumns <= 0) return false;

	constexpr int DestinationColumns = 4;
	const int destinationRows = (frameCount + DestinationColumns - 1) / DestinationColumns;
	const int atlasWidth = (int)frameWidth * DestinationColumns;
	const int atlasHeight = (int)frameHeight * destinationRows;
	std::vector<std::uint8_t> atlas((std::size_t)atlasWidth * atlasHeight * 4, 0);
	for (int frame = 0; frame < frameCount; ++frame) {
		const int sourceFrameX = (frame % sourceColumns) * frameWidth;
		const int sourceFrameY = (frame / sourceColumns) * frameHeight;
		const int destinationFrameX = (frame % DestinationColumns) * frameWidth;
		const int destinationFrameY = (frame / DestinationColumns) * frameHeight;
		for (std::uint32_t y = 0; y < frameHeight; ++y) {
			for (std::uint32_t x = 0; x < frameWidth; ++x) {
				const std::uint8_t* source = data +
					((std::size_t)(sourceFrameY + y) * width + sourceFrameX + x) * channelCount;
				const std::uint32_t color = BasePaletteAt(paletteOffset + source[0]);
				std::uint8_t* destination = atlas.data() +
					((std::size_t)(destinationFrameY + y) * atlasWidth + destinationFrameX + x) * 4;
				// Same tint the runtime used to apply, baked in so the sprite is sampleable as RGBA8888.
				destination[0] = (std::uint8_t)(((color & 0xff) * 45) / 100);
				destination[1] = (std::uint8_t)((((color >> 8) & 0xff) * 72) / 100);
				destination[2] = (std::uint8_t)((color >> 16) & 0xff);
				destination[3] = (std::uint8_t)(source[3] * ((color >> 24) & 0xff) / 255);
			}
		}
	}

	if (!BakeSpriteFromPixels(key, atlas.data(), atlasWidth, atlasHeight, 4, true, 0,
		(int)frameWidth, (int)frameHeight, out, true, false)) return false;
	out->meta.assign(auraHeader, auraHeader + 39);
	out->meta[11] |= 0x03; // Pre-coloured RGBA and no collision mask.
	out->meta[21] = DestinationColumns;
	out->meta[22] = (std::uint8_t)destinationRows;
	return true;
}

void CollectSpriteMetadata(const char* contentDir, std::unordered_map<std::string, int>& out)
{
	using fs = Death::IO::FileSystem;
	CollectMetadata(std::string(fs::CombinePath(StringView(contentDir), "Metadata"_s).data()), out);
}

int BakeBundledSprites(const char* contentDir, const std::unordered_map<std::string, int>& meta,
	std::vector<BakedTexture>& out)
{
	using fs = Death::IO::FileSystem;
	std::unordered_set<std::uint64_t> have;
	for (const auto& t : out) have.insert(t.key);

	int baked = 0, failed = 0;
	String animBase = fs::CombinePath(StringView(contentDir), "Animations"_s);
	for (const auto& kv : meta) {
		std::uint64_t key = NameHash(kv.first.c_str());
		if (have.count(key)) continue; // already baked (e.g. by the converter sink)
		String auraPath = fs::CombinePath(animBase, StringView(kv.first.c_str()));
		if (!fs::IsReadableFile(auraPath)) continue; // not bundled here - came (or will come) via the converter
		auto st = fs::Open(auraPath, FileAccess::Read);
		if (st == nullptr || !st->IsValid()) { failed++; continue; }
		BakedTexture bt;
		if (DecodeAura(*st, kv.second, key, kv.first == "UI/glow.aura", &bt)) { out.push_back(std::move(bt)); have.insert(key); baked++; }
		else failed++;
	}
	std::printf("Bundled sprites: baked %d, failed %d\n", baked, failed);
	return failed == 0 ? baked : -failed;
}

int BakeBundledSpritesToSink(const char* contentDir, const std::unordered_map<std::string, int>& meta,
	const std::unordered_set<std::uint64_t>& alreadyBaked, BakedTextureSink sink, void* sinkContext,
	BakeProgressSink progress, void* progressContext)
{
	using fs = Death::IO::FileSystem;
	// Track new keys separately; copying the converter's key set here would peak alongside graphics conversion.
	std::unordered_set<std::uint64_t> newlyBaked;
	int baked = 0, failed = 0;
	String animBase = fs::CombinePath(StringView(contentDir), "Animations"_s);
	struct Candidate {
		const std::string* path;
		int paletteOffset;
		std::uint64_t key;
		std::int64_t packedSize;
	};
	std::vector<Candidate> candidates;
	candidates.reserve(meta.size());
	for (const auto& kv : meta) {
		const std::uint64_t key = NameHash(kv.first.c_str());
		if (alreadyBaked.count(key)) continue;
		const String auraPath = fs::CombinePath(animBase, StringView(kv.first.c_str()));
		if (fs::IsReadableFile(auraPath))
			candidates.push_back({ &kv.first, kv.second, key, fs::GetFileSize(auraPath) });
	}
	// Largest first, while the PSP still has its biggest contiguous free block - shareware needs 107
	// fallbacks here, including multi-page Lori and the generated shield atlases.
	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
		return a.packedSize > b.packedSize;
	});
	const int total = (int)candidates.size();
	if (progress != nullptr) progress(progressContext, 0, total);
	for (const Candidate& candidate : candidates) {
		const std::uint64_t key = candidate.key;
		if (alreadyBaked.count(key) || newlyBaked.count(key)) continue;
		String auraPath = fs::CombinePath(animBase, StringView(candidate.path->c_str()));
		auto source = fs::Open(auraPath, FileAccess::Read);
		BakedTexture texture;
		if (source != nullptr && source->IsValid() && DecodeAura(*source, candidate.paletteOffset, key,
			*candidate.path == "UI/glow.aura", &texture) && sink != nullptr && sink(sinkContext, std::move(texture))) {
			newlyBaked.insert(key);
			++baked;
		} else ++failed;
		if (progress != nullptr) progress(progressContext, baked + failed, total);
	}
	std::printf("Bundled sprites: baked %d, failed %d\n", baked, failed);
	return failed == 0 ? baked : -failed;
}
