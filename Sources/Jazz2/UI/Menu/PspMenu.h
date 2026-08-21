#pragma once

#include "Jazz2/UI/Canvas.h"
#include "Jazz2/UI/Font.h"
#include "Jazz2/ContentResolver.h"
#include "Jazz2/PreferencesCache.h"
#include "Jazz2/PspSavedata.h"
#include "Jazz2/LevelInitialization.h"
#include "Jazz2/PlayerType.h"
#include "Jazz2/GameDifficulty.h"
#include "Jazz2/Multiplayer/Teams.h"
#include "nCine/Backends/Psp/PspAdhoc.h"
#include "nCine/Backends/Psp/PspDiagnostics.h"
#include "nCine/Primitives/Colorf.h"
#include "nCine/Primitives/Vector2.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

// PSP-native menu tree, drawn through the UI Canvas/Font path. Kept separate from the application and
// lifecycle orchestration in App/main.cpp.
namespace Jazz2::UI::Menu
{
	class PspMenu : public Jazz2::UI::Canvas
	{
	public:
		enum class StorageAction { None, Save, AutoSave, Load };

		// Lori only exists from The Secret Files (1.24) on - the demo, 1.20/1.23, Holiday Hare and Christmas
		// Chronicles have no sprites for her - so this keys on the baked assets, not on a version number.
		static bool IsLoriAvailable()
		{
			static const bool available = ReadLoriFlag();
			return available;
		}
		// psp-bake records what it actually baked in Cache/Source.version. A missing or older descriptor
		// defaults to available, i.e. behaves like the full game.
		static bool ReadLoriFlag()
		{
			auto file = fs::Open(fs::CombinePath(ContentResolver::Get().GetCachePath(), "Source.version"_s),
				FileAccess::Read);
			if (file == nullptr || !file->IsValid()) return true;
			char buffer[256];
			const std::int64_t read = file->Read(buffer, sizeof(buffer) - 1);
			if (read <= 0) return true;
			buffer[read] = '\0';
			const char* found = std::strstr(buffer, "lori=");
			return (found == nullptr || found[5] != '0');
		}
		// 0=Jazz, 1=Spaz, 2=Lori go on the wire in the ad-hoc handshake and roster: never renumber when Lori
		// is missing, only the count changes.
		static int CharacterCount() { return (IsLoriAvailable() ? 3 : 2); }
		// A preference saved by a full-game build can name Lori, which is out of range on a shareware build.
		static int ClampCharacter(int index) { return (index >= 0 && index < CharacterCount() ? index : 0); }

		// True while a Sony utility (OSK / network config) owns the screen; PspSystemUtilityDimOverlay dims under it.
		bool IsSystemUtilityBusy() const { return _oskBusy || _netconfBusy; }

		explicit PspMenu(PspSavedataManager* savedata, nCine::PspAdhoc& adhoc) : _savedata(savedata), _adhoc(adhoc)
		{
			SceCtrlData pad{};
			sceCtrlPeekBufferPositive(&pad, 1);
			_last = pad.Buttons; // Do not carry a held gameplay/transition button into a fresh menu.
			LoadSystemNickname();
			_playerFurColor = PreferencesCache::PlayerFurColor;
			EnumEpisodes();
			LoadLevelCatalog();
			_menuMeta = ContentResolver::Get().RequestMetadata("UI/MainMenu"_s,
				/*forceIndexed*/ false, /*forceStreamed*/ true);
			_music = ContentResolver::Get().GetMusic("menu.j2b"_s);
			if (_music != nullptr) {
				_music->setGain(PreferencesCache::MasterVolume * PreferencesCache::MusicVolume);
				_music->setSourceRelative(true);
				_music->setLooping(true);
				_music->play();
			}
		}
		~PspMenu() override
		{
			if (_utilityOwner == this) _utilityOwner = nullptr;
			if (_previewPaletteOffset >= 0) ContentResolver::Get().ReleasePaletteOffset(_previewPaletteOffset);
		}

		bool StartRequested() const { return _startRequested; }
		bool MultiplayerStartRequested() const { return _adhoc.HasStartAcknowledged(); }
		bool QuitRequested() const { return _quitRequested; }
		StorageAction RequestedStorageAction() const { return _storageAction; }
		void ClearStorageAction() { _storageAction = StorageAction::None; }
		bool RegenerateCacheRequested() const { return _regenerateCacheRequested; }
		void ClearRegenerateCacheRequest() { _regenerateCacheRequested = false; }
		void LatchInput() { SceCtrlData pad{}; sceCtrlPeekBufferPositive(&pad, 1); _last = pad.Buttons; _waitForRelease = true; }
		void ShowStorageResult(const char* message)
		{
			std::snprintf(_message, sizeof(_message), "%s", message);
			ApplyMenuMusicVolume();
		}
		void ShowMessage(const char* message) { ShowStorageResult(message); }
		void ShowNotice(const char* title, const char* message)
		{
			std::snprintf(_noticeTitle, sizeof(_noticeTitle), "%s", title);
			std::snprintf(_noticeMessage, sizeof(_noticeMessage), "%s", message);
			_screen = Screen::Notice;
			_row = 0;
			LatchInput();
			ApplyMenuMusicVolume();
		}
		void BeginHighscoreEntry(Death::Containers::StringView episode, std::uint32_t score)
		{
			std::memset(_entryName, 0, sizeof(_entryName));
			std::strcpy(_entryName, "JAZZ");
			std::snprintf(_entryEpisode, sizeof(_entryEpisode), "%.*s", std::min<int>((int)episode.size(), 11), episode.data());
			_entryScore = score;
			_screen = Screen::HighscoreEntry;
			_row = 0;
		}

		LevelInitialization BuildLevelInit() const
		{
			PlayerType pt = (_charIdx == 0 ? PlayerType::Jazz : _charIdx == 1 ? PlayerType::Spaz : PlayerType::Lori);
			GameDifficulty gd = (_diffIdx == 0 ? GameDifficulty::Easy : _diffIdx == 1 ? GameDifficulty::Normal : GameDifficulty::Hard);
			if (_customSelected && _customIdx >= 0 && _customIdx < (int)_customLevels.size()) {
				return LevelInitialization(Death::Containers::String("unknown/"_s + _customLevels[_customIdx].fileName), gd,
					/*isReforged*/ true, /*cheatsUsed*/ false, pt);
			}
			const EpisodeItem& ep = _episodes[_epIdx];
			return LevelInitialization(Death::Containers::String(ep.name + '/' + ep.firstLevel), gd, /*isReforged*/ true, /*cheatsUsed*/ false, pt);
		}

		LevelInitialization BuildMultiplayerLevelInit() const
		{
			const std::uint8_t character = _adhoc.GetLocalCharacter();
			const PlayerType playerType = (character == 0 ? PlayerType::Jazz : character == 1 ? PlayerType::Spaz : PlayerType::Lori);
			const std::uint8_t difficulty = _adhoc.GetSessionSettings().Difficulty;
			const GameDifficulty gameDifficulty = (difficulty == 0 ? GameDifficulty::Easy : difficulty == 2 ? GameDifficulty::Hard : GameDifficulty::Normal);
			return LevelInitialization(Death::Containers::String(_adhoc.GetSessionSettings().LevelName), gameDifficulty,
				/*isReforged*/ true, /*cheatsUsed*/ false, playerType);
		}

