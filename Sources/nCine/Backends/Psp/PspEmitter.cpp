// PSP GU emitter. nCine builds sprite quads in the vertex shader from gl_VertexID plus an InstanceBlock UBO
// (modelMatrix, color, texRect, spriteSize); the PSP has no shaders, so RenderCommand::Issue lands in
// PspEmitCommand, which reconstructs the quad on the CPU and draws it through the GU with the camera
// matrices the engine already computed. Texture uploads are captured from GLTexture::TexImage2D into GU
// textures keyed by the GLTexture pointer.

#include "nCine/Graphics/RenderCommand.h"
#include "nCine/Graphics/Material.h"
#include "nCine/Graphics/GL/GLTexture.h"
#include "nCine/Graphics/GL/GLShim.h"
#include "nCine/Graphics/RenderResources.h"
#include "nCine/Graphics/Camera.h"
#include "nCine/Primitives/Matrix4x4.h"
#include "Jazz2/ContentResolver.h"
#include "PspAssetPack.h"
#include "PspDiagnostics.h"
#include "PspGu.h"
#include "PspHardware.h"
#include "PspTextureStreamer.h"
#include "PspVram.h"

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <malloc.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <pspsysmem.h>

namespace nCine
{
	namespace
	{
		// A texture over the GU's 512x512 limit (a full tileset atlas) is split into a grid of 512x512 pages,
		// aligned to the 34px tile cell so no tile straddles a page boundary (see PageTexels).
		struct GuPage {
			void* data = nullptr;   // 512*512*bpp, tile data in the top-left (origin at atlas (ox,oy))
			int ox = 0, oy = 0;     // atlas-texel offset of this page's top-left
			int w = 0, h = 0;       // valid texels in this page
			std::uint32_t lastUsedFrame = 0;
		};

		// Page pixels live in a VRAM slot (PspVram.h) or the heap, never both. VRAM wins where it can: the GE
		// samples the tilemap across the whole screen every frame, and a pinned page costs no main RAM.
		// Route EVERY page allocation and free through this pair - std::free() on a VRAM pointer corrupts the heap.
		void* AllocPageStorage(std::size_t bytes)
		{
			// Tilesets bake to T8 and match the slot size exactly; a paged RGBA8888 atlas is refused by the pool.
			if (void* slot = PspVramAcquireSlot(bytes)) return slot;
			return memalign(16, bytes);
		}

		void FreePageStorage(void*& data)
		{
			if (data == nullptr) return;
			if (PspVramContains(data)) PspVramReleaseSlot(data);
			else std::free(data);
			data = nullptr;
		}

		// Distinct palette rows one RG8 sprite can be baked through in a frame; four covers four players each
		// picking a different fur colour on the same character sprite.
		constexpr int MaxRg8Variants = 4;
		struct Rg8Variant {
			void* data = nullptr;
			int offset = -1;              // flat palette offset this bake used
			std::uint32_t frame = 0;      // last frame a draw was queued against this bake
			bool built = false;
		};

		struct GuTexture {
			void* data = nullptr;
			int width = 0, height = 0;    // Logical size
			int stride = 0, strideH = 0;  // Power-of-two allocation
			int bpp = 4;                  // Bytes per texel: 4 = RGBA8888, 1 = 8-bit palette index (GU_PSM_T8), 2 = RG8 (index + alpha)
			// Paging (only when width/height exceed the GU limit)
			bool oversized = false;
			bool pagesBuilt = false;
			bool spritePaged = false; // frame-aligned pages rather than 34px tileset pages
			bool swizzled = false;   // pages hold pre-swizzled data from the baked pack (GU samples swizzled)
			int pagesX = 0, pagesY = 0;
			std::vector<GuPage> pages;
			const PspGpu::TexEntry* packEntry = nullptr;
			bool lowMemoryStreamed = false;
			std::uint32_t lastUsedFrame = 0;
			// Async streaming (non-paged sprites on the low-memory model): a background read fills `pendingData`, which
			// is promoted to `data` once complete. streamReq is the in-flight PspTextureStreamer handle (-1 = none).
			void* pendingData = nullptr;
			int streamReq = -1;
			// RG8 (bpp==2) has no GU sampler mode, so it is baked to RGBA8888 through the palette. One buffer per
			// palette row: the GE consumes sceGuTexImage lazily, so a buffer already handed to a queued draw must
			// not be re-baked later in the same frame - a single shared buffer made two players with different fur
			// colours both render whichever was baked last.
			Rg8Variant rgbaVariants[MaxRg8Variants];
			// Scratch for the paged path only (one page at a time; oversized atlases are R8 in practice).
			void* rgba8888 = nullptr;
			// Own 256-entry CLUT for a pre-coloured palettised sprite (PspGpu::TextureFlagOwnClut), else nullptr to
			// sample the live game palette. Owned here rather than read per bind because sceGuClutLoad only records
			// the pointer - the GE reads it when the deferred list runs - so it must outlive the queueing frame.
			std::uint32_t* packClut = nullptr;
		};

		void InvalidateRg8Variants(GuTexture& t)
		{
			for (Rg8Variant& v : t.rgbaVariants) {
				v.built = false;
				v.offset = -1;
			}
		}

		void FreeRg8Variants(GuTexture& t)
		{
			for (Rg8Variant& v : t.rgbaVariants) {
				std::free(v.data);
				v.data = nullptr;
				v.built = false;
				v.offset = -1;
				v.frame = 0;
			}
		}

		std::size_t Rg8VariantBytes(const GuTexture& t)
		{
			std::size_t bytes = 0;
			for (const Rg8Variant& v : t.rgbaVariants) {
				if (v.data != nullptr) bytes += static_cast<std::size_t>(t.stride) * t.strideH * 4;
			}
			return bytes;
		}

		// Cancel any in-flight background read and free its buffer. Blocks on the worker first: it may still be
		// writing into pendingData, and a live sceIoRead must never be handed freed memory.
		void CancelStreamRequest(GuTexture& texture)
		{
			if (texture.streamReq >= 0) {
				PspTextureStreamer::Get().Wait(texture.streamReq);
				PspTextureStreamer::Get().Release(texture.streamReq);
				texture.streamReq = -1;
			}
			std::free(texture.pendingData);
			texture.pendingData = nullptr;
		}

		// Full teardown: primary buffer, per-page allocations, RG8 bakes and own CLUT. Kept in one place so
		// deleting or replacing a GLTexture cannot leave any of those PSP-side allocations behind.
		void ReleaseGuTextureStorage(GuTexture& texture)
		{
			CancelStreamRequest(texture);
			std::free(texture.data);
			texture.data = nullptr;
			std::free(texture.rgba8888);
			texture.rgba8888 = nullptr;
			FreeRg8Variants(texture);
			// Deliberately NOT freed in ReleaseSpriteResidency: that path keeps the texture alive to stream back,
			// and re-reading 1 KiB of CLUT each round trip is pure waste.
			std::free(texture.packClut);
			texture.packClut = nullptr;
			// Also returns this atlas's VRAM slots to the pool - how a level change frees them for the next tileset.
			for (GuPage& page : texture.pages) {
				FreePageStorage(page.data);
			}
			texture.pages.clear();
			texture.pagesBuilt = false;
			texture.spritePaged = false;
			texture.pagesX = 0;
			texture.pagesY = 0;
			texture.packEntry = nullptr;
			texture.lowMemoryStreamed = false;
			texture.lastUsedFrame = 0;
		}

		// Jazz2 tileset atlases pack 34px cells (32px tile + 1px padding each side). A page holds the largest whole
		// number of cells fitting in 512 texels (15*34 = 510), so every tile's sample rect lies within one page.
		constexpr int TileCell = 34;
		constexpr int PageCells = 512 / TileCell;      // 15
		constexpr int PageTexels = PageCells * TileCell; // 510
		constexpr int PageStride = 512;                 // pow2 GU buffer size per page
		constexpr int LowMemoryResidentPages = 5;
		// Actor metadata references every animation a level might need - several MiB of baked pixels resident for
		// a small visible subset. On 01g, cap the working set and reload cold sprites from the seekable texture pack.
		constexpr std::size_t LowMemorySpriteBytes = 3 * 1024 * 1024;
		// Watermarks for the per-frame cold sweep (see EvictColdSprites). Shedding starts at 80% of the soft cap
		// and runs down to 60%, so a working set that naturally sits near the cap cannot re-evict every frame.
		constexpr std::size_t ColdEvictHighWater = (LowMemorySpriteBytes / 5) * 4;
		constexpr std::size_t ColdEvictLowWater = (LowMemorySpriteBytes / 5) * 3;
		constexpr std::int32_t ColdEvictFrames = 90; // not drawn in ~1.5-3 s (depends on present rate); conservative
		constexpr int MaxColdEvictionsPerFrame = 4;  // bound the per-frame stall; the sweep resumes next frame
		std::uint32_t g_frameSerial = 1;

		// pspsdk's sceGuGetMemory does NO bounds checking - it bumps a pointer and writes a jump over the reserved
		// span, never comparing against the buffer end. Once a frame's cumulative geometry exceeds the list
		// (PspGuDisplayListBytes, allocated in PspGuInit), every further draw writes vertices and GE commands into whatever heap
		// block follows it. Route every allocation through GuAlloc so the consumption is at least observable; the
		// per-call allowance below covers the jump sceGuGetMemory writes, making the count a close upper bound.
		constexpr std::size_t GuAllocCallOverhead = 16;
		// Held back for the GE command stream, which shares the list but bypasses GuAlloc and so is invisible to
		// the counter (a sceGuSetMatrix alone is 68 bytes, three per draw), for the one command that may still be
		// admitted after the last check, and for the end-of-frame flush. A quarter, so it tracks the list size.
		std::size_t GuListReserveBytes() { return PspGuDisplayListBytes() / 4; }
		// No single mesh may exceed what the budget can absorb in one go.
		std::size_t MaxMeshBytes() { return PspGuDisplayListBytes() / 4; }
		std::size_t g_guListBytesThisFrame = 0;
		std::size_t g_guListHighWater = 0;
		std::uint32_t g_guListOverflowFrames = 0;
		std::uint32_t g_guListDroppedCommands = 0;

		std::size_t GuListBudget()
		{
			const std::size_t capacity = PspGuDisplayListBytes();
			const std::size_t reserve = GuListReserveBytes();
			return capacity > reserve ? capacity - reserve : 0;
		}

		// Checked once per render command BEFORE any GU state is touched, so an over-budget frame drops whole
		// draws instead of half-emitting one and leaving the GE in a state the next command does not expect.
		bool GuListExhausted()
		{
			return g_guListBytesThisFrame >= GuListBudget();
		}

		void* GuAlloc(std::size_t bytes)
		{
			g_guListBytesThisFrame += bytes + GuAllocCallOverhead;
			if (g_guListBytesThisFrame > g_guListHighWater) g_guListHighWater = g_guListBytesThisFrame;
			if (g_guListBytesThisFrame > PspGuDisplayListBytes()) g_guListOverflowFrames++;
			return sceGuGetMemory((int)bytes);
		}

		std::unordered_map<const GLTexture*, GuTexture> g_textures;

		void TrimLowMemorySpriteTextures(std::size_t requiredBytes, const GuTexture* keep);
		bool EnsurePackTextureResident(GuTexture& texture);

