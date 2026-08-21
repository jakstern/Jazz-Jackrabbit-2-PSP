#pragma once

#include "Jazz2/UI/Canvas.h"
#include "Jazz2/UI/Font.h"
#include "Jazz2/ContentResolver.h"
#include <cstdio>

namespace Jazz2::UI::Menu
{
	// PSP-native pause overlay. LevelHandler still owns the authoritative pause and audio state; this Canvas
	// only presents the choices and reports the picked Action to PspEventHandler.
	class PspPauseMenu : public Jazz2::UI::Canvas
	{
	public:
		enum class Action {
			None, Resume, Save, Load, ToggleJoining, ToggleDiagnostics,
			CheatNext, CheatPrevious, CheatFly, CheatGod, CheatShield, CheatCoins,
			CheatBird, CheatGuns, CheatLives, CheatGems, CheatPower, QuitToMenu
		};

		PspPauseMenu(Metadata* menuMeta, std::vector<std::shared_ptr<nCine::AudioBufferPlayer>>& sounds,
			bool onlineSession, bool allowSaveLoad, bool canToggleJoining,
			bool joiningAllowed, bool allowCheats, bool flyEnabled, bool godEnabled, ShieldType shieldType)
			: _onlineSession(onlineSession), _allowSaveLoad(allowSaveLoad), _canToggleJoining(canToggleJoining),
			  _joiningAllowed(joiningAllowed), _allowCheats(allowCheats), _flyEnabled(flyEnabled),
			  _godEnabled(godEnabled), _shieldType(shieldType), _menuMeta(menuMeta), _sounds(sounds)
		{
			SceCtrlData pad{};
			sceCtrlPeekBufferPositive(&pad, 1);
			_last = pad.Buttons;
		}

		Action RequestedAction() const { return _action; }
		void ClearAction() { _action = Action::None; }
		void PlayOpenSfx() { PlayMenuSfx(0.6f); }
		void PlayCloseSfx() { PlayMenuSfx(0.6f); }
		void SetTestingState(bool flyEnabled, bool godEnabled)
		{
			_flyEnabled = flyEnabled;
			_godEnabled = godEnabled;
		}
		void SetShieldState(ShieldType shieldType) { _shieldType = shieldType; }
		void ShowStorageResult(const char* message) { std::snprintf(_message, sizeof(_message), "%s", message); }
		void SetJoiningAllowed(bool allowed)
		{
			_joiningAllowed = allowed;
			_onlineSession = allowed;
			_row = std::min(_row, ItemCount() - 1);
		}
		void LatchInput() { SceCtrlData pad{}; sceCtrlPeekBufferPositive(&pad, 1); _last = pad.Buttons; _waitForRelease = true; }

		bool OnDraw(nCine::RenderQueue& renderQueue) override
		{
			Jazz2::UI::Canvas::OnDraw(renderQueue);
			auto* fontM = ContentResolver::Get().GetFont(Resources::FontType::Medium);
			auto* fontS = ContentResolver::Get().GetFont(Resources::FontType::Small);
			DrawBackground();
			DrawLabel(fontM, _testingMenu ? "Cheats"_s : "Paused"_s, 240.0f, _testingMenu ? 28.0f : 46.0f, false, 1.0f);
			const int itemCount = ItemCount();
			if (_testingMenu) {
				for (int i = 0; i < itemCount; i++) {
					DrawLabel(fontM, ItemLabel(i), (i & 1) == 0 ? 124.0f : 356.0f,
						61.0f + (i / 2) * 32.0f, i == _row, 0.68f);
				}
			} else {
				for (int i = 0; i < itemCount; i++) {
					DrawLabel(fontM, ItemLabel(i), 240.0f,
						(itemCount >= 5 ? 82.0f : itemCount >= 4 ? 96.0f : 115.0f) + i * (itemCount >= 5 ? 32.0f : itemCount >= 4 ? 34.0f : 38.0f),
						i == _row, 0.76f);
				}
			}
			if (_message[0] != '\0') {
				DrawLabel(fontS, Death::Containers::StringView(_message), 240.0f, 245.0f, false, 0.9f);
			} else if (_testingMenu) {
				DrawLabel(fontS, "Select or Circle: Back"_s, 240.0f, 253.0f, false, 0.9f);
			} else if (_onlineSession) {
				DrawLabel(fontS, "Multiplayer enabled - game continues in background"_s,
					240.0f, 245.0f, false, 0.9f);
			}
			return true;
		}