		bool OnDraw(nCine::RenderQueue& renderQueue) override
		{
			Jazz2::UI::Canvas::OnDraw(renderQueue);
			auto& r = ContentResolver::Get();
			Jazz2::UI::Font* fontM = r.GetFont(Resources::FontType::Medium);
			Jazz2::UI::Font* fontS = r.GetFont(Resources::FontType::Small);

			DrawBackground();
			DrawGameLogo();

			char buf[80];
			if (_screen == Screen::Main) {
				static constexpr const char* MainItems[] = { "New Game", "Load Game", "High Scores", "Options", "Quit" };
				for (int i = 0; i < 5; i++) DrawText(fontM, Death::Containers::StringView(MainItems[i]),
					240.0f, 84.0f + i * 31.0f, _row == i, MenuItemScale);
				if (_message[0] != '\0') DrawText(fontS, Death::Containers::StringView(_message), 240.0f, 238.0f, false, SecondaryTextScale);
				DrawHint(fontS, "Main Menu"_s);
				DrawBuildStamp(fontS);
			} else if (_screen == Screen::NewGame) {
				DrawText(fontM, "Single Player"_s, 120.0f, 137.0f, _row == 0, MenuItemScale);
				DrawText(fontM, "Party Mode"_s, 120.0f, 183.0f, _row == 1, MenuItemScale);
				DrawOriginalMenuArt("UI/multiplayer_mode.aura"_s, _row == 0 ? 0 : 1, 350.0f, 145.0f, 1.725f);
				DrawHint(fontS, "Choose a Game Type"_s);
			} else if (_screen == Screen::PartyGate) {
				const char* status = _adhoc.GetStatusText();
				const bool failed = (_adhoc.GetState() == nCine::PspAdhoc::State::Failed);
				const bool wlanOff = !nCine::PspAdhoc::IsWlanSwitchOn();
				if (wlanOff) {
					DrawWigglyText(fontM, "Turn on the WLAN switch"_s, 240.0f, 137.0f,
						nCine::Colorf(1.0f, 0.72f, 0.72f, 1.0f), MenuItemScale);
				} else {
					DrawWigglyText(fontM, failed ? Death::Containers::StringView(status) : "Connecting to ad-hoc..."_s,
						240.0f, 137.0f, failed ? nCine::Colorf(1.0f, 0.65f, 0.65f, 1.0f) : nCine::Colorf::White, MenuItemScale);
				}
				DrawHint(fontS, "Party Mode"_s);
			} else if (_screen == Screen::PartySelect) {
				DrawText(fontM, "Join Game"_s, 240.0f, 132.0f, _row == 0, MenuItemScale);
				DrawTextDisabled(fontM, "Host Game"_s, 240.0f, 176.0f, !_savedata->IsInfrastructureMode(),
					_row == 1, MenuItemScale);
				DrawHint(fontS, "Choose Party Mode"_s);
			} else if (_screen == Screen::JoinGame) {
				DrawText(fontS, "Host"_s, 56.0f, 96.0f, false, TableHeadingScale);
				DrawText(fontS, "Mode"_s, 170.0f, 96.0f, false, TableHeadingScale);
				DrawText(fontS, "Level"_s, 310.0f, 96.0f, false, TableHeadingScale);
				DrawText(fontS, "Players"_s, 426.0f, 96.0f, false, TableHeadingScale);
				if (_adhoc.GetDiscoveredHostCount() == 0) {
					if (_adhoc.GetState() == nCine::PspAdhoc::State::Connected)
						DrawWigglyText(fontS, "Searching..."_s, 240.0f, 145.0f, nCine::Colorf::White, ContentScale);
				} else {
					constexpr int VisibleHosts = 5;
					const int hostCount = _adhoc.GetDiscoveredHostCount();
					const int first = std::clamp(_joinHostIdx - VisibleHosts / 2, 0, std::max(0, hostCount - VisibleHosts));
					for (int i = first; i < std::min(first + VisibleHosts, hostCount); ++i) {
						const auto& host = _adhoc.GetDiscoveredHost(i);
						const float y = 123.0f + (i - first) * 29.0f;
						const bool enabled = (host.PlayerCount < host.MaxPlayerCount);
						const bool selected = (_joinHostIdx == i);
						if (enabled && selected) DrawSelectionGlowSized(240.0f, y, nCine::Vector2f(432.0f, 24.0f), 0.28f, 90);
						DrawTableCell(fontS, Death::Containers::StringView(host.Name), 56.0f, y, enabled, selected);
						DrawTableCell(fontS, Death::Containers::StringView(ModeName(host.Settings.GameMode)), 170.0f, y, enabled, selected);
						DrawTableCell(fontS, Death::Containers::StringView(host.Settings.LevelDisplayName), 310.0f, y, enabled, selected);
						std::snprintf(buf, sizeof(buf), "%u/%u", host.PlayerCount, host.MaxPlayerCount);
						DrawTableCell(fontS, Death::Containers::StringView(buf), 426.0f, y, enabled, selected);
					}
				}
				DrawHint(fontS, "Join Game"_s);
			} else if (_screen == Screen::MultiplayerCharacter) {
				const bool joining = _multiplayerJoining;
				std::snprintf(buf, sizeof(buf), "Player: %s", _playerName);
				DrawText(fontM, Death::Containers::StringView(buf), 302.0f, 50.0f, false, SectionHeadingScale);
				if (joining && _adhoc.GetDiscoveredHostCount() > 0) {
					const auto& host = _adhoc.GetDiscoveredHost(std::min(_joinHostIdx, _adhoc.GetDiscoveredHostCount() - 1));
					std::snprintf(buf, sizeof(buf), "Game: %s", host.Name);
					DrawText(fontS, Death::Containers::StringView(buf), 302.0f, 72.0f, false, DenseContentScale);
				}
				DrawCharacterChoices(fontM, _multiplayerCharIdx, 108.0f, _mpSettingsRow == 0);
				if (MultiplayerTeamModeSelected()) DrawTeamChoices(fontS, 156.0f);
				else DrawColorChoices(fontS, 156.0f);
				DrawShootingPreview(_multiplayerCharIdx, 365.0f, 153.0f, 1.15f);
				const char* cta = (!joining ? "Open Lobby" : (_selectedHostRunning ? "Join Game" : "Join Lobby"));
				DrawText(fontM, Death::Containers::StringView(cta), 240.0f, PrimaryActionY, _mpSettingsRow == 2, MenuItemScale);
				DrawHint(fontS, "Character Settings"_s);
			} else if (_screen == Screen::HostMode) {
				static constexpr const char* modes[] = { "Cooperation", "Battle", "Race", "Treasure Hunt", "Capture the Flag" };
				for (int i = 0; i < 5; ++i) DrawTextLeft(fontM, Death::Containers::StringView(modes[i]), 47.0f,
					85.0f + i * 34.0f, _row == i, MenuItemScale);
				DrawModeArt(349.0f, 145.0f);
				DrawHint(fontS, "Choose Game Mode"_s);
			} else if (_screen == Screen::HostSettings) {
				const bool hasLevel = HasFeasibleHostLevel();
				const char* levelName = (hasLevel ? _hostLevels[_hostLevelIdx].displayName.data() : "No compatible levels");
				std::snprintf(buf, sizeof(buf), "Level:  %s", levelName);
				DrawTextDisabled(fontM, Death::Containers::StringView(buf), 240.0f, 99.0f, hasLevel, _row == 0, MenuItemScale);
				DrawModeSettings(fontM, buf);
				DrawTextDisabled(fontM, "Continue"_s, 240.0f, PrimaryActionY, hasLevel,
					_row == HostSettingsRowCount() - 1, MenuItemScale);
				DrawHint(fontS, "Game Settings"_s);
			} else if (_screen == Screen::HostLevels) {
				DrawHostLevels(fontS);
			} else if (_screen == Screen::Lobby) {
				const bool hosting = !_multiplayerJoining;
				DrawSolid(nCine::Vector2f(239.0f, 92.0f), 80, nCine::Vector2f(2.0f, 124.0f), nCine::Colorf(0.65f, 0.45f, 0.85f, 0.7f), false);
				DrawText(fontM, "Players"_s, 120.0f, 99.0f, false, SectionHeadingScale);
				DrawText(fontM, "Game Settings"_s, 355.0f, 99.0f, false, SectionHeadingScale);
				// The host builds the roster and pushes it over the lobby stream, so guests see each other too.
				const int rosterCount = _adhoc.GetRosterCount();
				if (rosterCount > 0) {
					float rowY = 129.0f;
					for (int i = 0; i < rosterCount; ++i) {
						const auto& entry = _adhoc.GetRosterEntry(i);
						std::snprintf(buf, sizeof(buf), "%s%s  %s  %s", entry.Name, entry.IsHost ? " (Host)" : "",
							CharName(entry.Character), entry.Ready ? "Ready" : "Not Ready");
						// DrawText() hard-codes Font::DefaultColor, so tint the row by team via the font directly.
						const nCine::Colorf rowColor = (entry.Team == nCine::PspAdhoc::NoAdhocTeam
							? Jazz2::UI::Font::DefaultColor : TeamTextColor(entry.Team));
						if (fontS != nullptr) {
							std::int32_t rowOffset = 0;
							fontS->DrawString(this, Death::Containers::StringView(buf), rowOffset, 120.0f, rowY, 100,
								Jazz2::UI::Alignment::Center, rowColor, DenseContentScale, 0.0f, 0.0f, 1.0f, 0.3f);
						}
						rowY += 24.0f;
					}
					if (rosterCount < 2) {
						DrawWigglyText(fontS, "Waiting for players..."_s, 120.0f, rowY, nCine::Colorf::White, DenseContentScale);
					}
				} else {
					DrawWigglyText(fontS, "Waiting for players..."_s, 120.0f, 129.0f, nCine::Colorf::White, DenseContentScale);
				}
				const auto& settings = _adhoc.GetSessionSettings();
				DrawLobbySettings(fontS, settings);
				// A fresh match needs at least one ready guest; hot-join advertising is a separate path.
				const bool canStart = hosting && _adhoc.CanStartGame() && _adhoc.HasReadyGuest();
				DrawTextDisabled(fontM, hosting ? "Start Game"_s : (_adhoc.IsLocalReady() ? "Not Ready"_s : "Ready"_s),
					240.0f, PrimaryActionY, canStart || !hosting, true, MenuItemScale);
				DrawHint(fontS, "Lobby"_s);
			} else if (_screen == Screen::Episodes) {
				if (_episodes.empty()) {
					DrawText(fontS, "No episodes found"_s, 240.0f, 130.0f, false, ContentScale);
				} else {
					const float centerY = 145.0f, pitch = 58.0f;
					for (int i = 0; i < (int)_episodes.size(); i++) {
						const float y = centerY + (float)(i - _row) * pitch;
						if (y < 78.0f || y > 226.0f) continue;
						const bool sel = (i == _row);
						EpisodeItem& ep = _episodes[i];
						LoadEpisodeTitle(ep);
						if (ep.titleImage != nullptr) {
							constexpr float SelectedTitleScale = 0.78f;
							if (sel) {
								const nCine::Vector2i sourceSize = ep.titleImage->GetSize();
								const nCine::Vector2f selectedSize(sourceSize.X * SelectedTitleScale, sourceSize.Y * SelectedTitleScale);
								DrawSelectionGlowSized(130.0f, y,
									nCine::Vector2f(selectedSize.X * 1.2f, selectedSize.Y * 1.25f), 0.55f, 90);
							}
							DrawImage(ep.titleImage.get(), 130.0f, y, sel ? SelectedTitleScale : 0.54f,
								sel ? nCine::Colorf::White : nCine::Colorf(0.48f, 0.48f, 0.55f, 0.76f));
						} else {
							DrawText(fontS, ep.displayName, 130.0f, y, sel, sel ? SelectedListScale : ListContentScale);
						}
					}
					if (_episodePreview != nullptr) {
						const nCine::Vector2i size = _episodePreview->GetSize();
						const float fit = std::min(1.0f, std::min(200.0f / size.X, 233.0f / size.Y));
						const float eased = _episodePreviewScale * _episodePreviewScale * (3.0f - 2.0f * _episodePreviewScale);
						DrawImage(_episodePreview.get(), 370.0f, 136.0f, fit * eased, nCine::Colorf::White, 100);
					}
					if (_savedata->IsEpisodeCompleted(_episodes[_row].name))
						std::snprintf(buf, sizeof(buf), "Completed  -  %d / %d", _row + 1, (int)_episodes.size());
					else std::snprintf(buf, sizeof(buf), "%d / %d", _row + 1, (int)_episodes.size());
					DrawText(fontS, buf, 130.0f, 241.0f, false, SecondaryTextScale);
				}
				DrawHint(fontS, "Choose Episode"_s);
			} else if (_screen == Screen::CustomLevels) {
				DrawCustomLevels(fontS);
			} else if (_screen == Screen::CharacterSelect) {
				// Centre whatever characters this build actually has, rather than leaving a hole where Lori was
				const int charCount = CharacterCount();
				const float charSpacing = 140.0f;
				const float charFirstX = 240.0f - (charCount - 1) * charSpacing * 0.5f;
				for (int i = 0; i < charCount; ++i) {
					DrawCharacterSelectionArt(i, charFirstX + i * charSpacing, 137.0f, i == _charIdx ? 0.8f : 0.72f);
					DrawText(fontM, Death::Containers::StringView(CharName(i)), charFirstX + i * charSpacing, 224.0f, i == _charIdx, MenuItemScale);
				}
				DrawHint(fontS, "Choose Character"_s);
			} else if (_screen == Screen::Difficulty) {
				DrawDifficultyArt(_episodeForHost ? 0 : _charIdx, _diffIdx, 135.0f, 145.0f, 1.0f);
				for (int i = 0; i < 3; ++i) DrawText(fontM, Death::Containers::StringView(DiffName(i)), 349.0f,
					101.0f + i * 42.0f, _diffIdx == i, MenuItemScale);
				DrawHint(fontS, "Select Difficulty"_s);
			} else if (_screen == Screen::Options) {
				const float values[] = { PreferencesCache::SfxVolume, PreferencesCache::MusicVolume };
				const char* labels[] = { "SFX Volume", "Music Volume" };
				for (int i = 0; i < 2; i++) {
					std::snprintf(buf, sizeof(buf), "%s   < %3d%% >", labels[i], (int)std::lround(values[i] * 100.0f));
					DrawText(fontM, Death::Containers::StringView(buf), 240.0f, 69.0f + i * 30.0f, _row == i, MenuItemScale);
				}
				// Labelled "PPSSPP", not "Infrastructure": the mode is PSP infrastructure networking, but the only
				// thing it is ever used for is reaching a PPSSPP instance running its normal Ad Hoc mode.
				std::snprintf(buf, sizeof(buf), "Network   < %s >", _savedata->IsInfrastructureMode() ? "PPSSPP" : "Ad Hoc");
				DrawText(fontM, Death::Containers::StringView(buf), 240.0f, 129.0f, _row == 2, MenuItemScale);
				std::snprintf(buf, sizeof(buf), "Server   %s", _savedata->GetAemuServer());
				DrawText(fontM, Death::Containers::StringView(buf), 240.0f, 159.0f, _row == 3, MenuItemScale);
				DrawText(fontM, "Regenerate Asset Cache"_s, 240.0f, 189.0f, _row == 4, MenuItemScale);
				DrawText(fontM, "Save Preferences"_s, 240.0f, 222.0f, _row == 5, MenuItemScale);
				if (_message[0] != '\0') DrawText(fontS, Death::Containers::StringView(_message), 240.0f, 245.0f, false, SecondaryTextScale);
				DrawHint(fontS, "Options"_s);
			} else if (_screen == Screen::Highscores) {
				const auto& profile = _savedata->Profile();
				// Fixed-x columns like the Join screen; one centre-justified string never lines up.
				DrawText(fontS, "Name"_s, 118.0f, 96.0f, false, TableHeadingScale);
				DrawText(fontS, "Episode"_s, 288.0f, 96.0f, false, TableHeadingScale);
				DrawText(fontS, "Score"_s, 418.0f, 96.0f, false, TableHeadingScale);
				if (profile.highscoreCount == 0) {
					DrawText(fontS, "No highscores recorded yet"_s, 240.0f, 150.0f, false, ContentScale);
				} else {
					for (int i = 0; i < profile.highscoreCount; i++) {
						const auto& entry = profile.highscores[i];
						const float y = 123.0f + i * 24.0f;
						// level[0..11] is the player name, level[12..23] the episode it was set on.
						std::snprintf(buf, sizeof(buf), "%d. %.11s", i + 1, entry.level);
						DrawTableCell(fontS, Death::Containers::StringView(buf), 118.0f, y, true, false);
						DrawTableCell(fontS, Death::Containers::StringView(entry.level + 12), 288.0f, y, true, false);
						std::snprintf(buf, sizeof(buf), "%u", entry.score);
						DrawTableCell(fontS, Death::Containers::StringView(buf), 418.0f, y, true, false);
					}
				}
				DrawHint(fontS, "High Scores"_s);
			} else if (_screen == Screen::Notice) {
				DrawText(fontM, Death::Containers::StringView(_noticeTitle), 240.0f, 72.0f, false, 0.92f);
				DrawText(fontS, Death::Containers::StringView(_noticeMessage), 240.0f, 137.0f, false, ContentScale);
				DrawText(fontM, "OK"_s, 240.0f, 190.0f, true, MenuItemScale);
				DrawHint(fontS, Death::Containers::StringView(_noticeTitle));
			} else { // HighscoreEntry
				DrawText(fontM, "New High Score"_s, 240.0f, 62.0f, false, SectionHeadingScale);
				std::snprintf(buf, sizeof(buf), "%s   %u", _entryEpisode, _entryScore);
				DrawText(fontS, Death::Containers::StringView(buf), 240.0f, 100.0f, false, ContentScale);
				std::snprintf(buf, sizeof(buf), "Name:  %s", _entryName);
				DrawText(fontM, Death::Containers::StringView(buf), 240.0f, 150.0f, _row == 0, MenuItemScale);
				DrawText(fontM, "Save Score"_s, 240.0f, PrimaryActionY, _row == 1, MenuItemScale);
				DrawHint(fontS, "New High Score"_s);
			}
			return true;
		}

		void OnUpdate(float timeMult) override
		{
			// Sony utilities own the pad while this Canvas is suspended, and SceneNode's own early return is not
			// enough: without this guard the rest of this override still eats the same button presses.
			if (!isUpdateEnabled()) return;
			Jazz2::UI::Canvas::OnUpdate(timeMult);
			UpdateEpisodePreview(timeMult);
			if (_screen == Screen::CharacterSelect || _screen == Screen::MultiplayerCharacter) {
				const int selected = (_screen == Screen::CharacterSelect ? _charIdx : _multiplayerCharIdx);
				for (int i = 0; i < 3; ++i) {
					const float target = (i == selected ? 1.0f : 0.0f);
					const float delta = timeMult * 0.12f;
					_characterAnim[i] += std::clamp(target - _characterAnim[i], -delta, delta);
				}
			}
			if (_screen == Screen::PartyGate) {
				_partyRetryFrames += timeMult;
				if (!_savedata->IsInfrastructureMode() && nCine::PspAdhoc::IsWlanSwitchOn() &&
					_adhoc.GetState() == nCine::PspAdhoc::State::Failed && _partyRetryFrames >= 30.0f) {
					_adhoc.Stop();
					_adhoc.Start();
					_partyRetryFrames = 0.0f;
				}
			}
			SceCtrlData pad;
			sceCtrlReadBufferPositive(&pad, 1);
			std::uint32_t now = pad.Buttons;
			if (pad.Ly < 64) now |= PSP_CTRL_UP; else if (pad.Ly > 192) now |= PSP_CTRL_DOWN;
			if (pad.Lx < 64) now |= PSP_CTRL_LEFT; else if (pad.Lx > 192) now |= PSP_CTRL_RIGHT;
			if (_waitForRelease) {
				_last = now;
				if ((now & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE | PSP_CTRL_START)) == 0) _waitForRelease = false;
				return;
			}
			const std::uint32_t hit = now & ~_last;
			_last = now;