		// Indexed Jazz2 graphics arrive as GL_RED (raw palette index), true-colour as GL_RGBA; GL_RG is the
		// 2-byte gem/partial-alpha case.
		int BppForFormat(unsigned int format)
		{
			switch (format) {
				case GL_RED: return 1;
				case GL_RG: return 2;
				default: return 4;
			}
		}

		// Scratch CLUT for the per-glyph colorized font bake (16-byte aligned, as sceGuClutLoad requires).
		alignas(16) std::uint32_t g_clut[256];
		// Rainbow glyphs use an immutable neutral palette plus per-vertex dye. Must stay separate from g_clut: GU
		// commands are deferred, so reusing a mutable palette buffer makes every queued glyph observe the last one.
		alignas(16) std::uint32_t g_fontRainbowClut[256];
		bool g_fontRainbowClutInitialized = false;

		// Palette row currently loaded into the GE, so an indexed draw only re-uploads when the row changes. A
		// frame boundary always re-uploads once, so in-place palette cycling still shows.
		const std::uint32_t* g_clutSrc = nullptr;
		bool g_clutLoadedThisFrame = false;
		bool g_fontClutValid = false;
		std::uint32_t g_fontClutColor = 0;

		// GU texture state persists between draws and glyphs/particles submit many consecutive commands on one
		// atlas, so rebinding per quad only bloats the list. Reset per frame and whenever a path bypasses the binder.
		const GuTexture* g_boundTexture = nullptr;
		int g_boundPaletteOffset = 0;
		bool g_boundTiled = false;

		void InvalidateTextureBinding()
		{
			g_boundTexture = nullptr;
		}

		void EnsureClutLoaded(int paletteOffset = 0)
		{
			auto palettes = Jazz2::ContentResolver::Get().GetPalettes();
			if (paletteOffset < 0 || paletteOffset + 256 > (int)palettes.size()) paletteOffset = 0;
			const std::uint32_t* palette = palettes.data() + paletteOffset;
			// sceGuClutLoad only records the pointer - the GE reads it when the deferred list runs - so the buffer
			// must stay unmodified for the rest of the frame. Hence pointing straight at the immutable palette row
			// rather than staging through a shared scratch buffer: with two palettes live in one frame, every queued
			// load saw whichever was staged last and a recolored player flickered to the tileset's palette. Rows are
			// 1 KB apart and _palettes is 16-byte aligned, so each row alone satisfies sceGuClutLoad's alignment.
			if (g_clutLoadedThisFrame && palette == g_clutSrc) return;
			sceKernelDcacheWritebackRange(const_cast<std::uint32_t*>(palette), 256 * sizeof(std::uint32_t));
			sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
			sceGuClutLoad(256 / 8, palette);
			PspDiagnosticsRecordClutLoad();
			g_clutSrc = palette;
			g_clutLoadedThisFrame = true;
			g_fontClutValid = false;
		}

		// Bind the CLUT an indexed texture samples through: its own baked palette if it has one, else the live game
		// palette row. Same deferred-read rule as EnsureClutLoaded, satisfied by packClut living as long as the texture.
		void EnsureTextureClutLoaded(const GuTexture& t, int paletteOffset)
		{
			if (t.packClut == nullptr) {
				EnsureClutLoaded(paletteOffset);
				return;
			}
			if (g_clutLoadedThisFrame && t.packClut == g_clutSrc) return;
			sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
			sceGuClutLoad(256 / 8, t.packClut);
			PspDiagnosticsRecordClutLoad();
			g_clutSrc = t.packClut;
			g_clutLoadedThisFrame = true;
			g_fontClutValid = false;
		}

		int NextPow2(int v)
		{
			int p = 1;
			while (p < v) p <<= 1;
			return p;
		}

		// GU hardware limit: 512x512 texels. Anything larger cannot be bound directly and would hang the GU.
		constexpr int GuMaxTexDim = 512;

		struct SpriteVertex { float u, v; std::uint32_t color; float x, y, z; };

		// Column-major 4x4 multiply: out = a * b
		void Mat4Mul(const float* a, const float* b, float* out)
		{
			for (int c = 0; c < 4; ++c)
				for (int r = 0; r < 4; ++r) {
					float s = 0.0f;
					for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
					out[c * 4 + r] = s;
				}
		}

		// proj*view is constant for every command of one camera in a frame, so cache the product instead of
		// repeating the multiply per layer/actor. Reset by PspEmitResetFrameState; a camera switch refreshes it.
		const Camera* g_pvCamera = nullptr;
		float g_projectionView[16];

		const float* ProjectionView(const Camera& camera)
		{
			if (g_pvCamera != &camera) {
				Mat4Mul(camera.GetProjection().Data(), camera.GetView().Data(), g_projectionView);
				g_pvCamera = &camera;
			}
			return g_projectionView;
		}

		void SetGuMatrix(int type, const float* values)
		{
			ScePspFMatrix4 matrix;
			std::memcpy(&matrix, values, sizeof(matrix));
			sceGuSetMatrix(type, &matrix);
		}

		struct WorldBatchVertex { float u, v; std::uint32_t color; float x, y, z; };
		struct UiBatchVertex { std::int16_t u, v; std::uint32_t color; std::int16_t x, y, z; };

		constexpr std::size_t MaxSpriteBatchQuads = 256;
		GuTexture* g_spriteBatchTexture = nullptr;
		const Camera* g_spriteBatchCamera = nullptr;
		int g_spriteBatchPaletteOffset = 0;
		bool g_spriteBatchTiled = false;
		bool g_spriteBatchUi = false;
		bool g_spriteBatchFontRainbow = false;
		std::vector<WorldBatchVertex> g_worldBatchVertices;
		std::vector<UiBatchVertex> g_uiBatchVertices;
		std::vector<std::uint16_t> g_spriteBatchIndices;

		// Return this RG8 texture expanded to RGBA8888 through `paletteOffset`, reusing an existing bake: RGB from
		// palette[index] (R byte), alpha from the palette entry's alpha scaled by the per-texel alpha (G byte).
		// Used by gems and partial-alpha sprites; built on first bind, invalidated by realloc/PspUpdateSubTexture.
		// One buffer per palette row, so draws already queued against a bake stay correct once the GE runs the list.
		void* EnsureRg8Variant(GuTexture& t, int paletteOffset)
		{
			for (Rg8Variant& v : t.rgbaVariants) {
				if (v.built && v.data != nullptr && v.offset == paletteOffset) {
					v.frame = g_frameSerial;
					return v.data;
				}
			}
			Rg8Variant* slot = nullptr;
			for (Rg8Variant& v : t.rgbaVariants) {
				if (v.data == nullptr || !v.built) { slot = &v; break; }
			}
			if (slot == nullptr) {
				// Only recycle a bake this frame has not already queued a draw against.
				for (Rg8Variant& v : t.rgbaVariants) {
					if (v.frame != g_frameSerial) { slot = &v; break; }
				}
			}
			// More distinct rows in one frame than we can hold - unreachable with four players, so degrade to the
			// shared-buffer behaviour rather than dropping the texture.
			if (slot == nullptr) slot = &t.rgbaVariants[0];

			const std::size_t bytes = static_cast<std::size_t>(t.stride) * t.strideH * 4;
			if (slot->data == nullptr) {
				TrimLowMemorySpriteTextures(bytes, &t);
				slot->data = memalign(16, bytes);
			}
			if (t.data == nullptr || slot->data == nullptr) return nullptr;

			const std::size_t texels = static_cast<std::size_t>(t.stride) * t.strideH;
			auto pal = Jazz2::ContentResolver::Get().GetPalettes();  // flat 256x256 palette (65536 entries)
			const std::int32_t palCount = (std::int32_t)pal.size();
			const std::uint32_t* palData = pal.data();
			const auto* src = static_cast<const std::uint8_t*>(t.data);
			auto* dst = static_cast<std::uint32_t*>(slot->data);
			for (std::size_t i = 0; i < texels; ++i) {
				const std::uint8_t idx = src[(i * 2) + 0];
				const std::uint8_t a = src[(i * 2) + 1];
				std::int32_t pi = paletteOffset + idx;
				if (pi < 0 || pi >= palCount) pi = idx;               // bad offset -> base row
				const std::uint32_t p = palData[pi];                  // 0xAABBGGRR (GU 8888 byte order)
				const std::uint32_t pa = ((p >> 24) * a) / 255;       // palette alpha * per-texel alpha
				dst[i] = (p & 0x00FFFFFFu) | (pa << 24);
			}
			sceKernelDcacheWritebackRange(slot->data, texels * 4);
			slot->built = true;
			slot->offset = paletteOffset;
			slot->frame = g_frameSerial;
			return slot->data;
		}

		void ExpandRg8PageToRgba(GuTexture& t, const GuPage& page, int paletteOffset)
		{
			const std::size_t texels = static_cast<std::size_t>(PageStride) * PageStride;
			if (t.rgba8888 == nullptr) t.rgba8888 = memalign(16, texels * 4);
			if (page.data == nullptr || t.rgba8888 == nullptr) return;
			auto pal = Jazz2::ContentResolver::Get().GetPalettes();
			const std::int32_t palCount = (std::int32_t)pal.size();
			const auto* src = static_cast<const std::uint8_t*>(page.data);
			auto* dst = static_cast<std::uint32_t*>(t.rgba8888);
			for (std::size_t i = 0; i < texels; i++) {
				const std::uint8_t idx = src[i * 2];
				const std::uint8_t alpha = src[i * 2 + 1];
				std::int32_t pi = paletteOffset + idx;
				if (pi < 0 || pi >= palCount) pi = idx;
				const std::uint32_t p = pal[pi];
				const std::uint32_t a = ((p >> 24) * alpha) / 255;
				dst[i] = (p & 0x00ffffffu) | (a << 24);
			}
			sceKernelDcacheWritebackRange(t.rgba8888, texels * 4);
		}