		void OnUpdate(float timeMult) override
		{
			if (!isUpdateEnabled()) return;
			Jazz2::UI::Canvas::OnUpdate(timeMult);
			SceCtrlData pad{};
			sceCtrlReadBufferPositive(&pad, 1);
			std::uint32_t now = pad.Buttons;
			if (pad.Ly < 64) now |= PSP_CTRL_UP; else if (pad.Ly > 192) now |= PSP_CTRL_DOWN;
			if (pad.Lx < 64) now |= PSP_CTRL_LEFT; else if (pad.Lx > 192) now |= PSP_CTRL_RIGHT;
			if (_waitForRelease) {
				_last = now;
				if ((now & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE | PSP_CTRL_START | PSP_CTRL_SELECT)) == 0) _waitForRelease = false;
				return;
			}
			const std::uint32_t hit = now & ~_last;
			_last = now;
			const int itemCount = ItemCount();
			if (hit & PSP_CTRL_SELECT) {
				PlayMenuSfx(0.5f);
				_testingMenu = !_testingMenu;
				_row = 0;
				_message[0] = '\0';
				return;
			}
			const int previousRow = _row;
			if (_testingMenu) {
				if (hit & PSP_CTRL_UP) MoveTestingVertical(-1);
				if (hit & PSP_CTRL_DOWN) MoveTestingVertical(1);
				if ((hit & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) && itemCount > 1) {
					const int otherColumn = (_row & ~1) + ((_row & 1) == 0 ? 1 : 0);
					if (otherColumn < itemCount) _row = otherColumn;
				}
			} else {
				if (hit & PSP_CTRL_UP) _row = (_row + itemCount - 1) % itemCount;
				if (hit & PSP_CTRL_DOWN) _row = (_row + 1) % itemCount;
			}
			if (_row != previousRow) PlayMenuSfx(0.5f);
			if (hit & PSP_CTRL_CIRCLE) {
				PlayMenuSfx(0.5f);
				if (_testingMenu) {
					_testingMenu = false;
					_row = 0;
				} else {
					_action = Action::Resume;
				}
			}
			if (hit & PSP_CTRL_CROSS) {
				PlayMenuSfx(0.6f);
				_action = ItemAction(_row);
			}
		}

	private:
		void PlayMenuSfx(float gain)
		{
			if (_menuMeta == nullptr) return;
			auto it = _menuMeta->Sounds.find("MenuSelect"_s);
			if (it == _menuMeta->Sounds.end() || it->second.Buffers.empty()) return;
			_sounds.erase(std::remove_if(_sounds.begin(), _sounds.end(), [](const auto& player) {
				return player->isStopped();
			}), _sounds.end());
			const std::size_t index = (it->second.Buffers.size() > 1
				? nCine::Random().Next(0, (std::uint32_t)it->second.Buffers.size()) : 0);
			auto player = std::make_shared<nCine::AudioBufferPlayer>(&it->second.Buffers[index]->Buffer);
			player->setGain(gain * PreferencesCache::MasterVolume * PreferencesCache::SfxVolume);
			player->setSourceRelative(true);
			player->play();
			_sounds.push_back(std::move(player));
		}

		int ItemCount() const
		{
			return (_testingMenu ? (_allowCheats ? 12 : 1) : 2 + (_allowSaveLoad ? 2 : 0) + (_canToggleJoining ? 1 : 0));
		}
		void MoveTestingVertical(int direction)
		{
			const int itemCount = ItemCount();
			const int rowCount = (itemCount + 1) / 2;
			const int column = _row & 1;
			int row = (_row / 2 + direction + rowCount) % rowCount;
			int next = row * 2 + column;
			if (next >= itemCount) next = row * 2;
			_row = next;
		}
		Action ItemAction(int row) const
		{
			if (_testingMenu) {
				switch (row) {
					case 0: return Action::ToggleDiagnostics;
					case 1: return Action::CheatFly;
					case 2: return Action::CheatGod;
					case 3: return Action::CheatShield;
					case 4: return Action::CheatNext;
					case 5: return Action::CheatPrevious;
					case 6: return Action::CheatCoins;
					case 7: return Action::CheatBird;
					case 8: return Action::CheatGuns;
					case 9: return Action::CheatLives;
					case 10: return Action::CheatGems;
					case 11: return Action::CheatPower;
					default: return Action::None;
				}
			}
			int item = 0;
			if (row == item++) return Action::Resume;
			if (_allowSaveLoad) {
				if (row == item++) return Action::Save;
				if (row == item++) return Action::Load;
			}
			if (_canToggleJoining && row == item++) return Action::ToggleJoining;
			return Action::QuitToMenu;
		}
		Death::Containers::StringView ItemLabel(int row) const
		{
			if (_testingMenu) {
				switch (row) {
					case 0: return nCine::PspDiagnosticsIsVisible() ? "Diagnostics: On"_s : "Diagnostics: Off"_s;
					case 1: return _flyEnabled ? "Fly: On"_s : "Fly: Off"_s;
					case 2: return _godEnabled ? "God: On"_s : "God: Off"_s;
					case 3:
						switch (_shieldType) {
							case ShieldType::Water: return "Shield: Water"_s;
							case ShieldType::Fire: return "Shield: Fire"_s;
							case ShieldType::Lightning: return "Shield: Lightning"_s;
							default: return "Shield: Off"_s;
						}
					case 4: return "Next Level"_s;
					case 5: return "Previous Level"_s;
					case 6: return "Coins +5"_s;
					case 7: return "Spawn Bird"_s;
					case 8: return "Full Ammo"_s;
					case 9: return "Lives +5"_s;
					case 10: return "Gems +5"_s;
					case 11: return "Power-up Guns"_s;
					default: return {};
				}
			}
			int item = 0;
			if (row == item++) return "Resume"_s;
			if (_allowSaveLoad) {
				if (row == item++) return "Save"_s;
				if (row == item++) return "Load"_s;
			}
			if (_canToggleJoining && row == item++)
				return (_joiningAllowed ? "Allow Joining: On"_s : "Allow Joining: Off"_s);
			return "Quit to Menu"_s;
		}

