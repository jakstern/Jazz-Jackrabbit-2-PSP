#pragma once

#include "Jazz2/UI/Canvas.h"
#include "Jazz2/UI/Font.h"
#include "nCine/Backends/Psp/PspDiagnostics.h"
#include <cfloat>
#include <cmath>
#include <cstdio>

// Always-on overlays: diagnostics readout, the dim behind a Sony system utility, and the multiplayer banners.
namespace Jazz2::UI
{
	// A Sony system utility (savedata, network config, OSK) is on screen. Set once per frame by PspEventHandler.
	inline bool g_systemUtilityActive = false;
	class PspDiagnosticsOverlay : public Jazz2::UI::Canvas
	{
	public:
		bool OnDraw(nCine::RenderQueue& renderQueue) override
		{
			Jazz2::UI::Canvas::OnDraw(renderQueue);
			if (!nCine::PspDiagnosticsIsVisible()) return true;

			Jazz2::UI::Font* font = ContentResolver::Get().GetFont(Resources::FontType::Small);
			if (font == nullptr) return true;
			const auto& s = nCine::PspDiagnosticsGetSnapshot();
			const float fps = (s.totalMs > 0.0f ? 1000.0f / s.totalMs : 0.0f);

			DrawSolid(nCine::Vector2f(2.0f, 2.0f), 600, nCine::Vector2f(150.0f, 16.0f),
				nCine::Colorf(0.0f, 0.0f, 0.0f, 0.72f), false);
			char line[48];
			std::snprintf(line, sizeof(line), "%.1f fps  %.2f ms", fps, s.totalMs);
			std::int32_t charOffset = 0;
			font->DrawString(this, line, charOffset, 5.0f, 4.0f, 610, Jazz2::UI::Alignment::TopLeft,
				nCine::Colorf(0.88f, 0.92f, 0.88f, 1.0f), 0.55f);
			return true;
		}

	};

	// Sony's utility panels are composited over our last frame without dimming it, so their text is unreadable
	// over bright art. Dim OUR output only; the PSP still draws the panel on top at full brightness.
	class PspSystemUtilityDimOverlay : public Jazz2::UI::Canvas
	{
	public:
		bool OnDraw(nCine::RenderQueue& renderQueue) override
		{
			Jazz2::UI::Canvas::OnDraw(renderQueue);
			if (!g_systemUtilityActive) return true;
			// Above everything the game and menu draw; the sync overlay (1000) is never up at the same time.
			DrawSolid(nCine::Vector2f::Zero, 900, nCine::Vector2f(480.0f, 272.0f),
				nCine::Colorf(0.0f, 0.0f, 0.0f, 0.75f), false);
			return true;
		}
	};

	class PspMultiplayerSyncOverlay : public Jazz2::UI::Canvas
	{
	public:
		bool OnDraw(nCine::RenderQueue& renderQueue) override
		{
			Jazz2::UI::Canvas::OnDraw(renderQueue);
			DrawSolid(nCine::Vector2f::Zero, 1000, nCine::Vector2f(480.0f, 272.0f), nCine::Colorf::Black, false);
			auto* font = ContentResolver::Get().GetFont(Resources::FontType::Medium);
			if (font != nullptr) {
				std::int32_t charOffset = 0;
				font->DrawString(this, "Synchronizing..."_s, charOffset, 240.0f, 136.0f, 1010,
					Jazz2::UI::Alignment::Center, nCine::Colorf::White, 0.76f);
			}
			return true;
		}
	};

	// Water. On desktop this is a full-screen post-process shader (CombineWithWaterLowFs in
	// ContentResolver.Shaders.h) applied by CombineRenderer, but the PSP has no pass to run it in:
	// LevelHandler::OnInitializeViewport draws the scene root straight to the screen viewport and never creates a
	// PlayerViewport or a CombineRenderer at all. So the low-quality variant is rebuilt here out of plain quads:
	//
	//     below the surface   mix(scene, waterColor, 0.4)      one 40%-alpha quad in waterColor
	//     approaching it      += 0.2 * (1 - topDist)^2         a stack of white quads, quadratic falloff
	//     on the surface      += 0.2                           one brighter row
	//
	// Two things the shader does are dropped. Its horizontal wobble resamples the rendered frame, which nothing on
	// PSP can read back cheaply. And its highlights are additive, whereas the GU holds one global blend mode
	// (PspGu.cpp sets GU_SRC_ALPHA/GU_ONE_MINUS_SRC_ALPHA once), so they blend toward white instead. The
	// substitution is close for the intensities involved: mix(c, 1, v) and c + v agree exactly where c is dark and
	// again where both saturate, and differ by at most v*c in between.
	class PspWaterOverlay : public Jazz2::UI::Canvas
	{
	public:
		/** @brief Sets the world Y of the water surface, the camera centre the scene was drawn with, and level time */
		void SetWater(float waterLevel, float cameraX, float cameraY, float elapsedFrames) {
			_waterLevel = waterLevel;
			_cameraX = cameraX;
			_cameraY = cameraY;
			_elapsedFrames = elapsedFrames;
		}
		/** @brief Stops drawing; used when no level is running so a stale level's water can't leak into the menu */
		void Clear() {
			_waterLevel = FLT_MAX;
		}