		// Bind a captured GU texture for MODULATE/nearest sampling: RGBA as GU_PSM_8888, R8 as GU_PSM_T8 through a
		// CLUT, RG8 through its palette-baked RGBA copy. False if too large for the GU - those take the paged path.
		bool BindGuTexture(GuTexture& t, int paletteOffset, bool tiled = false)
		{
			if (!EnsurePackTextureResident(t)) return false;
			// Too big for the GU - skip texturing rather than hang.
			if (t.stride > GuMaxTexDim || t.strideH > GuMaxTexDim) return false;
			sceGuEnable(GU_TEXTURE_2D);
			// A texture with its own CLUT renders identically whatever row the caller asks for, so keep the offset
			// out of the binding key; otherwise the same sprite drawn at two offsets would rebind for nothing.
			const int bindingPaletteOffset = (t.packClut != nullptr ? 0 : (t.bpp == 1 || t.bpp == 2 ? paletteOffset : 0));
			if (g_boundTexture == &t && g_boundPaletteOffset == bindingPaletteOffset && g_boundTiled == tiled) return true;
			// Repeat wrap lets a small texture tile across a large quad via >1 texcoords (the menu's rotating
			// background); everything else clamps so edge texels don't bleed.
			sceGuTexWrap(tiled ? GU_REPEAT : GU_CLAMP, tiled ? GU_REPEAT : GU_CLAMP);
			const int swz = (t.swizzled ? 1 : 0);
			if (t.bpp == 1) {
				EnsureTextureClutLoaded(t, paletteOffset);
				sceGuTexMode(GU_PSM_T8, 0, 0, swz);
				sceGuTexImage(0, t.stride, t.strideH, t.stride, t.data);
				PspDiagnosticsRecordTextureImage();
			} else if (t.bpp == 2) {
				void* expanded = EnsureRg8Variant(t, paletteOffset);
				if (expanded == nullptr) return false;
				// The bake is produced linearly on the device, so it binds unswizzled even when the RG8 source is
				// a swizzled pack entry - passing `swz` here would sample garbage.
				sceGuTexMode(GU_PSM_8888, 0, 0, 0);
				sceGuTexImage(0, t.stride, t.strideH, t.stride, expanded);
				PspDiagnosticsRecordTextureImage();
			} else if (t.bpp == 4) {
				sceGuTexMode(GU_PSM_8888, 0, 0, swz);
				sceGuTexImage(0, t.stride, t.strideH, t.stride, t.data);
				PspDiagnosticsRecordTextureImage();
			} else {
				sceGuDisable(GU_TEXTURE_2D);
				return false;
			}
			sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
			sceGuTexFilter(GU_NEAREST, GU_NEAREST);
			g_boundTexture = &t;
			g_boundPaletteOffset = bindingPaletteOffset;
			g_boundTiled = tiled;
			return true;
		}

		bool BindFontRainbowTexture(GuTexture& t)
		{
			if (!EnsurePackTextureResident(t) || t.bpp != 1 || t.stride > GuMaxTexDim || t.strideH > GuMaxTexDim)
				return false;
			constexpr int FontRainbowPaletteOffset = -1;
			sceGuEnable(GU_TEXTURE_2D);
			if (g_boundTexture == &t && g_boundPaletteOffset == FontRainbowPaletteOffset && !g_boundTiled) return true;

			if (!g_fontRainbowClutInitialized) {
				auto byte = [](float value) -> std::uint32_t {
					return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
				};
				for (int i = 0; i < 256; i++) {
					const std::uint32_t coverage = byte(float(i >> 4) / 15.0f);
					const std::uint32_t gray = byte(float(i & 15) / 10.0f);
					g_fontRainbowClut[i] = (coverage << 24) | (gray << 16) | (gray << 8) | gray;
				}
				sceKernelDcacheWritebackRange(g_fontRainbowClut, sizeof(g_fontRainbowClut));
				g_fontRainbowClutInitialized = true;
			}

			sceGuTexWrap(GU_CLAMP, GU_CLAMP);
			sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
			sceGuClutLoad(256 / 8, g_fontRainbowClut);
			PspDiagnosticsRecordClutLoad();
			sceGuTexMode(GU_PSM_T8, 0, 0, t.swizzled ? 1 : 0);
			sceGuTexImage(0, t.stride, t.strideH, t.stride, t.data);
			PspDiagnosticsRecordTextureImage();
			sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
			sceGuTexFilter(GU_NEAREST, GU_NEAREST);
			g_boundTexture = &t;
			g_boundPaletteOffset = FontRainbowPaletteOffset;
			g_boundTiled = false;
			g_clutLoadedThisFrame = false;
			g_fontClutValid = false;
			return true;
		}

		void ClearSpriteBatch()
		{
			g_spriteBatchTexture = nullptr;
			g_spriteBatchCamera = nullptr;
			g_spriteBatchFontRainbow = false;
			g_worldBatchVertices.clear();
			g_uiBatchVertices.clear();
			g_spriteBatchIndices.clear();
		}

		void FlushSpriteBatch()
		{
			if (g_spriteBatchTexture == nullptr || g_spriteBatchIndices.empty()) {
				ClearSpriteBatch();
				return;
			}
			PspDiagnosticsRecordBatchFlush(static_cast<std::uint32_t>(g_spriteBatchIndices.size() / 6));

			GuTexture& texture = *g_spriteBatchTexture;
			const bool textureBound = (g_spriteBatchFontRainbow ? BindFontRainbowTexture(texture) :
				BindGuTexture(texture, g_spriteBatchPaletteOffset, g_spriteBatchTiled));
			if (!textureBound) {
				ClearSpriteBatch();
				return;
			}
			sceGuTexScale(1.0f, 1.0f);
			sceGuTexOffset(0.0f, 0.0f);
			const std::uint8_t textureFlags = (texture.packEntry != nullptr ? texture.packEntry->flags : 0);
			bool vertexAlphaOpaque = (textureFlags & PspGpu::TextureFlagBinaryAlpha) != 0;
			if (vertexAlphaOpaque && g_spriteBatchUi) {
				for (const UiBatchVertex& vertex : g_uiBatchVertices)
					vertexAlphaOpaque = vertexAlphaOpaque && ((vertex.color >> 24) == 255);
			} else if (vertexAlphaOpaque) {
				for (const WorldBatchVertex& vertex : g_worldBatchVertices)
					vertexAlphaOpaque = vertexAlphaOpaque && ((vertex.color >> 24) == 255);
			}
			const bool binaryAlphaFastPath = !g_spriteBatchFontRainbow && vertexAlphaOpaque;
			const bool opaqueFastPath = binaryAlphaFastPath && (textureFlags & PspGpu::TextureFlagOpaque) != 0;
			if (binaryAlphaFastPath) sceGuDisable(GU_BLEND);
			if (opaqueFastPath) sceGuDisable(GU_ALPHA_TEST);

			auto* indices = static_cast<std::uint16_t*>(GuAlloc(g_spriteBatchIndices.size() * sizeof(std::uint16_t)));
			std::memcpy(indices, g_spriteBatchIndices.data(), g_spriteBatchIndices.size() * sizeof(std::uint16_t));
			if (g_spriteBatchUi) {
				auto* vertices = static_cast<UiBatchVertex*>(GuAlloc(g_uiBatchVertices.size() * sizeof(UiBatchVertex)));
				std::memcpy(vertices, g_uiBatchVertices.data(), g_uiBatchVertices.size() * sizeof(UiBatchVertex));
				sceGuDrawArray(GU_TRIANGLES,
					GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_2D,
					static_cast<int>(g_spriteBatchIndices.size()), indices, vertices);
				PspDiagnosticsRecordDraw(static_cast<std::uint32_t>(g_uiBatchVertices.size()));
			} else {
				static const float Identity[16] = {
					1.0f, 0.0f, 0.0f, 0.0f,
					0.0f, 1.0f, 0.0f, 0.0f,
					0.0f, 0.0f, 1.0f, 0.0f,
					0.0f, 0.0f, 0.0f, 1.0f
				};
				SetGuMatrix(GU_PROJECTION, g_spriteBatchCamera->GetProjection().Data());
				SetGuMatrix(GU_VIEW, g_spriteBatchCamera->GetView().Data());
				SetGuMatrix(GU_MODEL, Identity);
				auto* vertices = static_cast<WorldBatchVertex*>(GuAlloc(g_worldBatchVertices.size() * sizeof(WorldBatchVertex)));
				std::memcpy(vertices, g_worldBatchVertices.data(), g_worldBatchVertices.size() * sizeof(WorldBatchVertex));
				sceGuDrawArray(GU_TRIANGLES,
					GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
					static_cast<int>(g_spriteBatchIndices.size()), indices, vertices);
				PspDiagnosticsRecordDraw(static_cast<std::uint32_t>(g_worldBatchVertices.size()));
			}
			if (opaqueFastPath) sceGuEnable(GU_ALPHA_TEST);
			if (binaryAlphaFastPath) sceGuEnable(GU_BLEND);
			sceGuDisable(GU_TEXTURE_2D);
			ClearSpriteBatch();
		}

		bool SpriteBatchCompatible(GuTexture& texture, int paletteOffset, bool tiled, bool uiSpace, const Camera* camera,
			bool fontRainbow)
		{
			const int bindingPaletteOffset = (texture.bpp == 1 || texture.bpp == 2 ? paletteOffset : 0);
			return g_spriteBatchTexture == &texture && g_spriteBatchPaletteOffset == bindingPaletteOffset &&
				g_spriteBatchTiled == tiled && g_spriteBatchUi == uiSpace && g_spriteBatchFontRainbow == fontRainbow &&
				(uiSpace || g_spriteBatchCamera == camera);
		}

		void BeginSpriteBatch(GuTexture& texture, int paletteOffset, bool tiled, bool uiSpace, const Camera* camera,
			bool fontRainbow)
		{
			g_spriteBatchTexture = &texture;
			g_spriteBatchCamera = camera;
			g_spriteBatchPaletteOffset = (texture.bpp == 1 || texture.bpp == 2 ? paletteOffset : 0);
			g_spriteBatchTiled = tiled;
			g_spriteBatchUi = uiSpace;
			g_spriteBatchFontRainbow = fontRainbow;
		}

		void PrepareSpriteBatch(GuTexture& texture, int paletteOffset, bool tiled, bool uiSpace, const Camera* camera,
			bool fontRainbow = false)
		{
			const std::size_t quadCount = g_spriteBatchIndices.size() / 6;
			if (!SpriteBatchCompatible(texture, paletteOffset, tiled, uiSpace, camera, fontRainbow) || quadCount >= MaxSpriteBatchQuads) {
				FlushSpriteBatch();
				BeginSpriteBatch(texture, paletteOffset, tiled, uiSpace, camera, fontRainbow);
			}
		}

		void AppendSpriteBatchIndices(std::size_t firstVertex)
		{
			const std::uint16_t base = static_cast<std::uint16_t>(firstVertex);
			g_spriteBatchIndices.push_back(base + 0);
			g_spriteBatchIndices.push_back(base + 1);
			g_spriteBatchIndices.push_back(base + 2);
			g_spriteBatchIndices.push_back(base + 2);
			g_spriteBatchIndices.push_back(base + 1);
			g_spriteBatchIndices.push_back(base + 3);
		}

		// Tileset atlases are pre-baked on the host (Tools/psp-bake) into cell-aligned, pre-swizzled 512-pages and
		// streamed from Cache/PspTextures.pack; the device never builds pages at runtime. ContentResolver sets this
		// pending name-hash just before creating the placeholder Texture, and PspCaptureTexture consumes it.
		std::uint64_t g_pendingPackKey = 0;
		bool g_pendingPackStreamed = false;

