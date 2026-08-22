// PSP entry point. Drives the stock nCine Application lifecycle through the PSP backend seam
// (PspBackend.cpp) and the real Jazz2::LevelHandler; rendering goes out via the GU backend.
// PSP-specific glue is only: an IRootController stub, sceCtrl -> per-player input, and our own camera on
// the screen viewport, because the shader-based PlayerViewport is bypassed (LevelHandler::OnInitializeViewport,
// under DEATH_TARGET_PSP).

#include <pspkernel.h>
#include <pspctrl.h>
#include <psppower.h>
#include <psputility.h>
#include <psputility_sysparam.h>
#include <psputility_savedata.h>
#include <psputility_osk.h>
#include <psputility_netconf.h>
#include <pspsysmem.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <malloc.h> // mallinfo
#include <memory>
#include <unistd.h> // getcwd

#include "nCine/IAppEventHandler.h"
#include "nCine/Application.h"
#include "nCine/ServiceLocator.h"
#include "nCine/Graphics/Viewport.h"
#include "nCine/Graphics/Camera.h"
#include "nCine/Primitives/Colorf.h"
#include "nCine/Primitives/Vector2.h"

#include "nCine/Backends/Psp/PspGu.h" // PspGuBeginFrame/EndFrame - used to flash stage colors during the (pre-loop) init
#include "nCine/Backends/Psp/PspDiagnostics.h"
#include "nCine/Backends/Psp/PspAudioBackend.h"
#include "nCine/Backends/Psp/PspAdhoc.h"
#include "nCine/Backends/Psp/PspHardware.h"
#include "nCine/Backends/Psp/PspAssetPack.h"
#include "nCine/AppConfiguration.h"

#include "Jazz2/ContentResolver.h"
#include "Jazz2/LevelHandler.h"
#include "Jazz2/IRootController.h"
#include "Jazz2/LevelInitialization.h"
#include "Jazz2/Multiplayer/INetworkHandler.h"
#include "Jazz2/Multiplayer/MpLevelHandler.h"
#include "Jazz2/Multiplayer/Teams.h"
#include "Jazz2/Multiplayer/NetworkManager.h"
#include "Jazz2/Multiplayer/PacketTypes.h"
#include "Jazz2/GameDifficulty.h"
#include "Jazz2/LevelFlags.h"
#include "Jazz2/PlayerType.h"
#include "Jazz2/PlayerAction.h"
#include "Jazz2/Tiles/TileMap.h"
#include "Jazz2/Actors/Player.h"
#include "Jazz2/Actors/Multiplayer/MpPlayer.h"
#include "Jazz2/UI/HUD.h"
#include "Jazz2/UI/Canvas.h"
#include "Jazz2/UI/Font.h"
#include "Jazz2/UI/Alignment.h"
#include "Jazz2/PreferencesCache.h"
#include "Jazz2/Resources.h"
#include "PspBake.h"
#include "nCine/Backends/Psp/EmbeddedContent.h"
#include "nCine/Graphics/SceneNode.h"
#include "nCine/Graphics/Texture.h"
#include "nCine/Audio/AudioBufferPlayer.h"
#include "nCine/Base/Random.h"
#include "Shared/IO/FileSystem.h"
#include <IO/Compression/DeflateStream.h>
#include <IO/MemoryStream.h>

#include <Containers/StringConcatenable.h>
#include <vector>
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>

