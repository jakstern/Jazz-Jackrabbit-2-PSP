#pragma once

#include "nCine/Backends/Psp/PspDebugLog.h"
#include "Jazz2/LevelInitialization.h"
#include "Jazz2/PreferencesCache.h"
#include "Jazz2/ContentResolver.h"
#include <psputility_savedata.h>
#include <pspkernel.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

// PSP savedata: versioned profile (settings, progression, highscores) and resumable game state, both
// through sceUtilitySavedata. The utility is modal and pumped once per frame by UpdatePspSystemUtility().
namespace Jazz2
{
	struct PspNamedPlayerState {
		Death::Containers::String Name;
		PlayerCarryOver CarryOver{};
		std::int32_t Health = 5;
	};

	static constexpr std::uint8_t PspSaveVersion = 6;
	static constexpr std::size_t MaxSavedPlayerStates = 8;

	inline void WriteNamedPlayerState(Death::IO::Stream& stream, const PspNamedPlayerState& state)
	{
		const std::uint8_t nameLength = (std::uint8_t)std::min<std::size_t>(state.Name.size(), 31);
		stream.WriteValue<std::uint8_t>(nameLength);
		stream.Write(state.Name.data(), nameLength);
		stream.WriteVariableInt32(state.Health);
		stream.WriteValue<std::uint8_t>((std::uint8_t)state.CarryOver.CurrentWeapon);
		stream.WriteValue<std::uint8_t>(state.CarryOver.Lives);
		stream.WriteValue<std::uint8_t>(state.CarryOver.FoodEaten);
		stream.WriteVariableInt32(state.CarryOver.Score);
		for (std::int32_t value : state.CarryOver.Gems) stream.WriteVariableInt32(value);
		for (std::uint16_t value : state.CarryOver.Ammo) stream.WriteValueAsLE<std::uint16_t>(value);
		for (std::uint8_t value : state.CarryOver.WeaponUpgrades) stream.WriteValue<std::uint8_t>(value);
	}

	inline bool ReadNamedPlayerState(Death::IO::Stream& stream, PspNamedPlayerState& state)
	{
		const std::uint8_t nameLength = stream.ReadValue<std::uint8_t>();
		if (nameLength == 0 || nameLength > 31) return false;
		state.Name = Death::Containers::String{NoInit, nameLength};
		if (stream.Read(state.Name.data(), nameLength) != nameLength) return false;
		state.Health = stream.ReadVariableInt32();
		const std::uint8_t weapon = stream.ReadValue<std::uint8_t>();
		if (weapon >= (std::uint8_t)WeaponType::Count) return false;
		state.CarryOver.Type = PlayerType::None;
		state.CarryOver.CurrentWeapon = (WeaponType)weapon;
		state.CarryOver.Lives = stream.ReadValue<std::uint8_t>();
		state.CarryOver.FoodEaten = stream.ReadValue<std::uint8_t>();
		state.CarryOver.Score = stream.ReadVariableInt32();
		for (std::int32_t& value : state.CarryOver.Gems) value = stream.ReadVariableInt32();
		for (std::uint16_t& value : state.CarryOver.Ammo) value = stream.ReadValueAsLE<std::uint16_t>();
		for (std::uint8_t& value : state.CarryOver.WeaponUpgrades) value = stream.ReadValue<std::uint8_t>();
		return true;
	}
	// Versioned profile: progression and highscore slots exist from day one so later versions can start
	// using them without invalidating an existing save. V1 is migrated field-by-field in ValidateLoadedProfile().
	struct PspHighscoreEntry {
		char level[24];
		std::uint32_t score;
	};

	struct alignas(16) PspProfileDataV1 {
		std::uint32_t magic;
		std::uint16_t version;
		std::uint16_t size;
		std::uint32_t checksum;
		std::uint8_t masterVolume;
		std::uint8_t sfxVolume;
		std::uint8_t musicVolume;
		std::uint8_t highscoreCount;
		std::uint32_t completedEpisodeMask; // First of eight hashed episode IDs (legacy field name).
		std::uint32_t progressionReserved[7];
		PspHighscoreEntry highscores[8];
	};

