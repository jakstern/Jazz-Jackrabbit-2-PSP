#pragma once

#include "Jazz2/IRootController.h"
#include "Jazz2/LevelInitialization.h"
#include <memory>

namespace Jazz2
{
	// Minimal IRootController. LevelHandler drives InvokeAsync, ChangeLevel and GoToMainMenu; the latter two
	// only record a request, which PspEventHandler applies at end-of-frame. Everything else returns a default.
	class PspRootController : public IRootController
	{
	public:
		void InvokeAsync(Function<void()>&& callback) override { callback(); }
		void InvokeAsync(std::weak_ptr<void>, Function<void()>&& callback) override { callback(); }
		void GoToMainMenu(bool) override { _returnToMenu = true; }
		void ChangeLevel(LevelInitialization&& levelInit) override { _pendingLevel = std::make_unique<LevelInitialization>(std::move(levelInit)); _hasPendingLevel = true; }
		// Resumable-state STORAGE is deliberately stubbed on PSP: report "no saved state" rather than claim a
		// save/restore that never happened. The save FORMAT (SerializeResumableToStream) is fully present;
		// only the Memory Stick wiring is missing. See Jazz2/PspSaveStorage.h and TODO.md.
		bool HasResumableState() const override { return false; }
		void ResumeSavedState() override {}
		bool SaveCurrentStateIfAny() override { return false; }
		void ConnectToServer(StringView, std::uint16_t, StringView) override {}
		bool CreateServer(Multiplayer::ServerInitialization&&) override { return false; }
		Flags GetFlags() const override { return Flags::IsInitialized | Flags::IsVerified | Flags::IsPlayable; }
		StringView GetNewestVersion() const override { return {}; }
		void RefreshCacheLevels(bool) override {}

		// Pending transition, consumed by PspEventHandler at end-of-frame.
		bool _hasPendingLevel = false;
		bool _returnToMenu = false;
		std::unique_ptr<LevelInitialization> _pendingLevel;
	};
}