		bool OnDraw(nCine::RenderQueue& renderQueue) override
		{
			Jazz2::UI::Canvas::OnDraw(renderQueue);

			// The same expression CombineRenderer feeds its shader, kept in screen pixels rather than normalized
			// view space. Levels without water carry 32767, which lands far below the screen on its own.
			const float surfaceY = _waterLevel - _cameraY + ViewHeight * 0.5f;
			if (surfaceY >= ViewHeight) return true;

			DrawBody(surfaceY);
			DrawRays(surfaceY);

			// isVeryNearTop brightens exactly one row at the surface, on top of what the body already puts there.
			if (surfaceY >= 0.0f) {
				DrawQuad(0.0f, surfaceY, ViewWidth, 1.0f, 0.0f,
					nCine::Colorf(1.0f, 1.0f, 1.0f, 0.2f), SurfaceLayer);
			}
			return true;
		}

	private:
		static constexpr float ViewWidth = 480.0f;
		static constexpr float ViewHeight = 272.0f;
		static constexpr std::int32_t Bands = 10;

		// Rays are placed on a fixed world-space grid so they stay put while the camera pans, and each one is cut
		// into segments because a PSP quad carries a single flat colour and cannot fade along its own length.
		static constexpr std::int32_t RayCount = 6;
		static constexpr std::int32_t RaySegments = 4;
		static constexpr float RaySpacing = 132.0f;
		static constexpr float RayWidth = 26.0f;
		static constexpr float RayLength = 150.0f;
		static constexpr float RayShear = 0.34f;
		static constexpr float RayAlpha = 0.09f;
		static constexpr float RayDrift = 14.0f;

		// Canvas::DrawSolid offsets everything by PspUiLayerBase so it sorts above the whole scene. That is right
		// for the HUD and wrong for water, which has to cover the scene yet stay under the HUD. These sit just
		// below that base: above every tilemap and actor depth (those stay in the low hundreds) and below the
		// lowest layer the HUD uses.
		static constexpr std::uint16_t BodyLayer = nCine::RenderCommand::PspUiLayerBase - 30;
		static constexpr std::uint16_t RayLayer = nCine::RenderCommand::PspUiLayerBase - 20;
		static constexpr std::uint16_t SurfaceLayer = nCine::RenderCommand::PspUiLayerBase - 10;

		// The shader tints every depth with one flat waterColor. Real water keeps absorbing as it deepens, so the
		// tint is graded instead: the shader's exact colour and 0.4 alpha at the surface, deepening to a darker and
		// thicker blue further down.
		//
		// The shader's near-surface glow (isNearTop = 0.2 * max(1 - topDist, 0)^2) is folded into the same quads
		// rather than laid over them. Applying the tint and then the glow as two blends gives
		//     scene*(1-at)*(1-ag) + tint*at*(1-ag) + white*ag
		// which is itself a single mix(scene, C, A), so one quad per band reproduces both exactly.
		void DrawBody(float surfaceY)
		{
			const float top = (surfaceY > 0.0f ? surfaceY : 0.0f);
			const float submerged = ViewHeight - top;
			for (std::int32_t i = 0; i < Bands; i++) {
				const float y0 = top + submerged * ((float)i / Bands);
				const float y1 = top + submerged * ((float)(i + 1) / Bands);
				const float depth = ((y0 + y1) * 0.5f - surfaceY) / ViewHeight;
				const float t = (depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth));

				const float tintR = 0.40f + (0.10f - 0.40f) * t;
				const float tintG = 0.60f + (0.22f - 0.60f) * t;
				const float tintB = 0.80f + (0.45f - 0.80f) * t;
				const float tintA = 0.40f + (0.62f - 0.40f) * t;

				const float gradient = (depth < 1.0f ? 1.0f - depth : 0.0f);
				const float glowA = 0.2f * gradient * gradient;

				const float alpha = 1.0f - (1.0f - tintA) * (1.0f - glowA);
				if (alpha <= 0.0f) continue;
				const float tintWeight = tintA * (1.0f - glowA) / alpha;
				const float glowWeight = glowA / alpha;
				DrawQuad(0.0f, y0, ViewWidth, y1 - y0, 0.0f, nCine::Colorf(
					tintR * tintWeight + glowWeight,
					tintG * tintWeight + glowWeight,
					tintB * tintWeight + glowWeight, alpha), BodyLayer);
			}
		}

