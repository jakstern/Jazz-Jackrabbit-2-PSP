#pragma once

// GU texture formats, page layout, swizzling and the on-disk pack container, shared verbatim by the host
// baker (Tools/psp-bake) and the device loader (PspAssetPack) so the two agree byte for byte.
// Keep this header free of pspgu / GL / engine dependencies — it must stay includable from the host tool.

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace nCine { namespace PspGpu {

	enum class PixelFormat : std::uint8_t {
		Rgba8888 = 0,   // 4 bytes/texel, GU_PSM_8888
		Indexed8 = 1,   // 1 byte/texel index, GU_PSM_T8 + a live 256-entry CLUT
		Rg8      = 2,   // 2 bytes/texel (index, per-texel alpha); expanded to RGBA on device per draw offset
	};

	inline int BytesPerTexel(PixelFormat f) {
		return f == PixelFormat::Indexed8 ? 1 : (f == PixelFormat::Rg8 ? 2 : 4);
	}

	enum TextureFlags : std::uint8_t {
		TextureFlagBinaryAlpha = 0x01,
		TextureFlagOpaque = 0x02,
		// Bind the entry's own CLUT instead of the live game palette. Tilesets and ordinary indexed sprites
		// also store a CLUT, but theirs is only a snapshot of the game palette and must NOT be bound - palette
		// cycling and per-player recolouring depend on the live one. Hence the flag rather than testing
		// clutOffset != 0xFFFFFFFF.
		TextureFlagOwnClut = 0x04,
	};

	// Tileset atlases pack 34px cells (32px tile + 1px padding each side). The GU texture limit is 512, so a
	// page is the largest whole number of cells that fits: 15*34 = 510. Pages therefore break on cell
	// boundaries and no tile's sample rect ever straddles two pages.
	constexpr int TileCell   = 34;
	constexpr int PageCells  = 512 / TileCell;       // 15
	constexpr int PageTexels = PageCells * TileCell;  // 510
	constexpr int PageStride = 512;                   // pow2 GU buffer edge per page

	inline int NextPow2(int v) { int p = 1; while (p < v) p <<= 1; return p; }
	inline bool Oversized(int strideW, int strideH) { return strideW > 512 || strideH > 512; }

	// Canonical swizzle_fast: reorders into the block-linear (16-byte x 8-row) layout the GE samples fastest.
	// `widthBytes` is the row pitch in BYTES, `out` must hold widthBytes*height. Both dimensions must be whole
	// multiples of the block or this corrupts the texture - check with CanSwizzle. GU strides always qualify.
	inline void Swizzle(std::uint8_t* out, const std::uint8_t* in, unsigned int widthBytes, unsigned int height)
	{
		unsigned int widthBlocks  = widthBytes / 16;
		unsigned int heightBlocks = height / 8;
		unsigned int srcPitch = (widthBytes - 16) / 4;
		unsigned int srcRow   = widthBytes * 8;
		const std::uint8_t* ysrc = in;
		std::uint32_t* dst = reinterpret_cast<std::uint32_t*>(out);
		for (unsigned int by = 0; by < heightBlocks; ++by) {
			const std::uint8_t* xsrc = ysrc;
			for (unsigned int bx = 0; bx < widthBlocks; ++bx) {
				const std::uint32_t* src = reinterpret_cast<const std::uint32_t*>(xsrc);
				for (unsigned int n = 0; n < 8; ++n) {
					*dst++ = *src++; *dst++ = *src++; *dst++ = *src++; *dst++ = *src++;
					src += srcPitch;
				}
				xsrc += 16;
			}
			ysrc += srcRow;
		}
	}

	inline bool CanSwizzle(unsigned int widthBytes, unsigned int height) {
		return (widthBytes % 16) == 0 && (height % 8) == 0 && widthBytes >= 16 && height >= 8;
	}

	// FNV-1a 64. Textures are keyed by asset name (e.g. the tileset stem "castle1") because the device knows
	// that before it decodes anything, letting it skip decode + atlas assembly entirely, not just conversion.
	inline std::uint64_t NameHash(const char* name)
	{
		std::uint64_t hsh = 0xcbf29ce484222325ull;
		for (const char* p = name; *p; ++p) hsh = (hsh ^ (std::uint8_t)*p) * 0x100000001b3ull;
		return hsh;
	}
	// Over pixels; for optional integrity checks / capture-point keying.
	inline std::uint64_t ContentHash(const void* data, std::size_t size, int w, int h, PixelFormat fmt)
	{
		std::uint64_t hsh = 0xcbf29ce484222325ull;
		auto mix = [&](std::uint64_t v) { hsh = (hsh ^ v) * 0x100000001b3ull; };
		mix((std::uint64_t)w); mix((std::uint64_t)h); mix((std::uint64_t)fmt);
		const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
		for (std::size_t i = 0; i < size; ++i) hsh = (hsh ^ p[i]) * 0x100000001b3ull;
		return hsh;
	}

	// On-disk container. All offsets are from the start of the file; the writer appends the entry/page tables
	// after the blob so it can finalize atomically, so readers must follow entriesOffset/blobOffset and never
	// assume section order.
	constexpr std::uint32_t PackMagic = 0x50475350; // 'PSPG' (little-endian in file)
	// v6: pre-coloured RGBA sprites with <= 256 colours are palettised to Indexed8 with their own CLUT, and
	//     non-paged entries may carry a CLUT. v5 packs stay structurally readable but cost 4x the memory for
	//     those sprites, so the bump exists to force a re-bake rather than silently keep the old layout.
	constexpr std::uint16_t PackVersion = 6;

	// Collision masks bake to 1 bit per texel (alpha > threshold); the runtime only asks "is this texel solid?".
	// Must match Jazz2::Actors::ActorBase::AlphaThreshold.
	constexpr std::uint8_t MaskAlphaThreshold = 40;
	inline std::size_t MaskPackedBytes(std::size_t texels) { return (texels + 7) / 8; }

	struct PackHeader {
		std::uint32_t magic;        // PackMagic
		std::uint16_t version;      // PackVersion
		std::uint16_t reserved;
		std::uint32_t entryCount;
		std::uint32_t entriesOffset; // -> TexEntry[entryCount]
		std::uint32_t blobOffset;    // -> pixel/clut blob region
		std::uint32_t blobSize;
	};

	struct TexPage {
		std::uint32_t dataOffset;   // -> swizzled page pixels in the blob (PageStride*PageStride*bpp)
		std::int16_t ox, oy;        // atlas-texel offset of this page's top-left
		std::int16_t w, h;          // valid texels in this page
	};

	// One baked texture, keyed by NameHash(asset name).
	//   paged == 0 (sprites):  one blob at dataOffset, stride*strideH*bpp bytes (pow2 padded dims).
	//   paged == 1 (tilesets): a pagesX*pagesY grid from TexPage[firstPage]; stride/strideH == PageStride.
	struct TexEntry {
		std::uint64_t key;          // NameHash of the asset name
		std::uint16_t width, height;// logical texel size
		std::uint16_t stride;       // pow2 row texels (single) / PageStride (paged)
		std::uint16_t strideH;      // pow2 rows   (single) / PageStride (paged)
		std::uint8_t  format;       // PixelFormat
		std::uint8_t  swizzled;     // 1 if pixel data is swizzled
		std::uint8_t  paged;        // 0 = single texture (dataOffset), 1 = paged atlas (firstPage/pagesX/pagesY)
		std::uint8_t  pagesX, pagesY;
		std::uint8_t  flags;        // TextureFlags; enables safe fixed-function fast paths
		std::uint32_t clutOffset;   // -> 256*uint32 CLUT in the blob, or 0xFFFFFFFF if none
		std::uint32_t dataOffset;   // single: -> pixel blob; paged: unused
		std::uint32_t firstPage;    // paged: index into the TexPage array (pagesX*pagesY consecutive)
		std::uint32_t maskOffset;   // single sprites: -> width*height collision mask (1 byte/texel), or 0xFFFFFFFF
		std::uint32_t metaOffset;   // single sprites: -> the .aura header bytes (frame metadata), or 0xFFFFFFFF
		std::uint16_t metaSize;     // number of .aura header bytes at metaOffset
	};

}} // namespace nCine::PspGpu