		// Fill a GuTexture from a baked pack entry: single (sprite) entries load one blob into t.data, paged
		// (tileset) entries get one pre-swizzled page allocation each.
		bool LoadPackTexture(GuTexture& t, const PspGpu::TexEntry& e, bool forceStreamed)
		{
			PspAssetPack& pack = PspAssetPack::Get();
			ReleaseGuTextureStorage(t);
			t.bpp = PspGpu::BytesPerTexel((PspGpu::PixelFormat)e.format);
			t.width = e.width; t.height = e.height;
			t.swizzled = (e.swizzled != 0);
			t.packEntry = &e;
			t.lowMemoryStreamed = PspIsLowMemoryModel() || forceStreamed;

			// A pre-coloured sprite samples through its OWN CLUT, so load it eagerly even when streamed: 1 KiB
			// against pixel blobs measured in megabytes, and the bind path then never touches the pack mid-frame.
			if ((e.flags & PspGpu::TextureFlagOwnClut) != 0) {
				t.packClut = static_cast<std::uint32_t*>(memalign(16, 256 * sizeof(std::uint32_t)));
				if (t.packClut != nullptr) {
					if (pack.ReadOwnClut(e, t.packClut, 256 * sizeof(std::uint32_t))) {
						// The GE reads this buffer directly; it is immutable from here on, so one writeback covers
						// every future bind.
						sceKernelDcacheWritebackRange(t.packClut, 256 * sizeof(std::uint32_t));
					} else {
						std::free(t.packClut);
						t.packClut = nullptr;
					}
				}
			}

			if (e.paged == 0) {
				t.stride = e.stride; t.strideH = e.strideH;
				t.oversized = false; t.pagesBuilt = false; t.spritePaged = false; t.pagesX = t.pagesY = 0;
				const std::size_t bytes = static_cast<std::size_t>(t.stride) * t.strideH * t.bpp;
				if (!t.lowMemoryStreamed) {
					t.data = memalign(16, bytes);
					if (t.data != nullptr) {
						if (!pack.ReadData(e, t.data, bytes)) std::memset(t.data, 0, bytes);
						sceKernelDcacheWritebackRange(t.data, bytes);
					}
				}
				return true;
			}

			// Paged tileset atlas.
			t.stride = PageStride; t.strideH = PageStride;
			t.oversized = true; t.pagesBuilt = true;
			t.spritePaged = (e.metaSize != 0);
			t.pagesX = e.pagesX; t.pagesY = e.pagesY;
			const std::size_t pageBytes = static_cast<std::size_t>(PageStride) * PageStride * t.bpp;
			const int numPages = e.pagesX * e.pagesY;
			// Only true tilesets compete for VRAM slots: the 15 spritePaged atlases are paged for size, not because
			// they are drawn everywhere, and letting one claim the pool pushes the terrain back into main RAM.
			const bool pinToVram = !t.spritePaged;
			for (int i = 0; i < numPages; ++i) {
				const PspGpu::TexPage* meta = pack.Page(e, i);
				GuPage page;
				if (meta != nullptr) { page.ox = meta->ox; page.oy = meta->oy; page.w = meta->w; page.h = meta->h; }
				if (!t.lowMemoryStreamed) {
					// Eager model: pages stay resident for the texture's whole life, so they are the best slot tenants.
					page.data = (pinToVram ? AllocPageStorage(pageBytes) : memalign(16, pageBytes));
					if (page.data != nullptr) {
						if (!pack.ReadPage(e, i, page.data, pageBytes)) std::memset(page.data, 0, pageBytes);
						sceKernelDcacheWritebackRange(page.data, pageBytes);
					}
				}
				t.pages.push_back(page);
			}
			return true;
		}

		std::size_t ResidentSpriteBytes(const GuTexture& texture)
		{
			if (!texture.lowMemoryStreamed || texture.packEntry == nullptr || texture.oversized) return 0;
			std::size_t bytes = 0;
			if (texture.data != nullptr) bytes += static_cast<std::size_t>(texture.stride) * texture.strideH * texture.bpp;
			// A pending read has already reserved its final buffer; count it so the budget can't overcommit.
			if (texture.pendingData != nullptr) bytes += static_cast<std::size_t>(texture.stride) * texture.strideH * texture.bpp;
			if (texture.rgba8888 != nullptr) bytes += static_cast<std::size_t>(texture.stride) * texture.strideH * 4;
			bytes += Rg8VariantBytes(texture);
			return bytes;
		}

		void ReleaseSpriteResidency(GuTexture& texture)
		{
			// A pending sprite batch holds its own texture pointer and would otherwise be flushed AFTER these pixels
			// are freed. Emit it while the data is valid, then drain: the caller's fence ran before this flush.
			if (g_spriteBatchTexture == &texture) {
				FlushSpriteBatch();
				PspGuWaitForPreviousFrame();
			}
			CancelStreamRequest(texture);
			std::free(texture.data);
			texture.data = nullptr;
			std::free(texture.rgba8888);
			texture.rgba8888 = nullptr;
			FreeRg8Variants(texture);
			if (g_boundTexture == &texture) InvalidateTextureBinding();
		}

		void TrimLowMemorySpriteTextures(std::size_t requiredBytes, const GuTexture* keep)
		{
			std::size_t residentBytes = 0;
			for (const auto& item : g_textures) residentBytes += ResidentSpriteBytes(item.second);
			if (residentBytes + requiredBytes <= LowMemorySpriteBytes) return;

			bool fenced = false;
			while (residentBytes + requiredBytes > LowMemorySpriteBytes) {
				GuTexture* oldest = nullptr;
				std::uint32_t oldestFrame = UINT32_MAX;
				for (auto& item : g_textures) {
					GuTexture& candidate = item.second;
					// Never evict a texture with a read in flight: the worker still owns pendingData, and blocking
					// on it to free safely would reintroduce the render-thread stall this path exists to remove.
					if (&candidate == keep || ResidentSpriteBytes(candidate) == 0 ||
						candidate.streamReq >= 0 || candidate.lastUsedFrame == g_frameSerial) continue;
					if (candidate.lastUsedFrame < oldestFrame) {
						oldest = &candidate;
						oldestFrame = candidate.lastUsedFrame;
					}
				}
				if (oldest == nullptr) break; // This frame's visible working set is allowed to exceed the soft cap.
				if (!fenced) {
					PspGuWaitForPreviousFrame();
					PspDiagnosticsRecordEvictionFence();
					fenced = true;
				}
				const std::size_t evicted = ResidentSpriteBytes(*oldest);
				residentBytes -= evicted;
				ReleaseSpriteResidency(*oldest);
				PspDiagnosticsRecordEviction(static_cast<std::uint32_t>(evicted));
			}
		}

		// Per-frame sweep that sheds non-paged sprite pixels once residency is genuinely under pressure.
		// TrimLowMemorySpriteTextures only fires on allocation, so without this the resident set on 01g sat far
		// above the soft cap with nothing allocating to trigger a trim. Three guards, all required:
		//  1. The background streamer must be running - eviction is only cheap if the pixels can come back off the
		//     render thread. It starts only on the low-memory model, but ContentResolver flags the player sheets
		//     `forceStreamed` on EVERY model, so on a PSP-2000/3000 dozens of textures carry `lowMemoryStreamed`
		//     with nothing to stream them; sweeping those produced a continuous evict/re-read cycle that fragmented
		//     the heap (~60 KB/s) and stalled the render thread.
		//  2. Resident bytes at or above ColdEvictHighWater; below that evicting buys nothing but churn.
		//  3. Not drawn for ColdEvictFrames, so the visible working set is never touched here.
		// Victims go coldest-first and the pass is bounded per frame, so one frame cannot stall on a long run.
		void EvictColdSprites()
		{
			if (!PspTextureStreamer::Get().IsRunning()) return;

			std::size_t residentBytes = 0;
			for (const auto& item : g_textures) residentBytes += ResidentSpriteBytes(item.second);
			if (residentBytes < ColdEvictHighWater) return;

			bool fenced = false;
			for (int n = 0; n < MaxColdEvictionsPerFrame && residentBytes > ColdEvictLowWater; n++) {
				GuTexture* coldest = nullptr;
				std::int32_t coldestAge = ColdEvictFrames;
				for (auto& item : g_textures) {
					GuTexture& t = item.second;
					// Never touch a texture with a read in flight: the worker still owns pendingData.
					if (!t.lowMemoryStreamed || t.oversized || t.data == nullptr || t.streamReq >= 0 || t.lastUsedFrame == 0)
						continue;
					// Signed diff so the g_frameSerial wrap (UINT32_MAX -> 1) stays correct.
					const std::int32_t age = (std::int32_t)(g_frameSerial - t.lastUsedFrame);
					if (age > coldestAge) {
						coldest = &t;
						coldestAge = age;
					}
				}
				if (coldest == nullptr) break; // Nothing cold enough; the live working set may hold the cap.
				if (!fenced) {
					PspGuWaitForPreviousFrame();
					PspDiagnosticsRecordEvictionFence();
					fenced = true;
				}
				const std::size_t freed = ResidentSpriteBytes(*coldest);
				residentBytes -= freed;
				ReleaseSpriteResidency(*coldest);
				PspDiagnosticsRecordEviction(static_cast<std::uint32_t>(freed));
			}
		}

		bool EnsurePackTextureResident(GuTexture& texture)
		{
			// Only non-paged textures explicitly deferred on 01g stream: eager and runtime-created textures are
			// already resident, paged atlases are handled per page by EnsurePageResident.
			if (!texture.lowMemoryStreamed) return texture.data != nullptr;
			if (texture.data != nullptr) {
				PspDiagnosticsRecordCacheHit();
				texture.lastUsedFrame = g_frameSerial;
				return true;
			}

			const std::size_t bytes = static_cast<std::size_t>(texture.stride) * texture.strideH * texture.bpp;
			PspTextureStreamer& streamer = PspTextureStreamer::Get();

			if (texture.streamReq >= 0) {
				if (!streamer.IsComplete(texture.streamReq)) {
					PspDiagnosticsRecordStreamPending();
					return false; // still reading; the sprite is skipped this frame (brief pop-in, not a hitch)
				}
				const bool ok = streamer.Succeeded(texture.streamReq);
				streamer.Release(texture.streamReq);
				texture.streamReq = -1;
				if (!ok) {
					std::free(texture.pendingData);
					texture.pendingData = nullptr;
					return false;
				}
				texture.data = texture.pendingData;
				texture.pendingData = nullptr;
				PspDiagnosticsRecordStreamRead(static_cast<std::uint32_t>(bytes));
				texture.lastUsedFrame = g_frameSerial;
				return true;
			}

			// Cold: reserve the final buffer here (evicting to stay in budget), then hand the seek+read to the
			// worker - no file I/O on the render thread.
			TrimLowMemorySpriteTextures(bytes, &texture);
			if (texture.pendingData == nullptr) texture.pendingData = memalign(16, bytes);
			if (texture.pendingData == nullptr) return false;

			texture.streamReq = streamer.Submit(texture.packEntry->dataOffset, texture.pendingData,
				static_cast<std::uint32_t>(bytes));
			if (texture.streamReq >= 0) {
				PspDiagnosticsRecordStreamSubmit();
				return false; // queued; becomes resident in a later frame
			}

			// Streamer stopped or its queue is full: fall back to a synchronous read so the sprite is never dropped.
			if (!PspAssetPack::Get().ReadData(*texture.packEntry, texture.pendingData, bytes)) {
				std::free(texture.pendingData);
				texture.pendingData = nullptr;
				return false;
			}
			sceKernelDcacheWritebackRange(texture.pendingData, bytes);
			texture.data = texture.pendingData;
			texture.pendingData = nullptr;
			PspDiagnosticsRecordStreamRead(static_cast<std::uint32_t>(bytes));
			texture.lastUsedFrame = g_frameSerial;
			return true;
		}