PSP_MODULE_INFO("JAZZ2ENGINE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
// Leave 1.5 MiB outside newlib for the PSP kernel, networking, and Sony utilities;
// the application heap receives the remainder of the largest user-memory block.
PSP_HEAP_SIZE_KB(-1);
PSP_HEAP_THRESHOLD_SIZE_KB(1536);
#include "Jazz2/PspSavedata.h"
#include "Jazz2/PspRootController.h"
#include "Jazz2/UI/PspOverlays.h"
#include "Jazz2/UI/PspLoadingScreen.h"
#include "Jazz2/UI/PspLevelCompleteOverlay.h"
#include "Jazz2/UI/PspAssetPreparationScreen.h"
#include "Jazz2/UI/Menu/PspMenu.h"
#include "Jazz2/UI/Menu/PspPauseMenu.h"
#include "nCine/Backends/Psp/PspDebugLog.h"

using namespace Jazz2;
using nCine::DbgLog;
using nCine::DbgHeap;
using nCine::DbgReset;
using nCine::DbgStart;
using nCine::DbgStop;
using Jazz2::PspNamedPlayerState;
using Jazz2::PspSavedataManager;
using Jazz2::UpdatePspSystemUtility;
using Jazz2::g_savedataManager;
using Jazz2::ReadNamedPlayerState;
using Jazz2::WriteNamedPlayerState;
using Jazz2::PspRootController;
using Jazz2::UI::g_systemUtilityActive;
using Jazz2::UI::PspBakeStatus;
using Jazz2::UI::PspAssetPreparationScreen;
using Jazz2::UI::PspLoadingScreen;
using Jazz2::UI::PspLevelCompleteOverlay;
using Jazz2::UI::PspDiagnosticsOverlay;
using Jazz2::UI::PspSystemUtilityDimOverlay;
using Jazz2::UI::PspWaterOverlay;
using Jazz2::UI::PspMultiplayerSyncOverlay;
using Jazz2::UI::PspPeerOverlay;
using Jazz2::UI::Menu::PspMenu;
using Jazz2::UI::Menu::PspPauseMenu;

namespace nCine
{
	void PspBootEngine(std::unique_ptr<IAppEventHandler> (*createAppEventHandler)());
	void PspStepEngine();
	void PspSuspendEngine();
	void PspResumeEngine();
	bool PspEngineWantsQuit();
	void PspShutdownEngine();
}

namespace
{
	static constexpr std::uint32_t MultiplayerProtocolVersion = 1;

	std::atomic<bool> g_running{true};
	std::atomic<bool> g_powerSuspendRequested{false};
	std::atomic<bool> g_powerSuspendPrepared{false};
	std::atomic<bool> g_powerResumeRequested{false};

	int ExitCallback(int, int, void*)
	{
		g_running.store(false, std::memory_order_release);
		return 0;
	}

	int PowerCallback(int, int powerInfo, void*)
	{
		if ((powerInfo & (PSP_POWER_CB_POWER_SWITCH | PSP_POWER_CB_SUSPENDING)) != 0) {
			// Power callbacks run on CallbackThread, while every engine object belongs to the main thread. Locking
			// standby gives the frame loop time to drain GU/audio/I/O without racing it from this callback.
			const bool firstRequest = !g_powerSuspendRequested.exchange(true, std::memory_order_acq_rel);
			int powerLockResult = -1;
			if (firstRequest) {
				g_powerResumeRequested.store(false, std::memory_order_release);
				powerLockResult = scePowerLock(0);
			}
			while (g_running.load(std::memory_order_acquire) &&
				!g_powerSuspendPrepared.load(std::memory_order_acquire)) {
				sceKernelDelayThread(1000);
			}
			if (firstRequest && powerLockResult >= 0) scePowerUnlock(0);
		}
		// Some real firmware revisions report RESUMING promptly but defer RESUME_COMPLETE until a later power
		// event. Resource reopen has bounded retries, so either notification is a safe wake signal.
		if ((powerInfo & (PSP_POWER_CB_RESUMING | PSP_POWER_CB_RESUME_COMPLETE)) != 0)
			g_powerResumeRequested.store(true, std::memory_order_release);
		return 0;
	}

	int CallbackThread(SceSize, void*)
	{
		const int exitCallback = sceKernelCreateCallback("ExitCallback", ExitCallback, nullptr);
		if (exitCallback >= 0) sceKernelRegisterExitCallback(exitCallback);
		const int powerCallback = sceKernelCreateCallback("PowerCallback", PowerCallback, nullptr);
		if (powerCallback >= 0) scePowerRegisterCallback(-1, powerCallback);
		sceKernelSleepThreadCB();
		return 0;
	}

	void SetupCallbacks()
	{
		int th = sceKernelCreateThread("CallbackThread", CallbackThread, 0x11, 0xFA0, 0, nullptr);
		if (th >= 0) sceKernelStartThread(th, 0, nullptr);
	}

	bool ServicePowerState()
	{
		if (!g_powerSuspendRequested.load(std::memory_order_acquire)) return true;
		if (!g_powerSuspendPrepared.load(std::memory_order_acquire)) {
			nCine::PspSuspendEngine();
			g_powerSuspendPrepared.store(true, std::memory_order_release);
		}
		if (!g_powerResumeRequested.load(std::memory_order_acquire)) {
			sceKernelDelayThread(1000);
			return false;
		}

		nCine::PspResumeEngine();
		g_powerResumeRequested.store(false, std::memory_order_release);
		g_powerSuspendPrepared.store(false, std::memory_order_release);
		g_powerSuspendRequested.store(false, std::memory_order_release);
		return true;
	}




	class PspEventHandler : public nCine::IAppEventHandler, public Multiplayer::INetworkHandler
	{
	public:
		void OnPreInitialize(nCine::AppConfiguration& config) override
		{
			// Keep gameplay at the engine's native 60 Hz (collision depends on it); Step presents every
			// second frame instead, giving 30 Hz on screen.
			if (nCine::PspIsLowMemoryModel()) config.frameLimit = 60;
		}

		void OnShutdown() override
		{
			nCine::PspGuSetSystemUtilityUpdate(nullptr);
			s_hotJoinOwner = nullptr;
			g_savedataManager = nullptr;
			DbgLog("stage: shutdown");
			DbgStop();
		}

		void OnSuspend() override
		{
			DbgLog("stage: power suspend");
			nCine::PspAssetPack::Get().Suspend();
			DbgStop();
		}

		void OnResume() override
		{
			DbgStart();
			bool packReady = false;
			for (int attempt = 0; attempt < 50 && !packReady; ++attempt) {
				packReady = nCine::PspAssetPack::Get().Resume();
				if (!packReady) sceKernelDelayThread(10000);
			}
			DbgLog(packReady ? "stage: power resume" : "stage: power resume (texture pack unavailable)");

			SceCtrlData pad{};
			pad.Lx = 128;
			pad.Ly = 128;
			if (sceCtrlPeekBufferPositive(&pad, 1) <= 0) pad.Buttons = 0;
			pauseButtonsLast_ = pad.Buttons;
			if (levelHandler_ != nullptr) levelHandler_->SuppressInputUntilRelease();
			if (menu_ != nullptr) menu_->LatchInput();
			if (pauseMenu_ != nullptr) pauseMenu_->LatchInput();
			if (preparationScreen_ != nullptr) preparationScreen_->LatchInput();
		}

		Multiplayer::ConnectionResult OnPeerConnected(const Multiplayer::Peer&, std::uint32_t) override
		{
			return true;
		}

		void OnPeerDisconnected(const Multiplayer::Peer& peer, Multiplayer::Reason reason) override
		{
			const auto& transport = adhoc_.GetTransportDiagnostics();
			const char* operation = (transport.LastOperation == nCine::PspAdhoc::TransportOperation::Send ? "send" :
				transport.LastOperation == nCine::PspAdhoc::TransportOperation::Receive ? "recv" : "none");
			char transportLine[256];
			std::snprintf(transportLine, sizeof(transportLine),
				"ptp disconnect reason=%u buffer=%d op=%s result=%08X requested=%d transferred=%d queued=%d sends=%u recvs=%u failures=%u",
				(unsigned)reason, nCine::PspAdhoc::TransportBufferSize, operation,
				(unsigned)transport.LastResult, transport.LastRequestedBytes, transport.LastTransferredBytes,
				transport.LastQueuedBytes, transport.SendCalls, transport.ReceiveCalls, transport.Failures);
			DbgLog(transportLine);
			if (multiplayerActive_ && levelHandler_ != nullptr) {
				auto* mp = static_cast<Multiplayer::MpLevelHandler*>(levelHandler_.get());
				if (multiplayerHost_ && networkManager_ != nullptr) {
					auto peerDesc = networkManager_->GetPeerDescriptor(peer);
					if (peerDesc != nullptr) CaptureGuestState(*mp, peer, peerDesc->PlayerName);
				}
				mp->OnPeerDisconnected(peer);
				if (multiplayerHost_) {
					// Retire only the guest that left. Clearing every slot re-ran AttachAdhoc, which
					// re-announced the survivors as new peers mid-game and (state is matched by player
					// name) let one inherit the leaver's carry-over.
					const int slot = static_cast<int>(peer.GetId()) - 1;
					if (slot >= 0 && slot < nCine::PspAdhoc::MaxGuests) {
						multiplayerPeerConfigured_[slot] = false;
					}
				} else {
					// A guest only ever has the host as a peer, so losing it ends the session.
					ResetPeerConfiguration();
					multiplayerTransportAttached_ = false;
					std::strcpy(pendingDisconnectMessage_, "Host closed the game");
					root_._returnToMenu = true;
				}
			}
		}

		void OnPacketReceived(const Multiplayer::Peer& peer, std::uint8_t channelId, std::uint8_t packetType,
			Death::Containers::ArrayView<const std::uint8_t> data) override
		{
			if (multiplayerActive_ && levelHandler_ != nullptr) {
				if (channelId == std::uint8_t(Multiplayer::NetworkChannel::Main)) {
					++mpReliablePackets_;
				} else if (channelId == std::uint8_t(Multiplayer::NetworkChannel::UnreliableUpdates)) {
					++mpUpdatePackets_;
				}
				if (!multiplayerHost_) {
					switch (Multiplayer::ServerPacketType(packetType)) {
						case Multiplayer::ServerPacketType::CreateControllablePlayer: ++mpCreatePlayerPackets_; break;
						case Multiplayer::ServerPacketType::CreateRemoteActor:
						case Multiplayer::ServerPacketType::CreateMirroredActor: ++mpCreateActorPackets_; break;
						default: break;
					}
				}
				static_cast<Multiplayer::MpLevelHandler*>(levelHandler_.get())->OnPacketReceived(peer, channelId, packetType, data);
			}
		}

		static void ReportAssetBakeProgress(void* userData, int completed, int total, const char* currentItem)
		{
			auto* status = static_cast<PspBakeStatus*>(userData);
			(void)currentItem;
			status->Completed.store(completed, std::memory_order_relaxed);
			status->Total.store(std::max(total, 1), std::memory_order_relaxed);
			status->Phase.store(0, std::memory_order_release);
		}

		static void ReportAssetBakeDetail(void* userData, PspBakeStage stage, int completed, int total)
		{
			auto* status = static_cast<PspBakeStatus*>(userData);
			status->DetailCompleted.store(completed, std::memory_order_relaxed);
			status->DetailTotal.store(std::max(total, 1), std::memory_order_relaxed);
			status->DetailStage.store((int)stage, std::memory_order_release);
			status->Phase.store(0, std::memory_order_release);
		}

		static void ReportMusicBakeProgress(void* userData, std::uint32_t completedMilliseconds,
			std::uint32_t totalMilliseconds, int completedTracks, int totalTracks, const char* currentTrack)
		{
			auto* status = static_cast<PspBakeStatus*>(userData);
			(void)currentTrack;
			if (status->Phase.load(std::memory_order_relaxed) != 1) {
				status->MusicStartedAtUs = sceKernelGetSystemTimeWide();
				status->MusicRate = 10.0; // PPSSPP-measured seed; cumulative timing quickly replaces it.
			}
			const std::uint64_t elapsedUs = sceKernelGetSystemTimeWide() - status->MusicStartedAtUs;
			if (completedMilliseconds >= 5000 && elapsedUs >= 500000) {
				const double measuredRate = double(completedMilliseconds) * 1000.0 / double(elapsedUs);
				status->MusicRate = status->MusicRate * 0.8 + measuredRate * 0.2;
			}
			const std::uint32_t pendingMilliseconds = (totalMilliseconds > completedMilliseconds
				? totalMilliseconds - completedMilliseconds : 0);
			const int remainingSeconds = (status->MusicRate > 0.01
				? int(double(pendingMilliseconds) / status->MusicRate / 1000.0 + 0.5) : -1);
			status->MusicMilliseconds.store((int)completedMilliseconds, std::memory_order_relaxed);
			status->MusicTotalMilliseconds.store((int)std::max(totalMilliseconds, 1U), std::memory_order_relaxed);
			status->MusicTracksCompleted.store(completedTracks, std::memory_order_relaxed);
			status->MusicTracksTotal.store(totalTracks, std::memory_order_relaxed);
			status->RemainingSeconds.store(remainingSeconds, std::memory_order_relaxed);
			status->Phase.store(1, std::memory_order_release);
		}

		static int AssetBakeThread(SceSize args, void* argp)
		{
			if (args != sizeof(PspEventHandler*) || argp == nullptr) return 1;
			PspEventHandler* owner = *static_cast<PspEventHandler**>(argp);
			if (owner->bakeRecreate_) {
				using fs = Death::IO::FileSystem;
				for (const char* file : { "audio.pak", "texture.pak", "Source.idx", "Source.version", "Levels.idx", "ICON0.PNG" })
					fs::RemoveFile(fs::CombinePath(owner->bakeCachePath_, Death::Containers::StringView(file)));
				fs::RemoveDirectoryRecursive(fs::CombinePath(owner->bakeCachePath_, "Episodes"_s));
				fs::RemoveDirectoryRecursive(fs::CombinePath(owner->bakeCachePath_, "Tilesets"_s));
				fs::RemoveDirectoryRecursive(fs::CombinePath(owner->bakeCachePath_, "Music"_s));
			}

			PspBakeOptions options{};
			options.SourceDirectory = owner->bakeSourcePath_;
			options.OutputDirectory = ".";
			options.ContentDirectory = owner->bakeContentPath_;
			options.Progress = &PspEventHandler::ReportAssetBakeProgress;
			options.MusicProgress = &PspEventHandler::ReportMusicBakeProgress;
			options.DetailProgress = &PspEventHandler::ReportAssetBakeDetail;
			options.ProgressUserData = &owner->bakeStatus_;
			const int result = RunPspBake(options);
			owner->bakeStatus_.Result.store(result, std::memory_order_relaxed);
			owner->bakeStatus_.Finished.store(true, std::memory_order_release);
			return result;
		}

		bool HasUsableAssetCache() const
		{
			using fs = Death::IO::FileSystem;
			bool hasPspTileset = false;
			const auto tilesetsPath = fs::CombinePath(bakeCachePath_, "Tilesets"_s);
			for (auto item : fs::Directory(tilesetsPath, fs::EnumerationOptions::SkipDirectories)) {
				if (fs::GetExtension(item) == "j2tpsp"_s) { hasPspTileset = true; break; }
			}
			return fs::IsReadableFile(fs::CombinePath(bakeCachePath_, "Source.idx"_s)) &&
				fs::IsReadableFile(fs::CombinePath(bakeCachePath_, "Levels.idx"_s)) &&
				fs::IsReadableFile(fs::CombinePath(bakeCachePath_, "texture.pak"_s)) &&
				// Reject a stale pack whose format predates the current PackVersion, forcing a one-time re-bake.
				nCine::PspAssetPack::IsCompatibleOnDisk(bakeCachePath_) &&
				fs::IsReadableFile(fs::CombinePath(bakeCachePath_, "audio.pak"_s)) &&
				fs::IsReadableFile(fs::CombinePath({ bakeCachePath_, "Music"_s, "menu.wav"_s })) &&
				hasPspTileset;
		}

		// The bake's peak allocations only fit the 64 MiB models, so refuse it up front on a PSP-1000 (model 0)
		// rather than after teardown. A negative model means kubridge is missing: let it try instead of blocking
		// what may well be a 2000/3000.
		bool IsAssetBakeSupported() const { return nCine::PspHardwareModel() != 0; }

		void ShowAssetBakeUnsupported()
		{
			if (preparationScreen_ != nullptr) { preparationScreen_->setParent(nullptr); preparationScreen_ = nullptr; }
			// Keep the menu alive (it is the only way back) but stop it from consuming input behind the message.
			const bool dismissable = (menu_ != nullptr);
			if (menu_ != nullptr) menu_->setUpdateEnabled(false);
			bakeStatus_.Result.store(-1, std::memory_order_relaxed);
			bakeStatus_.Finished.store(false, std::memory_order_release);
			preparationScreen_ = std::make_unique<PspAssetPreparationScreen>(bakeStatus_);
			preparationScreen_->MarkUnsupported(dismissable);
			preparationScreen_->setParent(&nCine::theApplication().GetRootNode());
			state_ = AppState::Preparing;
		}

		void StartAssetBake(bool recreate)
		{
			if (bakeThread_ >= 0) return;
			if (!IsAssetBakeSupported()) {
				ShowAssetBakeUnsupported();
				return;
			}
			auto& audioDevice = nCine::theServiceLocator().GetAudioDevice();
			// Stop the high-priority mixer before its menu players are destroyed under it.
			if (!bakeAudioSuspended_) {
				audioDevice.suspendDevice();
				bakeAudioSuspended_ = true;
			}
			audioDevice.stopPlayers();
			if (preparationScreen_ != nullptr) { preparationScreen_->setParent(nullptr); preparationScreen_ = nullptr; }
			if (menu_ != nullptr) { menu_->setParent(nullptr); menu_ = nullptr; }
			pauseMenuSounds_.clear();
			ContentResolver::Get().Release();
			// Only this path may unmount: regeneration replaces audio.pak. Ordinary scene changes also call
			// Release() and need the pack to stay mounted for the next level's sound effects.
			ContentResolver::Get().UnmountPaks();
			nCine::PspReleaseRendererCaches();
			nCine::PspAssetPack::Get().Reset();
			bakeStatus_.Completed.store(0, std::memory_order_relaxed);
			bakeStatus_.Total.store(1, std::memory_order_relaxed);
			bakeStatus_.DetailStage.store((int)PspBakeStage::Animations, std::memory_order_relaxed);
			bakeStatus_.DetailCompleted.store(0, std::memory_order_relaxed);
			bakeStatus_.DetailTotal.store(0, std::memory_order_relaxed);
			bakeStatus_.Phase.store(0, std::memory_order_relaxed);
			bakeStatus_.MusicMilliseconds.store(0, std::memory_order_relaxed);
			bakeStatus_.MusicTotalMilliseconds.store(0, std::memory_order_relaxed);
			bakeStatus_.MusicTracksCompleted.store(0, std::memory_order_relaxed);
			bakeStatus_.MusicTracksTotal.store(0, std::memory_order_relaxed);
			bakeStatus_.RemainingSeconds.store(-1, std::memory_order_relaxed);
			bakeStatus_.Result.store(-1, std::memory_order_relaxed);
			bakeStatus_.Finished.store(false, std::memory_order_release);
			bakeRecreate_ = recreate;
			preparationScreen_ = std::make_unique<PspAssetPreparationScreen>(bakeStatus_);
			preparationScreen_->setParent(&nCine::theApplication().GetRootNode());
			state_ = AppState::Preparing;

			bakeThread_ = sceKernelCreateThread("Jazz2AssetBake", AssetBakeThread, 0x28, 192 * 1024,
				PSP_THREAD_ATTR_USER, nullptr);
			PspEventHandler* self = this;
			if (bakeThread_ < 0 || sceKernelStartThread(bakeThread_, sizeof(self), &self) < 0) {
				if (bakeThread_ >= 0) sceKernelDeleteThread(bakeThread_);
				bakeThread_ = -1;
				bakeStatus_.Result.store(1, std::memory_order_relaxed);
				bakeStatus_.Finished.store(true, std::memory_order_release);
			}
		}

		void FinishAssetBake()
		{
			if (bakeThread_ >= 0) {
				sceKernelWaitThreadEnd(bakeThread_, nullptr);
				sceKernelDeleteThread(bakeThread_);
				bakeThread_ = -1;
			}
			if (bakeStatus_.Result.load(std::memory_order_relaxed) != 0) return;
			if (preparationScreen_ != nullptr) { preparationScreen_->setParent(nullptr); preparationScreen_ = nullptr; }
			nCine::PspAssetPack::Get().Reset();
			ContentResolver::Get().RemountPaks();
			if (bakeAudioSuspended_) {
				nCine::theServiceLocator().GetAudioDevice().resumeDevice();
				bakeAudioSuspended_ = false;
			}
			menu_ = std::make_unique<PspMenu>(&savedata_, adhoc_);
			menu_->setParent(&nCine::theApplication().GetRootNode());
			state_ = AppState::Menu;
			if (!profileBootLoadStarted_) {
				profileBootLoadStarted_ = true;
				if (savedata_.BeginBootLoad()) SuspendForSystemUtility();
			}
		}

		void OnInitialize() override
		{
			DbgReset();
			DbgStart();
#if defined(JAZZ2_PSP_DEBUG)
			// Let the memory report reach the async debug writer and read the collision-mask / cache footprint.
			nCine::PspDiagnosticsSetLogSink([](const char* line) { DbgLog(line); });
			nCine::PspDiagnosticsSetContentMemoryProvider([](nCine::PspMemoryReport& r) {
				std::uint32_t maskBytes = 0;
				std::uint16_t metadataCount = 0, graphicsCount = 0, soundsCount = 0;
				ContentResolver::Get().GetPspMemoryFootprint(maskBytes, metadataCount, graphicsCount, soundsCount);
				r.maskResidentBytes = maskBytes;
				r.cachedMetadata = metadataCount;
				r.cachedGraphics = graphicsCount;
				r.cachedSounds = soundsCount;
			});
#endif
			const int clockResult = scePowerSetClockFrequency(333, 333, 166);
			{
				char clocks[96];
				std::snprintf(clocks, sizeof(clocks), "clock: set=%08X pll=333 cpu=%d bus=%d",
					(unsigned)clockResult, scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt());
				DbgLog(clocks);
			}
			char cwd[512];
			DbgLog("cwd:", (getcwd(cwd, sizeof(cwd)) != nullptr ? cwd : "<getcwd failed>"));
			DbgLog("stage: OnInitialize enter");
			{
#if defined(JAZZ2_PSP_LEGACY_MEMORY)
				constexpr int legacyMem = 1;
#else
				constexpr int legacyMem = 0;
#endif
				char hardware[128];
				std::snprintf(hardware, sizeof(hardware), "hardware: model=%d memoryProfile=%u lowMemory=%d legacyMem=%d frameLimit=%u",
					nCine::PspHardwareModel(), (unsigned)nCine::PspHeapCapacityBytes(), nCine::PspIsLowMemoryModel() ? 1 : 0,
					legacyMem, nCine::theApplication().GetAppConfiguration().frameLimit);
				DbgLog(hardware);

				// PSP_HEAP_SIZE_KB(-1) makes _sbrk grab one block of (sceKernelMaxFreeMemSize() - THRESHOLD) at the
				// first malloc. Afterwards sceKernelMaxFreeMemSize() only sees the ~1.5 MiB left outside it and
				// mallinfo() only what has been sbrk'd, so neither is the real ceiling - hence this probe.
				// It goes through sbrk, not malloc: sbrk owns the ceiling (refuses once top+incr passes heap_end),
				// whereas a malloc probe once returned non-null for a 64 MiB request on a 64 MiB machine.
				{
					const void* breakAtBoot = sbrk(0);
					std::size_t lo = 0, hi = (std::size_t)192 * 1024 * 1024;
					while (lo < hi) {
						const std::size_t mid = lo + (hi - lo + 1) / 2;
						void* probe = sbrk((intptr_t)mid);
						if (probe != (void*)-1) { sbrk(-(intptr_t)mid); lo = mid; } else { hi = mid - 1; }
					}
					const struct mallinfo mi = mallinfo();
					char heap[224];
					std::snprintf(heap, sizeof(heap),
						"heap: arena=%u sbrkHeadroom=%u capacity~=%u kernelFree=%u kernelMax=%u",
						(unsigned)mi.arena, (unsigned)lo, (unsigned)(mi.arena + lo),
						(unsigned)sceKernelTotalFreeMemSize(), (unsigned)sceKernelMaxFreeMemSize());
					DbgLog(heap);
					// Where the heap lives, not just how big it is: user RAM starts at 0x08800000 and the standard
					// partition ends at 0x0A000000. Past that is the extended partition MEMSIZE=1 grants on a
					// 2000/3000, so a break that crosses it makes "out of memory" a question about that region.
					char span[224];
					std::snprintf(span, sizeof(span),
						"heap.span: break=%p end~=%p crosses0x0A000000=%d extendedBytes=%d",
						breakAtBoot, (const void*)((const char*)breakAtBoot + lo),
						((std::uintptr_t)breakAtBoot + lo) > 0x0A000000u ? 1 : 0,
						(int)(((std::uintptr_t)breakAtBoot + lo) > 0x0A000000u
							? ((std::uintptr_t)breakAtBoot + lo) - 0x0A000000u : 0u));
					DbgLog(span);
				}
			}

			auto& viewport = nCine::theApplication().GetScreenViewport();
			auto& resolver = ContentResolver::Get();
			std::snprintf(bakeSourcePath_, sizeof(bakeSourcePath_), "%.*s", (int)resolver.GetSourcePath().size(), resolver.GetSourcePath().data());
			std::snprintf(bakeCachePath_, sizeof(bakeCachePath_), "%.*s", (int)resolver.GetCachePath().size(), resolver.GetCachePath().data());
			std::snprintf(bakeContentPath_, sizeof(bakeContentPath_), "%.*s", (int)resolver.GetContentPath().size(), resolver.GetContentPath().data());

			// Safe because the PSP viewport refreshes drawable AABBs after the camera is finalized. TileMap
			// keeps using its own visible-tile culling.
			nCine::theApplication().GetRenderingSettings().cullingEnabled = true;
			// The stock batcher's command format needs a vertex shader to unpack instances; the PSP emitter
			// merges compatible quads into indexed GU draws itself.
			nCine::theApplication().GetRenderingSettings().batchingEnabled = false;

			// The menu that normally sets these is filtered out on PSP. Without a base palette the CLUT is
			// all-zero; a level's own palette still overrides it in Initialize.
			resolver.ApplyDefaultPalette();

			Jazz2::PreferencesCache::EnableReforgedHUD = false;
			Jazz2::PreferencesCache::MasterVolume = 1.0f;

			// The ScreenViewport has no camera of its own; once set it becomes the current camera each Draw,
			// so tilemap culling and the emitter follow it.
			cam_.SetOrthoProjection(0.0f, 480.0f, 0.0f, 272.0f);
			viewport.SetCamera(&cam_);

			sceCtrlSetSamplingCycle(0);
			sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

			g_savedataManager = &savedata_;
			diagnostics_ = std::make_unique<PspDiagnosticsOverlay>();
			diagnostics_->setParent(&nCine::theApplication().GetRootNode());
			utilityDim_ = std::make_unique<PspSystemUtilityDimOverlay>();
			utilityDim_->setParent(&nCine::theApplication().GetRootNode());
			peerOverlay_ = std::make_unique<PspPeerOverlay>(adhoc_);
			peerOverlay_->setParent(&nCine::theApplication().GetRootNode());
			waterOverlay_ = std::make_unique<PspWaterOverlay>();
			waterOverlay_->setParent(&nCine::theApplication().GetRootNode());
			if (HasUsableAssetCache()) {
				menu_ = std::make_unique<PspMenu>(&savedata_, adhoc_);
				menu_->setParent(&nCine::theApplication().GetRootNode());
				state_ = AppState::Menu;
				profileBootLoadStarted_ = true;
				if (savedata_.BeginBootLoad()) SuspendForSystemUtility();
			} else {
				StartAssetBake(false);
			}

			viewport.SetClearColor(nCine::Colorf(0.03f, 0.02f, 0.06f, 1.0f));
			DbgLog("stage: menu ready");
		}

		Death::Containers::String GetLoadingTitle(Death::Containers::StringView levelName)
		{
			auto parts = levelName.partition('/');
			Death::Containers::String fallback(!parts[2].empty() ? parts[2] : parts[0]);
			auto& resolver = ContentResolver::Get();
			for (auto base : { resolver.GetCachePath(), resolver.GetContentPath() }) {
				auto path = Death::IO::FileSystem::CombinePath({ base, "Episodes"_s,
					Death::Containers::String(levelName + ".j2l"_s) });
				auto stream = Death::IO::FileSystem::Open(path, FileAccess::Read);
				if (stream == nullptr || !stream->IsValid()) continue;
				if (stream->ReadValueAsLE<std::uint64_t>() != 0x2095A59FF0BFBBEFull ||
					stream->ReadValue<std::uint8_t>() != ContentResolver::LevelFile) continue;
				stream->ReadValueAsLE<std::uint16_t>(); // Level flags
				const std::int32_t compressedSize = stream->ReadValueAsLE<std::int32_t>();
				if (compressedSize <= 0) continue;
				Death::IO::Compression::DeflateStream data(*stream, compressedSize);
				const std::uint8_t length = data.ReadValue<std::uint8_t>();
				if (length == 0) continue;
				Death::Containers::String displayName{NoInit, length};
				if (data.Read(displayName.data(), length) == length) return displayName;
			}
			return fallback;
		}

		// Tears down the outgoing scene and shows the loading canvas. The level itself is built a few frames
		// later, once that canvas has actually reached the display.
		void StartLevel(LevelInitialization&& init)
		{
			CaptureConnectedGuestState();
			if (multiplayerSyncOverlay_ != nullptr) {
				multiplayerSyncOverlay_->setParent(nullptr);
				multiplayerSyncOverlay_ = nullptr;
			}
			savedGameLoadingPending_ = false;
			_queuedLevelInit = std::make_unique<LevelInitialization>(std::move(init));
			DbgLog("transition: loading level", _queuedLevelInit->LevelName.data());
			auto& resolver = ContentResolver::Get();
			if (menu_ != nullptr) {
				// Drop the menu's RGBA art (~8 MiB of portraits plus backgrounds) here rather than in
				// FinishLoadingLevel: otherwise it co-resides with the loading font through the tightest
				// memory window (heapFree measured down to ~185 KB on 01g). The scoped GC is safe - only the
				// loading screen holds cached graphics at this point.
				menu_->setParent(nullptr);
				menu_ = nullptr;
				resolver.BeginLoading();
				loadingScreenMeta_ = resolver.RequestMetadata("UI/Loading"_s);
				resolver.EndLoading();
			} else if (loadingScreenMeta_ == nullptr) {
				loadingScreenMeta_ = resolver.RequestMetadata("UI/Loading"_s);
			}
			const auto loadingTitle = GetLoadingTitle(_queuedLevelInit->LevelName);
			loadingScreen_ = std::make_unique<PspLoadingScreen>(loadingScreenMeta_, loadingTitle);
			loadingScreen_->setParent(&nCine::theApplication().GetRootNode());
			loadingFrames_ = 0;
			state_ = AppState::Loading;
			DbgHeap("transition: loading screen ready");
		}

		std::unique_ptr<Multiplayer::NetworkManager> CreateMultiplayerNetworkManager()
		{
			auto manager = std::make_unique<Multiplayer::NetworkManager>();
			if (multiplayerHost_ || !multiplayerActive_) {
				const auto& session = adhoc_.GetSessionSettings();
				Multiplayer::ServerConfiguration config{};
				config.ServerName = adhoc_.GetLocalName();
				config.WelcomeMessage = "PSP ad-hoc game";
				config.MaxPlayerCount = 1 + nCine::PspAdhoc::MaxGuests;
				config.MinPlayerCount = 1;
				config.GameMode = (multiplayerActive_
					? static_cast<Multiplayer::MpGameMode>(session.GameMode)
					: Multiplayer::MpGameMode::Cooperation);
				config.ServerPort = 31001;
				config.IsPrivate = true;
				config.AllowAssetStreaming = false;
				config.AllowedPlayerTypes = 0x07;
				config.IdleKickTimeSecs = -1;
				config.ReforgedGameplay = true;
				config.TeamCount = 2;
				// Off on purpose, so a deliberate 2v1 / 3v1 stack is honoured. "Auto" players still land on the
				// smallest team: ResolveTeam sends NoPreferredTeam down the balancing path regardless of this flag.
			config.AutoBalanceTeams = false;
				config.MaxTeamSizeDiff = 1;
				config.AllowTeamSelection = true;
				config.AllowMinimap = false;
				config.ColorizePlayersByTeam = true;
				config.InitialPlayerHealth = 5;
				config.MaxGameTimeSecs = (multiplayerActive_ ? session.MaxGameTimeSecs : 0);
				config.TotalKills = (multiplayerActive_ ? session.TotalKills : 0);
				config.TotalLaps = (multiplayerActive_ ? session.TotalLaps : 0);
				config.TotalTreasureCollected = (multiplayerActive_ ? session.TotalTreasureCollected : 0);
				config.OvertimeSecs = (multiplayerActive_ ? session.OvertimeSecs : 0);
				config.SpawnInvulnerableSecs = 0;
				config.EnableSpectate = true;
				config.PlayerStacking = true;
				config.EnableFreeCamera = true;
				config.AllowJoinDuringRound = true;
				config.PlaylistIndex = -1;
				if (multiplayerActive_) manager->CreateServer(nullptr, std::move(config));
				else manager->CreateLocalServer(nullptr, std::move(config));
			} else {
				manager->CreateClient(nullptr, {}, 31001,
					0xDEA00000 | (MultiplayerProtocolVersion & 0x000fffff));
			}
			return manager;
		}

		void FinishLoadingLevel()
		{
			if (_queuedLevelInit == nullptr) { ReturnToMenu("Level request was lost"); return; }
			LevelInitialization ownedInit(std::move(*_queuedLevelInit));
			_queuedLevelInit = nullptr;
			// The already-presented splash stays in the physical framebuffer through the synchronous decode
			// below, even though its resources are released here.
			nCine::PspGuWaitForPreviousFrame();
			if (loadingScreen_ != nullptr) { loadingScreen_->setParent(nullptr); loadingScreen_ = nullptr; }
			if (completeOverlay_ != nullptr) { completeOverlay_->setParent(nullptr); completeOverlay_ = nullptr; }
			if (pauseMenu_ != nullptr) { pauseMenu_->setParent(nullptr); pauseMenu_ = nullptr; }
			if (menu_ != nullptr) { menu_->setParent(nullptr); menu_ = nullptr; }
			root_._returnToMenu = false;
			root_._hasPendingLevel = false;
			root_._pendingLevel = nullptr;
			levelHandler_ = nullptr;
			if (waterOverlay_ != nullptr) waterOverlay_->Clear();
			pauseMenuMeta_ = nullptr;
			loadingScreenMeta_ = nullptr;
			pauseMenuSounds_.clear();
			ContentResolver::Get().Release();
			DbgHeap("transition: cleared splash before level");
			ownedInit.IsLocalSession = !multiplayerActive_;
			ResetPeerConfiguration();
			multiplayerTransportAttached_ = false;
			networkManager_ = CreateMultiplayerNetworkManager();
			std::unique_ptr<LevelHandler> lh = std::make_unique<Multiplayer::MpLevelHandler>(&root_, networkManager_.get(),
				Multiplayer::MpLevelHandler::LevelState::InitialUpdatePending, true);
			// Single-player always uses the character's native palette, so hide the profile's fur colour only
			// while players are created and restore it for the next party session.
			const std::uint32_t savedFurColor = PreferencesCache::PlayerFurColor;
			if (!multiplayerActive_) PreferencesCache::PlayerFurColor = 0;
			// The host's team preference must land BEFORE Initialize(): SpawnPlayers() resolves the team on the
			// spot and an unset preference goes through FindSmallestTeam(), which breaks ties randomly - an
			// explicit choice lost to a coin flip. PlayerName/FurColor stay below because SpawnPlayers
			// overwrites those, whereas PreferredTeam is only read.
			if (multiplayerActive_ && multiplayerHost_) {
				auto localBeforeSpawn = networkManager_->AddLocalPlayer(0);
				if (localBeforeSpawn != nullptr && !localBeforeSpawn->TeamLocked) {
					localBeforeSpawn->PreferredTeam = adhoc_.GetLocalTeam();
				}
			}
			const bool initialized = lh->Initialize(ownedInit);
			PreferencesCache::PlayerFurColor = savedFurColor;
			if (!initialized) {
				DbgLog("StartLevel: Initialize FALSE");
				ReturnToMenu();
				return;
			}
			if (multiplayerActive_) {
				auto local = networkManager_->GetPeerDescriptor(Multiplayer::LocalPeer);
				if (local != nullptr) {
					local->PlayerName = adhoc_.GetLocalName();
					local->FurColor = PreferencesCache::PlayerFurColor;
				}
				if (!multiplayerHost_ || adhoc_.IsSessionReady()) {
					if (!adhoc_.BeginEngineTransport()) {
						ReturnToMenu("Cannot start multiplayer transport");
						return;
					}
					networkManager_->AttachAdhoc(&adhoc_, multiplayerHost_, this,
						0xDEA00000 | (MultiplayerProtocolVersion & 0x000fffff));
					multiplayerTransportAttached_ = true;
				}
			}
			levelHandler_ = std::move(lh);
			levelHandler_->SuppressInputUntilRelease();
			levelHandler_->SetPspFlyCheat(cheatFlyEnabled_);
			levelHandler_->SetPspGodCheat(cheatGodEnabled_);
			// Preload the pause menu's rotating-plane art now, while the transition is already loading,
			// so the first START press does not stall.
			pauseMenuMeta_ = ContentResolver::Get().RequestMetadata("UI/MainMenu"_s);
			currentLevelName_ = ownedInit.LevelName;
			if (multiplayerActive_ && multiplayerHost_) {
				const auto levelDisplayName = GetLoadingTitle(ownedInit.LevelName);
				adhoc_.SetSessionLevel(ownedInit.LevelName.data(), levelDisplayName.data());
			}
			levelHandler_->OnInitializeViewport(480, 272);
			cameraViewCenterInitialized_ = false;
			FollowCamera();
			if (multiplayerActive_ && !multiplayerHost_ &&
				!static_cast<Multiplayer::MpLevelHandler*>(levelHandler_.get())->IsInitialSyncReady()) {
				multiplayerSyncOverlay_ = std::make_unique<PspMultiplayerSyncOverlay>();
				multiplayerSyncOverlay_->setParent(&nCine::theApplication().GetRootNode());
			}
			state_ = AppState::Game;
			startMusicAfterPresent_ = true;
		}

		void ReturnToMenu(const char* message = nullptr, bool autoSaveProfile = true)
		{
			const bool hostClosedGame = (pendingDisconnectMessage_[0] != '\0' && message == nullptr);
			const char* displayMessage = (message != nullptr ? message
				: (pendingDisconnectMessage_[0] != '\0' ? pendingDisconnectMessage_ : nullptr));
			if (multiplayerActive_) adhoc_.DisconnectPeer();
			if (networkManager_ != nullptr) { networkManager_->Dispose(); networkManager_ = nullptr; }
			adhoc_.Stop();
			nCine::PspGuWaitForPreviousFrame();
			if (completeOverlay_ != nullptr) { completeOverlay_->setParent(nullptr); completeOverlay_ = nullptr; }
			if (loadingScreen_ != nullptr) { loadingScreen_->setParent(nullptr); loadingScreen_ = nullptr; }
			if (multiplayerSyncOverlay_ != nullptr) { multiplayerSyncOverlay_->setParent(nullptr); multiplayerSyncOverlay_ = nullptr; }
			if (pauseMenu_ != nullptr) { pauseMenu_->setParent(nullptr); pauseMenu_ = nullptr; }
			if (menu_ != nullptr) { menu_->setParent(nullptr); menu_ = nullptr; }
			root_._returnToMenu = false;
			root_._hasPendingLevel = false;
			root_._pendingLevel = nullptr;
			levelHandler_ = nullptr;
			if (waterOverlay_ != nullptr) waterOverlay_->Clear();
			pauseMenuMeta_ = nullptr;
			loadingScreenMeta_ = nullptr;
			pauseMenuSounds_.clear();
			ContentResolver::Get().Release();
			DbgHeap("transition: cleared before menu");
			currentLevelName_ = {};
			savedGameLevelName_ = {};
			levelHistory_.clear();
			suppressNextHistoryPush_ = false;
			_queuedLevelInit = nullptr;
			startMusicAfterPresent_ = false;
			savedGameLoadingPending_ = false;
			_afterProfileSaveTransition = nullptr;
			_pendingTerminalEpisode = {};
			_pendingTerminalScore = 0;
			cameraViewCenterInitialized_ = false;
			multiplayerActive_ = false;
			multiplayerHost_ = false;
			ResetPeerConfiguration();
			multiplayerTransportAttached_ = false;
			guestStates_.clear();
			menu_ = std::make_unique<PspMenu>(&savedata_, adhoc_);
			menu_->setParent(&nCine::theApplication().GetRootNode());
			if (hostClosedGame) menu_->ShowNotice("Host Closed Game", "The host ended the multiplayer session.");
			else if (displayMessage != nullptr) menu_->ShowMessage(displayMessage);
			pendingDisconnectMessage_[0] = '\0';
			state_ = AppState::Menu;
			if (autoSaveProfile && !savedata_.IsBusy() && savedata_.BeginProfileAutoSave()) SuspendForSystemUtility();
		}

		void ApplyLevelTransition(LevelInitialization&& levelInit)
		{
			const PlayerCarryOver* firstPlayer = nullptr;
			levelInit.GetPlayerCount(&firstPlayer);
			if (levelInit.LevelName.empty()) { ReturnToMenu(); return; }
			auto parts = levelInit.LevelName.partition('/');
			const auto target = (!parts[2].empty() ? parts[2] : parts[0]);
			if (target == ":end"_s || target == ":credits"_s) {
				_pendingTerminalScore = (firstPlayer != nullptr ? firstPlayer->Score : 0);
				_pendingTerminalEpisode = levelInit.LastEpisodeName;
				savedata_.MarkEpisodeCompleted(levelInit.LastEpisodeName);
				_afterProfileSaveTransition = std::make_unique<LevelInitialization>(std::move(levelInit));
				if (levelHandler_ != nullptr) levelHandler_->PauseForPspMenu();
				if (savedata_.BeginProfileAutoSave()) { SuspendForSystemUtility(); return; }
				LevelInitialization continuation(std::move(*_afterProfileSaveTransition));
				_afterProfileSaveTransition = nullptr;
				ContinueCompletedEpisode(std::move(continuation));
				return;
			}
			if (target == ":gameover"_s) {
				const std::uint32_t score = (firstPlayer != nullptr ? firstPlayer->Score : 0);
				Death::Containers::String episode(levelInit.LastEpisodeName);
				ReturnToMenu("Game over");
				if (menu_ != nullptr) menu_->BeginHighscoreEntry(episode, score);
				return;
			}
			if (suppressNextHistoryPush_) suppressNextHistoryPush_ = false;
			else if (!currentLevelName_.empty()) levelHistory_.emplace_back(currentLevelName_);
			StartLevel(std::move(levelInit));
		}

		void ContinueCompletedEpisode(LevelInitialization&& levelInit)
		{
			auto parts = levelInit.LevelName.partition('/');
			const auto target = (!parts[2].empty() ? parts[2] : parts[0]);
			if (target == ":end"_s && !levelInit.LastEpisodeName.empty()) {
				auto episode = ContentResolver::Get().GetEpisode(levelInit.LastEpisodeName);
				if (episode && !episode->NextEpisode.empty()) {
					auto next = ContentResolver::Get().GetEpisode(episode->NextEpisode);
					if (next && !next->FirstLevel.empty()) {
						levelInit.LevelName = episode->NextEpisode + '/' + next->FirstLevel;
						if (!currentLevelName_.empty()) levelHistory_.emplace_back(currentLevelName_);
						StartLevel(std::move(levelInit));
						return;
					}
				}
			}
			Death::Containers::String episode(_pendingTerminalEpisode);
			const std::uint32_t score = _pendingTerminalScore;
			ReturnToMenu("Episode completed", false);
			if (menu_ != nullptr) menu_->BeginHighscoreEntry(episode, score);
		}

		void OnBeginFrame() override
		{
			// Must be set BEFORE the early return: the utilities that need dimming are the ones that make
			// this function bail out.
			g_systemUtilityActive = (savedata_.IsBusy() || hotJoinInfra_ == HotJoinInfra::Netconf ||
				(menu_ != nullptr && menu_->IsSystemUtilityBusy()));
			if (savedata_.IsBusy() || hotJoinInfra_ == HotJoinInfra::Netconf) return;
			if (state_ == AppState::Preparing) return;
			adhoc_.Update();
			if (multiplayerActive_ && multiplayerHost_ && !multiplayerTransportAttached_ && levelHandler_ != nullptr &&
				networkManager_ != nullptr && adhoc_.IsSessionReady() && adhoc_.HasStartAcknowledged() &&
				adhoc_.BeginEngineTransport()) {
				networkManager_->AttachAdhoc(&adhoc_, true, this,
					0xDEA00000 | (MultiplayerProtocolVersion & 0x000fffff));
				multiplayerTransportAttached_ = true;
			} else if (multiplayerActive_ && multiplayerHost_ && multiplayerTransportAttached_) {
				// Idempotent per peer, so this only catches a guest that reconnected mid-game: the one-shot
				// attach above already ran, so without this it never crosses the engine barrier, is never
				// announced (OnPeerConnected is gated on IsPeerEngineReady) and hangs on "Synchronizing".
				// Deliberately not a re-AttachAdhoc, which would re-announce the survivors as well.
				adhoc_.BeginEngineTransport();
			}
			if (networkManager_ != nullptr) {
				networkManager_->Update();
				if (multiplayerHost_) ConfigureConnectedGuests();
			}
			if (multiplayerSyncOverlay_ != nullptr && multiplayerActive_ && !multiplayerHost_ && levelHandler_ != nullptr &&
				static_cast<Multiplayer::MpLevelHandler*>(levelHandler_.get())->IsInitialSyncReady()) {
				multiplayerSyncOverlay_->setParent(nullptr);
				multiplayerSyncOverlay_ = nullptr;
			}
			if (state_ == AppState::Game && startMusicAfterPresent_ && levelHandler_ != nullptr && multiplayerSyncOverlay_ == nullptr) {
				// One full gameplay frame is now on screen, so the music can no longer lead the picture.
				levelHandler_->StartPspMusicAfterFirstPresent();
				startMusicAfterPresent_ = false;
			}
			SceCtrlData pad{};
			pad.Lx = 128;
			pad.Ly = 128;
			if (sceCtrlPeekBufferPositive(&pad, 1) <= 0) pad.Buttons = pauseButtonsLast_;
			const std::uint32_t pauseHit = pad.Buttons & ~pauseButtonsLast_;
			pauseButtonsLast_ = pad.Buttons;
			// Engine order is OnBeginFrame -> viewport update (player moves) -> OnPostUpdate -> Draw, and input
			// is injected inside LevelHandler::OnBeginFrame so it is live for this frame's update.
			if (state_ == AppState::Game && levelHandler_ != nullptr &&
				!levelHandler_->IsPspNormalLevelExitActive() && (pauseHit & PSP_CTRL_START)) {
				levelHandler_->PauseForPspMenu();
				const bool allowSaveLoad = !multiplayerActive_ ||
					(multiplayerHost_ && static_cast<Multiplayer::MpLevelHandler*>(levelHandler_.get())->GetGameMode() ==
						Multiplayer::MpGameMode::Cooperation);
				const bool canToggleJoining =
					static_cast<Multiplayer::MpLevelHandler*>(levelHandler_.get())->GetGameMode() == Multiplayer::MpGameMode::Cooperation &&
					(!multiplayerActive_ || multiplayerHost_);
				const bool allowTestingCheats = !multiplayerActive_ || multiplayerHost_;
				ShieldType activeShield = ShieldType::None;
				const auto players = levelHandler_->GetPlayers();
				if (!players.empty()) activeShield = players[0]->GetActiveShield();
				pauseMenu_ = std::make_unique<PspPauseMenu>(pauseMenuMeta_, pauseMenuSounds_, multiplayerActive_, allowSaveLoad,
					canToggleJoining, multiplayerActive_ && multiplayerHost_, allowTestingCheats,
					cheatFlyEnabled_, cheatGodEnabled_, activeShield);
				pauseMenu_->setParent(&nCine::theApplication().GetRootNode());
				pauseMenu_->PlayOpenSfx();
				state_ = AppState::Paused;
				// Online actors stay live behind the pause menu, so the MpLevelHandler lifecycle must complete.
				if (multiplayerActive_) {
					levelHandler_->SetPspMenuBackgroundSimulation(true);
					levelHandler_->OnBeginFrame();
				}
				return;
			}
			if (state_ == AppState::Paused && levelHandler_ != nullptr && (pauseHit & PSP_CTRL_START)) {
				if (pauseMenu_ != nullptr) pauseMenu_->PlayCloseSfx();
				ResumePausedGame();
				return;
			}
			if (levelHandler_ != nullptr &&
				((state_ == AppState::Game && multiplayerSyncOverlay_ == nullptr) ||
				 (state_ == AppState::Paused && multiplayerActive_))) {
				levelHandler_->OnBeginFrame();
			}
		}

		void OnPostUpdate() override
		{
			QueueDiagnosticsSnapshot();
			if (state_ == AppState::Preparing) {
				if (bakeStatus_.Finished.load(std::memory_order_acquire)) FinishAssetBake();
				if (state_ == AppState::Preparing && preparationScreen_ != nullptr && preparationScreen_->RetryRequested()) {
					preparationScreen_->ClearRetry();
					StartAssetBake(true);
				}
				if (state_ == AppState::Preparing && preparationScreen_ != nullptr && preparationScreen_->DismissRequested()) {
					preparationScreen_->ClearDismiss();
					if (menu_ != nullptr) {
						preparationScreen_->setParent(nullptr);
						preparationScreen_ = nullptr;
						menu_->setUpdateEnabled(true);
						menu_->LatchInput();
						state_ = AppState::Menu;
					}
				}
				return;
			}
			if (savedata_.IsBusy() || hotJoinInfra_ == HotJoinInfra::Netconf) {
				// Drawing the frozen game under Sony's translucent UI is fine; skipping OnEndFrame is not -
				// that is what leaked the level's per-frame render command caches.
				if (state_ == AppState::Paused && levelHandler_ != nullptr) levelHandler_->OnEndFrame();
				return;
			}
			if (utilitySuspended_) {
				FinishSavedataInteraction();
				// That call may have just attached the splash (Sony load, or an autosaved episode transition);
				// leave its loading state for the next frame so the splash gets drawn.
				if (state_ == AppState::Loading) return;
			}
			if (state_ == AppState::Loading) {
				// Hold a complete splash frame in the framebuffer across the synchronous construction below.
				// Two updates, not one: a PSP-1000 presents every second 60 Hz update, so a transition
				// requested on a skipped render update would otherwise never submit the splash.
				if (++loadingFrames_ >= 2) {
					// The frame submitted at the end of the previous Step is normally presented just before
					// this Step's Draw, which we are about to block; swap the finished splash to the front now.
					nCine::PspGuPrepareFrame();
					if (savedGameLoadingPending_) {
						savedGameLoadingPending_ = false;
						if (!LoadGameState()) ReturnToMenu("Saved game is invalid or incompatible");
					} else FinishLoadingLevel();
				}
				return;
			}
			if (state_ == AppState::Menu) {
				// The menu canvas consumed input during the scene update; act on its result here, at frame end.
				if (menu_ != nullptr) {
					if (menu_->RegenerateCacheRequested()) {
						menu_->ClearRegenerateCacheRequest();
						StartAssetBake(true);
						return;
					}
					if (menu_->QuitRequested()) { g_running = false; return; }
					if (menu_->MultiplayerStartRequested()) {
						multiplayerActive_ = true;
						multiplayerHost_ = adhoc_.IsHosting();
						multiplayerTransportAttached_ = false;
						mpReliablePackets_ = 0;
						mpUpdatePackets_ = 0;
						mpCreateActorPackets_ = 0;
						mpCreatePlayerPackets_ = 0;
						StartLevel(menu_->BuildMultiplayerLevelInit());
					}
					else if (menu_->StartRequested()) { StartLevel(menu_->BuildLevelInit()); }
					else if (menu_->RequestedStorageAction() != PspMenu::StorageAction::None) {
						const auto action = menu_->RequestedStorageAction();
						menu_->ClearStorageAction();
						if (action == PspMenu::StorageAction::Save) BeginProfileSaveInteraction();
						else if (action == PspMenu::StorageAction::AutoSave) BeginProfileAutoSaveInteraction();
						else BeginGameSavedataInteraction(false);
					}
				}
				return;
			}
			if (state_ == AppState::Paused) {
				if (pauseMenu_ == nullptr) return;
				// Online keeps simulating behind the menu, single-player is frozen, but both must finish the
				// render lifecycle to recycle the TileMap command caches.
				if (levelHandler_ != nullptr) levelHandler_->OnEndFrame();
				if (multiplayerActive_) {
					if (root_._returnToMenu) {
						root_._returnToMenu = false;
						root_._hasPendingLevel = false;
						ReturnToMenu();
						return;
					}
					if (root_._hasPendingLevel && root_._pendingLevel != nullptr) {
						root_._hasPendingLevel = false;
						LevelInitialization transition(std::move(*root_._pendingLevel));
						root_._pendingLevel = nullptr;
						ApplyLevelTransition(std::move(transition));
						return;
					}
				}
				switch (pauseMenu_->RequestedAction()) {
					case PspPauseMenu::Action::Resume: ResumePausedGame(); break;
					case PspPauseMenu::Action::Save: pauseMenu_->ClearAction(); BeginGameSavedataInteraction(true); break;
					case PspPauseMenu::Action::Load: pauseMenu_->ClearAction(); BeginGameSavedataInteraction(false); break;
					case PspPauseMenu::Action::ToggleJoining:
						pauseMenu_->ClearAction();
						if (multiplayerActive_) StopHotJoinHosting();
						else BeginHotJoinHosting();
						break;
					case PspPauseMenu::Action::ToggleDiagnostics:
						pauseMenu_->ClearAction();
						nCine::PspDiagnosticsToggleVisible();
						break;
					case PspPauseMenu::Action::CheatNext:
						levelHandler_->ShowLevelText("CHEAT: NEXT LEVEL"_s);
						levelHandler_->PspCheatSkipForward();
						ResumePausedGame();
						break;
					case PspPauseMenu::Action::CheatPrevious:
						if (!levelHistory_.empty()) {
							Death::Containers::String previous(std::move(levelHistory_.back()));
							levelHistory_.pop_back();
							suppressNextHistoryPush_ = true;
							levelHandler_->ShowLevelText("CHEAT: PREVIOUS LEVEL"_s);
							levelHandler_->PspCheatSkipTo(previous);
							ResumePausedGame();
						} else {
							pauseMenu_->ClearAction();
							pauseMenu_->ShowStorageResult("No previous level");
						}
						break;
					case PspPauseMenu::Action::CheatFly:
						pauseMenu_->ClearAction();
						cheatFlyEnabled_ = !cheatFlyEnabled_;
						levelHandler_->SetPspFlyCheat(cheatFlyEnabled_);
						pauseMenu_->SetTestingState(cheatFlyEnabled_, cheatGodEnabled_);
						break;
					case PspPauseMenu::Action::CheatGod:
						pauseMenu_->ClearAction();
						cheatGodEnabled_ = !cheatGodEnabled_;
						levelHandler_->SetPspGodCheat(cheatGodEnabled_);
						pauseMenu_->SetTestingState(cheatFlyEnabled_, cheatGodEnabled_);
						break;
					case PspPauseMenu::Action::CheatShield:
						pauseMenu_->ClearAction();
						pauseMenu_->SetShieldState(levelHandler_->PspCheatCycleShield());
						break;
					case PspPauseMenu::Action::CheatCoins:
						pauseMenu_->ClearAction();
						levelHandler_->PspInvokeCheat("jjcoins"_s);
						break;
					case PspPauseMenu::Action::CheatBird:
						pauseMenu_->ClearAction();
						levelHandler_->PspInvokeCheat("jjbird"_s);
						break;
					case PspPauseMenu::Action::CheatGuns:
						pauseMenu_->ClearAction();
						levelHandler_->PspInvokeCheat("jjguns"_s);
						break;
					case PspPauseMenu::Action::CheatLives:
						pauseMenu_->ClearAction();
						levelHandler_->PspInvokeCheat("jjlife"_s);
						break;
					case PspPauseMenu::Action::CheatGems:
						pauseMenu_->ClearAction();
						levelHandler_->PspInvokeCheat("jjgems"_s);
						break;
					case PspPauseMenu::Action::CheatPower:
						pauseMenu_->ClearAction();
						levelHandler_->PspInvokeCheat("jjpower"_s);
						break;
					case PspPauseMenu::Action::QuitToMenu: ReturnToMenu(); break;
					default: break;
				}
				return;
			}

			if (levelHandler_ == nullptr) return;
			// The scene update has moved the player: resolve collisions, then place the camera before Draw.
			levelHandler_->OnEndFrame();
			FollowCamera();

			if (levelHandler_->IsPspNormalLevelExitActive()) {
				auto players = levelHandler_->GetPlayers();
				nCine::Vector2f screenPos(240.0f, 136.0f);
				if (!players.empty()) {
					const nCine::Vector2f pos = players[0]->GetPos();
					screenPos = nCine::Vector2f(pos.X - cameraViewCenterX_ + 240.0f,
						pos.Y - cameraViewCenterY_ + 136.0f);
				}
				if (completeOverlay_ == nullptr) {
					std::array<std::int32_t, 4> gems{};
					if (!players.empty()) {
						for (int i = 0; i < 4; i++) gems[i] = players[0]->GetGems((std::uint8_t)i);
					}
					completeOverlay_ = std::make_unique<PspLevelCompleteOverlay>(gems, screenPos);
					completeOverlay_->setParent(&nCine::theApplication().GetRootNode());
				}
				if (!players.empty()) {
					const bool runOff = players[0]->IsPspEndOfLevelTransition();
					players[0]->SetPspEndOfLevelForeground(runOff);
					if (runOff) completeOverlay_->BeginRunOff(screenPos);
				}
			} else if (completeOverlay_ != nullptr) {
				auto players = levelHandler_->GetPlayers();
				if (!players.empty()) players[0]->SetPspEndOfLevelForeground(false);
				completeOverlay_->setParent(nullptr);
				completeOverlay_ = nullptr;
			}

			// Transitions are applied here, not when requested, so the old LevelHandler is never destroyed
			// from inside its own update.
			if (root_._returnToMenu) { root_._returnToMenu = false; root_._hasPendingLevel = false; ReturnToMenu(); return; }
			if (root_._hasPendingLevel && root_._pendingLevel != nullptr) {
				root_._hasPendingLevel = false;
				LevelInitialization transition(std::move(*root_._pendingLevel));
				root_._pendingLevel = nullptr;
				ApplyLevelTransition(std::move(transition));
				return;
			}

		}

	private:
		enum class AppState { Preparing, Menu, Loading, Game, Paused };

		void QueueDiagnosticsSnapshot()
		{
#if !defined(JAZZ2_PSP_DEBUG)
			// Nowhere to write it, and the on-screen overlay reads the same counters directly - so skip the
			// five snprintf into a 1 KB buffer entirely.
			return;
#else
			// Twice a second; the low-priority debug worker does all the file I/O.
			if ((++frameLog_ % 30) != 0) return;

			const auto& diagnostics = nCine::PspDiagnosticsGetSnapshot();
			const char* stateName = "menu";
			switch (state_) {
				case AppState::Preparing: stateName = "preparing"; break;
				case AppState::Loading: stateName = "loading"; break;
				case AppState::Game: stateName = "game"; break;
				case AppState::Paused: stateName = "paused"; break;
				default: break;
			}
			char b[1024];
			std::size_t length = (std::size_t)std::snprintf(b, sizeof(b), "alive frame=%d heapUsed=%u kernelFree=%u", frameLog_,
				diagnostics.heapUsedBytes, diagnostics.kernelFreeBytes);
			auto phase = [&](nCine::PspDiagnosticsPhase p) { return diagnostics.phaseMs[static_cast<int>(p)]; };
			const float gpuWait = phase(nCine::PspDiagnosticsPhase::GpuWait);
			const float vblank = phase(nCine::PspDiagnosticsPhase::VBlank);
			const float work = std::max(0.0f, diagnostics.totalMs - gpuWait - vblank);
			length += (std::size_t)std::snprintf(b + length, sizeof(b) - length,
				"\nrender state=%s overlay=%u frame=%.2f work=%.2f gpu=%.2f vb=%.2f visit=%.2f cull=%.2f collect=%.2f queue=%.2f emit=%.2f spr=%u mesh=%u/%u draw=%u/%u batch=%u/%u culled=%u tex=%u clut=%u",
				stateName, nCine::PspDiagnosticsIsVisible() ? 1u : 0u, diagnostics.totalMs, work, gpuWait, vblank,
				phase(nCine::PspDiagnosticsPhase::Visit), phase(nCine::PspDiagnosticsPhase::VisitCulling),
				phase(nCine::PspDiagnosticsPhase::VisitCollect), phase(nCine::PspDiagnosticsPhase::Queue),
				phase(nCine::PspDiagnosticsPhase::Emit), diagnostics.spriteCommands, diagnostics.meshCommands,
				diagnostics.meshVertices, diagnostics.draws, diagnostics.vertices, diagnostics.batchFlushes,
				diagnostics.batchedQuads, diagnostics.culled, diagnostics.textureImages, diagnostics.clutLoads);
			const nCine::PspAudioStats audio = nCine::GetPspAudioStats();
			length += (std::size_t)std::snprintf(b + length, sizeof(b) - length,
				"\naudio loaded=%u failed=%u started=%u mixBlocks=%u sfx=%u mixUs=%u maxMixUs=%u misses=%u peak=%u saturated=%u errors=%u",
				audio.buffersLoaded, audio.buffersFailed, audio.voicesStarted, audio.mixBlocks,
				audio.activeSfxVoices, audio.mixMicroseconds, audio.maxMixMicroseconds, audio.deadlineMisses,
				audio.peakAccumulator, audio.saturatedSamples, audio.outputErrors);
			length += (std::size_t)std::snprintf(b + length, sizeof(b) - length, "\nmusic loaded=%u mixedFrames=%u",
				audio.musicStreamsLoaded, audio.musicFramesMixed);
			if (multiplayerActive_ && levelHandler_ != nullptr) {
				length += (std::size_t)std::snprintf(b + length, sizeof(b) - length, "\nmp role=%s actors=%u players=%u main=%u create=%u player=%u update=%u",
					multiplayerHost_ ? "host" : "client", (unsigned)levelHandler_->GetActors().size(),
					(unsigned)levelHandler_->GetPlayers().size(), mpReliablePackets_, mpCreateActorPackets_,
					mpCreatePlayerPackets_, mpUpdatePackets_);
				if (multiplayerHost_) {
					const auto& metrics = static_cast<Multiplayer::MpLevelHandler*>(levelHandler_.get())->GetActorUpdateMetrics();
					length += (std::size_t)std::snprintf(b + length, sizeof(b) - length,
						"\nmp stream candidates=%u emitted=%u packet=%uB wireRate=%uB/s serialize=%uus full=%u samples=%u",
						metrics.CandidateActors, metrics.EmittedActors, metrics.PacketBytes,
						metrics.PacketBytes * 30, metrics.SerializeMicros,
						metrics.ForceResync ? 1u : 0u, metrics.Samples);
				}
				const auto& transport = adhoc_.GetTransportDiagnostics();
				const char* operation = (transport.LastOperation == nCine::PspAdhoc::TransportOperation::Send ? "send" :
					transport.LastOperation == nCine::PspAdhoc::TransportOperation::Receive ? "recv" : "none");
				length += (std::size_t)std::snprintf(b + length, sizeof(b) - length,
					"\nptp buffer=%d op=%s result=%08X requested=%d transferred=%d queued=%d sends=%u recvs=%u failures=%u",
					nCine::PspAdhoc::TransportBufferSize, operation, (unsigned)transport.LastResult,
					transport.LastRequestedBytes, transport.LastTransferredBytes, transport.LastQueuedBytes,
					transport.SendCalls, transport.ReceiveCalls, transport.Failures);
			}
			DbgLog(b);
#endif
		}

		bool BeginHotJoinHosting()
		{
			if (levelHandler_ == nullptr || networkManager_ == nullptr || multiplayerActive_) return false;
			// Infrastructure mode needs the Sony network profile up first, and that dialog is asynchronous,
			// so hosting completes later in FinishHotJoinNetconf(). Plain ad-hoc can host immediately.
			if (savedata_.IsInfrastructureMode()) return BeginHotJoinInfrastructure();
			if (!adhoc_.Start()) {
				if (pauseMenu_ != nullptr) pauseMenu_->ShowStorageResult(adhoc_.GetStatusText());
				return false;
			}
			return FinishHotJoinHosting();
		}

		// Starts the Sony netconf dialog; hosting continues in FinishHotJoinNetconf(). The dialog is driven
		// from the shared system-utility callback slot, so nothing else may claim it meanwhile.
		bool BeginHotJoinInfrastructure()
		{
			if (hotJoinInfra_ != HotJoinInfra::Idle) return false;
			if (!adhoc_.PrepareInfrastructure()) {
				if (pauseMenu_ != nullptr) pauseMenu_->ShowStorageResult(adhoc_.GetStatusText());
				return false;
			}
			std::memset(&hotJoinNetconf_, 0, sizeof(hotJoinNetconf_));
			hotJoinNetconf_.base.size = sizeof(hotJoinNetconf_);
			hotJoinNetconf_.base.language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
			hotJoinNetconf_.base.buttonSwap = PSP_UTILITY_ACCEPT_CROSS;
			hotJoinNetconf_.base.graphicsThread = 0x11;
			hotJoinNetconf_.base.accessThread = 0x13;
			hotJoinNetconf_.base.fontThread = 0x12;
			hotJoinNetconf_.base.soundThread = 0x10;
			hotJoinNetconf_.action = PSP_NETCONF_ACTION_CONNECTAP;
			const int result = sceUtilityNetconfInitStart(&hotJoinNetconf_);
			if (result < 0) {
				char reason[64];
				std::snprintf(reason, sizeof(reason), "Network profile failed: %08X", static_cast<unsigned>(result));
				adhoc_.CancelInfrastructure(reason);
				if (pauseMenu_ != nullptr) pauseMenu_->ShowStorageResult(reason);
				return false;
			}
			hotJoinInfra_ = HotJoinInfra::Netconf;
			hotJoinNetconfShutdown_ = false;
			s_hotJoinOwner = this;
			if (menu_ != nullptr) menu_->setUpdateEnabled(false);
			if (pauseMenu_ != nullptr) pauseMenu_->setUpdateEnabled(false);
			nCine::theServiceLocator().GetAudioDevice().suspendDevice();
			nCine::PspGuSetSystemUtilityUpdate(&PspEventHandler::UpdateHotJoinUtility);
			return true;
		}

		void DriveHotJoinNetconf()
		{
			const int status = sceUtilityNetconfGetStatus();
			if (status == PSP_UTILITY_DIALOG_VISIBLE) {
				sceUtilityNetconfUpdate(1);
			} else if ((status == PSP_UTILITY_DIALOG_QUIT || status == PSP_UTILITY_DIALOG_FINISHED) && !hotJoinNetconfShutdown_) {
				sceUtilityNetconfShutdownStart();
				hotJoinNetconfShutdown_ = true;
			} else if (status == PSP_UTILITY_DIALOG_NONE) {
				FinishHotJoinNetconf();
			}
		}

		void FinishHotJoinNetconf()
		{
			if (hotJoinInfra_ != HotJoinInfra::Netconf) return;
			hotJoinInfra_ = HotJoinInfra::Idle;
			s_hotJoinOwner = nullptr;
			nCine::PspGuSetSystemUtilityUpdate(nullptr);
			nCine::PspGuNotifySystemUtilityFinished();
			nCine::theServiceLocator().GetAudioDevice().resumeDevice();
			if (menu_ != nullptr) { menu_->setUpdateEnabled(true); menu_->LatchInput(); }
			if (pauseMenu_ != nullptr) { pauseMenu_->setUpdateEnabled(true); pauseMenu_->LatchInput(); }
			if (hotJoinNetconf_.base.result != 0) {
				adhoc_.CancelInfrastructure("Network connection cancelled");
				if (pauseMenu_ != nullptr) pauseMenu_->ShowStorageResult("Network connection cancelled");
				return;
			}
			if (!adhoc_.StartInfrastructure(savedata_.GetAemuServer())) {
				if (pauseMenu_ != nullptr) pauseMenu_->ShowStorageResult(adhoc_.GetStatusText());
				adhoc_.Stop();
				return;
			}
			if (!FinishHotJoinHosting() && pauseMenu_ != nullptr) {
				pauseMenu_->ShowStorageResult("Cannot advertise game");
			}
		}

		static void UpdateHotJoinUtility()
		{
			if (s_hotJoinOwner != nullptr) s_hotJoinOwner->DriveHotJoinNetconf();
		}

		bool FinishHotJoinHosting()
		{
			char playerName[32]{};
			if (sceUtilityGetSystemParamString(PSP_SYSTEMPARAM_ID_STRING_NICKNAME, playerName, sizeof(playerName)) < 0 ||
				playerName[0] == '\0') {
				std::strcpy(playerName, "Player");
			}
			playerName[sizeof(playerName) - 1] = '\0';
			for (char* c = playerName; *c != '\0'; ++c) {
				if ((unsigned char)*c < 0x20 || *c == 0x7f) *c = ' ';
			}

			std::uint8_t character = 0;
			auto players = levelHandler_->GetPlayers();
			if (!players.empty()) {
				switch (players[0]->GetPlayerType()) {
					case PlayerType::Spaz: character = 1; break;
					case PlayerType::Lori: character = 2; break;
					default: break;
				}
			}

			nCine::PspAdhoc::SessionSettings settings{};
			settings.GameMode = static_cast<std::uint8_t>(Multiplayer::MpGameMode::Cooperation);
			switch (levelHandler_->GetDifficulty()) {
				case GameDifficulty::Easy: settings.Difficulty = 0; break;
				case GameDifficulty::Hard: settings.Difficulty = 2; break;
				default: settings.Difficulty = 1; break;
			}
			std::snprintf(settings.LevelName, sizeof(settings.LevelName), "%s", currentLevelName_.data());
			const auto levelDisplayName = GetLoadingTitle(currentLevelName_);
			std::snprintf(settings.LevelDisplayName, sizeof(settings.LevelDisplayName), "%s", levelDisplayName.data());
			adhoc_.BeginHosting(playerName, character, settings);
			if (!adhoc_.RequestStartGame()) {
				adhoc_.Stop();
				if (pauseMenu_ != nullptr) pauseMenu_->ShowStorageResult("Cannot advertise game");
				return false;
			}

			if (auto local = networkManager_->GetPeerDescriptor(Multiplayer::LocalPeer)) {
				local->PlayerName = playerName;
				// Started as single-player, so keep the character's native palette.
				local->FurColor = 0;
			}
			multiplayerActive_ = true;
			multiplayerHost_ = true;
			ResetPeerConfiguration();
			multiplayerTransportAttached_ = false;
			levelHandler_->SetPspMenuBackgroundSimulation(true);
			if (pauseMenu_ != nullptr) pauseMenu_->SetJoiningAllowed(true);
			return true;
		}

		void StopHotJoinHosting()
		{
			if (!multiplayerActive_ || !multiplayerHost_) return;
			CaptureConnectedGuestState();
			adhoc_.DisconnectPeer();
			adhoc_.Stop();
			if (networkManager_ != nullptr) {
				// Let the normal disconnect path drop the remote actor and descriptor before going local.
				networkManager_->Update();
				networkManager_->DetachAdhocToLocalSession();
			}
			multiplayerActive_ = false;
			multiplayerHost_ = false;
			ResetPeerConfiguration();
			multiplayerTransportAttached_ = false;
			if (levelHandler_ != nullptr) levelHandler_->SetPspMenuBackgroundSimulation(false);
			if (pauseMenu_ != nullptr) pauseMenu_->SetJoiningAllowed(false);
		}

		void SuspendForSystemUtility()
		{
			utilitySuspended_ = true;
			if (menu_ != nullptr) menu_->setUpdateEnabled(false);
			if (pauseMenu_ != nullptr) pauseMenu_->setUpdateEnabled(false);
			nCine::theServiceLocator().GetAudioDevice().suspendDevice();
		}
		void BeginProfileSaveInteraction()
		{
			if (savedata_.BeginProfileSave()) SuspendForSystemUtility();
		}
		void BeginProfileAutoSaveInteraction()
		{
			if (savedata_.BeginProfileAutoSave()) SuspendForSystemUtility();
		}
		void BeginGameSavedataInteraction(bool save)
		{
			if (save) CaptureConnectedGuestState();
			const std::vector<PspNamedPlayerState> playerStates = (save ? BuildSavedPlayerStates() : std::vector<PspNamedPlayerState>{});
			const bool started = save
				? (levelHandler_ != nullptr && savedata_.BeginGameSave(*levelHandler_, playerStates))
				: savedata_.BeginGameLoad();
			if (started) SuspendForSystemUtility();
			else if (save && pauseMenu_ != nullptr) pauseMenu_->ShowStorageResult("The current game cannot be saved");
		}
		void ResetPeerConfiguration()
		{
			for (auto& configured : multiplayerPeerConfigured_) configured = false;
		}
		// Host side: applies each guest's name, character and saved carry-over once, as soon as that guest
		// clears the engine barrier, so guests joining at different times are all handled.
		void ConfigureConnectedGuests()
		{
			if (networkManager_ == nullptr) return;
			for (int slot = 0; slot < nCine::PspAdhoc::MaxGuests; ++slot) {
				if (multiplayerPeerConfigured_[slot]) continue;
				if (!adhoc_.IsPeerConnected(slot) || !adhoc_.IsPeerEngineReady(slot)) continue;
				auto peer = networkManager_->GetPeerDescriptor(Multiplayer::Peer(std::uint32_t(slot) + 1));
				if (peer == nullptr) continue;
				peer->PlayerName = adhoc_.GetPeerName(slot);
				peer->IsAuthenticated = true;
				// Comes from the lobby handshake; without it every remote player replicates the default palette.
				peer->FurColor = adhoc_.GetPeerFurColor(slot);
				const std::uint8_t character = adhoc_.GetPeerCharacter(slot);
				peer->PreferredPlayerType = (character == 0 ? PlayerType::Jazz :
					character == 1 ? PlayerType::Spaz : PlayerType::Lori);
				// Stands in for ClientPacketType::PlayerReady, which a PSP guest never sends (it picked its
				// character in the ad-hoc lobby, so the in-game one is never shown). Write PreferredTeam, never
				// Team: ResolveTeam() overwrites Team at spawn, and the preference keeps balancing intact.
				if (!peer->TeamLocked) {
					peer->PreferredTeam = adhoc_.GetPeerTeam(slot);
				}
				if (const PspNamedPlayerState* stored = FindGuestState(peer->PlayerName)) {
					peer->CarryOver = stored->CarryOver;
					peer->CarryOver.Type = peer->PreferredPlayerType;
					peer->CarryOverHealth = stored->Health;
					peer->HasCarryOver = true;
				}
				// Do NOT force LevelState: IsPeerEngineReady() only means the transport barrier is up, not
				// that the guest finished loading. Claiming LevelLoaded spawned slow guests early and silently
				// killed their real report, since HandleClientPacketLevelReady only advances a state still
				// below LevelLoaded. Left at Unknown, the guest's own LevelReady packet does the job.
				multiplayerPeerConfigured_[slot] = true;
			}
		}
		PspNamedPlayerState* FindGuestState(Death::Containers::StringView name)
		{
			for (auto& state : guestStates_) if (state.Name == name) return &state;
			return nullptr;
		}
		const PspNamedPlayerState* FindGuestState(Death::Containers::StringView name) const
		{
			for (const auto& state : guestStates_) if (state.Name == name) return &state;
			return nullptr;
		}
		void CaptureGuestState(Multiplayer::MpLevelHandler& mp, const Multiplayer::Peer& peer,
			Death::Containers::StringView name)
		{
			if (name.empty()) return;
			PlayerCarryOver carryOver{};
			std::int32_t health = 5;
			if (!mp.CapturePeerState(peer, carryOver, health)) return;
			PspNamedPlayerState* state = FindGuestState(name);
			if (state == nullptr) {
				guestStates_.push_back(PspNamedPlayerState{});
				state = &guestStates_.back();
				state->Name = name;
			}
			carryOver.Type = PlayerType::None;
			state->CarryOver = carryOver;
			state->Health = health;
		}
		void CaptureConnectedGuestState()
		{
			if (!multiplayerActive_ || !multiplayerHost_ || levelHandler_ == nullptr || networkManager_ == nullptr) return;
			// Every connected guest, not just the first: each carry-over has to survive a save or a menu return.
			for (int slot = 0; slot < nCine::PspAdhoc::MaxGuests; ++slot) {
				const Multiplayer::Peer remote(std::uint32_t(slot) + 1);
				auto peer = networkManager_->GetPeerDescriptor(remote);
				if (peer == nullptr) continue;
				CaptureGuestState(*static_cast<Multiplayer::MpLevelHandler*>(levelHandler_.get()),
					remote, peer->PlayerName);
			}
		}
		std::vector<PspNamedPlayerState> BuildSavedPlayerStates() const
		{
			std::vector<PspNamedPlayerState> states;
			if (levelHandler_ == nullptr) return states;
			Actors::Player* localPlayer = nullptr;
			if (multiplayerActive_ && multiplayerHost_ && networkManager_ != nullptr) {
				auto local = networkManager_->GetPeerDescriptor(Multiplayer::LocalPeer);
				if (local != nullptr) localPlayer = local->Player;
			}
			if (localPlayer == nullptr) {
				auto players = levelHandler_->GetPlayers();
				if (!players.empty()) localPlayer = players[0];
			}
			if (localPlayer != nullptr) {
				PspNamedPlayerState local{};
				local.Name = adhoc_.GetLocalName();
				local.CarryOver = localPlayer->PrepareLevelCarryOver();
				local.CarryOver.Type = PlayerType::None;
				local.Health = localPlayer->GetHealth();
				states.push_back(std::move(local));
			}
			for (const auto& guest : guestStates_) {
				if (states.size() >= MaxSavedPlayerStates) break;
				bool duplicate = false;
				for (const auto& state : states) {
					if (state.Name == guest.Name) { duplicate = true; break; }
				}
				if (!duplicate) states.push_back(guest);
			}
			return states;
		}
		bool BeginSavedGameLoadingScreen()
		{
			Death::IO::MemoryStream stream(savedata_.GameData(), savedata_.GameDataSize());
			const std::uint64_t signature = stream.ReadValueAsLE<std::uint64_t>();
			const std::uint8_t fileType = stream.ReadValue<std::uint8_t>();
			const std::uint16_t version = stream.ReadValueAsLE<std::uint16_t>();
			if (signature != 0x2095A59FF0BFBBEF || fileType != ContentResolver::StateFile || version != PspSaveVersion) return false;
			Death::IO::Compression::DeflateStream decompressed(stream);
			decompressed.ReadValue<std::uint8_t>(); // State flags
			const std::uint8_t episodeLength = decompressed.ReadValue<std::uint8_t>();
			Death::Containers::String episode{NoInit, episodeLength};
			if (decompressed.Read(episode.data(), episodeLength) != episodeLength) return false;
			const std::uint8_t levelLength = decompressed.ReadValue<std::uint8_t>();
			Death::Containers::String level{NoInit, levelLength};
			if (decompressed.Read(level.data(), levelLength) != levelLength) return false;

			const Death::Containers::String levelName = episode + '/' + level;
			savedGameLevelName_ = levelName;
			loadingScreenMeta_ = ContentResolver::Get().RequestMetadata("UI/Loading"_s);
			loadingScreen_ = std::make_unique<PspLoadingScreen>(loadingScreenMeta_, GetLoadingTitle(levelName));
			loadingScreen_->setParent(&nCine::theApplication().GetRootNode());
			if (menu_ != nullptr) menu_->setUpdateEnabled(false);
			if (pauseMenu_ != nullptr) pauseMenu_->setUpdateEnabled(false);
			loadingFrames_ = 0;
			savedGameLoadingPending_ = true;
			state_ = AppState::Loading;
			return true;
		}
		bool LoadGameState()
		{
			Death::IO::MemoryStream stream(savedata_.GameData(), savedata_.GameDataSize());
			const std::uint64_t signature = stream.ReadValueAsLE<std::uint64_t>();
			const std::uint8_t fileType = stream.ReadValue<std::uint8_t>();
			const std::uint16_t version = stream.ReadValueAsLE<std::uint16_t>();
			if (signature != 0x2095A59FF0BFBBEF || fileType != ContentResolver::StateFile || version != PspSaveVersion) return false;
			Death::IO::Compression::DeflateStream decompressed(stream);
			if (multiplayerActive_ && multiplayerHost_ && levelHandler_ != nullptr && !savedGameLevelName_.empty()) {
				static_cast<Multiplayer::MpLevelHandler*>(levelHandler_.get())->NotifyPeersOfLevelLoad(savedGameLevelName_);
			}
			// Two fully initialized levels do not fit in PSP memory. The header is validated by now, so tear
			// the current scene down before building the loaded one out of the static savedata buffer.
			nCine::PspGuWaitForPreviousFrame();
			if (loadingScreen_ != nullptr) { loadingScreen_->setParent(nullptr); loadingScreen_ = nullptr; }
			if (pauseMenu_ != nullptr) { pauseMenu_->setParent(nullptr); pauseMenu_ = nullptr; }
			if (menu_ != nullptr) { menu_->setParent(nullptr); menu_ = nullptr; }
			levelHandler_ = nullptr;
			if (waterOverlay_ != nullptr) waterOverlay_->Clear();
			pauseMenuMeta_ = nullptr;
			loadingScreenMeta_ = nullptr;
			pauseMenuSounds_.clear();
			ContentResolver::Get().Release();
			DbgHeap("transition: cleared before loaded level");
			ResetPeerConfiguration();
			multiplayerTransportAttached_ = false;
			networkManager_ = CreateMultiplayerNetworkManager();
			std::unique_ptr<LevelHandler> loaded = std::make_unique<Multiplayer::MpLevelHandler>(&root_, networkManager_.get(),
				Multiplayer::MpLevelHandler::LevelState::InitialUpdatePending, true);
			// Same ordering rule as FinishLoadingLevel(): the team preference must land before the spawn.
			if (multiplayerActive_ && multiplayerHost_) {
				auto localBeforeSpawn = networkManager_->AddLocalPlayer(0);
				if (localBeforeSpawn != nullptr && !localBeforeSpawn->TeamLocked) {
					localBeforeSpawn->PreferredTeam = adhoc_.GetLocalTeam();
				}
			}
			if (!loaded->Initialize(decompressed, version)) return false;
			const std::uint8_t savedPlayerCount = decompressed.ReadValue<std::uint8_t>();
			if (savedPlayerCount > MaxSavedPlayerStates) return false;
			std::vector<PspNamedPlayerState> savedPlayerStates;
			savedPlayerStates.reserve(savedPlayerCount);
			for (std::uint8_t i = 0; i < savedPlayerCount; i++) {
				PspNamedPlayerState state{};
				if (!ReadNamedPlayerState(decompressed, state)) return false;
				savedPlayerStates.push_back(std::move(state));
			}
			guestStates_.clear();
			for (auto& state : savedPlayerStates) {
				if (state.Name != Death::Containers::StringView(adhoc_.GetLocalName())) guestStates_.push_back(std::move(state));
			}
			root_._returnToMenu = false;
			root_._hasPendingLevel = false;
			root_._pendingLevel = nullptr;
			if (multiplayerActive_) {
				auto local = networkManager_->GetPeerDescriptor(Multiplayer::LocalPeer);
				if (local != nullptr) {
					local->PlayerName = adhoc_.GetLocalName();
					local->FurColor = PreferencesCache::PlayerFurColor;
				}
				if (!multiplayerHost_ || adhoc_.IsSessionReady()) {
					if (!adhoc_.BeginEngineTransport()) return false;
					networkManager_->AttachAdhoc(&adhoc_, multiplayerHost_, this,
						0xDEA00000 | (MultiplayerProtocolVersion & 0x000fffff));
					multiplayerTransportAttached_ = true;
				}
			}
			levelHandler_ = std::move(loaded);
			levelHandler_->SuppressInputUntilRelease();
			levelHistory_.clear();
			suppressNextHistoryPush_ = false;
			levelHandler_->SetPspFlyCheat(cheatFlyEnabled_);
			levelHandler_->SetPspGodCheat(cheatGodEnabled_);
			pauseMenuMeta_ = ContentResolver::Get().RequestMetadata("UI/MainMenu"_s);
			levelHandler_->OnInitializeViewport(480, 272);
			currentLevelName_ = Death::Containers::String(levelHandler_->GetLevelName());
			savedGameLevelName_ = {};
			if (multiplayerActive_ && multiplayerHost_) {
				const auto levelDisplayName = GetLoadingTitle(currentLevelName_);
				adhoc_.SetSessionLevel(currentLevelName_.data(), levelDisplayName.data());
			}
			cameraViewCenterInitialized_ = false;
			FollowCamera();
			state_ = AppState::Game;
			startMusicAfterPresent_ = true;
			return true;
		}
		void FinishSavedataInteraction()
		{
			utilitySuspended_ = false;
			nCine::theServiceLocator().GetAudioDevice().resumeDevice();
			PspSavedataManager::Operation operation = PspSavedataManager::Operation::None;
			bool success = false, loaded = false;
			const char* message = "Saved data operation finished";
			const bool hasResult = savedata_.ConsumeResult(operation, success, loaded, message);
			(void)success;
			if (hasResult && operation == PspSavedataManager::Operation::ProfileAutoSave && _afterProfileSaveTransition != nullptr) {
				LevelInitialization continuation(std::move(*_afterProfileSaveTransition));
				_afterProfileSaveTransition = nullptr;
				ContinueCompletedEpisode(std::move(continuation));
				return;
			}
			if (hasResult && loaded && operation == PspSavedataManager::Operation::GameLoad) {
				if (BeginSavedGameLoadingScreen()) return;
				message = "Saved game is invalid or incompatible";
				loaded = false;
			}
			if (loaded && levelHandler_ != nullptr) levelHandler_->ApplyPspMusicVolume();
			if (menu_ != nullptr) {
				menu_->setUpdateEnabled(true);
				menu_->LatchInput();
				if (hasResult && operation != PspSavedataManager::Operation::ProfileAutoSave) menu_->ShowStorageResult(message);
			}
			if (pauseMenu_ != nullptr) {
				pauseMenu_->setUpdateEnabled(true);
				pauseMenu_->LatchInput();
				if (hasResult && operation != PspSavedataManager::Operation::ProfileAutoSave) pauseMenu_->ShowStorageResult(message);
			}
		}
		void ResumePausedGame()
		{
			if (levelHandler_ != nullptr) levelHandler_->ResumeFromPspMenu();
			if (pauseMenu_ != nullptr) { pauseMenu_->setParent(nullptr); pauseMenu_ = nullptr; }
			state_ = AppState::Game;
		}
		void FollowCamera()
		{
			auto players = levelHandler_->GetPlayers();
			if (players.empty()) return;
			const nCine::Vector2f p = players[0]->GetPos();
			if (!cameraViewCenterInitialized_) {
				cameraViewCenterY_ = p.Y;
				cameraViewCenterInitialized_ = true;
			}
			// Mimics PlayerViewport: ignore small vertical corrections over uneven ground, follow once the
			// player leaves the band, then ease back without a final few-pixel crawl. Otherwise running
			// horizontally makes the whole scene tremble.
			constexpr float VerticalDeadzone = 24.0f;
			constexpr float RecenterThreshold = 4.0f;
			const float verticalOffset = p.Y - cameraViewCenterY_;
			if (verticalOffset > VerticalDeadzone) {
				cameraViewCenterY_ = p.Y - VerticalDeadzone;
			} else if (verticalOffset < -VerticalDeadzone) {
				cameraViewCenterY_ = p.Y + VerticalDeadzone;
			} else if (std::abs(verticalOffset) >= RecenterThreshold) {
				cameraViewCenterY_ += verticalOffset * 0.05f;
			}

			float cx = p.X, cy = cameraViewCenterY_;
			const nCine::Vector2i bounds = (levelHandler_->TileMap() != nullptr ? levelHandler_->TileMap()->GetLevelBounds() : nCine::Vector2i(0, 0));
			const float lw = float(bounds.X), lh = float(bounds.Y);
			if (lw > 480.0f) cx = (cx < 240.0f ? 240.0f : (cx > lw - 240.0f ? lw - 240.0f : cx));
			if (lh > 272.0f) cy = (cy < 136.0f ? 136.0f : (cy > lh - 136.0f ? lh - 136.0f : cy));
			if (!Jazz2::PreferencesCache::UnalignedViewport) {
				cx = std::floor(cx);
				cy = std::floor(cy);
			}
			cameraViewCenterX_ = cx;
			cameraViewCenterY_ = cy;
			if (peerOverlay_ != nullptr) peerOverlay_->SetCameraCenter(cx, cy);
			if (waterOverlay_ != nullptr) {
				waterOverlay_->SetWater(levelHandler_->GetWaterLevel(), cx, cy, levelHandler_->GetElapsedFrames());
			}
			cam_.SetView(cx - 240.0f, cy - 136.0f, 0.0f, 1.0f);
			nCine::theServiceLocator().GetAudioDevice().updateListener(
				nCine::Vector3f(cx, cy, 0.0f), nCine::Vector3f::Zero);
		}

		nCine::Camera cam_;
		PspSavedataManager savedata_;
		PspRootController root_;
		nCine::PspAdhoc adhoc_;
		std::unique_ptr<LevelHandler> levelHandler_;
		std::unique_ptr<Multiplayer::NetworkManager> networkManager_;
		std::unique_ptr<PspMenu> menu_;
		std::unique_ptr<PspAssetPreparationScreen> preparationScreen_;
		std::unique_ptr<PspPauseMenu> pauseMenu_;
		std::vector<std::shared_ptr<nCine::AudioBufferPlayer>> pauseMenuSounds_;
		std::unique_ptr<PspLevelCompleteOverlay> completeOverlay_;
		std::unique_ptr<PspLoadingScreen> loadingScreen_;
		std::unique_ptr<PspMultiplayerSyncOverlay> multiplayerSyncOverlay_;
		Metadata* pauseMenuMeta_ = nullptr;
		Metadata* loadingScreenMeta_ = nullptr;
		std::unique_ptr<PspDiagnosticsOverlay> diagnostics_;
		std::unique_ptr<PspSystemUtilityDimOverlay> utilityDim_;
		std::unique_ptr<PspPeerOverlay> peerOverlay_;
		std::unique_ptr<PspWaterOverlay> waterOverlay_;
		AppState state_ = AppState::Menu;
		PspBakeStatus bakeStatus_{};
		SceUID bakeThread_ = -1;
		bool bakeRecreate_ = false;
		bool bakeAudioSuspended_ = false;
		bool profileBootLoadStarted_ = false;
		char bakeSourcePath_[512] = "Source/";
		char bakeCachePath_[512] = "Cache/";
		char bakeContentPath_[512] = "Content/";
		int frameLog_ = 0;
		std::uint32_t pauseButtonsLast_ = 0;
		float cameraViewCenterX_ = 240.0f;
		float cameraViewCenterY_ = 0.0f;
		bool cameraViewCenterInitialized_ = false;
		bool utilitySuspended_ = false;
		enum class HotJoinInfra { Idle, Netconf };
		HotJoinInfra hotJoinInfra_ = HotJoinInfra::Idle;
		pspUtilityNetconfData hotJoinNetconf_{};
		bool hotJoinNetconfShutdown_ = false;
		static PspEventHandler* s_hotJoinOwner;
		bool startMusicAfterPresent_ = false;
		bool savedGameLoadingPending_ = false;
		int loadingFrames_ = 0;
		Death::Containers::String currentLevelName_;
		Death::Containers::String savedGameLevelName_;
		std::vector<Death::Containers::String> levelHistory_;
		bool suppressNextHistoryPush_ = false;
		bool cheatFlyEnabled_ = false;
		bool cheatGodEnabled_ = false;
		bool multiplayerActive_ = false;
		bool multiplayerHost_ = false;
		// One entry per ad-hoc guest slot; see ConfigureConnectedGuests().
		bool multiplayerPeerConfigured_[nCine::PspAdhoc::MaxGuests]{};
		bool multiplayerTransportAttached_ = false;
		std::vector<PspNamedPlayerState> guestStates_;
		char pendingDisconnectMessage_[64]{};
		std::uint32_t mpReliablePackets_ = 0;
		std::uint32_t mpUpdatePackets_ = 0;
		std::uint32_t mpCreateActorPackets_ = 0;
		std::uint32_t mpCreatePlayerPackets_ = 0;
		std::unique_ptr<LevelInitialization> _queuedLevelInit;
		std::unique_ptr<LevelInitialization> _afterProfileSaveTransition;
		Death::Containers::String _pendingTerminalEpisode;
		std::uint32_t _pendingTerminalScore = 0;
	};
	PspEventHandler* PspEventHandler::s_hotJoinOwner = nullptr;
}

std::unique_ptr<nCine::IAppEventHandler> CreateAppEventHandler()
{
	return std::unique_ptr<nCine::IAppEventHandler>(new PspEventHandler());
}

int main()
{
	EnsureEmbeddedPspContent();

	nCine::PspBootEngine(&CreateAppEventHandler);
	// Register only after Boot: a callback cannot otherwise wait for a main-thread suspend boundary that does
	// not exist yet while initialization is still doing synchronous file and GU work.
	SetupCallbacks();
	while (g_running.load(std::memory_order_acquire) && !nCine::PspEngineWantsQuit()) {
		if (ServicePowerState()) nCine::PspStepEngine();
	}
	nCine::PspShutdownEngine();

	sceKernelExitGame();
	return 0;
}