		void DrawTiledLayer(std::int32_t animId, float repeats, float scale, float angle,
			float panX, float panY, std::uint16_t z)
		{
			if (_menuMeta == nullptr) return;
			auto* res = _menuMeta->FindAnimation((AnimState)animId);
			if (res == nullptr || res->Base == nullptr || res->Base->TextureDiffuse == nullptr) return;
			auto* base = res->Base;
			const nCine::Vector2f size((float)base->FrameDimensions.X * scale, (float)base->FrameDimensions.Y * scale);
			const float cx = 240.0f + panX, cy = 136.0f + panY;
			DrawTexture(*base->TextureDiffuse, nCine::Vector2f(cx - size.X * 0.5f, cy - size.Y * 0.5f), z, size,
				nCine::Vector4f(repeats, 0.0f, repeats, 0.0f), nCine::Colorf::White, false, angle, 0);
		}

		void DrawBackground()
		{
			// Opaque, so it has to sit above every gameplay/HUD element, including the PSP peer marker.
			DrawSolid(nCine::Vector2f::Zero, 2000, nCine::Vector2f(480.0f, 272.0f),
				nCine::Colorf(0.04f, 0.02f, 0.08f, 1.0f), false);
			const float t = AnimTime;
			// Keep the original quad coverage, but sample the tiles closer to reduce PSP texture minification cost.
			DrawTiledLayer(110, 69.12f, 0.64f * 96.0f, t * -0.2f, 0.0f, 0.0f, 2001);
			DrawTiledLayer(111, 40.32f, 0.64f * 56.0f, t * 0.4f,
				96.0f * std::sin(t * 0.37f), 96.0f * std::cos(t * 0.31f), 2002);
			DrawTiledLayer(112, 14.4f, 0.8f * 20.0f, t * 0.3f,
				64.0f * std::sin(t * 0.25f), 64.0f * std::cos(t * 0.32f), 2003);
		}

		void DrawLabel(Jazz2::UI::Font* font, Death::Containers::StringView text, float x, float y, bool selected, float scale)
		{
			if (font == nullptr) return;
			std::int32_t co = 0;
			font->DrawString(this, text, co, x, y, 2020, Jazz2::UI::Alignment::Center,
				selected ? nCine::Colorf(444.0f, 444.0f, 444.0f, 1.0f) : Jazz2::UI::Font::DefaultColor,
				selected ? scale * 1.08f : scale, selected ? 0.7f : 0.0f,
				selected ? 1.0f : 0.0f, selected ? 1.0f : 0.0f, selected ? 0.4f : 0.0f);
		}

		Action _action = Action::None;
		int _row = 0;
		std::uint32_t _last = 0;
		char _message[64]{};
		bool _waitForRelease = false;
		bool _onlineSession = false;
		bool _allowSaveLoad = true;
		bool _canToggleJoining = false;
		bool _joiningAllowed = false;
		bool _testingMenu = false;
		bool _allowCheats = true;
		bool _flyEnabled = false;
		bool _godEnabled = false;
		ShieldType _shieldType = ShieldType::None;
		Metadata* _menuMeta = nullptr;
		std::vector<std::shared_ptr<nCine::AudioBufferPlayer>>& _sounds;
	};
}
