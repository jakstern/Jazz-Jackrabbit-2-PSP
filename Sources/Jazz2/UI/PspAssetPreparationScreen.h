#pragma once

#include "Jazz2/UI/Canvas.h"
#include "Jazz2/UI/Font.h"
#include "Jazz2/ContentResolver.h"
#include <atomic>
#include <cstdio>

// Progress UI for the on-device asset bake. The bake runs on a worker thread while this screen keeps
// drawing, so every value it reports crosses threads through the atomics below.
namespace Jazz2::UI
{
	struct PspBakeStatus
	{
		std::atomic<int> Completed{0};
		std::atomic<int> Total{1};
		std::atomic<int> DetailStage{(int)PspBakeStage::Animations};
		std::atomic<int> DetailCompleted{0};
		std::atomic<int> DetailTotal{0};
		std::atomic<int> Phase{0}; // 0: general assets, 1: music
		std::atomic<int> MusicMilliseconds{0};
		std::atomic<int> MusicTotalMilliseconds{0};
		std::atomic<int> MusicTracksCompleted{0};
		std::atomic<int> MusicTracksTotal{0};
		std::atomic<int> RemainingSeconds{-1};
		std::atomic<int> Result{-1};
		std::atomic<bool> Finished{false};
		std::uint64_t MusicStartedAtUs = 0;
		double MusicRate = 10.0;
	};

	class PspAssetPreparationScreen : public Jazz2::UI::Canvas
	{
	public:
		explicit PspAssetPreparationScreen(PspBakeStatus& status) : _status(status) {}

		bool OnDraw(nCine::RenderQueue& renderQueue) override
		{
			Jazz2::UI::Canvas::OnDraw(renderQueue);
			DrawSolid(nCine::Vector2f::Zero, 2000, nCine::Vector2f(480.0f, 272.0f),
				nCine::Colorf(0.055f, 0.018f, 0.09f, 1.0f), false);
			auto* medium = ContentResolver::Get().GetFont(Resources::FontType::Medium);
			auto* small = ContentResolver::Get().GetFont(Resources::FontType::Small);
			const bool failed = _status.Finished.load(std::memory_order_acquire) &&
				_status.Result.load(std::memory_order_relaxed) != 0;
			const bool music = _status.Phase.load(std::memory_order_acquire) == 1;
			const auto detailStage = (PspBakeStage)_status.DetailStage.load(std::memory_order_relaxed);
			Death::Containers::StringView stageTitle;
			const char* detailNoun = "Assets";
			switch (detailStage) {
				case PspBakeStage::Animations: stageTitle = "Converting animations..."_s; detailNoun = "Sprites"; break;
				case PspBakeStage::GameData: stageTitle = "Converting game data..."_s; detailNoun = "Data files"; break;
				case PspBakeStage::Episodes: stageTitle = "Converting episodes..."_s; detailNoun = "Episodes"; break;
				case PspBakeStage::Levels: stageTitle = "Converting levels..."_s; detailNoun = "Levels"; break;
				case PspBakeStage::NativeTilesets: stageTitle = "Converting tilesets..."_s; detailNoun = "Tilesets"; break;
				case PspBakeStage::BundledSprites: stageTitle = "Adding bundled sprites..."_s; detailNoun = "Sprites"; break;
				case PspBakeStage::FinalizingGraphics: stageTitle = "Finalizing graphics..."_s; detailNoun = "Packs"; break;
				case PspBakeStage::CacheIndex: stageTitle = "Writing cache index..."_s; detailNoun = "Indexes"; break;
				case PspBakeStage::Music: stageTitle = "Converting music..."_s; detailNoun = "Tracks"; break;
			}
			if (music) stageTitle = "Converting music..."_s;
			if (_unsupported) {
				DrawWobbly(medium, "Asset preparation unsupported"_s, 240.0f, 74.0f, 0.86f);
				DrawPlain(small, "The PSP-1000 does not have enough memory to"_s, 240.0f, 122.0f, 0.9f);
				DrawPlain(small, "convert the original JJ2 files on device."_s, 240.0f, 143.0f, 0.9f);
				DrawPlain(small, "Run psp-bake on a PC and copy the generated"_s, 240.0f, 176.0f, 0.9f);
				DrawPlain(small, "Cache folder next to EBOOT.PBP."_s, 240.0f, 197.0f, 0.9f);
				if (_dismissable) DrawPlain(small, "X: Back"_s, 240.0f, 230.0f, 0.82f);
				return true;
			}
			DrawWobbly(medium, failed ? "Asset conversion failed"_s : stageTitle, 240.0f, 91.0f, 0.92f);

			const int completed = (music ? _status.MusicMilliseconds.load(std::memory_order_relaxed)
				: _status.DetailCompleted.load(std::memory_order_relaxed));
			const int reportedTotal = (music
				? _status.MusicTotalMilliseconds.load(std::memory_order_relaxed)
				: _status.DetailTotal.load(std::memory_order_relaxed));
			const int total = std::max(completed, reportedTotal);
			const float fraction = (total > 0 ? std::clamp((float)completed / total, 0.0f, 1.0f) : 0.0f);
			DrawSolid(nCine::Vector2f(79.0f, 139.0f), 2002, nCine::Vector2f(322.0f, 12.0f),
				nCine::Colorf(0.12f, 0.06f, 0.18f, 0.95f), false);
			DrawSolid(nCine::Vector2f(82.0f, 142.0f), 2003, nCine::Vector2f(316.0f * fraction, 6.0f),
				nCine::Colorf(0.78f, 0.36f, 0.96f, 1.0f), false);
			char progress[96];
			if (failed) {
				std::snprintf(progress, sizeof(progress), "Put original JJ2 files in Source/  -  X: Retry");
			} else if (music) {
				const int tracksDone = _status.MusicTracksCompleted.load(std::memory_order_relaxed);
				const int tracksTotal = _status.MusicTracksTotal.load(std::memory_order_relaxed);
				const int remaining = _status.RemainingSeconds.load(std::memory_order_relaxed);
				if (remaining >= 0) {
					const int minutes = std::max(1, (remaining + 59) / 60);
					std::snprintf(progress, sizeof(progress), "Music %d/%d  -  about %d minute%s remaining",
						tracksDone, tracksTotal, minutes, minutes == 1 ? "" : "s");
				} else {
					std::snprintf(progress, sizeof(progress), "Analyzing %d music tracks...", tracksTotal);
				}
			} else if (reportedTotal <= 0) {
				std::snprintf(progress, sizeof(progress), "Counting sprites...");
			} else {
				std::snprintf(progress, sizeof(progress), "%s %d / %d", detailNoun, completed, total);
			}
			DrawPlain(small, Death::Containers::StringView(progress), 240.0f, 168.0f, 0.95f);
			if (!_status.Finished.load(std::memory_order_acquire))
				DrawPlain(small, "Keep the PSP powered on"_s, 240.0f, 198.0f, 0.82f);
			return true;
		}