		bool EnsurePageResident(GuTexture& texture, int pageIndex)
		{
			if (pageIndex < 0 || pageIndex >= static_cast<int>(texture.pages.size())) return false;
			GuPage& requested = texture.pages[pageIndex];
			if (requested.data != nullptr) {
				requested.lastUsedFrame = g_frameSerial;
				PspDiagnosticsRecordCacheHit();
				return true;
			}
			if (texture.packEntry == nullptr) return false;

			const std::size_t pageBytes = static_cast<std::size_t>(PageStride) * PageStride * texture.bpp;
			const bool pinToVram = !texture.spritePaged;

			if (texture.lowMemoryStreamed) {
				int resident = 0;
				int oldestIndex = -1;
				std::uint32_t oldestFrame = UINT32_MAX;
				for (int i = 0; i < static_cast<int>(texture.pages.size()); ++i) {
					const GuPage& page = texture.pages[i];
					if (page.data == nullptr) continue;
					// A VRAM-pinned page costs no main RAM, so it neither counts against this heap cap nor is an
					// eviction candidate; counting them would make a fully pinned tileset evict itself on every
					// page fault, only to re-acquire the slot on the next draw.
					if (PspVramContains(page.data)) continue;
					++resident;
					// Never discard storage already referenced by the current GU list.
					if (page.lastUsedFrame != g_frameSerial && page.lastUsedFrame < oldestFrame) {
						oldestFrame = page.lastUsedFrame;
						oldestIndex = i;
					}
				}
				if (resident >= LowMemoryResidentPages && oldestIndex >= 0) {
					PspGuWaitForPreviousFrame();
					PspDiagnosticsRecordEvictionFence();
					FreePageStorage(texture.pages[oldestIndex].data);
					PspDiagnosticsRecordPageEviction(static_cast<std::uint32_t>(pageBytes));
				}
			}

			requested.data = (pinToVram ? AllocPageStorage(pageBytes) : memalign(16, pageBytes));
			if (requested.data == nullptr) return false;
			if (!PspAssetPack::Get().ReadPage(*texture.packEntry, pageIndex, requested.data, pageBytes)) {
				FreePageStorage(requested.data);
				return false;
			}
			sceKernelDcacheWritebackRange(requested.data, pageBytes);
			PspDiagnosticsRecordPageRead(static_cast<std::uint32_t>(pageBytes));
			requested.lastUsedFrame = g_frameSerial;
			return true;
		}

		bool DrawPagedWorldSprite(GuTexture& t, float sx0, float sy0, float sx1, float sy1,
			float width, float height, std::uint32_t color, const float* model, const Camera& camera,
			int paletteOffset)
		{
			const float minX = std::min(sx0, sx1), maxX = std::max(sx0, sx1);
			const float minY = std::min(sy0, sy1), maxY = std::max(sy0, sy1);
			int selectedIndex = -1;
			for (int i = 0; i < static_cast<int>(t.pages.size()); ++i) {
				const GuPage& page = t.pages[i];
				if (minX >= page.ox - 0.5f && maxX <= page.ox + page.w + 0.5f &&
					minY >= page.oy - 0.5f && maxY <= page.oy + page.h + 0.5f) {
					selectedIndex = i;
					break;
				}
			}
			if (!EnsurePageResident(t, selectedIndex)) return false;
			const GuPage* selected = &t.pages[selectedIndex];

			InvalidateTextureBinding();
			sceGuEnable(GU_TEXTURE_2D);
			sceGuTexWrap(GU_CLAMP, GU_CLAMP);
			if (t.bpp == 1) {
				EnsureTextureClutLoaded(t, paletteOffset);
				sceGuTexMode(GU_PSM_T8, 0, 0, t.swizzled ? 1 : 0);
				sceGuTexImage(0, PageStride, PageStride, PageStride, selected->data);
			} else if (t.bpp == 2) {
				ExpandRg8PageToRgba(t, *selected, paletteOffset);
				if (t.rgba8888 == nullptr) { sceGuDisable(GU_TEXTURE_2D); return false; }
				sceGuTexMode(GU_PSM_8888, 0, 0, 0);
				sceGuTexImage(0, PageStride, PageStride, PageStride, t.rgba8888);
			} else if (t.bpp == 4) {
				sceGuTexMode(GU_PSM_8888, 0, 0, t.swizzled ? 1 : 0);
				sceGuTexImage(0, PageStride, PageStride, PageStride, selected->data);
			} else {
				sceGuDisable(GU_TEXTURE_2D);
				return false;
			}
			PspDiagnosticsRecordTextureImage();
			sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
			sceGuTexFilter(GU_NEAREST, GU_NEAREST);
			sceGuTexScale(1.0f, 1.0f);
			sceGuTexOffset(0.0f, 0.0f);
			SetGuMatrix(GU_PROJECTION, camera.GetProjection().Data());
			SetGuMatrix(GU_VIEW, camera.GetView().Data());
			SetGuMatrix(GU_MODEL, model);

			struct Vertex { float u, v; std::uint32_t color; float x, y, z; };
			auto* out = static_cast<Vertex*>(GuAlloc(4 * sizeof(Vertex)));
			const float u0 = (sx0 - selected->ox) / PageStride, u1 = (sx1 - selected->ox) / PageStride;
			const float v0 = (sy0 - selected->oy) / PageStride, v1 = (sy1 - selected->oy) / PageStride;
			out[0] = { u0, v0, color, 0.0f, 0.0f, 0.0f };
			out[1] = { u1, v0, color, width, 0.0f, 0.0f };
			out[2] = { u0, v1, color, 0.0f, height, 0.0f };
			out[3] = { u1, v1, color, width, height, 0.0f };
			sceGuDrawArray(GU_TRIANGLE_STRIP,
				GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 4, nullptr, out);
			PspDiagnosticsRecordDraw(4);
			sceGuDisable(GU_TEXTURE_2D);
			return true;
		}

		bool DrawPagedUiSprite(GuTexture& t, float sx0, float sy0, float sx1, float sy1,
			std::uint32_t color, std::int16_t x0, std::int16_t y0, std::int16_t x1, std::int16_t y1,
			std::int16_t x2, std::int16_t y2, std::int16_t x3, std::int16_t y3, int paletteOffset)
		{
			const float minX = std::min(sx0, sx1), maxX = std::max(sx0, sx1);
			const float minY = std::min(sy0, sy1), maxY = std::max(sy0, sy1);
			int selectedIndex = -1;
			for (int i = 0; i < static_cast<int>(t.pages.size()); ++i) {
				const GuPage& page = t.pages[i];
				if (minX >= page.ox - 0.5f && maxX <= page.ox + page.w + 0.5f &&
					minY >= page.oy - 0.5f && maxY <= page.oy + page.h + 0.5f) {
					selectedIndex = i;
					break;
				}
			}
			if (!EnsurePageResident(t, selectedIndex)) return false;
			const GuPage* selected = &t.pages[selectedIndex];
			InvalidateTextureBinding();
			sceGuEnable(GU_TEXTURE_2D);
			sceGuTexWrap(GU_CLAMP, GU_CLAMP);
			if (t.bpp == 1) {
				EnsureTextureClutLoaded(t, paletteOffset);
				sceGuTexMode(GU_PSM_T8, 0, 0, t.swizzled ? 1 : 0);
				sceGuTexImage(0, PageStride, PageStride, PageStride, selected->data);
			} else if (t.bpp == 2) {
				ExpandRg8PageToRgba(t, *selected, paletteOffset);
				if (t.rgba8888 == nullptr) { sceGuDisable(GU_TEXTURE_2D); return false; }
				sceGuTexMode(GU_PSM_8888, 0, 0, 0);
				sceGuTexImage(0, PageStride, PageStride, PageStride, t.rgba8888);
			} else if (t.bpp == 4) {
				sceGuTexMode(GU_PSM_8888, 0, 0, t.swizzled ? 1 : 0);
				sceGuTexImage(0, PageStride, PageStride, PageStride, selected->data);
			} else {
				sceGuDisable(GU_TEXTURE_2D);
				return false;
			}
			PspDiagnosticsRecordTextureImage();
			sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
			sceGuTexFilter(GU_NEAREST, GU_NEAREST);
			sceGuTexScale(1.0f, 1.0f);
			sceGuTexOffset(0.0f, 0.0f);

			struct Vertex { std::int16_t u, v; std::uint32_t color; std::int16_t x, y, z; };
			auto* out = static_cast<Vertex*>(GuAlloc(4 * sizeof(Vertex)));
			const std::int16_t u0 = (std::int16_t)(sx0 - selected->ox), u1 = (std::int16_t)(sx1 - selected->ox);
			const std::int16_t v0 = (std::int16_t)(sy0 - selected->oy), v1 = (std::int16_t)(sy1 - selected->oy);
			out[0] = { u0, v0, color, x0, y0, 0 };
			out[1] = { u1, v0, color, x1, y1, 0 };
			out[2] = { u0, v1, color, x2, y2, 0 };
			out[3] = { u1, v1, color, x3, y3, 0 };
			sceGuDrawArray(GU_TRIANGLE_STRIP,
				GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, nullptr, out);
			PspDiagnosticsRecordDraw(4);
			sceGuDisable(GU_TEXTURE_2D);
			return true;
		}

		// Whether a tile mesh can swap blending for a plain alpha test this frame.
		bool CanUseOpaqueTilePath(const GuTexture& t, const float* verts, int numVertices, unsigned fpv)
		{
			if (t.bpp != 1 || (numVertices % 6) != 0) return false;

			// A tile's six vertices share one alpha, so checking the first avoids another full vertex pass.
			for (int i = 0; i < numVertices; i += 6) {
				if (verts[static_cast<std::size_t>(i) * fpv + 7] < 1.0f) return false;
			}

			// Alpha testing can replace blending only for a binary-alpha palette, and palette cycling is live, so
			// evaluate the current row each draw rather than assuming a level keeps the default alpha values.
			auto palettes = Jazz2::ContentResolver::Get().GetPalettes();
			if (palettes.size() < 256) return false;
			for (int i = 0; i < 256; ++i) {
				const std::uint32_t alpha = palettes[i] >> 24;
				if (alpha != 0 && alpha != 255) return false;
			}
			return true;
		}