	struct alignas(16) PspProfileData {
		std::uint32_t magic;
		std::uint16_t version;
		std::uint16_t size;
		std::uint32_t checksum;
		std::uint8_t masterVolume;
		std::uint8_t sfxVolume;
		std::uint8_t musicVolume;
		std::uint8_t highscoreCount;
		std::uint32_t completedEpisodeMask;
		std::uint32_t progressionReserved[7];
		PspHighscoreEntry highscores[8];
		std::uint8_t infrastructureMode;
		std::uint8_t networkReserved[3];
		char aemuServer[64];
	};
	inline void UpdatePspSystemUtility();
	alignas(64) inline std::uint8_t g_gameSaveBuffer[384 * 1024];
	class PspSavedataManager
	{
	public:
		enum class Operation { None, BootLoad, ProfileLoad, ProfileSave, ProfileAutoSave, GameLoad, GameSave };

		PspSavedataManager() { ResetProfile(); }

		bool BeginBootLoad() { return Begin(Operation::BootLoad); }
		bool BeginProfileSave()
		{
			CapturePreferences();
			_profile.checksum = 0;
			_profile.checksum = Checksum(&_profile, sizeof(_profile));
			return Begin(Operation::ProfileSave);
		}
		bool BeginProfileAutoSave()
		{
			CapturePreferences();
			_profile.checksum = 0;
			_profile.checksum = Checksum(&_profile, sizeof(_profile));
			// Profile content never changes during a level, so returning to the menu usually re-saves identical
			// bytes. Skip it: the savedata utility suspends the whole app and writes the Memory Stick.
			if (_profile.checksum == _lastSavedChecksum) {
				return false;
			}
			return Begin(Operation::ProfileAutoSave);
		}
		bool BeginGameLoad() { return Begin(Operation::GameLoad); }
		bool BeginGameSave(LevelHandler& levelHandler, const std::vector<PspNamedPlayerState>& playerStates)
		{
			Death::IO::MemoryStream stream(g_gameSaveBuffer, sizeof(g_gameSaveBuffer));
			stream.WriteValueAsLE<std::uint64_t>(0x2095A59FF0BFBBEF);
			stream.WriteValue<std::uint8_t>(ContentResolver::StateFile);
			stream.WriteValueAsLE<std::uint16_t>(PspSaveVersion);
			bool serialized = false;
			{
				Death::IO::Compression::DeflateWriter compressed(stream);
				serialized = levelHandler.SerializeCurrentStateToStream(compressed);
				const std::uint8_t playerCount = (std::uint8_t)std::min<std::size_t>(playerStates.size(), MaxSavedPlayerStates);
				compressed.WriteValue<std::uint8_t>(playerCount);
				for (std::uint8_t i = 0; i < playerCount; i++) WriteNamedPlayerState(compressed, playerStates[i]);
			}
			const std::int64_t bytesWritten = stream.GetPosition();
			if (!serialized || bytesWritten <= 11 || bytesWritten >= (std::int64_t)sizeof(g_gameSaveBuffer)) {
				SetResult(Operation::GameSave, false, false, "The current game could not be serialized");
				return false;
			}
			_gameDataSize = (std::size_t)bytesWritten;
			{
				const struct mallinfo heap = mallinfo();
				char line[128];
				std::snprintf(line, sizeof(line),
					"savedata: serialized=%u heapUsed=%d kernelFree=%u kernelMax=%u",
					(unsigned)_gameDataSize, heap.uordblks,
					(unsigned)sceKernelTotalFreeMemSize(), (unsigned)sceKernelMaxFreeMemSize());
				DbgLog(line);
			}
			std::snprintf(_gameDetail, sizeof(_gameDetail), "Level: %.*s", 48,
				levelHandler.GetLevelDisplayName().empty() ? levelHandler.GetLevelName().data() : levelHandler.GetLevelDisplayName().data());
			return Begin(Operation::GameSave);
		}
		bool IsBusy() const { return _operation != Operation::None; }
		bool ConsumeResult(Operation& operation, bool& success, bool& loaded, const char*& message)
		{
			if (!_resultReady) return false;
			_resultReady = false;
			operation = _completedOperation;
			success = _resultSuccess;
			loaded = _resultLoaded;
			message = _resultMessage;
			return true;
		}
		const PspProfileData& Profile() const { return _profile; }
		bool IsInfrastructureMode() const { return _profile.infrastructureMode != 0; }
		const char* GetAemuServer() const { return _profile.aemuServer; }
		void SetInfrastructureMode(bool enabled) { _profile.infrastructureMode = enabled ? 1 : 0; }
		void SetAemuServer(const char* address)
		{
			std::snprintf(_profile.aemuServer, sizeof(_profile.aemuServer), "%s",
				(address != nullptr && address[0] != '\0') ? address : "192.168.0.0");
		}
		const std::uint8_t* GameData() const { return g_gameSaveBuffer; }
		std::size_t GameDataSize() const { return _gameDataSize; }
		bool MarkEpisodeCompleted(Death::Containers::StringView episodeName)
		{
			if (episodeName.empty() || episodeName == "unknown"_s) return false;
			const std::uint32_t id = Checksum(episodeName.data(), episodeName.size());
			// Mask plus the seven reserved words are eight episode-ID slots; hashing the metadata
			// name keeps stored progression independent of episode order.
			if (_profile.completedEpisodeMask == id) return false;
			for (std::uint32_t stored : _profile.progressionReserved) if (stored == id) return false;
			if (_profile.completedEpisodeMask == 0) { _profile.completedEpisodeMask = id; return true; }
			for (auto& stored : _profile.progressionReserved) if (stored == 0) { stored = id; return true; }
			return false;
		}
		bool IsEpisodeCompleted(Death::Containers::StringView episodeName) const
		{
			if (episodeName.empty()) return false;
			const std::uint32_t id = Checksum(episodeName.data(), episodeName.size());
			if (_profile.completedEpisodeMask == id) return true;
			for (std::uint32_t stored : _profile.progressionReserved) if (stored == id) return true;
			return false;
		}
		void AddHighscore(Death::Containers::StringView playerName, Death::Containers::StringView episode, std::uint32_t score)
		{
			const int count = std::min<int>(_profile.highscoreCount, 7);
			for (int i = count; i > 0; i--) _profile.highscores[i] = _profile.highscores[i - 1];
			auto& entry = _profile.highscores[0];
			std::memset(&entry, 0, sizeof(entry));
			const int nameLength = std::min<int>((int)playerName.size(), 11);
			const int episodeLength = std::min<int>((int)episode.size(), 11);
			std::snprintf(entry.level, 12, "%.*s", nameLength, playerName.data());
			std::snprintf(entry.level + 12, 12, "%.*s", episodeLength, episode.data());
			entry.score = score;
			_profile.highscoreCount = std::min<std::uint8_t>((std::uint8_t)(_profile.highscoreCount + 1), 8);
		}