		// Sunlight entering the surface. The shader builds these from simplex noise, which is out of reach here, but
		// a few sheared translucent quads leaning the same way read the same at this resolution.
		void DrawRays(float surfaceY)
		{
			if (surfaceY <= -RayLength) return; // Surface far above the view; its rays cannot reach the screen

			const float halfView = ViewWidth * 0.5f;
			const float firstIndex = std::floor((_cameraX - halfView - RayWidth) / RaySpacing);
			const float segmentHeight = RayLength / RaySegments;
			for (std::int32_t i = 0; i < RayCount; i++) {
				const float index = firstIndex + (float)i;
				// Seeded from the world-space index, so a ray keeps its phase as the camera pans past it.
				const float drift = std::sin(_elapsedFrames * 0.004f + index * 1.7f) * RayDrift;
				const float x = index * RaySpacing - _cameraX + halfView + drift;
				if (x < -RayWidth * 2.0f || x > ViewWidth + RayWidth) continue;

				for (std::int32_t s = 0; s < RaySegments; s++) {
					const float y = surfaceY + s * segmentHeight;
					if (y + segmentHeight <= 0.0f || y >= ViewHeight) continue;
					const float fade = 1.0f - ((float)s + 0.5f) / RaySegments;
					DrawQuad(x + RayShear * (s * segmentHeight), y, RayWidth, segmentHeight, RayShear,
						nCine::Colorf(0.85f, 0.95f, 1.0f, RayAlpha * fade * fade), RayLayer);
				}
			}
		}

		// Canvas::DrawSolid's PSP path, but with an absolute layer instead of a UI-relative one, and an optional
		// shear. Alpha stays below 255 throughout, which also keeps the emitter's opaque fast path (it drops
		// blending entirely) out of play.
		void DrawQuad(float x, float y, float width, float height, float shear, const nCine::Colorf& color,
			std::uint16_t layer)
		{
			if (width <= 0.0f || height <= 0.0f || color.A <= 0.0f) return;
			auto* command = RentRenderCommand();
			auto transform = nCine::Matrix4x4f::Translation(x, y, 0.0f);
			// Column 1, row 0: local Y leaks into X, making the quad a parallelogram whose top and bottom edges
			// stay horizontal. The emitter runs all four corners through this matrix, so the shear costs nothing.
			transform[1].X = shear;
			command->SetTransformation(transform);
			command->SetLayer(layer);
			command->GetMaterial().SetTexture(0, nullptr);
			command->SetPspSpriteData(color.Data(), 1.0f, 0.0f, 1.0f, 0.0f, width, height);
			command->pspUiSpace_ = true;
			command->GetGeometry().SetDrawParameters(GL_TRIANGLE_STRIP, 0, 4);
			DrawRenderCommand(command);
		}

		float _waterLevel = FLT_MAX;
		float _cameraX = ViewWidth * 0.5f;
		float _cameraY = ViewHeight * 0.5f;
		float _elapsedFrames = 0.0f;
	};

	class PspPeerOverlay : public Jazz2::UI::Canvas
	{
	public:
		explicit PspPeerOverlay(nCine::PspAdhoc& adhoc) : _adhoc(adhoc) {}
		void SetCameraCenter(float x, float y) { _cameraX = x; _cameraY = y; }

		bool OnDraw(nCine::RenderQueue& renderQueue) override
		{
			Jazz2::UI::Canvas::OnDraw(renderQueue);
			if (!_adhoc.HasRemotePlayerPosition()) return true;
			const float x = _adhoc.GetRemotePlayerX() - _cameraX + 240.0f;
			const float y = _adhoc.GetRemotePlayerY() - _cameraY + 136.0f;
			if (x < -16.0f || x > 496.0f || y < -16.0f || y > 288.0f) return true;

			const nCine::Colorf markerColor(0.25f, 0.95f, 1.0f, 0.9f);
			DrawSolid(nCine::Vector2f(x - 1.0f, y - 10.0f), 580, nCine::Vector2f(3.0f, 21.0f), markerColor, false);
			DrawSolid(nCine::Vector2f(x - 10.0f, y - 1.0f), 580, nCine::Vector2f(21.0f, 3.0f), markerColor, false);
			DrawSolid(nCine::Vector2f(x - 42.0f, y - 31.0f), 579, nCine::Vector2f(84.0f, 15.0f),
				nCine::Colorf(0.0f, 0.0f, 0.0f, 0.58f), false);
			auto* font = ContentResolver::Get().GetFont(Resources::FontType::Small);
			if (font != nullptr) {
				std::int32_t charOffset = 0;
				font->DrawString(this, Death::Containers::StringView(_adhoc.GetPeerName()), charOffset, x, y - 30.0f, 581,
					Jazz2::UI::Alignment::Center, nCine::Colorf(0.75f, 1.0f, 1.0f, 1.0f), 0.55f);
			}
			return true;
		}

	private:
		nCine::PspAdhoc& _adhoc;
		float _cameraX = 240.0f;
		float _cameraY = 136.0f;
	};
}
