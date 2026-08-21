#pragma once

#include "Jazz2/UI/Canvas.h"
#include "Jazz2/UI/Font.h"
#include "Jazz2/ContentResolver.h"
#include <cstdio>

namespace Jazz2::UI
{
	// End-of-level gem summary plus the run-off iris wipe. Owns no textures of its own - it borrows the
	// already-resident HUD metadata so the transition costs nothing extra.
	class PspLevelCompleteOverlay : public Jazz2::UI::Canvas
	{
	public:
		PspLevelCompleteOverlay(const std::array<std::int32_t, 4>& gems, nCine::Vector2f playerScreenPos)
			: _gems(gems), _playerScreenPos(playerScreenPos)
		{
			_hudMeta = ContentResolver::Get().RequestMetadata("UI/HUD"_s);
		}
		void BeginRunOff(nCine::Vector2f playerScreenPos)
		{
			if (!_runOffActive) {
				_playerScreenPos = playerScreenPos;
				_runOffActive = true;
				_blackoutElapsed = 0.0f;
			}
		}

		bool OnDraw(nCine::RenderQueue& renderQueue) override
		{
			Jazz2::UI::Canvas::OnDraw(renderQueue);
			DrawGemSummary();

			if (_runOffActive) {
				constexpr float BlackoutFrames = 55.0f;
				const float raw = _blackoutElapsed / BlackoutFrames;
				const float progress = std::clamp(raw * 1.08f, 0.0f, 1.0f);
				const float eased = progress * progress * (3.0f - 2.0f * progress);
				const float farX = std::max(_playerScreenPos.X, 480.0f - _playerScreenPos.X);
				const float farY = std::max(_playerScreenPos.Y, 272.0f - _playerScreenPos.Y);
				DrawBlackDisk(std::sqrt(farX * farX + farY * farY) * eased);
			}
			return true;
		}

		void OnUpdate(float timeMult) override
		{
			Jazz2::UI::Canvas::OnUpdate(timeMult);
			_elapsed += timeMult;
			if (_runOffActive) _blackoutElapsed += timeMult;
		}

	private:
		void DrawAnimation(AnimState state, float cx, float cy, float scale)
		{
			if (_hudMeta == nullptr) return;
			auto* res = _hudMeta->FindAnimation(state);
			if (res == nullptr || res->Base == nullptr || res->Base->TextureDiffuse == nullptr) return;
			auto* base = res->Base;
			const int frame = res->FrameOffset + (res->FrameCount > 1 && res->AnimDuration > 0.0f
				? ((int)(AnimTime * res->FrameCount / res->AnimDuration) % res->FrameCount) : 0);
			const int columns = std::max(1, base->FrameConfiguration.X);
			const int col = frame % columns;
			const int row = frame / columns;
			const auto texSize = base->TextureDiffuse->GetSize();
			const nCine::Vector2f size((float)base->FrameDimensions.X * scale, (float)base->FrameDimensions.Y * scale);
			const nCine::Vector4f uv((float)base->FrameDimensions.X / texSize.X,
				(float)(col * base->FrameDimensions.X) / texSize.X,
				(float)base->FrameDimensions.Y / texSize.Y,
				(float)(row * base->FrameDimensions.Y) / texSize.Y);
			const int palette = ((base->Flags & GenericGraphicResourceFlags::Indexed) == GenericGraphicResourceFlags::Indexed
				? res->PaletteOffset : -1);
			DrawTexture(*base->TextureDiffuse, nCine::Vector2f(cx - size.X * 0.5f, cy - size.Y * 0.5f),
				710, size, uv, nCine::Colorf::White, false, 0.0f, palette);
		}

		void DrawWobbly(Jazz2::UI::Font* font, Death::Containers::StringView text, float x, float y,
			Jazz2::UI::Alignment alignment, float scale)
		{
			if (font == nullptr) return;
			std::int32_t charOffset = 0;
			font->DrawString(this, text, charOffset, x, y, 720, alignment, Jazz2::UI::Font::DefaultColor,
				scale, 0.65f, 1.05f, 1.05f, 0.38f);
		}

		void DrawGemSummary()
		{
			auto* font = ContentResolver::Get().GetFont(Resources::FontType::Medium);
			static constexpr int Weights[3] = { 1, 5, 10 };
			static constexpr AnimState GemStates[3] = { (AnimState)71, (AnimState)72, (AnimState)73 };
			const float fadeIn = std::clamp(_elapsed / 16.0f, 0.0f, 1.0f);
			// Match the cubic horizontal fly-in used by HUD level/sign messages.
			const float flyOffset = (_elapsed < 55.0f ? std::pow((55.0f - _elapsed) / 8.0f, 3.0f) : 0.0f);
			DrawSolid(nCine::Vector2f(138.0f + flyOffset, 30.0f), 690, nCine::Vector2f(204.0f, 218.0f),
				nCine::Colorf(0.0f, 0.0f, 0.0f, 0.32f * fadeIn), false);
			std::int32_t total = 0;
			for (int i = 0; i < 3; i++) {
				const float y = 66.0f + i * 51.0f;
				DrawAnimation(GemStates[i], 171.0f + flyOffset, y + 4.0f, 1.25f);
				char count[24], multiplier[16];
				std::snprintf(count, sizeof(count), "%d", _gems[i]);
				std::snprintf(multiplier, sizeof(multiplier), "x %d", Weights[i]);
				DrawWobbly(font, Death::Containers::StringView(count), 214.0f + flyOffset, y, Jazz2::UI::Alignment::Center, 0.9f);
				DrawWobbly(font, Death::Containers::StringView(multiplier), 285.0f + flyOffset, y, Jazz2::UI::Alignment::Center, 0.9f);
				total += _gems[i] * Weights[i];
			}
			char totalText[24];
			std::snprintf(totalText, sizeof(totalText), "%d", total);
			DrawWobbly(font, "TOTAL"_s, 178.0f + flyOffset, 224.0f, Jazz2::UI::Alignment::Center, 0.92f);
			DrawWobbly(font, Death::Containers::StringView(totalText), 288.0f + flyOffset, 224.0f, Jazz2::UI::Alignment::Center, 0.92f);
		}

		void DrawBlackDisk(float radius)
		{
			if (radius <= 0.0f) return;
			// Canvas can only draw rectangles; 40 horizontal strips read as a circle at 272p and are far
			// cheaper than adding a mask pass.
			constexpr int Bands = 40;
			const float top = _playerScreenPos.Y - radius;
			const float bandHeight = std::max(2.0f, radius * 2.0f / Bands + 1.0f);
			for (int i = 0; i < Bands; i++) {
				const float y = top + (i + 0.5f) * radius * 2.0f / Bands;
				if (y + bandHeight < 0.0f || y - bandHeight > 272.0f) continue;
				const float dy = y - _playerScreenPos.Y;
				const float halfWidth = std::sqrt(std::max(0.0f, radius * radius - dy * dy));
				// Players draw at 520; stay just under them so the run-off stays visible over the disk, like JJ2.
				DrawSolid(nCine::Vector2f(_playerScreenPos.X - halfWidth, y - bandHeight * 0.5f), 515,
					nCine::Vector2f(halfWidth * 2.0f, bandHeight), nCine::Colorf::Black, false);
			}
		}

		std::array<std::int32_t, 4> _gems{};
		nCine::Vector2f _playerScreenPos;
		Metadata* _hudMeta = nullptr;
		float _elapsed = 0.0f;
		float _blackoutElapsed = 0.0f;
		bool _runOffActive = false;
	};
}