		// Draw an 8-float, 6-verts-per-tile GL_TRIANGLES mesh against a paged atlas: each tile is binned to the one
		// page containing its cell, then one GU draw is issued per page.
		void DrawPagedMesh(GuTexture& t, const float* verts, int numVertices, unsigned fpv, bool opaqueFastPath, int fixedPage = -1)
		{
			if (!t.pagesBuilt) return; // pages come pre-baked from the pack; nothing to draw if absent
			const int numPages = t.pagesX * t.pagesY;
			const int tiles = numVertices / 6;
			if (numPages <= 0 || tiles <= 0) return;
			InvalidateTextureBinding();

			auto ch = [](float v) -> std::uint32_t { return std::uint32_t((v < 0 ? 0 : (v > 1 ? 1 : v)) * 255.0f); };

			static std::vector<int> tilePage;
			static std::vector<int> pageVerts;
			pageVerts.assign(numPages, 0);
			if (fixedPage >= 0 && fixedPage < numPages) {
				pageVerts[fixedPage] = numVertices;
			} else {
				fixedPage = -1;
				tilePage.resize(tiles);
				for (int ti = 0; ti < tiles; ++ti) {
					const float* s = verts + static_cast<std::size_t>(ti * 6) * fpv;
					int px = int(s[2] * t.width) / PageTexels;
					int py = int(s[3] * t.height) / PageTexels;
					if (px < 0) px = 0; if (px >= t.pagesX) px = t.pagesX - 1;
					if (py < 0) py = 0; if (py >= t.pagesY) py = t.pagesY - 1;
					const int pi = py * t.pagesX + px;
					tilePage[ti] = pi;
					pageVerts[pi] += 6;
				}
			}

			const int swz = (t.swizzled ? 1 : 0);
			sceGuEnable(GU_TEXTURE_2D);
			sceGuTexWrap(GU_CLAMP, GU_CLAMP);
			if (t.bpp == 1) {
				EnsureClutLoaded();
				sceGuTexMode(GU_PSM_T8, 0, 0, swz);
			} else {
				sceGuTexMode(GU_PSM_8888, 0, 0, swz);
			}
			sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
			sceGuTexFilter(GU_NEAREST, GU_NEAREST);
			sceGuTexScale(1.0f, 1.0f);
			sceGuTexOffset(0.0f, 0.0f);
			if (opaqueFastPath) {
				sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
				sceGuEnable(GU_ALPHA_TEST);
				sceGuDisable(GU_BLEND);
			}

			struct TexVertex { float u, v; std::uint32_t color; float x, y, z; };
			for (int pi = 0; pi < numPages; ++pi) {
				if (pageVerts[pi] == 0 || !EnsurePageResident(t, pi)) continue;
				const GuPage& pg = t.pages[pi];
				auto* out = static_cast<TexVertex*>(GuAlloc(pageVerts[pi] * sizeof(TexVertex)));
				int w = 0;
				for (int ti = 0; ti < tiles; ++ti) {
					if (fixedPage < 0 && tilePage[ti] != pi) continue;
					for (int k = 0; k < 6; ++k) {
						const float* s = verts + static_cast<std::size_t>(ti * 6 + k) * fpv;
						out[w].x = s[0]; out[w].y = s[1]; out[w].z = 0.0f;
						// Integer texcoords are texel units only in GU_TRANSFORM_2D; in 3D they are normalized, so
						// emit explicit normalized floats for the page-local texels.
						out[w].u = float(int(s[2] * t.width) - pg.ox) / float(PageStride);
						out[w].v = float(int(s[3] * t.height) - pg.oy) / float(PageStride);
						out[w].color = (ch(s[7]) << 24) | (ch(s[6]) << 16) | (ch(s[5]) << 8) | ch(s[4]);
						w++;
					}
				}
				sceGuTexImage(0, PageStride, PageStride, PageStride, pg.data);
				PspDiagnosticsRecordTextureImage();
				sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
					pageVerts[pi], nullptr, out);
				PspDiagnosticsRecordDraw(pageVerts[pi]);
			}
			if (opaqueFastPath) {
				sceGuEnable(GU_BLEND);
				sceGuDisable(GU_ALPHA_TEST);
			}
			sceGuDisable(GU_TEXTURE_2D);
		}
	}

	// Called by ContentResolver::RequestTileSet just before it creates a tileset's placeholder Texture: the next
	// texture allocation is served straight from the baked pack instead of a runtime upload.
	void PspSetPendingPackTexture(std::uint64_t nameHash, bool streamed)
	{
		g_pendingPackKey = nameHash;
		g_pendingPackStreamed = streamed;
	}

	// Called from GLTexture::TexImage2D on PSP; captures the upload into a GU texture keyed by `key`.
	void PspCaptureTexture(const GLTexture* key, int width, int height, unsigned int format, unsigned int type, const void* data)
	{
		(void)type;
		if (key == nullptr || width <= 0 || height <= 0) return;
		PspGuWaitForPreviousFrame();
		ClearSpriteBatch();
		InvalidateTextureBinding();

		// Baked tileset: fill straight from the pre-swizzled/paged pack entry and skip the runtime pad/upload path.
		if (g_pendingPackKey != 0) {
			const std::uint64_t k = g_pendingPackKey;
			const bool streamed = g_pendingPackStreamed;
			g_pendingPackKey = 0;
			g_pendingPackStreamed = false;
			const PspGpu::TexEntry* e = PspAssetPack::Get().Find(k);
			if (e != nullptr) {
				GuTexture& tex = g_textures[key];
				LoadPackTexture(tex, *e, streamed);
				return;
			}
		}

		const int bpp = BppForFormat(format);
		GuTexture& tex = g_textures[key];
		const int stride = NextPow2(width);
		const int strideH = NextPow2(height);
		if (tex.data == nullptr || tex.stride != stride || tex.strideH != strideH || tex.bpp != bpp) {
			ReleaseGuTextureStorage(tex);
			tex.data = memalign(16, static_cast<std::size_t>(stride) * strideH * bpp);
			tex.stride = stride;
			tex.strideH = strideH;
			tex.bpp = bpp;
		}
		tex.width = width;
		tex.height = height;
		tex.swizzled = false; // Runtime GL uploads arrive in linear row-major order.
		tex.oversized = (stride > GuMaxTexDim || strideH > GuMaxTexDim);
		tex.pagesBuilt = false;
		InvalidateRg8Variants(tex);  // data is about to be overwritten; re-bake RG8 on next bind
		if (tex.data == nullptr) return;

		std::memset(tex.data, 0, static_cast<std::size_t>(stride) * strideH * bpp);
		if (data != nullptr) {
			const auto* src = static_cast<const std::uint8_t*>(data);
			auto* dst = static_cast<std::uint8_t*>(tex.data);
			for (int y = 0; y < height; ++y) {
				std::memcpy(dst + static_cast<std::size_t>(y) * stride * bpp, src + static_cast<std::size_t>(y) * width * bpp,
					static_cast<std::size_t>(width) * bpp);
			}
		}
		sceKernelDcacheWritebackRange(tex.data, static_cast<std::size_t>(stride) * strideH * bpp);
	}

	// Called from GLTexture::TexSubImage2D on PSP. The engine typically allocates via TexImage2D(null) then
	// fills pixels via TexSubImage2D, so this is where the real pixels arrive.
	void PspUpdateSubTexture(const GLTexture* key, int x, int y, int width, int height, const void* data)
	{
		auto it = g_textures.find(key);
		if (it == g_textures.end() || it->second.data == nullptr || data == nullptr) return;
		PspGuWaitForPreviousFrame();
		ClearSpriteBatch();
		InvalidateTextureBinding();
		GuTexture& t = it->second;
		t.pagesBuilt = false;
		InvalidateRg8Variants(t);  // data changed; re-bake RG8 on next bind
		const int bpp = t.bpp;
		auto* dst = static_cast<std::uint8_t*>(t.data);
		const auto* src = static_cast<const std::uint8_t*>(data);
		for (int row = 0; row < height; ++row) {
			const int dy = y + row;
			if (dy < 0 || dy >= t.strideH || x < 0 || x + width > t.stride) continue;
			std::memcpy(dst + (static_cast<std::size_t>(dy) * t.stride + x) * bpp, src + static_cast<std::size_t>(row) * width * bpp,
				static_cast<std::size_t>(width) * bpp);
		}
		sceKernelDcacheWritebackRange(t.data, static_cast<std::size_t>(t.stride) * t.strideH * bpp);
	}

	void PspReleaseTexture(const GLTexture* key)
	{
		auto it = g_textures.find(key);
		if (it != g_textures.end()) {
			PspGuWaitForPreviousFrame();
			if (g_spriteBatchTexture == &it->second) ClearSpriteBatch();
			if (g_boundTexture == &it->second) InvalidateTextureBinding();
			ReleaseGuTextureStorage(it->second);
			g_textures.erase(it);
		}
	}

	void PspEmitCollectTextureMemory(PspMemoryReport& report)
	{
		// Called at most once a second by the diagnostics sampler; sums the resident-byte breakdown the streaming
		// budget is measured against.
		for (const auto& item : g_textures) {
			const GuTexture& t = item.second;
			report.texCount++;
			if (t.lowMemoryStreamed) report.texStreamed++;
			if (t.oversized) {
				report.texPagedAtlases++;
				const std::size_t pageBytes = static_cast<std::size_t>(PageStride) * PageStride * t.bpp;
				for (const GuPage& page : t.pages) {
					if (page.data != nullptr) {
						report.texResidentPages++;
						// VRAM-pinned pages occupy no main RAM, and this report is the heap accounting the streaming
						// budget reads, so counting them would overstate pressure by up to the whole pool.
						if (!PspVramContains(page.data)) report.texPagePixels += static_cast<std::uint32_t>(pageBytes);
					}
				}
				if (t.rgba8888 != nullptr) {
					report.texVariantBytes += static_cast<std::uint32_t>(static_cast<std::size_t>(PageStride) * PageStride * 4);
				}
			} else {
				if (t.data != nullptr) {
					report.texResidentSprites++;
					report.texSpritePixels += static_cast<std::uint32_t>(static_cast<std::size_t>(t.stride) * t.strideH * t.bpp);
				}
				if (t.rgba8888 != nullptr) {
					report.texVariantBytes += static_cast<std::uint32_t>(static_cast<std::size_t>(t.stride) * t.strideH * 4);
				}
				report.texVariantBytes += static_cast<std::uint32_t>(Rg8VariantBytes(t));
			}
		}
		report.texResidentTotal = report.texSpritePixels + report.texPagePixels + report.texVariantBytes;
		report.streamerRunning = PspTextureStreamer::Get().IsRunning() ? 1 : 0;
		report.guListBytes = (std::uint32_t)g_guListBytesThisFrame;
		report.guListHighWater = (std::uint32_t)g_guListHighWater;
		report.guListCapacity = (std::uint32_t)PspGuDisplayListBytes();
		report.guListOverflowFrames = g_guListOverflowFrames;
		report.guListDroppedCommands = g_guListDroppedCommands;
	}

	void PspForceResident(const void* key)
	{
		auto it = g_textures.find(static_cast<const GLTexture*>(key));
		if (it == g_textures.end()) return;
		GuTexture& t = it->second;
		// Only the non-paged streamed path defers pixels; eager textures and paged atlases are already handled.
		if (!t.lowMemoryStreamed || t.oversized || t.data != nullptr) return;
		const std::size_t bytes = static_cast<std::size_t>(t.stride) * t.strideH * t.bpp;
		CancelStreamRequest(t); // we are loading it right now, so drop any read in flight
		TrimLowMemorySpriteTextures(bytes, &t);
		t.data = memalign(16, bytes);
		if (t.data == nullptr) return;
		if (!PspAssetPack::Get().ReadData(*t.packEntry, t.data, bytes)) {
			std::free(t.data);
			t.data = nullptr;
			return;
		}
		sceKernelDcacheWritebackRange(t.data, bytes);
		t.lastUsedFrame = g_frameSerial;
	}

	void PspRequestResident(const void* key)
	{
		if (key == nullptr) return;
		auto it = g_textures.find(static_cast<const GLTexture*>(key));
		if (it == g_textures.end()) return;
		GuTexture& t = it->second;
		if (!t.lowMemoryStreamed || t.oversized || t.data != nullptr) return;
		// Drives the async state machine (cold -> reserve+submit, in-flight -> poll & promote); readiness is observed
		// separately via PspIsResident. Bump lastUsedFrame so a deliberate preload isn't evicted while it is awaited.
		EnsurePackTextureResident(t);
		t.lastUsedFrame = g_frameSerial;
	}

	bool PspIsResident(const void* key)
	{
		if (key == nullptr) return true;
		auto it = g_textures.find(static_cast<const GLTexture*>(key));
		if (it == g_textures.end()) return true; // not captured yet -> nothing to hold for; don't defer
		const GuTexture& t = it->second;
		// Only a non-paged streamed sprite can be mid-stream. Eager textures and paged atlases (their pages stream
		// at draw time) report ready, so the actor layer never gets stuck holding for them.
		if (!t.lowMemoryStreamed || t.oversized) return true;
		return t.data != nullptr;
	}

	void PspReleaseRendererCaches()
	{
		PspGuWaitForPreviousFrame();
		ClearSpriteBatch();
		InvalidateTextureBinding();
		for (auto& item : g_textures) ReleaseGuTextureStorage(item.second);
		g_textures.clear();
		g_textures.rehash(0);
		std::vector<WorldBatchVertex>().swap(g_worldBatchVertices);
		std::vector<UiBatchVertex>().swap(g_uiBatchVertices);
		std::vector<std::uint16_t>().swap(g_spriteBatchIndices);
		g_pendingPackKey = 0;
		g_pendingPackStreamed = false;
		g_pvCamera = nullptr;
	}

	namespace
	{
		// GU primitive for a GL primitive; -1 for anything the mesh path can't draw.
		int GuPrimitive(unsigned int glPrim)
		{
			switch (glPrim) {
				case GL_TRIANGLES: return GU_TRIANGLES;
				case GL_TRIANGLE_STRIP: return GU_TRIANGLE_STRIP;
				default: return -1; // GL_TRIANGLE_FAN isn't in the GL shim and isn't used by the mesh paths
			}
		}
	}

	// Host-vertex mesh path (TileMap::EmitLayerMesh layers and MeshSprite): real CPU vertices, so no InstanceBlock
	// reconstruction. Layout is 8 floats - position.xy (world), texcoord.uv (normalized), color.rgba - transformed
	// by proj*view*model on the CPU and drawn in screen space (GU_TRANSFORM_2D). The per-vertex color carries each
	// tile's alpha; the layer tint lives in the (dead-on-PSP) InstanceBlock color and is not applied yet.
	static bool PspEmitMesh(RenderCommand& command)
	{
		FlushSpriteBatch();
		const Geometry& geom = command.GetGeometry();
		const float* verts = geom.GetHostVertexPointer();
		const int numVertices = geom.GetVertexCount();
		const unsigned int floatsPerVertex = geom.GetElementsPerVertex();
		if (verts == nullptr || numVertices <= 0) return false;
		// Other layouts (e.g. MeshSprite's local-space Vertex format) are skipped rather than mis-drawn.
		if (floatsPerVertex != 8) return false;

		const int guPrim = GuPrimitive(geom.GetPrimitiveType());
		if (guPrim < 0) return false;
		PspDiagnosticsRecordMeshCommand(static_cast<std::uint32_t>(numVertices));

		// Skip absurdly large meshes rather than overflow the shared display list and hang; this bounds how far one
		// command can overshoot the budget checked in PspEmitCommand (see GuListReserveBytes).
		if ((std::size_t)numVertices * 24u > MaxMeshBytes()) return false;

		const Camera* camera = RenderResources::GetCurrentCamera();
		if (camera == nullptr) return false;

		Material& material = command.GetMaterial();
		const GLTexture* texKey = material.GetTexture(0);
		auto texIt = (texKey != nullptr ? g_textures.find(texKey) : g_textures.end());
		GuTexture* gt = (texIt != g_textures.end() ? &texIt->second : nullptr);

		// Baked tileset atlas: one GU draw per 512x512 pre-swizzled page.
		if (gt != nullptr && gt->oversized && gt->pagesBuilt && (gt->bpp == 1 || gt->bpp == 4) &&
			guPrim == GU_TRIANGLES && (numVertices % 6) == 0) {
			SetGuMatrix(GU_PROJECTION, camera->GetProjection().Data());
			SetGuMatrix(GU_VIEW, camera->GetView().Data());
			SetGuMatrix(GU_MODEL, command.GetTransformation().Data());
			DrawPagedMesh(*gt, verts, numVertices, floatsPerVertex,
				CanUseOpaqueTilePath(*gt, verts, numVertices, floatsPerVertex), command.pspTexturePage_);
			return true;
		}

		float mvp[16];
		Mat4Mul(ProjectionView(*camera), command.GetTransformation().Data(), mvp);

		const bool textured = (gt != nullptr && BindGuTexture(*gt, (int)command.pspPaletteOffset_));

		auto ch = [](float v) -> std::uint32_t { return std::uint32_t((v < 0 ? 0 : (v > 1 ? 1 : v)) * 255.0f); };

		if (textured) {
			const GuTexture& t = texIt->second;
			struct TexVertex { std::int16_t u, v; std::uint32_t color; std::int16_t x, y, z; };
			auto* out = static_cast<TexVertex*>(GuAlloc(numVertices * sizeof(TexVertex)));
			for (int i = 0; i < numVertices; ++i) {
				const float* s = verts + i * floatsPerVertex;
				const float lx = s[0], ly = s[1];
				const float cx = mvp[0] * lx + mvp[4] * ly + mvp[12];
				const float cy = mvp[1] * lx + mvp[5] * ly + mvp[13];
				const float cw = mvp[3] * lx + mvp[7] * ly + mvp[15];
				const float inv = (cw != 0.0f ? 1.0f / cw : 1.0f);
				out[i].x = std::int16_t((cx * inv * 0.5f + 0.5f) * 480.0f);
				out[i].y = std::int16_t((1.0f - (cy * inv * 0.5f + 0.5f)) * 272.0f);
				out[i].z = 0;
				out[i].u = std::int16_t(s[2] * t.width);
				out[i].v = std::int16_t(s[3] * t.height);
				out[i].color = (ch(s[7]) << 24) | (ch(s[6]) << 16) | (ch(s[5]) << 8) | ch(s[4]);
			}
			sceGuDrawArray(guPrim, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
				numVertices, nullptr, out);
			PspDiagnosticsRecordDraw(numVertices);
			sceGuDisable(GU_TEXTURE_2D);
		} else {
			struct ColorVertex { std::uint32_t color; std::int16_t x, y, z; };
			sceGuDisable(GU_TEXTURE_2D);
			auto* out = static_cast<ColorVertex*>(GuAlloc(numVertices * sizeof(ColorVertex)));
			for (int i = 0; i < numVertices; ++i) {
				const float* s = verts + i * floatsPerVertex;
				const float lx = s[0], ly = s[1];
				const float cx = mvp[0] * lx + mvp[4] * ly + mvp[12];
				const float cy = mvp[1] * lx + mvp[5] * ly + mvp[13];
				const float cw = mvp[3] * lx + mvp[7] * ly + mvp[15];
				const float inv = (cw != 0.0f ? 1.0f / cw : 1.0f);
				out[i].x = std::int16_t((cx * inv * 0.5f + 0.5f) * 480.0f);
				out[i].y = std::int16_t((1.0f - (cy * inv * 0.5f + 0.5f)) * 272.0f);
				out[i].z = 0;
				out[i].color = (ch(s[7]) << 24) | (ch(s[6]) << 16) | (ch(s[5]) << 8) | ch(s[4]);
			}
			sceGuDrawArray(guPrim, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, numVertices, nullptr, out);
			PspDiagnosticsRecordDraw(numVertices);
		}
		return true;
	}

	void PspEmitCommand(RenderCommand& command)
	{
		// Hard stop once the display list budget is spent: dropping the tail of a frame costs a few missing sprites,
		// while running past the end of the list corrupts whatever heap block follows it.
		if (GuListExhausted()) {
			g_guListDroppedCommands++;
			return;
		}
		// Meshes carry real CPU vertices, sprites don't (their quad comes from the InstanceBlock).
		if (command.GetGeometry().GetHostVertexPointer() != nullptr) {
			PspEmitMesh(command);
			return;
		}
		if (!command.pspHasSprite_) return; // Nothing to draw through either path
		PspDiagnosticsRecordSpriteCommand();

		Material& material = command.GetMaterial();
		const float* model = command.GetTransformation().Data();
		const float* size = command.pspSpriteSize_;
		const float* colorF = command.pspColor_;
		const float* texRect = command.pspTexRect_;

		// UI (HUD/menu) commands are already in screen-pixel space and need no scene camera.
		const bool uiSpace = command.pspUiSpace_;
		const Camera* camera = RenderResources::GetCurrentCamera();
		if (camera == nullptr && !uiSpace) return;

		std::uint32_t color = 0xffffffff;
		{
			auto ch = [](float v) -> std::uint32_t { return std::uint32_t((v < 0 ? 0 : (v > 1 ? 1 : v)) * 255.0f); };
			if (command.pspFontRainbow_) {
				// Match the Colorized shader's dye at the vertex: the neutral font CLUT supplies luminance and
				// coverage once per batch, so each glyph can carry its own color without another palette upload.
				const float dyeR = 1.0f + (colorF[0] - 0.5f) * 4.0f;
				const float dyeG = 1.0f + (colorF[1] - 0.5f) * 4.0f;
				const float dyeB = 1.0f + (colorF[2] - 0.5f) * 4.0f;
				color = (ch(colorF[3]) << 24) | (ch(dyeB) << 16) | (ch(dyeG) << 8) | ch(dyeR);
			} else {
				color = (ch(colorF[3]) << 24) | (ch(colorF[2]) << 16) | (ch(colorF[1]) << 8) | ch(colorF[0]);
			}
		}

		const float rx = texRect[0], ry = texRect[1], rz = texRect[2], rw = texRect[3];

		// Scene sprites keep their local quad and let the GE apply proj/view/model. Texcoords must normalize against
		// the padded GU allocation (stride/strideH), not the logical image size.
		if (!uiSpace) {
			const GLTexture* texKey = material.GetTexture(0);
			auto texIt = (texKey != nullptr ? g_textures.find(texKey) : g_textures.end());
			const bool tiled = (rx > 1.5f || rz > 1.5f);
			if (texIt != g_textures.end() && texIt->second.oversized && texIt->second.pagesBuilt &&
				texIt->second.spritePaged) {
				FlushSpriteBatch();
				GuTexture& t = texIt->second;
				DrawPagedWorldSprite(t, ry * t.width, rw * t.height, (rx + ry) * t.width,
					(rz + rw) * t.height, size[0], size[1], color, model, *camera,
					(int)command.pspPaletteOffset_);
				return;
			}
			const bool textured = (texIt != g_textures.end() &&
				texIt->second.stride <= GuMaxTexDim && texIt->second.strideH <= GuMaxTexDim &&
				(texIt->second.data != nullptr || EnsurePackTextureResident(texIt->second)));

			if (textured) {
				GuTexture& t = texIt->second;
				PrepareSpriteBatch(t, (int)command.pspPaletteOffset_, tiled, false, camera);
				const float texScaleX = float(t.width) / float(t.stride);
				const float texScaleY = float(t.height) / float(t.strideH);
				const float u0 = ry * texScaleX;
				const float u1 = (rx + ry) * texScaleX;
				const float v0 = rw * texScaleY;
				const float v1 = (rz + rw) * texScaleY;
				const std::size_t firstVertex = g_worldBatchVertices.size();
				auto append = [&](float u, float v, float lx, float ly) {
					g_worldBatchVertices.push_back(WorldBatchVertex{ u, v, color,
						model[0] * lx + model[4] * ly + model[12],
						model[1] * lx + model[5] * ly + model[13],
						model[2] * lx + model[6] * ly + model[14] });
				};
				append(u0, v0, 0.0f, 0.0f);
				append(u1, v0, size[0], 0.0f);
				append(u0, v1, 0.0f, size[1]);
				append(u1, v1, size[0], size[1]);
				AppendSpriteBatchIndices(firstVertex);
			} else if (texKey != nullptr) {
				// Pixels still streaming in (or failed): skip this frame. Falling through to the untextured quad
				// below would flash the sprite as a solid white square.
			} else {
				FlushSpriteBatch();
				SetGuMatrix(GU_PROJECTION, camera->GetProjection().Data());
				SetGuMatrix(GU_VIEW, camera->GetView().Data());
				SetGuMatrix(GU_MODEL, model);
				struct ColorVertex { std::uint32_t color; float x, y, z; };
				sceGuDisable(GU_TEXTURE_2D);
				auto* v = static_cast<ColorVertex*>(GuAlloc(4 * sizeof(ColorVertex)));
				v[0] = { color, 0.0f, 0.0f, 0.0f };
				v[1] = { color, size[0], 0.0f, 0.0f };
				v[2] = { color, 0.0f, size[1], 0.0f };
				v[3] = { color, size[0], size[1], 0.0f };
				sceGuDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, 4, nullptr, v);
				PspDiagnosticsRecordDraw(4);
			}
			return;
		}

		// UI model matrices map local coordinates straight to the framebuffer (0..480 x 0..272, y-down); the integer
		// GU_TRANSFORM_2D path keeps text crisp.
		auto toScreen = [&](float lx, float ly, std::int16_t& sx, std::int16_t& sy) {
			sx = std::int16_t(model[0] * lx + model[4] * ly + model[12]);
			sy = std::int16_t(model[1] * lx + model[5] * ly + model[13]);
		};
		// All four corners go through the model matrix so rotating menu layers stay proper quads.
		std::int16_t x0, y0, x1, y1, x2, y2, x3, y3;
		toScreen(0.0f, 0.0f, x0, y0);         // top-left
		toScreen(size[0], 0.0f, x1, y1);      // top-right
		toScreen(0.0f, size[1], x2, y2);      // bottom-left
		toScreen(size[0], size[1], x3, y3);   // bottom-right

		const GLTexture* texKey = material.GetTexture(0);
		auto texIt = (texKey != nullptr ? g_textures.find(texKey) : g_textures.end());
		// texRect scale > 1 means the caller wants the texture tiled across the quad.
		const bool tiled = (rx > 1.5f || rz > 1.5f);
		if (texIt != g_textures.end() && texIt->second.oversized && texIt->second.pagesBuilt &&
			texIt->second.spritePaged) {
			FlushSpriteBatch();
			GuTexture& t = texIt->second;
			DrawPagedUiSprite(t, ry * t.width, rw * t.height, (rx + ry) * t.width, (rz + rw) * t.height,
				color, x0, y0, x1, y1, x2, y2, x3, y3, (int)command.pspPaletteOffset_);
			return;
		}
		const bool textured = (texIt != g_textures.end() &&
			texIt->second.stride <= GuMaxTexDim && texIt->second.strideH <= GuMaxTexDim &&
			(texIt->second.data != nullptr || EnsurePackTextureResident(texIt->second)));

		if (textured) {
			GuTexture& t = texIt->second;
			if (command.pspFontColorized_ && !command.pspFontRainbow_ && t.bpp == 1) {
				// Reproduce Jazz2's Colorized fragment shader: the font's T8 index stores coverage in the high
				// nibble and shader grayscale (0..1.5) in the low nibble, and a per-glyph CLUT applies
				// dye = 1 + (color - 0.5) * 4, retaining the original white shine.
				FlushSpriteBatch();
				auto byte = [](float value) -> std::uint32_t {
					return (std::uint32_t)(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
				};
				const float dyeR = 1.0f + (colorF[0] - 0.5f) * 4.0f;
				const float dyeG = 1.0f + (colorF[1] - 0.5f) * 4.0f;
				const float dyeB = 1.0f + (colorF[2] - 0.5f) * 4.0f;
				if (!g_fontClutValid || g_fontClutColor != color) {
					for (int i = 0; i < 256; i++) {
						const float coverage = float(i >> 4) / 15.0f;
						// Low nibble is grayscale*10 (desktop range 0..1.5). Render at 0.85 of desktop exposure:
						// dim enough that mid-tones keep their saturated hue instead of washing out, while the
						// brightest source pixels still clip into the characteristic white shine.
						const float gray = float(i & 15) * (0.85f / 10.0f);
						g_clut[i] = (byte(coverage * std::clamp(colorF[3], 0.0f, 1.0f)) << 24) |
							(byte(gray * dyeB) << 16) | (byte(gray * dyeG) << 8) | byte(gray * dyeR);
					}
					sceKernelDcacheWritebackRange(g_clut, sizeof(g_clut));
					sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
					sceGuClutLoad(256 / 8, g_clut);
					PspDiagnosticsRecordClutLoad();
					g_fontClutValid = true;
					g_fontClutColor = color;
				}
				sceGuEnable(GU_TEXTURE_2D);
				sceGuTexWrap(GU_CLAMP, GU_CLAMP);
				sceGuTexMode(GU_PSM_T8, 0, 0, t.swizzled ? 1 : 0);
				sceGuTexImage(0, t.stride, t.strideH, t.stride, t.data);
				PspDiagnosticsRecordTextureImage();
				sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
				sceGuTexFilter(GU_NEAREST, GU_NEAREST);
				sceGuTexScale(1.0f, 1.0f);
				sceGuTexOffset(0.0f, 0.0f);

				const std::int16_t u0 = std::int16_t(ry * t.width);
				const std::int16_t u1 = std::int16_t((rx + ry) * t.width);
				const std::int16_t v0 = std::int16_t(rw * t.height);
				const std::int16_t v1 = std::int16_t((rz + rw) * t.height);
				auto* vertices = static_cast<UiBatchVertex*>(GuAlloc(4 * sizeof(UiBatchVertex)));
				vertices[0] = { u0, v0, 0xffffffff, x0, y0, 0 };
				vertices[1] = { u1, v0, 0xffffffff, x1, y1, 0 };
				vertices[2] = { u0, v1, 0xffffffff, x2, y2, 0 };
				vertices[3] = { u1, v1, 0xffffffff, x3, y3, 0 };
				sceGuDrawArray(GU_TRIANGLE_STRIP,
					GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, nullptr, vertices);
				PspDiagnosticsRecordDraw(4);
				sceGuDisable(GU_TEXTURE_2D);
				g_clutLoadedThisFrame = false; // The custom font CLUT replaced the live game palette.
				InvalidateTextureBinding();
				return;
			}
			PrepareSpriteBatch(t, (int)command.pspPaletteOffset_, tiled, true, nullptr, command.pspFontRainbow_);
			// texRect = (scaleX, biasX, scaleY, biasY), as in the desktop sprite shader: in texels the source is
			// x0 = biasX*texW, x1 = (biasX+scaleX)*texW (likewise Y). Always a valid rect - BaseSprite defaults
			// scale to 1 / bias to 0 before the texture size is known.
			//
			// Flipped sprites (flippedX_/flippedY_) arrive with a NEGATIVE scale, so x1 < x0. The GU samples from
			// the two given corners, which mirrors the texture for free - do NOT sort them.
			const std::int16_t u0 = std::int16_t(ry * t.width);
			const std::int16_t u1 = std::int16_t((rx + ry) * t.width);
			const std::int16_t v0 = std::int16_t(rw * t.height);
			const std::int16_t v1 = std::int16_t((rz + rw) * t.height);
			const std::size_t firstVertex = g_uiBatchVertices.size();
			g_uiBatchVertices.push_back(UiBatchVertex{ u0, v0, color, x0, y0, 0 });
			g_uiBatchVertices.push_back(UiBatchVertex{ u1, v0, color, x1, y1, 0 });
			g_uiBatchVertices.push_back(UiBatchVertex{ u0, v1, color, x2, y2, 0 });
			g_uiBatchVertices.push_back(UiBatchVertex{ u1, v1, color, x3, y3, 0 });
			AppendSpriteBatchIndices(firstVertex);
		} else if (texKey != nullptr) {
			// Pixels still streaming in (or failed): skip rather than flash the untextured quad as a white square.
		} else {
			FlushSpriteBatch();
			struct ColorVertex { std::uint32_t color; std::int16_t x, y, z; };
			const bool opaqueFastPath = ((color >> 24) == 255);
			if (opaqueFastPath) {
				sceGuDisable(GU_BLEND);
				sceGuDisable(GU_ALPHA_TEST);
			}
			sceGuDisable(GU_TEXTURE_2D);
			auto* v = static_cast<ColorVertex*>(GuAlloc(4 * sizeof(ColorVertex)));
			v[0] = { color, x0, y0, 0 };
			v[1] = { color, x1, y1, 0 };
			v[2] = { color, x2, y2, 0 };
			v[3] = { color, x3, y3, 0 };
			sceGuDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, nullptr, v);
			PspDiagnosticsRecordDraw(4);
			if (opaqueFastPath) {
				sceGuEnable(GU_ALPHA_TEST);
				sceGuEnable(GU_BLEND);
			}
		}
	}

	void PspEmitFlushBatches()
	{
		FlushSpriteBatch();
	}

	// Called after PspGuWaitForPreviousFrame drains and restarts the display list mid-frame. The list buffer is
	// rewound, so every pointer handed out of it is stale and the byte budget starts over; the GE's own register
	// state persists, but re-emitting the bindings is cheap and drains are rare (only when something is freed).
	void PspEmitInvalidateGuState()
	{
		g_guListBytesThisFrame = 0;
		InvalidateTextureBinding();
		g_clutLoadedThisFrame = false;
		g_fontClutValid = false;
	}

	void PspEmitResetFrameState()
	{
		if (++g_frameSerial == 0) g_frameSerial = 1;
		g_guListBytesThisFrame = 0;
		EvictColdSprites();
		if (g_worldBatchVertices.capacity() < MaxSpriteBatchQuads * 4) g_worldBatchVertices.reserve(MaxSpriteBatchQuads * 4);
		if (g_uiBatchVertices.capacity() < MaxSpriteBatchQuads * 4) g_uiBatchVertices.reserve(MaxSpriteBatchQuads * 4);
		if (g_spriteBatchIndices.capacity() < MaxSpriteBatchQuads * 6) g_spriteBatchIndices.reserve(MaxSpriteBatchQuads * 6);
		g_clutLoadedThisFrame = false;
		g_fontClutValid = false;
		InvalidateTextureBinding();
		ClearSpriteBatch();
		g_pvCamera = nullptr;
	}
}