			auto moveRow = [&](int count) {
				if (count <= 0) return;
				const int old = _row;
				if (hit & PSP_CTRL_UP) _row = (_row + count - 1) % count;
				if (hit & PSP_CTRL_DOWN) _row = (_row + 1) % count;
				if (_row != old) PlayMenuSfx(0.5f);
			};

			if (_screen == Screen::Main) {
				moveRow(5);
				if (hit & PSP_CTRL_CROSS) {
					PlayMenuSfx(0.6f);
					if (_row == 0) { _screen = Screen::NewGame; _row = 0; }
					else if (_row == 1) { _storageAction = StorageAction::Load; _message[0] = '\0'; }
					else if (_row == 2) { _screen = Screen::Highscores; _row = 0; }
					else if (_row == 3) { _screen = Screen::Options; _row = 0; _message[0] = '\0'; }
					else { _quitRequested = true; }
				}
			} else if (_screen == Screen::NewGame) {
				if (hit & (PSP_CTRL_LEFT | PSP_CTRL_UP)) { _row = 0; PlayMenuSfx(0.5f); }
				if (hit & (PSP_CTRL_RIGHT | PSP_CTRL_DOWN)) { _row = 1; PlayMenuSfx(0.5f); }
				if (hit & PSP_CTRL_CROSS) {
					PlayMenuSfx(0.6f);
					if (_row == 0) { _episodeForHost = false; if (!_episodes.empty()) OpenEpisodeBrowser(0); }
					else {
						_screen = Screen::PartyGate;
						_partyRetryFrames = 0;
						if (_savedata->IsInfrastructureMode()) BeginNetworkProfileSelection();
						else _adhoc.Start();
					}
				}
				if (hit & PSP_CTRL_CIRCLE) { PlayMenuSfx(0.5f); _screen = Screen::Main; _row = 0; }
			} else if (_screen == Screen::PartyGate) {
				if (_adhoc.GetState() == nCine::PspAdhoc::State::Connected) { _screen = Screen::PartySelect; _row = 0; }
				if ((hit & PSP_CTRL_CROSS) && _adhoc.GetState() == nCine::PspAdhoc::State::Failed) {
					if (_savedata->IsInfrastructureMode()) BeginNetworkProfileSelection();
					else { _adhoc.Stop(); _adhoc.Start(); }
					_partyRetryFrames = 0;
				}
				if (hit & PSP_CTRL_CIRCLE) { PlayMenuSfx(0.5f); _adhoc.Stop(); _screen = Screen::NewGame; _row = 1; }
			} else if (_screen == Screen::PartySelect) {
				if (_savedata->IsInfrastructureMode()) _row = 0;
				else moveRow(2);
				if ((hit & PSP_CTRL_CROSS) && (_row == 0 || !_savedata->IsInfrastructureMode())) {
					PlayMenuSfx(0.6f);
					if (_row == 0) {
						_adhoc.BeginDiscovery();
						_screen = Screen::JoinGame;
					} else {
						_screen = Screen::HostMode;
					}
					_row = 0;
				}
				if (hit & PSP_CTRL_CIRCLE) { PlayMenuSfx(0.5f); _adhoc.Stop(); _screen = Screen::NewGame; _row = 1; }
			} else if (_screen == Screen::JoinGame) {
				if (_adhoc.GetDiscoveredHostCount() > 0) {
					_joinHostIdx = std::min(_joinHostIdx, _adhoc.GetDiscoveredHostCount() - 1);
					if (hit & PSP_CTRL_UP) { _joinHostIdx = (_joinHostIdx + _adhoc.GetDiscoveredHostCount() - 1) % _adhoc.GetDiscoveredHostCount(); PlayMenuSfx(0.5f); }
					if (hit & PSP_CTRL_DOWN) { _joinHostIdx = (_joinHostIdx + 1) % _adhoc.GetDiscoveredHostCount(); PlayMenuSfx(0.5f); }
					if ((hit & PSP_CTRL_CROSS) && _adhoc.GetDiscoveredHost(_joinHostIdx).PlayerCount < _adhoc.GetDiscoveredHost(_joinHostIdx).MaxPlayerCount) {
						_multiplayerJoining = true;
						_selectedHostRunning = _adhoc.GetDiscoveredHost(_joinHostIdx).IsRunning;
						_screen = Screen::MultiplayerCharacter;
						_mpSettingsRow = 0;
						PlayMenuSfx(0.6f);
					}
				}
				if (hit & PSP_CTRL_CIRCLE) {
					PlayMenuSfx(0.5f);
					_adhoc.EndDiscovery();
					_row = 0;
					_screen = Screen::PartySelect;
				}
			} else if (_screen == Screen::HostMode) {
				moveRow(5);
				if (hit & PSP_CTRL_CROSS) {
					_hostModeIdx = _row;
					_multiplayerCharIdx = ClampCharacter(_multiplayerCharIdx);
					PlayMenuSfx(0.6f);
					_episodeForHost = (CurrentHostMode() == HostMode::Cooperation);
					if (_episodeForHost) {
						if (!_episodes.empty()) OpenEpisodeBrowser(0);
					} else {
						SelectFirstFeasibleHostLevel();
						_hostLevelFromSettings = false;
						_screen = Screen::HostSettings;
						_row = 0;
					}
				}
				if (hit & PSP_CTRL_CIRCLE) { PlayMenuSfx(0.5f); _screen = Screen::PartySelect; _row = 1; }
			} else if (_screen == Screen::HostSettings) {
				moveRow(HostSettingsRowCount());
				if (_row == 0 && HasFeasibleHostLevel() && (hit & PSP_CTRL_CROSS)) {
					_row = CurrentFeasibleHostLevelRow();
					_hostLevelFromSettings = true;
					_screen = Screen::HostLevels;
					PlayMenuSfx(0.6f);
					_last = now;
					return;
				}
				AdjustHostSetting(hit | ((_row > 0 && _row < HostSettingsRowCount() - 1 && (hit & PSP_CTRL_CROSS))
					? PSP_CTRL_RIGHT : 0));
				if ((hit & PSP_CTRL_CROSS) && _row == HostSettingsRowCount() - 1 && HasFeasibleHostLevel()) {
					PlayMenuSfx(0.6f);
					_multiplayerJoining = false;
					_screen = Screen::MultiplayerCharacter;
					_mpSettingsRow = 0;
					_row = 0;
				}
				if (hit & PSP_CTRL_CIRCLE) { PlayMenuSfx(0.5f); _screen = Screen::HostMode; _row = _hostModeIdx; }
			} else if (_screen == Screen::HostLevels) {
				const int count = FeasibleHostLevelCount();
				moveRow(count);
				if ((hit & PSP_CTRL_CROSS) && count > 0) {
					_hostLevelIdx = NthFeasibleHostLevelIndex(_row);
					_screen = Screen::HostSettings;
					_row = 0;
					PlayMenuSfx(0.6f);
				}
				if (hit & PSP_CTRL_CIRCLE) {
					PlayMenuSfx(0.5f);
					_screen = (_hostLevelFromSettings ? Screen::HostSettings : Screen::HostMode);
					_row = (_hostLevelFromSettings ? 0 : _hostModeIdx);
				}
			} else if (_screen == Screen::MultiplayerCharacter) {
				if (_multiplayerJoining && _adhoc.IsSessionRunning() &&
					_adhoc.GetPeerState() != nCine::PspAdhoc::PeerState::Disconnected) {
					if (hit & PSP_CTRL_CIRCLE) {
						PlayMenuSfx(0.5f);
						_adhoc.BeginDiscovery();
						_screen = Screen::JoinGame;
						_row = 0;
					}
					return;
				}
				if (hit & PSP_CTRL_UP) { _mpSettingsRow = (_mpSettingsRow + 2) % 3; PlayMenuSfx(0.5f); }
				if (hit & PSP_CTRL_DOWN) { _mpSettingsRow = (_mpSettingsRow + 1) % 3; PlayMenuSfx(0.5f); }
				if (_mpSettingsRow == 0) {
					if (hit & PSP_CTRL_LEFT) { _multiplayerCharIdx = (_multiplayerCharIdx + CharacterCount() - 1) % CharacterCount(); ResetPlayerColor(); PlayMenuSfx(0.5f); }
					if (hit & PSP_CTRL_RIGHT) { _multiplayerCharIdx = (_multiplayerCharIdx + 1) % CharacterCount(); ResetPlayerColor(); PlayMenuSfx(0.5f); }
				} else if (_mpSettingsRow == 1) {
					if (MultiplayerTeamModeSelected()) {
						if (hit & PSP_CTRL_LEFT) { CycleTeam(-1); PlayMenuSfx(0.5f); }
						if (hit & PSP_CTRL_RIGHT) { CycleTeam(1); PlayMenuSfx(0.5f); }
					} else {
						if (hit & PSP_CTRL_LEFT) { _colorSection = (_colorSection + 4) % 5; PlayMenuSfx(0.5f); }
						if (hit & PSP_CTRL_RIGHT) { _colorSection = (_colorSection + 1) % 5; PlayMenuSfx(0.5f); }
					}
				}
				if (hit & PSP_CTRL_CROSS) {
					if (_mpSettingsRow == 0) {
						_multiplayerCharIdx = (_multiplayerCharIdx + 1) % CharacterCount();
						ResetPlayerColor();
						PlayMenuSfx(0.5f);
						_last = now;
						return;
					}
					if (_mpSettingsRow == 1) {
						if (MultiplayerTeamModeSelected()) CycleTeam(1);
						else CycleFurSection(_colorSection);
						PlayMenuSfx(0.5f);
						_last = now;
						return;
					}
					PreferencesCache::PlayerFurColor = CurrentPlayerColor();
					if (!_multiplayerJoining) {
						nCine::PspAdhoc::SessionSettings settings{};
						settings.GameMode = EffectiveGameMode();
						settings.Difficulty = static_cast<std::uint8_t>(_diffIdx);
						settings.AllowMinimap = false;
						settings.TotalKills = static_cast<std::uint32_t>(_totalKills);
						settings.TotalLaps = static_cast<std::uint32_t>(_totalLaps);
						settings.TotalTreasureCollected = static_cast<std::uint32_t>(_totalTreasure);
						settings.MaxGameTimeSecs = static_cast<std::uint32_t>(_maxTimeMinutes * 60);
						settings.OvertimeSecs = static_cast<std::uint32_t>(_overtimeSeconds);
						std::snprintf(settings.LevelName, sizeof(settings.LevelName), "%s", _hostLevels[_hostLevelIdx].name.data());
						std::snprintf(settings.LevelDisplayName, sizeof(settings.LevelDisplayName), "%s", _hostLevels[_hostLevelIdx].displayName.data());
						_adhoc.SetLocalFurColor(PreferencesCache::PlayerFurColor);
						_adhoc.SetLocalTeam(MultiplayerTeamModeSelected() ? _multiplayerTeam : nCine::PspAdhoc::NoAdhocTeam);
						_adhoc.BeginHosting(_playerName, static_cast<std::uint8_t>(_multiplayerCharIdx), settings);
						_adhoc.SetLocalReady(true);
						_screen = Screen::Lobby;
						PlayMenuSfx(0.6f);
					} else if (_joinHostIdx < _adhoc.GetDiscoveredHostCount() &&
						(_adhoc.SetLocalFurColor(PreferencesCache::PlayerFurColor), true) &&
						(_adhoc.SetLocalTeam(MultiplayerTeamModeSelected() ? _multiplayerTeam
							: nCine::PspAdhoc::NoAdhocTeam), true) &&
						_adhoc.ConnectToHost(_joinHostIdx, _playerName, static_cast<std::uint8_t>(_multiplayerCharIdx))) {
						if (!_adhoc.IsSessionRunning()) { _adhoc.SetLocalReady(false); _screen = Screen::Lobby; }
						PlayMenuSfx(0.6f);
					}
				}
				if (hit & PSP_CTRL_CIRCLE) { PlayMenuSfx(0.5f); _screen = (_multiplayerJoining ? Screen::JoinGame : (CurrentHostMode() == HostMode::Cooperation ? Screen::Difficulty : Screen::HostSettings)); _row = 0; }
			} else if (_screen == Screen::Lobby) {
				if (!_multiplayerJoining && (hit & PSP_CTRL_CROSS) && _adhoc.CanStartGame() && _adhoc.HasReadyGuest() && _adhoc.RequestStartGame()) PlayMenuSfx(0.6f);
				if (_multiplayerJoining && (hit & PSP_CTRL_CROSS)) { _adhoc.SetLocalReady(!_adhoc.IsLocalReady()); PlayMenuSfx(0.55f); }
				if (hit & PSP_CTRL_CIRCLE) {
					const bool hosting = !_multiplayerJoining;
					PlayMenuSfx(0.5f);
					_adhoc.EndDiscovery();
					if (hosting) _screen = Screen::MultiplayerCharacter;
					else { _adhoc.BeginDiscovery(); _screen = Screen::JoinGame; }
					_row = 0;
				}
			} else if (_screen == Screen::Episodes) {
				const int previousEpisode = _row;
				moveRow((int)_episodes.size());
				if (_row != previousEpisode) QueueEpisodePreview(_row);
				if ((hit & PSP_CTRL_CROSS) && !_episodes.empty()) {
					PlayMenuSfx(0.6f);
					_epIdx = _row;
					CloseEpisodePreview();
					if (_episodes[_epIdx].customLevels) {
						_screen = Screen::CustomLevels;
						_row = std::max(0, _customIdx);
					} else {
						_customSelected = false;
						if (_episodeForHost) SelectCooperationLevel();
						_screen = (_episodeForHost ? Screen::Difficulty : Screen::CharacterSelect);
						_row = (_episodeForHost ? _diffIdx : 0);
					}
				}
				if (hit & PSP_CTRL_CIRCLE) { PlayMenuSfx(0.5f); CloseEpisodePreview(); _screen = (_episodeForHost ? Screen::HostMode : Screen::NewGame); _row = (_episodeForHost ? _hostModeIdx : 0); }
			} else if (_screen == Screen::CustomLevels) {
				moveRow((int)_customLevels.size());
				if ((hit & PSP_CTRL_CROSS) && !_customLevels.empty()) {
					PlayMenuSfx(0.6f);
					_customIdx = _row;
					_customSelected = true;
					if (_episodeForHost) SelectCooperationLevel();
					_screen = (_episodeForHost ? Screen::Difficulty : Screen::CharacterSelect);
					_row = (_episodeForHost ? _diffIdx : 0);
				}
				if (hit & PSP_CTRL_CIRCLE) { PlayMenuSfx(0.5f); OpenEpisodeBrowser(_epIdx); }
			} else if (_screen == Screen::CharacterSelect) {
				if (hit & PSP_CTRL_LEFT) { _charIdx = (_charIdx + CharacterCount() - 1) % CharacterCount(); PlayMenuSfx(0.5f); }
				if (hit & PSP_CTRL_RIGHT) { _charIdx = (_charIdx + 1) % CharacterCount(); PlayMenuSfx(0.5f); }
				if (hit & PSP_CTRL_CROSS) { PlayMenuSfx(0.6f); _screen = Screen::Difficulty; }
				if (hit & PSP_CTRL_CIRCLE) {
					PlayMenuSfx(0.5f);
					if (_customSelected) { _screen = Screen::CustomLevels; _row = _customIdx; }
					else OpenEpisodeBrowser(_epIdx);
				}
			} else if (_screen == Screen::Difficulty) {
				if (hit & PSP_CTRL_UP) { _diffIdx = (_diffIdx + 2) % 3; PlayMenuSfx(0.5f); }
				if (hit & PSP_CTRL_DOWN) { _diffIdx = (_diffIdx + 1) % 3; PlayMenuSfx(0.5f); }
				if (hit & PSP_CTRL_CROSS) {
					PlayMenuSfx(0.6f);
					if (_episodeForHost) { _multiplayerJoining = false; _screen = Screen::MultiplayerCharacter; _mpSettingsRow = 0; }
					else _startRequested = true;
				}
				if (hit & PSP_CTRL_CIRCLE) {
					PlayMenuSfx(0.5f);
					if (_episodeForHost) OpenEpisodeBrowser(_epIdx);
					else _screen = Screen::CharacterSelect;
				}
			} else if (_screen == Screen::Options) {
				moveRow(6);
				if (_row < 2 && (hit & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_CROSS))) {
					float* value = (_row == 0 ? &PreferencesCache::SfxVolume : &PreferencesCache::MusicVolume);
					const float delta = (hit & PSP_CTRL_LEFT) ? -0.05f : 0.05f;
					*value = std::clamp(*value + delta, 0.0f, 1.0f);
					ApplyMenuMusicVolume();
					PlayMenuSfx(0.45f);
					_message[0] = '\0';
				}
				if (_row == 2 && (hit & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_CROSS))) {
					_savedata->SetInfrastructureMode(!_savedata->IsInfrastructureMode());
					PlayMenuSfx(0.5f);
					_message[0] = '\0';
				}
				if ((hit & PSP_CTRL_CROSS) && _row == 3) BeginServerAddressEdit();
				if ((hit & PSP_CTRL_CROSS) && _row == 4) { PlayMenuSfx(0.6f); _regenerateCacheRequested = true; }
				if ((hit & PSP_CTRL_CROSS) && _row == 5) { PlayMenuSfx(0.6f); _storageAction = StorageAction::Save; }
				if (hit & PSP_CTRL_CIRCLE) { PlayMenuSfx(0.5f); _screen = Screen::Main; _row = 3; _message[0] = '\0'; }
			} else if (_screen == Screen::Highscores) {
				if (hit & PSP_CTRL_CIRCLE) { PlayMenuSfx(0.5f); _screen = Screen::Main; _row = 2; }
			} else if (_screen == Screen::Notice) {
				if (hit & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE)) {
					PlayMenuSfx(0.5f);
					_screen = Screen::Main;
					_row = 1;
				}
			} else { // HighscoreEntry
				if (hit & (PSP_CTRL_UP | PSP_CTRL_DOWN)) { _row ^= 1; PlayMenuSfx(0.5f); }
				if (hit & PSP_CTRL_CROSS) {
					if (_row == 0) {
						BeginHighscoreNameEdit();
					} else {
						if (_entryName[0] == '\0') std::strcpy(_entryName, "JAZZ");
						_savedata->AddHighscore(Death::Containers::StringView(_entryName), Death::Containers::StringView(_entryEpisode), _entryScore);
						_storageAction = StorageAction::AutoSave;
						_screen = Screen::Highscores;
						_row = 0;
						PlayMenuSfx(0.6f);
					}
				}
			}
		}

	private:
		static constexpr float MenuItemScale = 0.76f;
		static constexpr float SectionHeadingScale = MenuItemScale;
		static constexpr float TableHeadingScale = 1.0f;
		static constexpr float ContentScale = 1.0f;
		static constexpr float DenseContentScale = 0.9f;
		static constexpr float ListContentScale = 1.0f;
		static constexpr float SelectedListScale = 1.1f;
		static constexpr float SecondaryTextScale = 0.9f;
		static constexpr float PrimaryActionY = 230.0f;

		static void UpdateSystemUtility()
		{
			if (_utilityOwner == nullptr) return;
			if (_utilityOwner->_oskBusy) _utilityOwner->UpdateTextEdit();
			else if (_utilityOwner->_netconfBusy) _utilityOwner->UpdateNetworkProfileSelection();
		}

		// Which field the shared OSK finish handler should apply its result to.
		enum class OskTarget { ServerAddress, HighscoreName };

		// Launches the Sony OSK for any single-line text field. FinishTextEdit() applies the result per target.
		void BeginTextEdit(OskTarget target, const char* description, const char* initial, unsigned int inputType, int outLimit)
		{
			if (_oskBusy || _savedata->IsBusy()) return;
			_oskTarget = target;
			std::memset(&_oskParams, 0, sizeof(_oskParams));
			std::memset(&_oskData, 0, sizeof(_oskData));
			std::memset(_oskDescription, 0, sizeof(_oskDescription));
			std::memset(_oskInput, 0, sizeof(_oskInput));
			std::memset(_oskOutput, 0, sizeof(_oskOutput));
			CopyAsciiToUtf16(description, _oskDescription, arraySize(_oskDescription));
			CopyAsciiToUtf16(initial, _oskInput, arraySize(_oskInput));
			_oskData.language = PSP_UTILITY_OSK_LANGUAGE_ENGLISH;
			_oskData.inputtype = inputType;
			_oskData.lines = 1;
			_oskData.unk_24 = 1;
			_oskData.desc = _oskDescription;
			_oskData.intext = _oskInput;
			_oskData.outtextlength = arraySize(_oskOutput);
			_oskData.outtextlimit = std::min<int>(outLimit, (int)arraySize(_oskOutput) - 1);
			_oskData.outtext = _oskOutput;
			_oskParams.base.size = sizeof(_oskParams);
			_oskParams.base.language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
			_oskParams.base.buttonSwap = PSP_UTILITY_ACCEPT_CROSS;
			_oskParams.base.graphicsThread = 0x11;
			_oskParams.base.accessThread = 0x13;
			_oskParams.base.fontThread = 0x12;
			_oskParams.base.soundThread = 0x10;
			_oskParams.datacount = 1;
			_oskParams.data = &_oskData;
			const int result = sceUtilityOskInitStart(&_oskParams);
			if (result < 0) {
				std::snprintf(_message, sizeof(_message), "Keyboard failed: %08X", (unsigned)result);
				return;
			}
			_oskBusy = true;
			_utilityOwner = this;
			setUpdateEnabled(false);
			nCine::theServiceLocator().GetAudioDevice().suspendDevice();
			nCine::PspGuSetSystemUtilityUpdate(&PspMenu::UpdateSystemUtility);
		}

		void BeginServerAddressEdit()
		{
			BeginTextEdit(OskTarget::ServerAddress, "AEMU server address", _savedata->GetAemuServer(),
				PSP_UTILITY_OSK_INPUTTYPE_URL | PSP_UTILITY_OSK_INPUTTYPE_LATIN_DIGIT |
				PSP_UTILITY_OSK_INPUTTYPE_LATIN_LOWERCASE | PSP_UTILITY_OSK_INPUTTYPE_LATIN_UPPERCASE,
				(int)arraySize(_oskOutput) - 1);
		}

		void BeginHighscoreNameEdit()
		{
			BeginTextEdit(OskTarget::HighscoreName, "Enter your name", _entryName,
				PSP_UTILITY_OSK_INPUTTYPE_LATIN_DIGIT | PSP_UTILITY_OSK_INPUTTYPE_LATIN_LOWERCASE |
				PSP_UTILITY_OSK_INPUTTYPE_LATIN_UPPERCASE, (int)sizeof(_entryName) - 1);
		}

		void UpdateTextEdit()
		{
			switch (sceUtilityOskGetStatus()) {
				case PSP_UTILITY_DIALOG_VISIBLE: sceUtilityOskUpdate(1); break;
				case PSP_UTILITY_DIALOG_QUIT: sceUtilityOskShutdownStart(); break;
				case PSP_UTILITY_DIALOG_NONE: FinishTextEdit(); break;
				default: break;
			}
		}

		void FinishTextEdit()
		{
			if (!_oskBusy) return;
			_oskBusy = false;
			_utilityOwner = nullptr;
			nCine::PspGuSetSystemUtilityUpdate(nullptr);
			nCine::PspGuNotifySystemUtilityFinished();
			if (_oskData.result == PSP_UTILITY_OSK_RESULT_CHANGED) {
				if (_oskTarget == OskTarget::ServerAddress) {
					char address[64]{};
					int length = 0;
					for (; length < (int)sizeof(address) - 1 && _oskOutput[length] != 0; ++length) {
						const unsigned short ch = _oskOutput[length];
						if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
							(ch >= '0' && ch <= '9') || ch == '.' || ch == '-')) {
							std::strcpy(_message, "Use letters, numbers, dots and dashes");
							length = -1;
							break;
						}
						address[length] = (char)ch;
					}
					if (length > 0) {
						address[length] = '\0';
						_savedata->SetAemuServer(address);
						_message[0] = '\0';
					} else if (length == 0) {
						std::strcpy(_message, "Server address cannot be empty");
					}
				} else { // HighscoreName
					char name[sizeof(_entryName)]{};
					int length = 0;
					for (; length < (int)sizeof(name) - 1 && _oskOutput[length] != 0; ++length) {
						const unsigned short ch = _oskOutput[length];
						// Highscores are a fixed-width ASCII table; keep printable ASCII, fold anything else to a space.
						name[length] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : ' ';
					}
					while (length > 0 && name[length - 1] == ' ') name[--length] = '\0';
					if (length > 0) std::strcpy(_entryName, name);
				}
			}
			nCine::theServiceLocator().GetAudioDevice().resumeDevice();
			setUpdateEnabled(true);
			LatchInput();
		}

		void BeginNetworkProfileSelection()
		{
			if (_netconfBusy || _oskBusy || _savedata->IsBusy()) return;
			if (!_adhoc.PrepareInfrastructure()) return;
			std::memset(&_netconfParams, 0, sizeof(_netconfParams));
			_netconfParams.base.size = sizeof(_netconfParams);
			_netconfParams.base.language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
			_netconfParams.base.buttonSwap = PSP_UTILITY_ACCEPT_CROSS;
			_netconfParams.base.graphicsThread = 0x11;
			_netconfParams.base.accessThread = 0x13;
			_netconfParams.base.fontThread = 0x12;
			_netconfParams.base.soundThread = 0x10;
			_netconfParams.action = PSP_NETCONF_ACTION_CONNECTAP;
			const int result = sceUtilityNetconfInitStart(&_netconfParams);
			if (result < 0) {
				char reason[64];
				std::snprintf(reason, sizeof(reason), "Network profile failed: %08X", static_cast<unsigned>(result));
				_adhoc.CancelInfrastructure(reason);
				return;
			}
			_netconfBusy = true;
			_netconfShutdownStarted = false;
			_utilityOwner = this;
			setUpdateEnabled(false);
			nCine::theServiceLocator().GetAudioDevice().suspendDevice();
			nCine::PspGuSetSystemUtilityUpdate(&PspMenu::UpdateSystemUtility);
		}

		void UpdateNetworkProfileSelection()
		{
			const int status = sceUtilityNetconfGetStatus();
			if (status == PSP_UTILITY_DIALOG_VISIBLE) {
				sceUtilityNetconfUpdate(1);
			} else if ((status == PSP_UTILITY_DIALOG_QUIT || status == PSP_UTILITY_DIALOG_FINISHED) && !_netconfShutdownStarted) {
				sceUtilityNetconfShutdownStart();
				_netconfShutdownStarted = true;
			} else if (status == PSP_UTILITY_DIALOG_NONE) {
				FinishNetworkProfileSelection();
			}
		}

		void FinishNetworkProfileSelection()
		{
			if (!_netconfBusy) return;
			_netconfBusy = false;
			_utilityOwner = nullptr;
			nCine::PspGuSetSystemUtilityUpdate(nullptr);
			nCine::PspGuNotifySystemUtilityFinished();
			if (_netconfParams.base.result == 0) _adhoc.StartInfrastructure(_savedata->GetAemuServer());
			else _adhoc.CancelInfrastructure("Network connection cancelled");
			nCine::theServiceLocator().GetAudioDevice().resumeDevice();
			setUpdateEnabled(true);
			LatchInput();
		}

		static void CopyAsciiToUtf16(const char* source, unsigned short* destination, int capacity)
		{
			int i = 0;
			for (; i + 1 < capacity && source[i] != '\0'; ++i) destination[i] = (unsigned char)source[i];
			destination[i] = 0;
		}

		enum class Screen {
			Main, NewGame, PartyGate, PartySelect, JoinGame, MultiplayerCharacter,
			HostMode, HostSettings, HostLevels, Lobby, Episodes, CustomLevels, CharacterSelect, Difficulty,
			Options, Highscores, Notice, HighscoreEntry
		};
		struct EpisodeItem {
			Death::Containers::String name, displayName, firstLevel;
			std::uint16_t position;
			std::unique_ptr<nCine::Texture> titleImage;
			bool titleLoaded = false;
			bool customLevels = false;
		};
		struct CustomLevelItem { Death::Containers::String fileName, displayName; };
		struct HostLevelItem { Death::Containers::String name, displayName; };
		// These values ARE Jazz2::Multiplayer::MpGameMode - cast straight across in CreateMultiplayerNetworkManager().
		// The Team* variants are the same modes with teams on; picking one is all it takes, nothing else changes.
		enum class HostMode : std::uint8_t { Cooperation = 8, Battle = 1, TeamBattle = 2, Race = 3, TeamRace = 4,
			TreasureHunt = 5, TeamTreasureHunt = 6, CaptureTheFlag = 7 };

		// The mode under the cursor, always the free-for-all variant, so artwork, settings rows and level
		// filtering never have to know about the team toggle.
		HostMode CurrentHostMode() const
		{
			static constexpr HostMode Modes[] = { HostMode::Cooperation, HostMode::Battle, HostMode::Race,
				HostMode::TreasureHunt, HostMode::CaptureTheFlag };
			return Modes[std::clamp(_hostModeIdx, 0, 4)];
		}

		// Capture the Flag is a team game by definition and Cooperation/Race have no team story here.
		bool SupportsTeamToggle() const
		{
			return (CurrentHostMode() == HostMode::Battle || CurrentHostMode() == HostMode::TreasureHunt);
		}

		// What actually goes on the wire - the only place the toggle is applied.
		std::uint8_t EffectiveGameMode() const
		{
			if (_hostTeamMode) {
				if (CurrentHostMode() == HostMode::Battle) return (std::uint8_t)HostMode::TeamBattle;
				if (CurrentHostMode() == HostMode::TreasureHunt) return (std::uint8_t)HostMode::TeamTreasureHunt;
			}
			return (std::uint8_t)CurrentHostMode();
		}

		// Collapses a team variant back onto its free-for-all base for display code.
		static HostMode BaseHostMode(std::uint8_t mode)
		{
			switch (static_cast<HostMode>(mode)) {
				case HostMode::TeamBattle: return HostMode::Battle;
				case HostMode::TeamRace: return HostMode::Race;
				case HostMode::TeamTreasureHunt: return HostMode::TreasureHunt;
				default: return static_cast<HostMode>(mode);
			}
		}

		static bool IsTeamHostMode(std::uint8_t mode)
		{
			switch (static_cast<HostMode>(mode)) {
				case HostMode::TeamBattle:
				case HostMode::TeamRace:
				case HostMode::TeamTreasureHunt:
				case HostMode::CaptureTheFlag: return true;
				default: return false;
			}
		}

		// A host knows from its own selection; a joiner reads it off the picked host's beacon.
		bool MultiplayerTeamModeSelected() const
		{
			if (!_multiplayerJoining) return IsTeamHostMode(EffectiveGameMode());
			if (_adhoc.GetDiscoveredHostCount() <= 0) return false;
			const auto& host = _adhoc.GetDiscoveredHost(std::min(_joinHostIdx, _adhoc.GetDiscoveredHostCount() - 1));
			return IsTeamHostMode(host.Settings.GameMode);
		}

		// Auto first: it is the default, and it is the only choice that lets the server balance.
		static constexpr std::uint8_t TeamChoices[] = { nCine::PspAdhoc::NoAdhocTeam, 0, 1 };
		int TeamChoiceIndex() const
		{
			for (int i = 0; i < 3; ++i) if (TeamChoices[i] == _multiplayerTeam) return i;
			return 0;
		}
		void CycleTeam(int direction) { _multiplayerTeam = TeamChoices[(TeamChoiceIndex() + 3 + direction) % 3]; }

		// In a team game the server overwrites the fur with the team gradient, so preview that instead of a
		// colour that will be ignored.
		std::uint32_t PreviewFurColor() const
		{
			if (MultiplayerTeamModeSelected() && _multiplayerTeam != nCine::PspAdhoc::NoAdhocTeam) {
				return Jazz2::Multiplayer::ApplyTeamFurColor(_playerFurColor, _multiplayerTeam);
			}
			return _playerFurColor;
		}

		static const char* ModeName(std::uint8_t mode)
		{
			switch (static_cast<HostMode>(mode)) {
				case HostMode::Battle: return "Battle";
				case HostMode::TeamBattle: return "Team Battle";
				case HostMode::Race: return "Race";
				case HostMode::TeamRace: return "Race (Teams)";
				case HostMode::TreasureHunt: return "Treasure Hunt";
				case HostMode::TeamTreasureHunt: return "Team Treasure Hunt";
				case HostMode::CaptureTheFlag: return "Capture the Flag";
				default: return "Cooperation";
			}
		}

		int HostSettingsRowCount() const
		{
			// Level, mode option x2, [Teams], Continue
			return (SupportsTeamToggle() ? 5 : 4);
		}

		bool SelectCooperationLevel()
		{
			Death::Containers::String name;
			if (_customSelected && _customIdx >= 0 && _customIdx < (int)_customLevels.size())
				name = Death::Containers::String("unknown/"_s + _customLevels[_customIdx].fileName);
			else if (_epIdx >= 0 && _epIdx < (int)_episodes.size())
				name = Death::Containers::String(_episodes[_epIdx].name + '/' + _episodes[_epIdx].firstLevel);
			else return false;

			for (int i = 0; i < (int)_hostLevels.size(); ++i) {
				if (_hostLevels[i].name == name) {
					_hostLevelIdx = i;
					return true;
				}
			}
			return false;
		}

		bool IsHostLevelFeasible(const HostLevelItem&) const
		{
			// JJ2's multiplayer flags are incomplete, so Party Mode deliberately offers every baked level in
			// every mode - the baked catalog is already the authoritative list.
			return true;
		}

		bool HasFeasibleHostLevel() const
		{
			return _hostLevelIdx >= 0 && _hostLevelIdx < (int)_hostLevels.size() &&
				IsHostLevelFeasible(_hostLevels[_hostLevelIdx]);
		}

		void SelectFirstFeasibleHostLevel()
		{
			for (int i = 0; i < (int)_hostLevels.size(); ++i) {
				if (IsHostLevelFeasible(_hostLevels[i])) {
					_hostLevelIdx = i;
					return;
				}
			}
			_hostLevelIdx = -1;
		}

		int FeasibleHostLevelCount() const
		{
			int count = 0;
			for (const auto& level : _hostLevels) if (IsHostLevelFeasible(level)) ++count;
			return count;
		}

		int NthFeasibleHostLevelIndex(int row) const
		{
			for (int i = 0; i < (int)_hostLevels.size(); ++i) {
				if (IsHostLevelFeasible(_hostLevels[i]) && row-- == 0) return i;
			}
			return -1;
		}

		int CurrentFeasibleHostLevelRow() const
		{
			int row = 0;
			for (int i = 0; i < (int)_hostLevels.size(); ++i) {
				if (!IsHostLevelFeasible(_hostLevels[i])) continue;
				if (i == _hostLevelIdx) return row;
				++row;
			}
			return 0;
		}

		void AdjustHostSetting(std::uint32_t hit)
		{
			if ((hit & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) == 0 || _row == 0 || _row == HostSettingsRowCount() - 1) return;
			const int direction = (hit & PSP_CTRL_LEFT) ? -1 : 1;
			// Must precede the switch: its branches are `if (_row == 1) ... else ...`, so an unhandled row would
			// fall into the `else` and silently edit the wrong setting.
			if (SupportsTeamToggle() && _row == 3) {
				_hostTeamMode = !_hostTeamMode;
				PlayMenuSfx(0.5f);
				return;
			}
			switch (CurrentHostMode()) {
				case HostMode::Battle: if (_row == 1) _totalKills = std::clamp(_totalKills + direction * 5, 5, 100); else _maxTimeMinutes = std::clamp(_maxTimeMinutes + direction * 5, 0, 60); break;
				case HostMode::Race: if (_row == 1) _totalLaps = std::clamp(_totalLaps + direction, 1, 20); else _overtimeSeconds = std::clamp(_overtimeSeconds + direction * 10, 0, 120); break;
				case HostMode::TreasureHunt: if (_row == 1) _totalTreasure = std::clamp(_totalTreasure + direction * 10, 10, 500); else _maxTimeMinutes = std::clamp(_maxTimeMinutes + direction * 5, 0, 60); break;
				case HostMode::CaptureTheFlag: if (_row == 1) _totalKills = std::clamp(_totalKills + direction, 1, 20); else _maxTimeMinutes = std::clamp(_maxTimeMinutes + direction * 5, 0, 60); break;
				default: break;
			}
			PlayMenuSfx(0.5f);
		}

		static constexpr std::uint8_t FurGradientStarts[] = { 0x00, 0x10, 0x18, 0x20, 0x28,
			0x30, 0x40, 0x48, 0x50, 0x58 };
		std::uint32_t CurrentPlayerColor() const { return _playerFurColor; }
		std::uint8_t DefaultFurSection(int section) const
		{
			// Sections are Primary Fur, Secondary Fur, Weapon, Misc. A packed zero still means "keep the native
			// palette" internally; these only pick which ramp an untouched bubble displays.
			static constexpr std::uint8_t Defaults[3][4] = {
				{ 0x10, 0x18, 0x20, 0x28 }, // Jazz: green, red band, blue weapon, tan accents
				{ 0x18, 0x28, 0x10, 0x20 }, // Spaz: red, orange, green weapon, blue accents
				{ 0x28, 0x58, 0x20, 0x30 }  // Lori: blonde hair, purple suit, blue weapon, red lips
			};
			return Defaults[std::clamp(_multiplayerCharIdx, 0, 2)][section];
		}
		int FurGradientIndex(std::uint8_t value) const
		{
			for (int i = 0; i < (int)(sizeof(FurGradientStarts) / sizeof(FurGradientStarts[0])); ++i) {
				if (FurGradientStarts[i] == value) return i;
			}
			return 0;
		}
		void CycleFurSection(int section)
		{
			if (section >= 4) {
				_playerFurColor = 0;
				return;
			}
			const std::uint32_t shift = (std::uint32_t)section * 8;
			std::uint8_t current = (std::uint8_t)((_playerFurColor >> shift) & 0xff);
			if (current == 0) current = DefaultFurSection(section);
			const int count = (int)(sizeof(FurGradientStarts) / sizeof(FurGradientStarts[0]));
			const std::uint8_t next = FurGradientStarts[(FurGradientIndex(current) + 1) % count];
			_playerFurColor = (_playerFurColor & ~(0xffu << shift)) | ((std::uint32_t)next << shift);
		}
		void ResetPlayerColor() { _playerFurColor = 0; }

		void DrawWigglyText(Jazz2::UI::Font* font, Death::Containers::StringView value, float x, float y,
			const nCine::Colorf& color, float scale)
		{
			if (font == nullptr) return;
			std::int32_t offset = 0;
			font->DrawString(this, value, offset, x, y, 100,
				Jazz2::UI::Alignment::Center, color, scale, 0.7f, 1.1f, 1.1f, 0.4f);
		}

		void DrawTextDisabled(Jazz2::UI::Font* font, Death::Containers::StringView value, float x, float y,
			bool enabled, bool selected, float scale)
		{
			if (enabled) DrawText(font, value, x, y, selected, scale);
			else if (font != nullptr) { std::int32_t offset = 0; font->DrawString(this, value, offset, x, y, 100,
				Jazz2::UI::Alignment::Center, nCine::Colorf(0.42f, 0.42f, 0.48f, 1.0f), scale); }
		}

		void DrawTableCell(Jazz2::UI::Font* font, Death::Containers::StringView value, float x, float y,
			bool enabled, bool selected)
		{
			if (font == nullptr) return;
			std::int32_t offset = 0;
			const bool activeSelected = (enabled && selected);
			const nCine::Colorf color = (!enabled ? nCine::Colorf(0.42f, 0.42f, 0.48f, 1.0f)
				: activeSelected ? nCine::Colorf(444.0f, 444.0f, 444.0f, 1.0f) : Jazz2::UI::Font::DefaultColor);
			font->DrawString(this, value, offset, x, y, 100, Jazz2::UI::Alignment::Center, color,
				activeSelected ? DenseContentScale * 1.06f : DenseContentScale,
				activeSelected ? 0.7f : 0.0f, activeSelected ? 1.1f : 0.0f,
				activeSelected ? 1.1f : 0.0f, activeSelected ? 0.4f : 0.0f);
		}

		void DrawCharacterChoices(Jazz2::UI::Font* font, int selected, float y, bool active)
		{
			static const nCine::Colorf colors[] = { nCine::Colorf(0.32f, 1.0f, 0.36f, 1.0f),
				nCine::Colorf(1.0f, 0.25f, 0.18f, 1.0f), nCine::Colorf(1.0f, 0.82f, 0.26f, 1.0f) };
			for (int i = 0; i < CharacterCount(); ++i) {
				if (font == nullptr) continue;
				const bool chosen = (i == selected);
				std::int32_t offset = 0;
				// Unchosen entries stay dimmed so the pick is still readable once focus moves elsewhere.
				font->DrawString(this, Death::Containers::StringView(CharName(i)), offset, 92.0f + i * 72.0f, y, 100,
					Jazz2::UI::Alignment::Center, OptionTone(colors[i], chosen), MenuItemScale,
					active && chosen ? 0.55f : 0.0f, active && chosen ? 0.9f : 0.0f, 1.0f, 0.3f);
			}
		}

		void DrawColorChoices(Jazz2::UI::Font* font, float y)
		{
			DrawText(font, "Colors"_s, 48.0f, y, false, DenseContentScale);
			for (int section = 0; section < 4; ++section) {
				const float x = 105.0f + section * 19.0f;
				std::uint8_t value = (std::uint8_t)((_playerFurColor >> (section * 8)) & 0xff);
				if (value == 0) value = DefaultFurSection(section);
				const int gradient = FurGradientIndex(value);
				if (_mpSettingsRow == 1 && _colorSection == section)
					DrawSelectionGlowSized(x, y, nCine::Vector2f(22.0f, 22.0f), 0.6f, 90);
				// Frame 5 is the original neutral/silver bubble used for an unchanged section.
				DrawOriginalMenuArt("UI/multiplayer_color.aura"_s, gradient == 0 ? 5 : gradient - 1, x, y, 0.52f);
			}
			DrawText(font, "Default"_s, 211.0f, y, _mpSettingsRow == 1 && _colorSection == 4, DenseContentScale);
		}

		// GetTeamColor() is tuned for tinting sprites and is too dark to read as text over the title art.
		// Brightened for the menu only; fur, minimap dots and the HUD keep the darker shared palette.
		static nCine::Colorf TeamTextColor(std::uint8_t team)
		{
			switch (team) {
				case 0: return nCine::Colorf(0.36f, 0.66f, 1.0f, 1.0f);		// Blue
				case 1: return nCine::Colorf(1.0f, 0.34f, 0.30f, 1.0f);		// Red
				default: return nCine::Colorf(0.78f, 0.78f, 0.78f, 1.0f);	// Auto / no preference
			}
		}

		// The focus glow alone cannot show a choice: it follows the cursor, so it leaves with it.
		static nCine::Colorf OptionTone(const nCine::Colorf& base, bool chosen)
		{
			if (chosen) return base;
			constexpr float Dim = 0.42f;
			return nCine::Colorf(base.R * Dim, base.G * Dim, base.B * Dim, 0.85f);
		}

		// Takes the colour row's slot in team games - the server forces the team gradient over any pick.
		void DrawTeamChoices(Jazz2::UI::Font* font, float y)
		{
			DrawText(font, "Team"_s, 48.0f, y, false, DenseContentScale);
			for (int i = 0; i < 3; ++i) {
				const std::uint8_t team = TeamChoices[i];
				const bool isAuto = (team == nCine::PspAdhoc::NoAdhocTeam);
				const float x = 122.0f + i * 48.0f;
				if (_mpSettingsRow == 1 && TeamChoiceIndex() == i) {
					DrawSelectionGlowSized(x, y, nCine::Vector2f(44.0f, 22.0f), 0.6f, 90);
				}
				if (font == nullptr) continue;
				const bool chosen = (_multiplayerTeam == team);
				std::int32_t offset = 0;
				font->DrawString(this, isAuto ? "Auto"_s : Jazz2::Multiplayer::GetTeamName(team), offset, x, y, 100,
					Jazz2::UI::Alignment::Center, OptionTone(TeamTextColor(team), chosen), DenseContentScale,
					chosen ? 0.35f : 0.0f, chosen ? 0.7f : 0.0f, 1.0f, 0.3f);
			}
		}

		void DrawGameLogo()
		{
			auto drawFitted = [&](Death::Containers::StringView path, float left, float top, float maxWidth, float maxHeight) {
				auto* graphic = ContentResolver::Get().RequestGraphics(path, 0,
					/*keepIndexed*/ false, /*forceStreamed*/ true);
				if (graphic == nullptr || graphic->FrameDimensions.X <= 0 || graphic->FrameDimensions.Y <= 0) return;
				const float scale = std::min(maxWidth / graphic->FrameDimensions.X, maxHeight / graphic->FrameDimensions.Y);
				const float width = graphic->FrameDimensions.X * scale;
				const float height = graphic->FrameDimensions.Y * scale;
				DrawOriginalMenuArt(path, 0, left + width * 0.5f, top + height * 0.5f, scale);
			};
			drawFitted("UI/logo_large.aura"_s, 4.0f, 3.0f, 161.0f, 87.0f);
		}

		void DrawOriginalMenuArt(Death::Containers::StringView path, int frame, float x, float y, float scale, std::uint16_t layer = 100)
		{
			auto* graphic = ContentResolver::Get().RequestGraphics(path, 0,
				/*keepIndexed*/ false, /*forceStreamed*/ true);
			if (graphic == nullptr || graphic->TextureDiffuse == nullptr || graphic->FrameCount <= 0) return;
			frame = std::clamp(frame, 0, graphic->FrameCount - 1);
			const int columns = std::max(1, graphic->FrameConfiguration.X);
			const int col = frame % columns, row = frame / columns;
			const nCine::Vector2i textureSize = graphic->TextureDiffuse->GetSize();
			const nCine::Vector2f size(graphic->FrameDimensions.X * scale, graphic->FrameDimensions.Y * scale);
			DrawTexture(*graphic->TextureDiffuse, nCine::Vector2f(x - size.X * 0.5f, y - size.Y * 0.5f), layer, size,
				nCine::Vector4f((float)graphic->FrameDimensions.X / textureSize.X,
					(float)(col * graphic->FrameDimensions.X) / textureSize.X,
					(float)graphic->FrameDimensions.Y / textureSize.Y,
					(float)(row * graphic->FrameDimensions.Y) / textureSize.Y), nCine::Colorf::White);
		}

		void DrawCharacterSelectionArt(int character, float x, float y, float scale)
		{
			static constexpr const char* Paths[] = { "UI/character_art_jazz.aura", "UI/character_art_spaz.aura",
				"UI/character_art_lori.aura" };
			character = std::clamp(character, 0, 2);
			auto* graphic = ContentResolver::Get().RequestGraphics(Death::Containers::StringView(Paths[character]), 0,
				/*keepIndexed*/ false, /*forceStreamed*/ true);
			if (graphic == nullptr || graphic->FrameCount <= 0) return;
			// The art runs forward from facing the player (frame 0) to turned away (last frame), while
			// _characterAnim rises to 1 for the selected one - so selection has to walk it BACKWARDS.
			const int last = graphic->FrameCount - 1;
			const int frame = last - std::clamp((int)std::lround(_characterAnim[character] * last), 0, last);
			DrawOriginalMenuArt(Death::Containers::StringView(Paths[character]), frame, x, y, scale);
		}

		void DrawDifficultyArt(int character, int difficulty, float x, float y, float scale)
		{
			if (_menuMeta == nullptr) return;
			auto* animation = _menuMeta->FindAnimation((AnimState)(11 + std::clamp(character, 0, 2)));
			if (animation == nullptr || animation->Base == nullptr || animation->Base->TextureDiffuse == nullptr) return;
			auto* base = animation->Base;
			const int frame = animation->FrameOffset + std::clamp(difficulty, 0, std::max(0, animation->FrameCount - 1));
			const int columns = std::max(1, base->FrameConfiguration.X);
			const int col = frame % columns, row = frame / columns;
			const nCine::Vector2i ts = base->TextureDiffuse->GetSize();
			const nCine::Vector2f size(base->FrameDimensions.X * scale, base->FrameDimensions.Y * scale);
			DrawTexture(*base->TextureDiffuse, nCine::Vector2f(x - size.X * 0.5f, y - size.Y * 0.5f), 100, size,
				nCine::Vector4f((float)base->FrameDimensions.X / ts.X, (float)(col * base->FrameDimensions.X) / ts.X,
					(float)base->FrameDimensions.Y / ts.Y, (float)(row * base->FrameDimensions.Y) / ts.Y),
				nCine::Colorf::White, false, 0.0f, animation->PaletteOffset);
		}

		void DrawShootingPreview(int character, float x, float y, float scale)
		{
			static constexpr const char* Paths[] = { "Jazz/shoot.aura", "Spaz/shoot.aura", "Lori/shoot.aura" };
			character = std::clamp(character, 0, 2);
			auto& resolver = ContentResolver::Get();
			auto* graphic = resolver.RequestGraphics(Death::Containers::StringView(Paths[character]), 0,
				/*keepIndexed*/ true, /*forceStreamed*/ true);
			if (graphic == nullptr || graphic->TextureDiffuse == nullptr || graphic->FrameCount <= 0) return;
			const std::uint32_t furColor = PreviewFurColor();
			if (_previewPaletteOffset < 0 || _previewPaletteColor != furColor || _previewPaletteCharacter != character) {
				if (_previewPaletteOffset >= 0) resolver.ReleasePaletteOffset(_previewPaletteOffset);
				const PlayerType type = (character == 0 ? PlayerType::Jazz : character == 1 ? PlayerType::Spaz : PlayerType::Lori);
				_previewPaletteOffset = resolver.AcquirePaletteOffset(furColor, type);
				_previewPaletteColor = furColor;
				_previewPaletteCharacter = character;
			}
			const int columns = std::max(1, graphic->FrameConfiguration.X);
			const int frame = (int)(AnimTime * 8.0f) % graphic->FrameCount;
			const int col = frame % columns, row = frame / columns;
			const nCine::Vector2i ts = graphic->TextureDiffuse->GetSize();
			const nCine::Vector2f size(graphic->FrameDimensions.X * scale, graphic->FrameDimensions.Y * scale);
			DrawTexture(*graphic->TextureDiffuse, nCine::Vector2f(x - size.X * 0.5f, y - size.Y * 0.5f), 100, size,
				nCine::Vector4f((float)graphic->FrameDimensions.X / ts.X, (float)(col * graphic->FrameDimensions.X) / ts.X,
					(float)graphic->FrameDimensions.Y / ts.Y, (float)(row * graphic->FrameDimensions.Y) / ts.Y),
				nCine::Colorf::White, false, 0.0f, _previewPaletteOffset);
		}

		void DrawModeArt(float x, float y)
		{
			// Frame 0 is the Single/Party selector; frames 1-5 are Cooperation, Battle, Race, Treasure Hunt and
			// Capture the Flag. Layer 60 keeps the decorative art behind the labels (100) and their glow (90).
			DrawOriginalMenuArt("UI/multiplayer_mode.aura"_s, _row + 1, x, y, 1.725f, 60);
		}

		// Rows 1..N-2 of Screen::HostSettings. Spacing only tightens where a team toggle adds a third row, so
		// the last one still clears Continue at PrimaryActionY.
		float ModeSettingRowY(int row) const
		{
			const bool tight = SupportsTeamToggle();
			return (tight ? 131.0f : 139.0f) + (row - 1) * (tight ? 32.0f : 40.0f);
		}

		void DrawModeSettings(Jazz2::UI::Font* font, char* buffer)
		{
			const float y1 = ModeSettingRowY(1), y2 = ModeSettingRowY(2);
			switch (CurrentHostMode()) {
				case HostMode::Battle:
					std::snprintf(buffer, 80, "Roasts:  < %d >", _totalKills); DrawText(font, buffer, 240, y1, _row == 1, MenuItemScale);
					if (_maxTimeMinutes == 0) std::snprintf(buffer, 80, "Time:  < Unlimited >"); else std::snprintf(buffer, 80, "Time:  < %d min >", _maxTimeMinutes);
					DrawText(font, buffer, 240, y2, _row == 2, MenuItemScale); break;
				case HostMode::Race:
					std::snprintf(buffer, 80, "Laps:  < %d >", _totalLaps); DrawText(font, buffer, 240, y1, _row == 1, MenuItemScale);
					std::snprintf(buffer, 80, "Overtime:  < %d sec >", _overtimeSeconds); DrawText(font, buffer, 240, y2, _row == 2, MenuItemScale); break;
				case HostMode::TreasureHunt:
					std::snprintf(buffer, 80, "Treasure:  < %d >", _totalTreasure); DrawText(font, buffer, 240, y1, _row == 1, MenuItemScale);
					std::snprintf(buffer, 80, "Time:  < %d min >", _maxTimeMinutes); DrawText(font, buffer, 240, y2, _row == 2, MenuItemScale); break;
				case HostMode::CaptureTheFlag:
					std::snprintf(buffer, 80, "Captures:  < %d >", _totalKills); DrawText(font, buffer, 240, y1, _row == 1, MenuItemScale);
					std::snprintf(buffer, 80, "Time:  < %d min >", _maxTimeMinutes); DrawText(font, buffer, 240, y2, _row == 2, MenuItemScale); break;
				default: break;
			}
			if (SupportsTeamToggle()) {
				std::snprintf(buffer, 80, "Teams:  < %s >", _hostTeamMode ? "Yes" : "No");
				DrawText(font, buffer, 240, ModeSettingRowY(3), _row == 3, MenuItemScale);
			}
		}

		void DrawLobbySettings(Jazz2::UI::Font* font, const nCine::PspAdhoc::SessionSettings& settings)
		{
			char buffer[64];
			DrawText(font, Death::Containers::StringView(ModeName(settings.GameMode)), 355.0f, 123.0f, false, ContentScale);
			DrawText(font, Death::Containers::StringView(settings.LevelDisplayName), 355.0f, 145.0f, false, DenseContentScale);
			int y = 167;
			switch (BaseHostMode(settings.GameMode)) {
				case HostMode::Battle: std::snprintf(buffer, sizeof(buffer), "Roasts: %u", settings.TotalKills); break;
				case HostMode::Race: std::snprintf(buffer, sizeof(buffer), "Laps: %u", settings.TotalLaps); break;
				case HostMode::TreasureHunt: std::snprintf(buffer, sizeof(buffer), "Treasure: %u", settings.TotalTreasureCollected); break;
				case HostMode::CaptureTheFlag: std::snprintf(buffer, sizeof(buffer), "Captures: %u", settings.TotalKills); break;
				default: std::snprintf(buffer, sizeof(buffer), "Difficulty: %s", DiffName(settings.Difficulty)); break;
			}
			DrawText(font, Death::Containers::StringView(buffer), 355.0f, (float)y, false, DenseContentScale);
			if (BaseHostMode(settings.GameMode) != HostMode::Cooperation) {
				if (settings.MaxGameTimeSecs == 0) std::snprintf(buffer, sizeof(buffer), "Time: Unlimited");
				else std::snprintf(buffer, sizeof(buffer), "Time: %u min", settings.MaxGameTimeSecs / 60);
				DrawText(font, Death::Containers::StringView(buffer), 355.0f, 188.0f, false, DenseContentScale);
				if (BaseHostMode(settings.GameMode) == HostMode::Race) {
					std::snprintf(buffer, sizeof(buffer), "Overtime: %u sec", settings.OvertimeSecs);
					DrawText(font, Death::Containers::StringView(buffer), 355.0f, 207.0f, false, DenseContentScale);
				}
			}
		}

		void LoadSystemNickname()
		{
			if (sceUtilityGetSystemParamString(PSP_SYSTEMPARAM_ID_STRING_NICKNAME, _playerName, sizeof(_playerName)) < 0 || _playerName[0] == '\0') {
				std::strcpy(_playerName, "Player");
			}
			_playerName[sizeof(_playerName) - 1] = '\0';
			for (char* c = _playerName; *c != '\0'; ++c) {
				if ((unsigned char)*c < 0x20 || *c == 0x7f) *c = ' ';
			}
		}

		// List titles stay resident; only one episode preview is held at a time, released before the next.
		void LoadEpisodeTitle(EpisodeItem& it)
		{
			if (it.titleLoaded) return;
			it.titleLoaded = true;
			auto ep = ContentResolver::Get().GetEpisode(it.name, /*withImages*/ true);
			if (ep) it.titleImage = std::move(ep->TitleImage);
		}

		void LoadEpisodePreview(int index)
		{
			_episodePreview = nullptr;
			_episodePreviewIndex = index;
			if (index < 0 || index >= (int)_episodes.size()) return;
			auto episode = ContentResolver::Get().GetEpisode(_episodes[index].name, /*withImages*/ true);
			if (episode) _episodePreview = std::move(episode->BackgroundImage);
		}

		void OpenEpisodeBrowser(int index)
		{
			_screen = Screen::Episodes;
			_row = std::clamp(index, 0, std::max(0, (int)_episodes.size() - 1));
			_episodePreviewPending = -1;
			_episodePreviewZoomingOut = false;
			LoadEpisodePreview(_row);
			_episodePreviewScale = 0.0f;
		}

		void CloseEpisodePreview()
		{
			_episodePreview = nullptr;
			_episodePreviewIndex = -1;
			_episodePreviewPending = -1;
			_episodePreviewScale = 0.0f;
			_episodePreviewZoomingOut = false;
		}

		void QueueEpisodePreview(int index)
		{
			if (index == _episodePreviewIndex && _episodePreviewPending < 0) return;
			_episodePreviewPending = index;
			if (_episodePreview == nullptr || _episodePreviewScale <= 0.0f) {
				LoadEpisodePreview(_episodePreviewPending);
				_episodePreviewPending = -1;
				_episodePreviewScale = 0.0f;
				_episodePreviewZoomingOut = false;
			} else {
				_episodePreviewZoomingOut = true;
			}
		}

		void UpdateEpisodePreview(float timeMult)
		{
			if (_screen != Screen::Episodes) return;
			constexpr float ZoomFrames = 10.0f;
			const float step = timeMult / ZoomFrames;
			if (_episodePreviewZoomingOut) {
				_episodePreviewScale = std::max(0.0f, _episodePreviewScale - step);
				if (_episodePreviewScale <= 0.0f) {
					LoadEpisodePreview(_episodePreviewPending);
					_episodePreviewPending = -1;
					_episodePreviewZoomingOut = false;
				}
			} else {
				_episodePreviewScale = std::min(1.0f, _episodePreviewScale + step);
			}
		}

		// Draws a whole texture centred at (cx, cy) through the Canvas sprite path.
		void DrawImage(nCine::Texture* tex, float cx, float cy, float scale, const nCine::Colorf& color, std::uint16_t z = 100)
		{
			if (tex == nullptr) return;
			nCine::Vector2i sz = tex->GetSize();
			nCine::Vector2f size((float)sz.X * scale, (float)sz.Y * scale);
			DrawTexture(*tex, nCine::Vector2f(cx - size.X * 0.5f, cy - size.Y * 0.5f), z, size,
				 nCine::Vector4f(1.0f, 0.0f, 1.0f, 0.0f), color, false, 0.0f, -1);
		}

		// Mirrors the original DrawStringGlow(): an additive MenuGlow sprite under the item, not a font highlight.
		void DrawSelectionGlowSized(float cx, float cy, const nCine::Vector2f& size, float alpha, std::uint16_t z)
		{
			if (_menuMeta == nullptr || size.X <= 0.0f || size.Y <= 0.0f) return;
			auto* res = _menuMeta->FindAnimation((AnimState)5);
			if (res == nullptr || res->Base == nullptr || res->Base->TextureDiffuse == nullptr) return;
			auto* base = res->Base;
			const int frame = res->FrameOffset;
			const int columns = std::max(1, base->FrameConfiguration.X);
			const int col = frame % columns, row = frame / columns;
			const nCine::Vector2i textureSize = base->TextureDiffuse->GetSize();
			const nCine::Vector4f uv((float)base->FrameDimensions.X / textureSize.X,
				(float)(col * base->FrameDimensions.X) / textureSize.X,
				(float)base->FrameDimensions.Y / textureSize.Y,
				(float)(row * base->FrameDimensions.Y) / textureSize.Y);
			const int palette = ((base->Flags & GenericGraphicResourceFlags::Indexed) == GenericGraphicResourceFlags::Indexed
				? res->PaletteOffset : -1);
			DrawTexture(*base->TextureDiffuse, nCine::Vector2f(cx - size.X * 0.5f, cy - size.Y * 0.5f), z,
				size, uv, nCine::Colorf(1.0f, 1.0f, 1.0f, alpha), true, 0.0f, palette);
		}

		void DrawSelectionGlow(float cx, float cy, float itemWidth, float scale, std::uint16_t z)
		{
			constexpr float BakedGlowScale = 8.0f;
			if (_menuMeta == nullptr || itemWidth <= 0.0f) return;
			auto* res = _menuMeta->FindAnimation((AnimState)5);
			if (res == nullptr || res->Base == nullptr) return;
			DrawSelectionGlowSized(cx, cy,
				nCine::Vector2f(res->Base->FrameDimensions.X / BakedGlowScale * (itemWidth + 30.0f) * 0.06f,
					res->Base->FrameDimensions.Y / BakedGlowScale * 4.0f * scale),
				0.4f * scale, z);
		}

		// Footer bar spanning y 252-272: names the current screen rather than listing controls, which stay
		// discoverable through the standard PSP bindings. DrawBuildStamp shares this bar at the left edge.
		void DrawHint(Jazz2::UI::Font* font, Death::Containers::StringView text)
		{
			DrawSolid(nCine::Vector2f(0.0f, 252.0f), 200, nCine::Vector2f(480.0f, 20.0f), nCine::Colorf(0.0f, 0.0f, 0.0f, 0.55f), false);
			if (font == nullptr) return;
			std::int32_t co = 0;
			font->DrawString(this, text, co, 474.0f, 262.0f, 210,
				Jazz2::UI::Alignment::Right, nCine::Colorf::White, SecondaryTextScale, 0.7f, 1.1f, 1.1f, 0.4f);
		}

		void DrawMenuLine(int frame, float y)
		{
			if (_menuMeta == nullptr) return;
			auto* res = _menuMeta->FindAnimation((AnimState)2);
			if (res == nullptr || res->Base == nullptr || res->Base->TextureDiffuse == nullptr) return;
			auto* base = res->Base;
			const int actualFrame = res->FrameOffset + std::min(frame, std::max(0, res->FrameCount - 1));
			const int columns = std::max(1, base->FrameConfiguration.X);
			const int fx = actualFrame % columns;
			const int fy = actualFrame / columns;
			const nCine::Vector2i ts = base->TextureDiffuse->GetSize();
			const nCine::Vector4f uv((float)base->FrameDimensions.X / ts.X,
				(float)(fx * base->FrameDimensions.X) / ts.X, (float)base->FrameDimensions.Y / ts.Y,
				(float)(fy * base->FrameDimensions.Y) / ts.Y);
			const nCine::Vector2f size((float)base->FrameDimensions.X * 1.6f, (float)base->FrameDimensions.Y * 1.6f);
			DrawTexture(*base->TextureDiffuse, nCine::Vector2f(240.0f - size.X * 0.5f, y - size.Y * 0.5f),
				120, size, uv, nCine::Colorf::White, false, 0.0f, 0);
		}

		void DrawCustomLevels(Jazz2::UI::Font* font)
		{
			constexpr int visible = 6;
			constexpr float top = 78.0f;
			constexpr float listStart = 92.0f;
			constexpr float pitch = 27.0f;
			// The list begins below the shared logo and ends above the footer.
			constexpr float bottom = listStart + (visible - 1) * pitch + 15.0f;
			DrawMenuLine(0, top);
			DrawMenuLine(1, bottom);
			if (font == nullptr) return;

			std::int32_t co = 0;
			if (_customLevels.empty()) {
				DrawText(font, "No custom level found!"_s, 240.0f, 125.0f, false, ContentScale);
				DrawHint(font, "Choose Level"_s);
				return;
			}

			const int first = std::clamp(_row - visible / 2, 0, std::max(0, (int)_customLevels.size() - visible));
			for (int i = first; i < std::min(first + visible, (int)_customLevels.size()); i++) {
				const float y = listStart + (i - first) * pitch;
				const bool selected = (i == _row);
				co = 0;
				if (selected) {
					const float width = font->MeasureString(_customLevels[i].fileName, SelectedListScale, 0.9f).X;
					DrawSelectionGlow(120.0f + width * 0.5f, y, width, SelectedListScale, 145);
				}
				font->DrawString(this, _customLevels[i].fileName, co, 120.0f, y, selected ? 160 : 150,
					Jazz2::UI::Alignment::Left, selected ? nCine::Colorf(444.0f, 444.0f, 444.0f, 1.0f) : Jazz2::UI::Font::DefaultColor,
					selected ? SelectedListScale : ListContentScale, selected ? 0.7f : 0.0f, selected ? 1.1f : 0.0f,
					selected ? 1.1f : 0.0f, selected ? 0.4f : 0.0f, 0.9f);
				co = 0;
				font->DrawString(this, _customLevels[i].displayName, co, 250.0f, y, 150,
					Jazz2::UI::Alignment::Left, Jazz2::UI::Font::DefaultColor, ListContentScale);
			}
			DrawHint(font, "Choose Level"_s);
		}

		void DrawHostLevels(Jazz2::UI::Font* font)
		{
			// Party Mode deliberately uses the same two-column level-list layout as Custom Levels.
			constexpr int visible = 6;
			constexpr float top = 78.0f;
			constexpr float listStart = 92.0f;
			constexpr float pitch = 27.0f;
			constexpr float bottom = listStart + (visible - 1) * pitch + 15.0f;
			DrawMenuLine(0, top);
			DrawMenuLine(1, bottom);
			if (font == nullptr) return;

			const int count = FeasibleHostLevelCount();
			if (count == 0) {
				DrawText(font, "No level found!"_s, 240.0f, 125.0f, false, ContentScale);
				DrawHint(font, "Choose Level"_s);
				return;
			}

			const int first = std::clamp(_row - visible / 2, 0, std::max(0, count - visible));
			for (int row = first; row < std::min(first + visible, count); ++row) {
				const int levelIndex = NthFeasibleHostLevelIndex(row);
				if (levelIndex < 0) continue;
				const HostLevelItem& level = _hostLevels[levelIndex];
				const char* slash = std::strrchr(level.name.data(), '/');
				const Death::Containers::StringView fileName(slash != nullptr ? slash + 1 : level.name.data());
				const float y = listStart + (row - first) * pitch;
				const bool selected = (row == _row);
				std::int32_t co = 0;
				if (selected) {
					const float width = font->MeasureString(fileName, SelectedListScale, 0.9f).X;
					DrawSelectionGlow(120.0f + width * 0.5f, y, width, SelectedListScale, 145);
				}
				font->DrawString(this, fileName, co, 120.0f, y, selected ? 160 : 150,
					Jazz2::UI::Alignment::Left, selected ? nCine::Colorf(444.0f, 444.0f, 444.0f, 1.0f) : Jazz2::UI::Font::DefaultColor,
					selected ? SelectedListScale : ListContentScale, selected ? 0.7f : 0.0f, selected ? 1.1f : 0.0f,
					selected ? 1.1f : 0.0f, selected ? 0.4f : 0.0f, 0.9f);
				co = 0;
				font->DrawString(this, level.displayName, co, 250.0f, y, 150,
					Jazz2::UI::Alignment::Left, Jazz2::UI::Font::DefaultColor, ListContentScale);
			}
			DrawHint(font, "Choose Level"_s);
		}

		void DrawBuildStamp(Jazz2::UI::Font* font)
		{
			if (font == nullptr) return;
			std::int32_t co = 0;
			font->DrawString(this, "Build " PSP_BUILD_STAMP, co, 3.0f, 262.0f, 220, Jazz2::UI::Alignment::Left,
				nCine::Colorf(0.55f, 0.55f, 0.62f, 1.0f), SecondaryTextScale);
		}

		static const char* CharName(int i) { return i == 0 ? "Jazz" : i == 1 ? "Spaz" : "Lori"; }
		static const char* DiffName(int i) { return i == 0 ? "Easy" : i == 1 ? "Normal" : "Hard"; }

		// One rotating, zooming plane of a Menu16/32/128 tile, repeat-wrapped across a big quad. Three layered
		// planes stand in for MainMenu's shader-warped background, which is not portable to the PSP GU.
		void DrawTiledLayer(std::int32_t animId, float repeats, float scale, float angle, float panX, float panY, std::uint16_t z)
		{
			if (_menuMeta == nullptr) return;
			auto* res = _menuMeta->FindAnimation((AnimState)animId);
			if (res == nullptr || res->Base == nullptr || res->Base->TextureDiffuse == nullptr) return;
			auto* base = res->Base;
			const nCine::Vector2f size((float)base->FrameDimensions.X * scale, (float)base->FrameDimensions.Y * scale);
			const float cx = 240.0f + panX, cy = 136.0f + panY;
			DrawTexture(*base->TextureDiffuse.get(), nCine::Vector2f(cx - size.X * 0.5f, cy - size.Y * 0.5f), z, size,
				nCine::Vector4f(repeats, 0.0f, repeats, 0.0f), nCine::Colorf(1.0f, 1.0f, 1.0f, 1.0f), false, angle, 0);
		}

		void DrawBackground()
		{
			DrawSolid(nCine::Vector2f(0.0f, 0.0f), 1, nCine::Vector2f(480.0f, 272.0f), nCine::Colorf(0.04f, 0.02f, 0.08f, 1.0f), false);
			const float t = AnimTime;
			// Menu16/32/128 planes (anims 110/111/112). Original quad coverage kept, but the tiles are sampled
			// closer to cut PSP minification cost; AnimTime only advances ~0.84/s, hence the large rate multipliers.
			DrawTiledLayer(110, 69.12f, 0.64f * 96.0f, t * -0.2f, 0.0f, 0.0f, 5);
			DrawTiledLayer(111, 40.32f, 0.64f * 56.0f, t * 0.4f, 96.0f * std::sin(t * 0.37f), 96.0f * std::cos(t * 0.31f), 6);
			DrawTiledLayer(112, 14.4f, 0.8f * 20.0f, t * 0.3f, 64.0f * std::sin(t * 0.25f), 64.0f * std::cos(t * 0.32f), 7);
		}

		void DrawText(Jazz2::UI::Font* font, Death::Containers::StringView text, float x, float y, bool selected, float scale)
		{
			if (font == nullptr) return;
			std::int32_t charOffset = 0;
			if (selected) {
				const float selectedScale = scale * 1.1f;
				DrawSelectionGlow(x, y, font->MeasureString(text, selectedScale).X, selectedScale, 90);
				// Colorf(444,444,444) is the Font::RandomColor per-glyph rainbow sentinel; the Font keeps its RGB
				// but honours the alpha passed here, so pass 1.0 for an opaque rainbow.
				font->DrawString(this, text, charOffset, x, y, 100, Jazz2::UI::Alignment::Center,
					nCine::Colorf(444.0f, 444.0f, 444.0f, 1.0f), selectedScale, 0.7f, 1.1f, 1.1f, 0.4f);
			} else {
				font->DrawString(this, text, charOffset, x, y, 100, Jazz2::UI::Alignment::Center,
					Jazz2::UI::Font::DefaultColor, scale, 0.0f, 0.0f, 0.0f, 0.0f);
			}
		}

		void DrawTextLeft(Jazz2::UI::Font* font, Death::Containers::StringView text, float x, float y, bool selected, float scale)
		{
			if (font == nullptr) return;
			std::int32_t charOffset = 0;
			if (selected) {
				const float selectedScale = scale * 1.1f;
				const float width = font->MeasureString(text, selectedScale).X;
				DrawSelectionGlow(x + width * 0.5f, y, width, selectedScale, 90);
				font->DrawString(this, text, charOffset, x, y, 100, Jazz2::UI::Alignment::Left,
					nCine::Colorf(444.0f, 444.0f, 444.0f, 1.0f), selectedScale, 0.7f, 1.1f, 1.1f, 0.4f);
			} else {
				font->DrawString(this, text, charOffset, x, y, 100, Jazz2::UI::Alignment::Left,
					Jazz2::UI::Font::DefaultColor, scale, 0.0f, 0.0f, 0.0f, 0.0f);
			}
		}

		void PlayMenuSfx(float gain)
		{
			if (_menuMeta == nullptr) return;
			auto it = _menuMeta->Sounds.find("MenuSelect"_s);
			if (it == _menuMeta->Sounds.end() || it->second.Buffers.empty()) return;
			const std::size_t idx = (it->second.Buffers.size() > 1
				? nCine::Random().Next(0, (std::uint32_t)it->second.Buffers.size()) : 0);
			auto player = std::make_shared<nCine::AudioBufferPlayer>(&it->second.Buffers[idx]->Buffer);
			player->setGain(gain * PreferencesCache::MasterVolume * PreferencesCache::SfxVolume);
			player->setSourceRelative(true);
			player->play();
			_menuSounds.erase(std::remove_if(_menuSounds.begin(), _menuSounds.end(), [](const auto& p) { return p->isStopped(); }), _menuSounds.end());
			_menuSounds.push_back(std::move(player));
		}

		void ApplyMenuMusicVolume()
		{
			if (_music != nullptr) _music->setGain(PreferencesCache::MasterVolume * PreferencesCache::MusicVolume);
		}

		void EnumEpisodes()
		{
			using fs = Death::IO::FileSystem;
			auto& r = ContentResolver::Get();
			std::vector<Death::Containers::String> names;
			auto scan = [&](Death::Containers::StringView base) {
				for (auto item : fs::Directory(fs::CombinePath(base, "Episodes"_s), fs::EnumerationOptions::SkipDirectories)) {
					Death::Containers::StringView fn = fs::GetFileName(item);
					if (fn.hasSuffix(".j2e"_s)) {
						Death::Containers::String nm = Death::Containers::String(fn.exceptSuffix(4));
						bool seen = false;
						for (auto& n : names) { if (n == nm) { seen = true; break; } }
						if (!seen) names.push_back(std::move(nm));
					}
				}
			};
			scan(r.GetContentPath());
			scan(r.GetCachePath());
			// Directory enumeration can come back empty on PSP, so also probe the standard episode names directly
			// (GetEpisode returns nullopt when the .j2e is absent). This is what makes the stock GOG content appear.
			for (const char* k : { "prince", "rescue", "flash", "monk", "secretf", "share", "home", "xmas98", "xmas99" }) {
				Death::Containers::String kn(k);
				bool seen = false;
				for (auto& n : names) { if (n == kn) { seen = true; break; } }
				if (!seen) names.push_back(std::move(kn));
			}
			for (auto& nm : names) {
				auto ep = r.GetEpisode(nm);
				if (!ep || ep->FirstLevel.empty()) continue;
				const bool custom = (ep->FirstLevel == ":custom-levels"_s);
				if (!custom && !r.LevelExists(Death::Containers::String(ep->Name + '/' + ep->FirstLevel))) continue;
				_episodes.push_back(EpisodeItem{ ep->Name, ep->DisplayName, ep->FirstLevel, ep->Position,
					{}, false, custom });
			}
			std::sort(_episodes.begin(), _episodes.end(), [](const EpisodeItem& a, const EpisodeItem& b) { return a.position < b.position; });
		}

		void LoadLevelCatalog()
		{
			using fs = Death::IO::FileSystem;
			auto stream = fs::Open(fs::CombinePath(ContentResolver::Get().GetCachePath(), "Levels.idx"_s), FileAccess::Read);
			if (stream == nullptr || !stream->IsValid() || stream->GetSize() < 12 || stream->GetSize() > 1024 * 1024 ||
				stream->ReadValueAsLE<std::uint64_t>() != PspLevelCatalogSignature ||
				stream->ReadValueAsLE<std::uint16_t>() != PspLevelCatalogVersion) return;

			const std::uint16_t count = stream->ReadValueAsLE<std::uint16_t>();
			_hostLevels.reserve(count);
			for (std::uint16_t i = 0; i < count; ++i) {
				const std::uint16_t nameSize = stream->ReadValueAsLE<std::uint16_t>();
				const std::uint16_t displayNameSize = stream->ReadValueAsLE<std::uint16_t>();
				if (nameSize == 0 || nameSize > 1024 || displayNameSize > 1024) {
					_hostLevels.clear(); _customLevels.clear(); return;
				}
				Death::Containers::String name{NoInit, nameSize};
				Death::Containers::String displayName{NoInit, displayNameSize};
				if (stream->Read(name.data(), nameSize) != nameSize ||
					stream->Read(displayName.data(), displayNameSize) != displayNameSize) {
					_hostLevels.clear(); _customLevels.clear(); return;
				}
				if (name.hasPrefix("unknown/"_s)) {
					_customLevels.push_back(CustomLevelItem{Death::Containers::String(name.exceptPrefix(8)),
						Death::Containers::String(displayName)});
				}
				_hostLevels.push_back(HostLevelItem{std::move(name), std::move(displayName)});
			}
			_hostLevelIdx = (_hostLevels.empty() ? -1 : 0);
			_customIdx = (_customLevels.empty() ? -1 : 0);
		}

		Screen _screen = Screen::Main;
		std::vector<EpisodeItem> _episodes;
		std::vector<CustomLevelItem> _customLevels;
		std::vector<HostLevelItem> _hostLevels;
		std::vector<std::shared_ptr<nCine::AudioBufferPlayer>> _menuSounds;
		std::unique_ptr<nCine::AudioStreamPlayer> _music;
		std::unique_ptr<nCine::Texture> _episodePreview;
		int _epIdx = 0;
		int _episodePreviewIndex = -1;
		int _episodePreviewPending = -1;
		float _episodePreviewScale = 0.0f;
		bool _episodePreviewZoomingOut = false;
		int _charIdx = 0;   // 0 Jazz, 1 Spaz, 2 Lori
		int _diffIdx = 1;   // 0 Easy, 1 Normal, 2 Hard
		int _multiplayerCharIdx = 0;
		bool _hostTeamMode = false;
		std::uint8_t _multiplayerTeam = nCine::PspAdhoc::NoAdhocTeam;
		int _joinHostIdx = 0;
		int _hostLevelIdx = 0;
		int _hostModeIdx = 0;
		int _mpSettingsRow = 0;
		int _colorSection = 0;
		std::uint32_t _playerFurColor = 0;
		int _totalKills = 10;
		int _totalLaps = 3;
		int _totalTreasure = 100;
		int _maxTimeMinutes = 10;
		int _overtimeSeconds = 30;
		int _row = 0;
		std::uint32_t _last = 0;
		bool _startRequested = false;
		bool _quitRequested = false;
		bool _regenerateCacheRequested = false;
		bool _customSelected = false;
		bool _episodeForHost = false;
		bool _hostLevelFromSettings = false;
		bool _multiplayerJoining = false;
		bool _selectedHostRunning = false;
		float _partyRetryFrames = 0.0f;
		float _characterAnim[3] = { 1.0f, 0.0f, 0.0f };
		std::int32_t _previewPaletteOffset = -1;
		std::uint32_t _previewPaletteColor = 0;
		int _previewPaletteCharacter = -1;
		int _customIdx = -1;
		Metadata* _menuMeta = nullptr;
		PspSavedataManager* _savedata = nullptr;
		StorageAction _storageAction = StorageAction::None;
		char _message[80]{};
		char _noticeTitle[48]{};
		char _noticeMessage[96]{};
		char _playerName[32]{};
		nCine::PspAdhoc& _adhoc;
		inline static PspMenu* _utilityOwner = nullptr;
		SceUtilityOskParams _oskParams{};
		SceUtilityOskData _oskData{};
		unsigned short _oskDescription[32]{};
		unsigned short _oskInput[64]{};
		unsigned short _oskOutput[64]{};
		OskTarget _oskTarget = OskTarget::ServerAddress;
		bool _oskBusy = false;
		pspUtilityNetconfData _netconfParams{};
		bool _netconfBusy = false;
		bool _netconfShutdownStarted = false;
		bool _waitForRelease = false;
		char _entryName[12]{};
		char _entryEpisode[12]{};
		std::uint32_t _entryScore = 0;
	};
}
