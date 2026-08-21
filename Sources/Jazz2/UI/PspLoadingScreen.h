#pragma once

#include "Jazz2/UI/Canvas.h"
#include "Jazz2/UI/Font.h"
#include "Jazz2/ContentResolver.h"
#include <cstdio>

namespace Jazz2::UI
{
	class PspLoadingScreen : public Jazz2::UI::Canvas
	{
	public:
		PspLoadingScreen(Metadata* metadata, Death::Containers::StringView levelTitle)
			: _metadata(metadata), _levelTitle(levelTitle)
		{
		}
		bool OnDraw(nCine::RenderQueue& renderQueue) override
		{
			Jazz2::UI::Canvas::OnDraw(renderQueue);
			DrawLoadingPicture();
			DrawLoadingTitle();
			return true;
		}

	private:
		GraphicResource* Resource(AnimState state) const
		{
			return (_metadata != nullptr ? _metadata->FindAnimation(state) : nullptr);
		}

		void DrawLoadingPicture()
		{
			auto* res = Resource((AnimState)1);
			if (res == nullptr || res->Base == nullptr || res->Base->TextureDiffuse == nullptr) return;
			// The splash lives ~2 frames, too short for the async streamer, so the picture would be skipped and
			// flash an empty background. Force it in synchronously; no-op once resident.
			nCine::PspForceResident(res->Base->TextureDiffuse->GetGuiTexId());
			// The baker already rescales Picture.Loading from the original 640x480 to the PSP framebuffer.
			DrawTexture(*res->Base->TextureDiffuse, nCine::Vector2f::Zero, 910,
				nCine::Vector2f(480.0f, 272.0f), nCine::Vector4f(1.0f, 0.0f, 1.0f, 0.0f),
				nCine::Colorf::White, false, 0.0f, -1);
		}

		void DrawLoadingTitle()
		{
			auto* res = Resource((AnimState)2);
			if (res == nullptr || res->Base == nullptr || res->Base->TextureDiffuse == nullptr) return;
			// Same for the font atlas: non-paged, so it streams in and would miss the splash.
			nCine::PspForceResident(res->Base->TextureDiffuse->GetGuiTexId());
			DrawLoadingText(*res->Base->TextureDiffuse, "Loading..."_s, 153.0f, 0.52f);
			DrawLoadingText(*res->Base->TextureDiffuse, _levelTitle, 190.0f, 0.42f);
		}

		void DrawLoadingText(nCine::Texture& texture, Death::Containers::StringView text, float y, float scale)
		{
			constexpr float AtlasWidth = 915.0f;
			constexpr float AtlasHeight = 1005.0f;
			constexpr float CellWidth = 61.0f;
			constexpr float CellHeight = 67.0f;
			constexpr int AtlasColumns = 15;
			static constexpr std::uint8_t GlyphMinX[95] = {
				0,6,7,0,7,0,0,0,0,0,0,0,5,6,6,0, 6,6,6,6,6,6,5,6,6,7,6,4,0,6,0,7,
				0,5,4,8,5,5,10,6,9,10,7,7,9,4,5,5, 8,7,4,9,8,7,9,5,10,9,7,0,0,0,6,0,
				5,5,5,6,6,7,6,6,6,6,5,7,6,6,5,6, 6,6,6,7,7,6,7,6,7,6,7,0,0,0,0
			};
			static constexpr std::uint8_t GlyphWidth[95] = {
				14,15,20,14,24,14,14,14,14,14,14,14,16,30,12,14, 30,12,32,27,30,27,25,22,26,22,10,16,14,32,14,27,
				14,34,36,29,35,34,26,33,28,25,30,32,27,37,36,36, 30,32,37,27,31,31,27,36,26,27,32,14,14,14,45,14,
				16,28,28,23,26,32,25,28,27,13,23,29,13,37,28,26, 26,28,25,24,24,28,25,35,23,23,25,14,14,14,14
			};
			float totalWidth = 0.0f;
			for (char c : text) {
				const unsigned code = static_cast<unsigned char>(c);
				totalWidth += (code >= 32 && code <= 126 ? GlyphWidth[code - 32] + 2 : 10) * scale;
			}
			float x = 240.0f - totalWidth * 0.5f;
			for (char c : text) {
				const unsigned code = static_cast<unsigned char>(c);
				if (code < 32 || code > 126) { x += 10.0f * scale; continue; }
				const int glyph = int(code) - 32;
				const int col = glyph % AtlasColumns, row = glyph / AtlasColumns;
				const float minX = GlyphMinX[glyph];
				const float width = GlyphWidth[glyph];
				const nCine::Vector4f uv(width / AtlasWidth, (col * CellWidth + minX) / AtlasWidth,
					CellHeight / AtlasHeight, (row * CellHeight) / AtlasHeight);
				DrawTexture(texture, nCine::Vector2f(x, y), 930,
					nCine::Vector2f(width * scale, CellHeight * scale), uv,
					nCine::Colorf::Black, false, 0.0f, -1);
				x += (width + 2.0f) * scale;
			}
		}

		Metadata* _metadata = nullptr;
		Death::Containers::String _levelTitle;
	};
}