		void OnUpdate(float timeMult) override
		{
			Jazz2::UI::Canvas::OnUpdate(timeMult);
			SceCtrlData pad{};
			sceCtrlPeekBufferPositive(&pad, 1);
			const std::uint32_t hit = pad.Buttons & ~_lastButtons;
			_lastButtons = pad.Buttons;
			if (_unsupported) {
				if (_dismissable && (hit & PSP_CTRL_CROSS)) _dismiss = true;
				return;
			}
			if (_status.Finished.load(std::memory_order_acquire) && _status.Result.load(std::memory_order_relaxed) != 0 &&
				(hit & PSP_CTRL_CROSS)) _retry = true;
		}

		bool RetryRequested() const { return _retry; }
		void ClearRetry() { _retry = false; }

		// Replaces the progress UI when the bake cannot run on this hardware at all. Dismissable only if a
		// usable cache already exists, i.e. if there is a menu to go back to.
		void MarkUnsupported(bool dismissable) { _unsupported = true; _dismissable = dismissable; }
		bool DismissRequested() const { return _dismiss; }
		void ClearDismiss() { _dismiss = false; }

	private:
		void DrawWobbly(Jazz2::UI::Font* font, Death::Containers::StringView text, float x, float y, float scale)
		{
			if (font == nullptr) return;
			std::int32_t offset = 0;
			font->DrawString(this, text, offset, x, y, 2010, Jazz2::UI::Alignment::Center,
				Jazz2::UI::Font::DefaultColor, scale, 0.72f, 1.15f, 1.15f, 0.38f);
		}
		void DrawPlain(Jazz2::UI::Font* font, Death::Containers::StringView text, float x, float y, float scale)
		{
			if (font == nullptr) return;
			std::int32_t offset = 0;
			font->DrawString(this, text, offset, x, y, 2010, Jazz2::UI::Alignment::Center,
				Jazz2::UI::Font::DefaultColor, scale, 0.0f, 0.0f, 0.0f, 0.0f);
		}

		PspBakeStatus& _status;
		std::uint32_t _lastButtons = 0;
		bool _retry = false;
		bool _unsupported = false;
		bool _dismissable = false;
		bool _dismiss = false;
	};
}
