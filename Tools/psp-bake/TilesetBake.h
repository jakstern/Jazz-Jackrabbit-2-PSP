#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

// One baked texture accumulated by the pack builder.
//   paged == false: single texture - `data` holds stride*strideH*bpp pixels.
//   paged == true:  paged atlas    - `pages` holds the 512-pages, `clut` the palette.
struct BakedTexture {
	std::uint64_t key = 0;
	int width = 0, height = 0;
	std::uint8_t format = 0;    // PspGpu::PixelFormat
	std::uint8_t flags = 0;     // PspGpu::TextureFlags
	bool swizzled = false;
	bool paged = false;

	// single (sprite)
	int stride = 0, strideH = 0;
	std::vector<std::uint8_t> data;
	std::vector<std::uint8_t> mask;   // width*height collision mask (1 byte/texel); empty if none
	std::vector<std::uint8_t> meta;   // the .aura header bytes (frame metadata); empty if none

	// paged (tileset)
	std::vector<std::uint32_t> clut;
	int pagesX = 0, pagesY = 0;
	struct Page { int ox = 0, oy = 0, w = 0, h = 0; std::vector<std::uint8_t> swizzled; };
	std::vector<Page> pages;
};

// Bake one native .j2t into *out (paged), keyed by `name`. Returns false for unsupported tilesets.
bool BakeTilesetJ2t(const char* path, const char* name, BakedTexture* out, const char** skipReason);

// Bake one sprite's device-format texture (R8/RG8/RGBA) into *out. `notIndexed` = the .aura's "palette
// already applied" flag. Shared by the .aura reader and the JJ2Anims::Convert sink so both emit identical bytes.
bool BakeSpriteFromPixels(std::uint64_t key, const std::uint8_t* data, int width, int height,
	int channelCount, bool notIndexed, int paletteOffset, int frameWidth, int frameHeight, BakedTexture* out,
	bool classifyAlpha = false, bool includeMask = true);

// Convert the desktop fire/lightning noise field into an animated RGBA sprite atlas. The Aura metadata in
// `out` is rewritten to describe the generated frames, so the runtime only has to select them.
bool BakeShieldAnimationFromPixels(std::uint64_t key, const std::uint8_t* data, int width, int height,
	int channelCount, bool lightning, const std::uint8_t* auraHeader, BakedTexture* out);

// Apply the water shield's palette and PSP tint during baking, and repack its wide 8x1 atlas to 4x2.
bool BakeWaterShieldFromPixels(std::uint64_t key, const std::uint8_t* data, int width, int height,
	int channelCount, int paletteOffset, const std::uint8_t* auraHeader, BakedTexture* out);

// Collect {animation asset path -> palette offset} from Content/Metadata/**.res (first occurrence wins).
void CollectSpriteMetadata(const char* contentDir, std::unordered_map<std::string, int>& out);

// Bake the bundled Content/Animations sprites for metadata paths not already in `out`. Returns the count.
int BakeBundledSprites(const char* contentDir, const std::unordered_map<std::string, int>& meta,
	std::vector<BakedTexture>& out);

// Serialize the baked textures into a PSP-GU pack file. Returns false on I/O error.
bool WriteTexturePack(const char* path, const std::vector<BakedTexture>& textures);

// Incremental writer used by the on-device baker: payloads are spooled immediately, so the PSP holds one
// decoded texture at a time instead of the whole (often 100+ MiB) pack.
struct TexturePackStream;
using BakeLogSink = void (*)(void* context, const char* message);
TexturePackStream* CreateTexturePackStream(const char* outputPath);
bool AppendTexturePack(TexturePackStream* stream, BakedTexture&& texture);
// Decode one native indexed tileset straight into the pack. Never allocates the full padded atlas: at most
// one linear plus one swizzled 512x512 page is live at a time.
bool AppendTilesetJ2t(TexturePackStream* stream, const char* path, const char* name, const char** skipReason,
	BakeLogSink log = nullptr, void* logContext = nullptr);
// Append an indexed tileset from JJ2Tileset::Convert's assembled pixels; the .j2tpsp sidecar then never
// stores or rereads a QOI diffuse image.
bool AppendTilesetPixels(TexturePackStream* stream, const char* name, const std::uint32_t* palette,
	const std::uint8_t* pixels, int width, int height, int channelCount, int tileCount,
	BakeLogSink log = nullptr, void* logContext = nullptr);
bool FinishTexturePack(TexturePackStream* stream, BakeLogSink log = nullptr, void* logContext = nullptr);
void DestroyTexturePackStream(TexturePackStream* stream);

using BakedTextureSink = bool (*)(void* context, BakedTexture&& texture);
using BakeProgressSink = void (*)(void* context, int completed, int total);
int BakeBundledSpritesToSink(const char* contentDir, const std::unordered_map<std::string, int>& meta,
	const std::unordered_set<std::uint64_t>& alreadyBaked, BakedTextureSink sink, void* sinkContext,
	BakeProgressSink progress = nullptr, void* progressContext = nullptr);