		void UpdateSystemUtility()
		{
			if (_operation == Operation::None) return;
			const int status = sceUtilitySavedataGetStatus();
			if (status != _lastStatus) {
				char statusLine[64];
				std::snprintf(statusLine, sizeof(statusLine), "savedata status=%d result=%d", status, _params.base.result);
				DbgLog(statusLine);
				_lastStatus = status;
			}
			switch (status) {
				case PSP_UTILITY_DIALOG_VISIBLE:
					sceUtilitySavedataUpdate(1);
					break;
				case PSP_UTILITY_DIALOG_QUIT:
					_dialogResult = _params.base.result;
					sceUtilitySavedataShutdownStart();
					break;
				case PSP_UTILITY_DIALOG_NONE:
					Finish();
					break;
				default:
					break;
			}
		}

	private:
		static constexpr std::uint32_t Magic = 0x5350324a; // "J2PS" in little endian
		static constexpr std::uint16_t Version = 2;

		void ResetProfile()
		{
			std::memset(&_profile, 0, sizeof(_profile));
			_profile.magic = Magic;
			_profile.version = Version;
			_profile.size = sizeof(_profile);
			std::strcpy(_profile.aemuServer, "192.168.0.0");
			CapturePreferences();
		}
		void CapturePreferences()
		{
			auto toByte = [](float value) { return (std::uint8_t)std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f); };
			_profile.masterVolume = toByte(PreferencesCache::MasterVolume);
			_profile.sfxVolume = toByte(PreferencesCache::SfxVolume);
			_profile.musicVolume = toByte(PreferencesCache::MusicVolume);
		}
		void ApplyPreferences()
		{
			// The hardware volume buttons are the master control; only the game mix is ours.
			PreferencesCache::MasterVolume = 1.0f;
			PreferencesCache::SfxVolume = _profile.sfxVolume / 255.0f;
			PreferencesCache::MusicVolume = _profile.musicVolume / 255.0f;
		}
		static std::uint32_t Checksum(const void* data, std::size_t size)
		{
			const auto* bytes = static_cast<const std::uint8_t*>(data);
			std::uint32_t hash = 2166136261u;
			for (std::size_t i = 0; i < size; i++) { hash ^= bytes[i]; hash *= 16777619u; }
			return hash;
		}
		bool ValidateLoadedProfile()
		{
			if (_params.dataSize < sizeof(PspProfileDataV1)) return false;
			PspProfileDataV1 legacy{};
			std::memcpy(&legacy, _buffer.data(), sizeof(legacy));
			if (legacy.magic != Magic) return false;
			if (legacy.version == 1 && legacy.size == sizeof(PspProfileDataV1)) {
				const std::uint32_t expected = legacy.checksum;
				legacy.checksum = 0;
				if (expected != Checksum(&legacy, sizeof(legacy))) return false;
				ResetProfile();
				_profile.masterVolume = legacy.masterVolume;
				_profile.sfxVolume = legacy.sfxVolume;
				_profile.musicVolume = legacy.musicVolume;
				_profile.highscoreCount = std::min<std::uint8_t>(legacy.highscoreCount, 8);
				_profile.completedEpisodeMask = legacy.completedEpisodeMask;
				std::memcpy(_profile.progressionReserved, legacy.progressionReserved, sizeof(legacy.progressionReserved));
				std::memcpy(_profile.highscores, legacy.highscores, sizeof(legacy.highscores));
				for (auto& entry : _profile.highscores) entry.level[sizeof(entry.level) - 1] = '\0';
				ApplyPreferences();
				return true;
			}
			if (_params.dataSize < sizeof(PspProfileData)) return false;
			PspProfileData loaded{};
			std::memcpy(&loaded, _buffer.data(), sizeof(loaded));
			if (loaded.magic != Magic || loaded.version != Version || loaded.size != sizeof(loaded)) return false;
			const std::uint32_t expected = loaded.checksum;
			loaded.checksum = 0;
			if (expected != Checksum(&loaded, sizeof(loaded))) return false;
			loaded.highscoreCount = std::min<std::uint8_t>(loaded.highscoreCount, 8);
			for (auto& entry : loaded.highscores) entry.level[sizeof(entry.level) - 1] = '\0';
			loaded.infrastructureMode = loaded.infrastructureMode != 0 ? 1 : 0;
			loaded.aemuServer[sizeof(loaded.aemuServer) - 1] = '\0';
			if (loaded.aemuServer[0] == '\0') std::strcpy(loaded.aemuServer, "192.168.0.0");
			_profile = loaded;
			ApplyPreferences();
			return true;
		}
		bool LoadSaveIcon()
		{
			if (!_saveIcon.empty()) return true;
			std::FILE* file = std::fopen("Cache/ICON0.PNG", "rb");
			if (file == nullptr) return false;
			std::fseek(file, 0, SEEK_END);
			const long size = std::ftell(file);
			std::fseek(file, 0, SEEK_SET);
			if (size <= 0 || size > 64 * 1024) { std::fclose(file); return false; }
			_saveIcon.resize((std::size_t)size);
			const bool loaded = std::fread(_saveIcon.data(), 1, _saveIcon.size(), file) == _saveIcon.size();
			std::fclose(file);
			if (!loaded) _saveIcon.clear();
			return loaded;
		}
		bool Begin(Operation operation)
		{
			if (IsBusy()) return false;
			std::memset(&_params, 0, sizeof(_params));
			std::memset(_buffer.data(), 0, _buffer.size());
			_params.base.size = sizeof(_params);
			_params.base.language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
			_params.base.buttonSwap = PSP_UTILITY_ACCEPT_CROSS;
			_params.base.graphicsThread = 0x11;
			_params.base.accessThread = 0x13;
			_params.base.fontThread = 0x12;
			_params.base.soundThread = 0x10;
			const bool profile = (operation == Operation::BootLoad || operation == Operation::ProfileLoad ||
				operation == Operation::ProfileSave || operation == Operation::ProfileAutoSave);
			const bool saving = (operation == Operation::ProfileSave || operation == Operation::ProfileAutoSave || operation == Operation::GameSave);
			_params.mode = (operation == Operation::ProfileSave ? PSP_UTILITY_SAVEDATA_SAVE
				: operation == Operation::ProfileAutoSave ? PSP_UTILITY_SAVEDATA_AUTOSAVE
				: operation == Operation::BootLoad ? PSP_UTILITY_SAVEDATA_AUTOLOAD
				: operation == Operation::ProfileLoad ? PSP_UTILITY_SAVEDATA_LOAD
				: operation == Operation::GameSave ? PSP_UTILITY_SAVEDATA_LISTSAVE : PSP_UTILITY_SAVEDATA_LISTLOAD);
			_params.overwrite = 1;
			_params.focus = saving ? PSP_UTILITY_SAVEDATA_FOCUS_FIRSTEMPTY : PSP_UTILITY_SAVEDATA_FOCUS_LATEST;
			std::strcpy(_params.gameName, "JAZZ20001");
			std::strcpy(_params.saveName, profile ? "PROFILE" : "0000");
			std::strcpy(_params.fileName, profile ? "PROFILE.BIN" : "STATE.BIN");
			std::strcpy(_params.key, "J2PSPSTATEKEY01");
			_params.dataBuf = profile ? (void*)_buffer.data() : (void*)g_gameSaveBuffer;
			_params.dataBufSize = profile ? _buffer.size() : sizeof(g_gameSaveBuffer);
			_params.dataSize = saving ? (profile ? sizeof(_profile) : _gameDataSize) : _params.dataBufSize;
			if (operation == Operation::ProfileSave || operation == Operation::ProfileAutoSave) std::memcpy(_buffer.data(), &_profile, sizeof(_profile));
			_newData.icon0 = {};
			if (saving && LoadSaveIcon()) {
				_params.icon0FileData.buf = _saveIcon.data();
				_params.icon0FileData.bufSize = _saveIcon.size();
				_params.icon0FileData.size = _saveIcon.size();
			}
			if (!profile) {
				_params.saveNameList = _gameSlots;
				if (saving) {
					_newData.title = const_cast<char*>("New Game Save");
					_params.newData = &_newData;
				}
			}
			if (saving) {
				std::strcpy(_params.sfoParam.title, "Jazz 2 Resurrection");
				std::strcpy(_params.sfoParam.savedataTitle, profile ? "Player Profile" : "Saved Game");
				std::strcpy(_params.sfoParam.detail, profile ? "Preferences, progression and high scores" : _gameDetail);
				_params.sfoParam.parentalLevel = 1;
			}
			_dialogResult = 0;
			_lastStatus = -1;
			_operation = operation;
			const int result = sceUtilitySavedataInitStart(&_params);
			{
				char line[112];
				std::snprintf(line, sizeof(line),
					"savedata: init result=%d mode=%d data=%u kernelFree=%u kernelMax=%u",
					result, _params.mode, (unsigned)_params.dataSize,
					(unsigned)sceKernelTotalFreeMemSize(), (unsigned)sceKernelMaxFreeMemSize());
				DbgLog(line);
			}
			if (result < 0) {
				_operation = Operation::None;
				SetResult(operation, false, false, saving ? "Could not open save dialog" : "Could not open load dialog");
				return false;
			}
			nCine::PspGuSetSystemUtilityUpdate(&UpdatePspSystemUtility);
			return true;
		}
		void Finish()
		{
			DbgLog("savedata: utility finished");
			nCine::PspGuNotifySystemUtilityFinished();
			nCine::PspGuSetSystemUtilityUpdate(nullptr);
			const Operation completed = _operation;
			_operation = Operation::None;
			_saveIcon.clear();
			_saveIcon.shrink_to_fit();
			_newData.icon0 = {};
			const bool sdkSuccess = (_dialogResult == 0 && _params.base.result == 0);
			if (completed == Operation::ProfileSave || completed == Operation::ProfileAutoSave || completed == Operation::GameSave) {
				if (sdkSuccess && completed != Operation::GameSave) _lastSavedChecksum = _profile.checksum;
				SetResult(completed, sdkSuccess, false, sdkSuccess
					? (completed == Operation::GameSave ? "Game saved" : "Profile saved") : "Save cancelled or failed");
			} else if (completed == Operation::GameLoad) {
				if (sdkSuccess) _gameDataSize = std::min<std::size_t>(_params.dataSize, sizeof(g_gameSaveBuffer));
				SetResult(completed, sdkSuccess, sdkSuccess, sdkSuccess ? "Game loaded" : "Load cancelled or failed");
			} else {
				const bool valid = sdkSuccess && ValidateLoadedProfile();
				// A just-loaded profile matches the disk, so the next autosave correctly finds nothing to write.
				if (valid) _lastSavedChecksum = _profile.checksum;
				// Boot autoload is deliberately silent - no status banner before the first menu frame.
				if (completed != Operation::BootLoad)
					SetResult(completed, valid, valid, valid ? "Profile loaded" : "No valid profile was loaded");
			}
		}
		void SetResult(Operation operation, bool success, bool loaded, const char* message)
		{
			_completedOperation = operation;
			_resultSuccess = success;
			_resultLoaded = loaded;
			std::snprintf(_resultMessage, sizeof(_resultMessage), "%s", message);
			_resultReady = true;
		}

		PspProfileData _profile{};
		// Profile as last written to (or read from) the Memory Stick; a matching autosave is skipped.
		std::uint32_t _lastSavedChecksum = 0;
		SceUtilitySavedataParam _params{};
		alignas(64) std::array<std::uint8_t, 4096> _buffer{};
		std::vector<std::uint8_t> _saveIcon;
		Operation _operation = Operation::None;
		Operation _completedOperation = Operation::None;
		int _dialogResult = 0;
		int _lastStatus = -1;
		bool _resultReady = false;
		bool _resultSuccess = false;
		bool _resultLoaded = false;
		char _resultMessage[64]{};
		std::size_t _gameDataSize = 0;
		char _gameDetail[96] = "Jazz 2 resumable game";
		char _gameSlots[6][20] = { "0000", "0001", "0002", "0003", "0004", "" };
		PspUtilitySavedataListSaveNewData _newData{};
	};

	inline PspSavedataManager* g_savedataManager = nullptr;
	inline void UpdatePspSystemUtility()
	{
		if (g_savedataManager != nullptr) g_savedataManager->UpdateSystemUtility();
	}
}
