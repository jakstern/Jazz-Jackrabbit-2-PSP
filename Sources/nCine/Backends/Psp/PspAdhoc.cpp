#include "PspAdhoc.h"
#include "PspInternetRelayTransport.h"

#include <pspnet.h>
#include <pspnet_adhoc.h>
#include <pspnet_adhocctl.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <psputility_netmodules.h>
#include <pspwlan.h>
#include <psprtc.h>

#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <cstring>

namespace nCine
{
	namespace
	{
		constexpr char ProductId[] = "JAZZ2PSP1";
		constexpr char GroupName[] = "JAZZ2MP";
		constexpr unsigned short DiscoveryPort = 31000;
		constexpr unsigned short HostPort = 31001;
		constexpr unsigned short ClientPort = 31002;
		constexpr unsigned short GameplayPort = 31003;
		constexpr std::uint8_t DiscoveryVersion = 4;
		constexpr std::uint8_t DiscoveryRequest = 1;
		constexpr std::uint8_t DiscoveryResponse = 2;
		// v2 added FurColor, v3 added Team. Bumping is mandatory for ROSTER changes too: the roster frame
		// carries no version and no length, so a reader accumulates exactly RosterWireSize bytes and a size
		// change against an old peer silently mis-frames every subsequent control byte instead of failing.
		// This version is the only guard the roster has; its size and this number must move together.
		constexpr std::uint8_t HandshakeVersion = 3;
		constexpr std::uint8_t HandshakeHello = 1;
		constexpr std::uint8_t HandshakeAck = 2;
		constexpr std::uint8_t HeartbeatByte = 0x7e;
		constexpr std::uint8_t StartGameRequest = 0x01;
		constexpr std::uint8_t StartGameAck = 0x02;
		constexpr std::uint8_t EngineTransportReady = 0x03;
		constexpr std::uint8_t PlayerReady = 0x04;
		constexpr std::uint8_t PlayerNotReady = 0x05;
		// Followed by a fixed-size roster payload (PspAdhoc::RosterWireSize bytes).
		constexpr std::uint8_t RosterUpdate = 0x06;
		constexpr std::uint8_t EngineDisconnectPacketType = 0xfe;
		constexpr std::uint32_t DiscoveryInterval = 60;
		constexpr std::uint32_t HostExpiry = 300;
		constexpr std::uint32_t HeartbeatInterval = 60;
		constexpr std::uint32_t PeerTimeout = 300;
		// In-game silence tolerated before a peer is declared dead, in REAL MILLISECONDS - not frames.
		// MEASURED: a real PSP-1000 takes 6-7s to synchronize into a level (metadata loading is Memory Stick
		// I/O) and says nothing for that whole window; a heavier level takes longer. This used to count frames
		// (ReadyTick) and kept misfiring, because the two ends run at wildly different frame rates during
		// synchronization - real time cannot be skewed by frame rate, a fast-forwarding emulator or a debug build.
		constexpr std::uint32_t EnginePeerTimeoutMs = 10000;

		// Monotonic-enough wall clock. sceRtcGetTickResolution() is microseconds on PSP; only deltas are used,
		// so the u32 wrap (~49 days) is irrelevant.
		std::uint32_t NowMs()
		{
			u64 tick = 0;
			sceRtcGetCurrentTick(&tick);
			const u32 resolution = sceRtcGetTickResolution();
			return (std::uint32_t)(resolution >= 1000 ? tick / (resolution / 1000) : tick);
		}
		constexpr int AdhocWouldBlock = static_cast<int>(0x80410709u);
		constexpr int NetworkBufferSize = PspAdhoc::TransportBufferSize;
		constexpr int NetworkPoolSize = 512 * 1024;
		constexpr int PtpSocketBufferSize = NetworkBufferSize;
		constexpr int NetworkDatagramSize = 16 * 1024;
		constexpr int NetworkHeaderSize = 4;
		constexpr int NetworkDatagramHeaderSize = 8;

#pragma pack(push, 1)
		struct DiscoveryPacket {
			char Magic[4];
			std::uint8_t Version;
			std::uint8_t Type;
			std::uint8_t PlayerCount;
			std::uint8_t MaxPlayerCount;
			std::uint8_t GameMode;
			std::uint8_t Difficulty;
			std::uint8_t Flags;
			std::uint8_t Reserved;
			std::uint32_t TotalKills;
			std::uint32_t TotalLaps;
			std::uint32_t TotalTreasureCollected;
			std::uint32_t MaxGameTimeSecs;
			std::uint32_t OvertimeSecs;
			char HostName[32];
			char LevelName[48];
			char LevelDisplayName[32];
		};
		struct HandshakePacket {
			char Magic[4];
			std::uint8_t Version;
			std::uint8_t Type;
			std::uint8_t Character;
			std::uint8_t Reserved;	// Flags, despite the name: bit 0x01 = session already running
			char PlayerName[32];
			std::uint32_t FurColor;
			std::uint8_t Team;		// Preference only; PspAdhoc::NoAdhocTeam = let the server decide
		};
		struct RosterWireEntry {
			char Name[32];
			std::uint8_t Character;
			std::uint8_t Ready;
			std::uint8_t IsHost;
			std::uint32_t FurColor;
			std::uint8_t Team;
		};
		struct RosterWirePacket {
			std::uint8_t Count;
			RosterWireEntry Entries[PspAdhoc::MaxRosterEntries];
		};
		struct GameplayPacket {
			char Magic[4];
			std::uint8_t Version;
			std::uint8_t Type;
			std::uint16_t Reserved;
			std::uint32_t Sequence;
			std::int32_t X;
			std::int32_t Y;
		};
#pragma pack(pop)

		static_assert(sizeof(DiscoveryPacket) == 144, "Discovery packet layout changed");
		static_assert(sizeof(HandshakePacket) == PspAdhoc::HandshakeWireSize, "Handshake packet layout changed");
		static_assert(sizeof(GameplayPacket) == 20, "Gameplay packet layout changed");
		static_assert(sizeof(RosterWireEntry) == PspAdhoc::RosterWireEntrySize, "Roster entry layout changed");
		static_assert(sizeof(RosterWirePacket) == PspAdhoc::RosterWireSize, "Roster packet layout changed");

		const char* CharacterName(std::uint8_t character)
		{
			switch (character) {
				case 0: return "Jazz";
				case 1: return "Spaz";
				case 2: return "Lori";
				default: return "Unknown";
			}
		}

#if defined(JAZZ2_PSP_DEBUG)
		void SessionLog(const char* format, ...)
		{
			FILE* file = std::fopen("aemu.log", "a");
			if (file == nullptr) return;
			std::fputs("session: ", file);
			va_list args;
			va_start(args, format);
			std::vfprintf(file, format, args);
			va_end(args);
			std::fputc('\n', file);
			std::fclose(file);
		}
#else
		// Compiled out unless -DPSP_DEBUG=ON: each call opens and closes aemu.log on the Memory Stick, and
		// several of these sit on the lobby/handshake path.
		inline void SessionLog(const char*, ...) {}
#endif
	}

	PspAdhoc::PspAdhoc() = default;

	PspAdhoc::~PspAdhoc()
	{
		Stop();
	}

	bool PspAdhoc::IsWlanSwitchOn()
	{
		return sceWlanGetSwitchState() != 0;
	}

	bool PspAdhoc::Start()
	{
		if (state_ == State::Connecting || state_ == State::Connected) return true;
		Stop();
		infrastructureMode_ = false;
		if (!IsWlanSwitchOn()) {
			state_ = State::Failed;
			std::strcpy(statusText_, "WLAN switch is off");
			return false;
		}

		int result = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
		if (result < 0) return Fail("common module", result);
		commonModuleLoaded_ = true;

		result = sceUtilityLoadNetModule(PSP_NET_MODULE_ADHOC);
		if (result < 0) return Fail("ad-hoc module", result);
		adhocModuleLoaded_ = true;

		result = sceNetInit(NetworkPoolSize, 0x20, 4 * 1024, 0x20, 4 * 1024);
		if (result < 0) return Fail("network init", result);
		netInitialized_ = true;

		result = sceNetAdhocInit();
		if (result < 0) return Fail("ad-hoc init", result);
		adhocInitialized_ = true;

		productStruct product{};
		std::memcpy(product.product, ProductId, sizeof(product.product));
		result = sceNetAdhocctlInit(8 * 1024, 0x30, &product);
		if (result < 0) return Fail("ad-hoc control", result);
		adhocctlInitialized_ = true;

		result = sceNetAdhocctlConnect(GroupName);
		if (result < 0) return Fail("group connect", result);

		state_ = State::Connecting;
		std::strcpy(statusText_, "Connecting to ad-hoc...");
		return true;
	}

	bool PspAdhoc::PrepareInfrastructure()
	{
		if (state_ == State::Connecting || state_ == State::Connected) return true;
		Stop();
		infrastructureMode_ = true;
		if (!IsWlanSwitchOn()) {
			state_ = State::Failed;
			std::strcpy(statusText_, "WLAN switch is off");
			return false;
		}

		int result = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
		if (result < 0) return Fail("common module", result);
		commonModuleLoaded_ = true;

		result = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
		if (result < 0) return Fail("internet module", result);
		inetModuleLoaded_ = true;

		result = sceNetInit(NetworkPoolSize, 0x20, 4 * 1024, 0x20, 4 * 1024);
		if (result < 0) return Fail("network init", result);
		netInitialized_ = true;

		result = sceNetInetInit();
		if (result < 0) return Fail("internet init", result);
		inetInitialized_ = true;

		result = sceNetApctlInit(0x8000, 0x30);
		if (result < 0) return Fail("access point init", result);
		apctlInitialized_ = true;
		state_ = State::Connecting;
		std::strcpy(statusText_, "Select a network profile");
		return true;
	}

	bool PspAdhoc::StartInfrastructure(const char* serverAddress)
	{
		if (!infrastructureMode_ || !netInitialized_ || !inetInitialized_ || !apctlInitialized_) {
			return Fail("internet was not prepared", -1);
		}
		int connectionState = 0;
		const int result = sceNetApctlGetState(&connectionState);
		if (result < 0 || connectionState != PSP_NET_APCTL_STATE_GOT_IP) {
			return Fail("network profile connection", result < 0 ? result : connectionState);
		}
		if (!internetRelay_) internetRelay_ = std::make_unique<PspInternetRelayTransport>();
		if (!internetRelay_->Start(serverAddress)) {
			state_ = State::Failed;
			std::snprintf(statusText_, sizeof(statusText_), "%s", internetRelay_->GetStatusText());
			return false;
		}
		state_ = State::Connecting;
		std::snprintf(statusText_, sizeof(statusText_), "%s", internetRelay_->GetStatusText());
		return true;
	}

	void PspAdhoc::CancelInfrastructure(const char* reason)
	{
		state_ = State::Failed;
		std::snprintf(statusText_, sizeof(statusText_), "%s", reason != nullptr ? reason : "Network connection cancelled");
		Stop();
	}

	void PspAdhoc::Update()
	{
		if (infrastructureMode_) {
			if (internetRelay_) {
				internetRelay_->Update();
				if (internetRelay_->GetState() == PspInternetRelayTransport::State::Connected) state_ = State::Connected;
				else if (internetRelay_->GetState() == PspInternetRelayTransport::State::Failed) state_ = State::Failed;
				std::snprintf(statusText_, sizeof(statusText_), "%s", internetRelay_->GetStatusText());
			}
			if (state_ != State::Connected) return;
		} else {
			// Do not tear down an active engine transport here; its disconnect path owns the peer
			// notification and the game-state transition. Menus and lobbies may react immediately.
			if ((state_ == State::Connecting || state_ == State::Connected) && !IsEngineTransportReady() && !IsWlanSwitchOn()) {
				state_ = State::Failed;
				std::strcpy(statusText_, "WLAN switch is off");
				Stop();
				return;
			}
			if (state_ == State::Connecting) {
				int connectionState = 0;
				const int result = sceNetAdhocctlGetState(&connectionState);
				if (result < 0) {
					Fail("connection state", result);
					return;
				}
				if (connectionState != 1) return;
				state_ = State::Connected;
				std::strcpy(statusText_, "Ad-hoc connected");
			}
		}

		if (state_ != State::Connected || discoveryMode_ == DiscoveryMode::Idle) return;
		if (!EnsureDiscoverySocket()) return;

		++discoveryTick_;
		if (discoveryMode_ == DiscoveryMode::Client && peers_[0].State == PeerState::Disconnected &&
			discoveryTick_ % DiscoveryInterval == 1) {
			SendDiscoveryRequest();
		}
		// Host beacon: broadcast periodically instead of only replying to requests, so a browsing peer
		// still finds us if its request was lost or it started browsing late (both common on PSP Wi-Fi).
		if (discoveryMode_ == DiscoveryMode::Host && GetConnectedPeerCount() < MaxGuests &&
			discoveryTick_ % DiscoveryInterval == 1) {
			std::uint8_t broadcastMac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
			SendDiscoveryResponse(broadcastMac);
		}
		ReceiveDiscoveryPackets();
		if (discoveryMode_ == DiscoveryMode::Client && peers_[0].State == PeerState::Disconnected) ExpireHosts();
		UpdatePeerConnection();
		// Lobby position preview only. Past the engine barrier the shared gameplay socket carries the
		// engine's J2NP datagrams and ReceiveNetworkPacketFrom owns it - draining here would eat them.
		if (!IsEngineTransportReady()) UpdateGameplayState();
	}

	void PspAdhoc::BeginDiscovery()
	{
		SessionLog("begin discovery peerState=%d handshake=%d", static_cast<int>(peers_[0].State),
			static_cast<int>(peers_[0].Handshake));
		ClosePeerSockets();
		CloseDiscoverySocket();
		discoveryMode_ = DiscoveryMode::Client;
		sessionRunning_ = false;
		discoveryTick_ = 0;
		hostCount_ = 0;
		rosterCount_ = 0;
	}

	void PspAdhoc::BeginHosting(const char* playerName, std::uint8_t character, const SessionSettings& settings)
	{
		SessionLog("begin hosting name=%s character=%u", playerName != nullptr ? playerName : "Player", character);
		ClosePeerSockets();
		CloseDiscoverySocket();
		discoveryMode_ = DiscoveryMode::Host;
		discoveryTick_ = 0;
		std::snprintf(hostName_, sizeof(hostName_), "%s", playerName != nullptr ? playerName : "Player");
		std::snprintf(localName_, sizeof(localName_), "%s", playerName != nullptr ? playerName : "Player");
		localCharacter_ = character;
		sessionSettings_ = settings;
		sessionRunning_ = false;
		// Show ourselves in the lobby list right away, before any guest arrives.
		MarkRosterDirty();
	}

	void PspAdhoc::EndDiscovery()
	{
		SessionLog("end discovery mode=%d peerState=%d handshake=%d", static_cast<int>(discoveryMode_),
			static_cast<int>(peers_[0].State), static_cast<int>(peers_[0].Handshake));
		ClosePeerSockets();
		CloseDiscoverySocket();
		discoveryMode_ = DiscoveryMode::Idle;
		sessionRunning_ = false;
		hostCount_ = 0;
		rosterCount_ = 0;
	}

	void PspAdhoc::SetSessionLevel(const char* levelName, const char* levelDisplayName)
	{
		if (discoveryMode_ != DiscoveryMode::Host) return;
		std::snprintf(sessionSettings_.LevelName, sizeof(sessionSettings_.LevelName), "%s", levelName != nullptr ? levelName : "");
		std::snprintf(sessionSettings_.LevelDisplayName, sizeof(sessionSettings_.LevelDisplayName), "%s",
			levelDisplayName != nullptr ? levelDisplayName : "");
	}

	bool PspAdhoc::ConnectToHost(int index, const char* playerName, std::uint8_t character)
	{
		Peer& peer = peers_[0];
		if (state_ != State::Connected || discoveryMode_ != DiscoveryMode::Client ||
			index < 0 || index >= hostCount_ || peer.State != PeerState::Disconnected) return false;

		std::uint8_t localMac[6]{};
		int result = GetLocalMac(localMac);
		if (result < 0) {
			peer.State = PeerState::Failed;
			std::snprintf(peer.StatusText, sizeof(peer.StatusText), "local MAC failed: %08X", static_cast<unsigned int>(result));
			return false;
		}

		const auto& host = hosts_[index].Info;
		sessionSettings_ = host.Settings;
		sessionRunning_ = host.IsRunning;
		std::snprintf(localName_, sizeof(localName_), "%s", playerName != nullptr ? playerName : "Player");
		localCharacter_ = character;
		std::memcpy(peer.Mac, host.Mac, sizeof(peer.Mac));
		std::snprintf(peer.Name, sizeof(peer.Name), "%s", host.Name);
		peer.Socket = OpenPtp(localMac, ClientPort, peer.Mac, HostPort, PtpSocketBufferSize);
		if (peer.Socket < 0) {
			peer.State = PeerState::Failed;
			std::snprintf(peer.StatusText, sizeof(peer.StatusText), "PTP open failed: %08X", static_cast<unsigned int>(peer.Socket));
			return false;
		}

		peer.State = PeerState::Connecting;
		peer.ConnectTick = 0;
		SessionLog("connect requested host=%s character=%u running=%d", peer.Name, character, sessionRunning_ ? 1 : 0);
		std::snprintf(peer.StatusText, sizeof(peer.StatusText), "Connecting to %s...", peer.Name);
		return true;
	}

	int PspAdhoc::GetConnectedPeerCount() const
	{
		int count = 0;
		for (const auto& peer : peers_) {
			if (peer.State == PeerState::Connected && peer.Handshake == HandshakeStep::Ready) ++count;
		}
		return count;
	}

	bool PspAdhoc::IsPeerConnected(int peerIndex) const
	{
		if (peerIndex < 0 || peerIndex >= MaxPeers) return false;
		return peers_[peerIndex].State == PeerState::Connected;
	}

	bool PspAdhoc::IsPeerSessionReady(int peerIndex) const
	{
		if (peerIndex < 0 || peerIndex >= MaxPeers) return false;
		return peers_[peerIndex].Handshake == HandshakeStep::Ready;
	}

	bool PspAdhoc::IsPeerEngineReady(int peerIndex) const
	{
		if (peerIndex < 0 || peerIndex >= MaxPeers) return false;
		return peers_[peerIndex].EngineTransport;
	}

	bool PspAdhoc::IsEngineTransportReady() const
	{
		for (const auto& peer : peers_) {
			if (peer.EngineTransport) return true;
		}
		return false;
	}

	PspAdhoc::PeerState PspAdhoc::GetPeerState(int peerIndex) const
	{
		if (peerIndex < 0 || peerIndex >= MaxPeers) return PeerState::Disconnected;
		return peers_[peerIndex].State;
	}

	const char* PspAdhoc::GetPeerStatusText(int peerIndex) const
	{
		if (peerIndex < 0 || peerIndex >= MaxPeers) return "No peer connected";
		return peers_[peerIndex].StatusText;
	}

	const char* PspAdhoc::GetPeerName(int peerIndex) const
	{
		if (peerIndex < 0 || peerIndex >= MaxPeers) return "Host";
		return peers_[peerIndex].Name;
	}

	std::uint8_t PspAdhoc::GetPeerCharacter(int peerIndex) const
	{
		if (peerIndex < 0 || peerIndex >= MaxPeers) return 0;
		return peers_[peerIndex].Character;
	}

	bool PspAdhoc::IsPeerReady(int peerIndex) const
	{
		if (peerIndex < 0 || peerIndex >= MaxPeers) return false;
		return peers_[peerIndex].PeerReady;
	}

	bool PspAdhoc::ConsumeGracefulDisconnect(int peerIndex)
	{
		if (peerIndex < 0 || peerIndex >= MaxPeers) return false;
		const bool result = peers_[peerIndex].GracefulDisconnectReceived;
		peers_[peerIndex].GracefulDisconnectReceived = false;
		return result;
	}

	bool PspAdhoc::HasRemotePlayerPosition(int peerIndex) const
	{
		if (peerIndex < 0 || peerIndex >= MaxPeers) return false;
		return peers_[peerIndex].RemotePositionValid;
	}

	float PspAdhoc::GetRemotePlayerX(int peerIndex) const
	{
		if (peerIndex < 0 || peerIndex >= MaxPeers) return 0.0f;
		return peers_[peerIndex].RemoteX;
	}

	float PspAdhoc::GetRemotePlayerY(int peerIndex) const
	{
		if (peerIndex < 0 || peerIndex >= MaxPeers) return 0.0f;
		return peers_[peerIndex].RemoteY;
	}

	const PspAdhoc::TransportDiagnostics& PspAdhoc::GetTransportDiagnostics(int peerIndex) const
	{
		const int index = (peerIndex < 0 || peerIndex >= MaxPeers) ? 0 : peerIndex;
		return peers_[index].Diagnostics;
	}

	bool PspAdhoc::RequestStartGame()
	{
		if (discoveryMode_ != DiscoveryMode::Host || startRequested_ || !CanStartGame()) return false;
		startRequested_ = true;
		bool invited = false;
		for (auto& peer : peers_) {
			if (peer.State != PeerState::Connected || peer.Handshake != HandshakeStep::Ready) continue;
			peer.PendingControlByte = StartGameRequest;
			peer.StartAcknowledged = false;
			std::strcpy(peer.StatusText, "Sending Start Game...");
			invited = true;
		}
		if (!invited) {
			// An empty lobby starts immediately; late guests are invited on handshake.
			sessionRunning_ = true;
			std::strcpy(peers_[0].StatusText, "Waiting for player...");
		}
		return true;
	}

	bool PspAdhoc::HasStartAcknowledged() const
	{
		if (discoveryMode_ == DiscoveryMode::Host) {
			if (!startRequested_) return false;
			for (const auto& peer : peers_) {
				if (peer.State == PeerState::Connected && peer.Handshake == HandshakeStep::Ready &&
					!peer.StartAcknowledged) return false;
			}
			return true;
		}
		return peers_[0].StartAcknowledged;
	}

	void PspAdhoc::SetLocalReady(bool ready)
	{
		if (localReady_ == ready) return;
		localReady_ = ready;
		for (auto& peer : peers_) peer.LocalReadyDirty = true;
		MarkRosterDirty();
	}

	bool PspAdhoc::CanStartGame() const
	{
		if (discoveryMode_ != DiscoveryMode::Host || sessionRunning_) return false;
		// Every connected guest must have handshaked and explicitly readied. An empty lobby is valid HERE:
		// this also gates hot-join "allow joining", which advertises a running game with no guests yet. The
		// "need an opponent to START a fresh match" rule is HasReadyGuest, used only by the Start button.
		for (const auto& peer : peers_) {
			if (peer.State != PeerState::Connected) continue;
			if (peer.Handshake != HandshakeStep::Ready || !peer.PeerReady) return false;
		}
		return true;
	}

	bool PspAdhoc::HasReadyGuest() const
	{
		if (discoveryMode_ != DiscoveryMode::Host) return false;
		for (const auto& peer : peers_) {
			if (peer.State == PeerState::Connected && peer.Handshake == HandshakeStep::Ready && peer.PeerReady) {
				return true;
			}
		}
		return false;
	}

	bool PspAdhoc::BeginEngineTransport()
	{
		if (!networkDatagram_) networkDatagram_ = std::make_unique<std::uint8_t[]>(NetworkDatagramSize);
		if (!networkDatagram_ || !EnsureGameplaySocket()) return false;

		// `started` gates the caller's one-shot AttachAdhoc; only `newlyRequested` may reset the local
		// position. This is polled every frame to re-arm reconnecting peers, so invalidating on `started`
		// would clear the host's position every frame and stop it broadcasting.
		bool started = false;
		bool newlyRequested = false;
		for (auto& peer : peers_) {
			if (peer.State != PeerState::Connected || peer.Handshake != HandshakeStep::Ready) continue;
			if (!peer.StartAcknowledged || peer.Socket < 0) continue;
			if (peer.EngineTransportRequested) {
				started = true;
				continue;
			}
			if (!peer.NetworkSend) peer.NetworkSend = std::make_unique<std::uint8_t[]>(NetworkBufferSize);
			if (!peer.NetworkReceive) peer.NetworkReceive = std::make_unique<std::uint8_t[]>(NetworkBufferSize);
			if (!peer.NetworkSend || !peer.NetworkReceive) continue;
			peer.NetworkSendOffset = 0;
			peer.NetworkSendSize = 0;
			peer.NetworkReceiveSize = 0;
			peer.Diagnostics = {};
			peer.GracefulDisconnectReceived = false;
			peer.EngineTransportRequested = true;
			peer.LocalEngineReadySent = false;
			peer.PendingControlByte = EngineTransportReady;
			SessionLog("engine transport requested role=%s peer=%s", discoveryMode_ == DiscoveryMode::Host ? "host" : "client",
				peer.Name);
			std::strcpy(peer.StatusText, "Waiting for peer level...");
			started = true;
			newlyRequested = true;
		}
		if (newlyRequested) localPlayerPositionValid_ = false;
		return started;
	}

	void PspAdhoc::DisconnectPeer()
	{
		for (auto& peer : peers_) {
			if (peer.Socket < 0 || peer.State != PeerState::Connected || !peer.EngineTransport) continue;
			const std::uint8_t reason = 0;
			if (SendNetworkPacketTo(static_cast<int>(&peer - peers_), 0, EngineDisconnectPacketType, &reason, sizeof(reason))) {
				FlushNetworkSend(peer);
				if (peer.Socket >= 0) FlushPtp(peer.Socket);
			}
		}
	}

	bool PspAdhoc::SendNetworkPacketTo(int peerIndex, std::uint8_t channel, std::uint8_t packetType,
		const std::uint8_t* data, int length)
	{
		if (peerIndex < 0 || peerIndex >= MaxPeers) return false;
		Peer& peer = peers_[peerIndex];
		if (!peer.EngineTransport || length < 0 || (data == nullptr && length != 0)) return false;
		if (channel == 0) {
			const int frameSize = NetworkHeaderSize + length;
			if (frameSize > NetworkBufferSize || length > 0xffff) return false;
			FlushNetworkSend(peer);
			if (peer.NetworkSendOffset > 0) {
				const int pending = peer.NetworkSendSize - peer.NetworkSendOffset;
				if (pending > 0) std::memmove(peer.NetworkSend.get(), peer.NetworkSend.get() + peer.NetworkSendOffset, pending);
				peer.NetworkSendOffset = 0;
				peer.NetworkSendSize = pending;
			}
			if (peer.NetworkSendSize + frameSize > NetworkBufferSize) return false;
			auto* frame = peer.NetworkSend.get() + peer.NetworkSendSize;
			frame[0] = std::uint8_t(length & 0xff);
			frame[1] = std::uint8_t((length >> 8) & 0xff);
			frame[2] = channel;
			frame[3] = packetType;
			if (length > 0) std::memcpy(frame + NetworkHeaderSize, data, length);
			peer.NetworkSendSize += frameSize;
			FlushNetworkSend(peer);
			return true;
		}

		if (channel != 1 || length + NetworkDatagramHeaderSize > NetworkDatagramSize || !EnsureGameplaySocket()) return false;
		auto* packet = networkDatagram_.get();
		std::memcpy(packet, "J2NP", 4);
		packet[4] = 1;
		packet[5] = channel;
		packet[6] = packetType;
		packet[7] = 0;
		if (length > 0) std::memcpy(packet + NetworkDatagramHeaderSize, data, length);
		const int result = SendPdp(gameplaySocket_, peer.Mac, GameplayPort, packet,
			length + NetworkDatagramHeaderSize);
		return result == 0 || result == AdhocWouldBlock;
	}

	int PspAdhoc::ReceiveNetworkPacketFrom(int& peerIndex, std::uint8_t& channel, std::uint8_t& packetType,
		std::uint8_t* data, int capacity)
	{
		if (data == nullptr || capacity < 0) return -1;

		// Reliable: service every peer's stream, returning the first complete frame.
		for (int i = 0; i < MaxPeers; ++i) {
			Peer& peer = peers_[i];
			if (!peer.EngineTransport) continue;

			while (peer.NetworkReceiveSize < NetworkBufferSize) {
				const int requested = NetworkBufferSize - peer.NetworkReceiveSize;
				int length = requested;
				const int result = ReceivePtp(peer.Socket, peer.NetworkReceive.get() + peer.NetworkReceiveSize, &length);
				RecordTransportOperation(peer, TransportOperation::Receive, result, requested,
					result == 0 ? length : 0, peer.NetworkReceiveSize);
				if (result == AdhocWouldBlock) break;
				if (result < 0 || length <= 0) {
					peerIndex = i;
					HandlePeerDisconnected(peer);
					return -1;
				}
				peer.NetworkReceiveSize += length;
				peer.LastActivityTick = peer.ReadyTick;
				peer.LastActivityMs = NowMs();
				peer.EngineTrafficSeen = true;
			}

			if (peer.NetworkReceiveSize >= NetworkHeaderSize) {
				const int payloadLength = peer.NetworkReceive[0] | (int(peer.NetworkReceive[1]) << 8);
				const int frameSize = NetworkHeaderSize + payloadLength;
				if (frameSize > NetworkBufferSize) {
					peerIndex = i;
					HandlePeerDisconnected(peer);
					return -1;
				}
				if (peer.NetworkReceiveSize >= frameSize) {
					if (payloadLength > capacity) return -1;
					peerIndex = i;
					channel = peer.NetworkReceive[2];
					packetType = peer.NetworkReceive[3];
					if (payloadLength > 0) std::memcpy(data, peer.NetworkReceive.get() + NetworkHeaderSize, payloadLength);
					const int remaining = peer.NetworkReceiveSize - frameSize;
					if (remaining > 0) std::memmove(peer.NetworkReceive.get(), peer.NetworkReceive.get() + frameSize, remaining);
					peer.NetworkReceiveSize = remaining;
					if (channel == 0 && packetType == EngineDisconnectPacketType) {
						peer.GracefulDisconnectReceived = true;
						HandlePeerDisconnected(peer);
						return -1;
					}
					return payloadLength;
				}
			}
		}

		// Unreliable: one shared PDP socket for every guest; the sender's MAC selects the slot.
		if (EnsureGameplaySocket()) {
			std::uint8_t sourceMac[6]{};
			unsigned short sourcePort = 0;
			unsigned int length = NetworkDatagramSize;
			const int result = ReceivePdp(gameplaySocket_, sourceMac, &sourcePort, networkDatagram_.get(), &length);
			if (result == 0 && length >= NetworkDatagramHeaderSize && sourcePort == GameplayPort &&
				std::memcmp(networkDatagram_.get(), "J2NP", 4) == 0 && networkDatagram_[4] == 1) {
				Peer* peer = FindPeerByMac(sourceMac);
				if (peer != nullptr && peer->EngineTransport) {
					// In-game liveness rides on THIS path, not the reliable one: once a round is running the
					// steady traffic is all unreliable, and the reliable stream can legitimately stay quiet
					// for minutes - stamping only there would drop a peer that simply was not shooting.
					peer->LastActivityTick = peer->ReadyTick;
					peer->LastActivityMs = NowMs();
					peer->EngineTrafficSeen = true;
					const int payloadLength = int(length) - NetworkDatagramHeaderSize;
					if (payloadLength > capacity) return -1;
					peerIndex = static_cast<int>(peer - peers_);
					channel = networkDatagram_[5];
					packetType = networkDatagram_[6];
					if (payloadLength > 0) std::memcpy(data, networkDatagram_.get() + NetworkDatagramHeaderSize, payloadLength);
					return payloadLength;
				}
			}
		}
		return NoPacketAvailable;
	}

	void PspAdhoc::FlushNetworkSend(Peer& peer)
	{
		while (peer.EngineTransport && peer.NetworkSendOffset < peer.NetworkSendSize) {
			const int queued = peer.NetworkSendSize - peer.NetworkSendOffset;
			int length = queued;
			const int result = SendPtp(peer.Socket, peer.NetworkSend.get() + peer.NetworkSendOffset, &length);
			RecordTransportOperation(peer, TransportOperation::Send, result, queued, result == 0 ? length : 0, queued);
			if (result == AdhocWouldBlock) return;
			if (result < 0 || length <= 0) {
				HandlePeerDisconnected(peer);
				return;
			}
			peer.NetworkSendOffset += length;
		}
		if (peer.NetworkSendOffset == peer.NetworkSendSize) peer.NetworkSendOffset = peer.NetworkSendSize = 0;
	}

	void PspAdhoc::RecordTransportOperation(Peer& peer, TransportOperation operation, int result, int requestedBytes,
		int transferredBytes, int queuedBytes)
	{
		if (operation == TransportOperation::Send) ++peer.Diagnostics.SendCalls;
		else if (operation == TransportOperation::Receive) ++peer.Diagnostics.ReceiveCalls;
		if ((result < 0 && result != AdhocWouldBlock) || (result == 0 && transferredBytes <= 0)) {
			++peer.Diagnostics.Failures;
		}
		peer.Diagnostics.LastOperation = operation;
		peer.Diagnostics.LastResult = result;
		peer.Diagnostics.LastRequestedBytes = requestedBytes;
		peer.Diagnostics.LastTransferredBytes = transferredBytes;
		peer.Diagnostics.LastQueuedBytes = queuedBytes;
	}

	void PspAdhoc::ResetNetworkTransport(Peer& peer)
	{
		peer.EngineTransport = false;
		peer.EngineTrafficSeen = false;
		peer.EngineTransportRequested = false;
		peer.LocalEngineReadySent = false;
		peer.RemoteEngineReadyReceived = false;
		peer.NetworkSendOffset = 0;
		peer.NetworkSendSize = 0;
		peer.NetworkReceiveSize = 0;
	}

	void PspAdhoc::SetLocalPlayerPosition(float x, float y)
	{
		localPlayerX_ = x;
		localPlayerY_ = y;
		localPlayerPositionValid_ = true;
	}

	int PspAdhoc::CreatePdp(const std::uint8_t* localMac, unsigned short localPort, int bufferSize)
	{
		if (infrastructureMode_) return internetRelay_ ? internetRelay_->PdpCreate(localMac, localPort) : -1;
		return sceNetAdhocPdpCreate(const_cast<std::uint8_t*>(localMac), localPort, bufferSize, 0);
	}

	int PspAdhoc::SendPdp(int socket, const std::uint8_t* destinationMac, unsigned short destinationPort,
		const void* data, int length)
	{
		if (infrastructureMode_) return internetRelay_ ? internetRelay_->PdpSend(socket, destinationMac, destinationPort, data, length) : -1;
		return sceNetAdhocPdpSend(socket, const_cast<std::uint8_t*>(destinationMac), destinationPort,
			const_cast<void*>(data), length, 0, 1);
	}

	int PspAdhoc::ReceivePdp(int socket, std::uint8_t* sourceMac, unsigned short* sourcePort,
		void* data, unsigned int* length)
	{
		if (infrastructureMode_) return internetRelay_ ? internetRelay_->PdpReceive(socket, sourceMac, sourcePort, data, length) : -1;
		return sceNetAdhocPdpRecv(socket, sourceMac, sourcePort, data, length, 0, 1);
	}

	void PspAdhoc::DeletePdp(int socket)
	{
		if (infrastructureMode_) {
			if (internetRelay_) internetRelay_->PdpDelete(socket);
		} else sceNetAdhocPdpDelete(socket, 0);
	}

	int PspAdhoc::ListenPtp(const std::uint8_t* localMac, unsigned short localPort, int bufferSize)
	{
		if (infrastructureMode_) return internetRelay_ ? internetRelay_->PtpListen(localMac, localPort) : -1;
		return sceNetAdhocPtpListen(const_cast<std::uint8_t*>(localMac), localPort, bufferSize, 100 * 1000, 10,
			MaxGuests, 0);
	}

	int PspAdhoc::AcceptPtp(int socket, std::uint8_t* peerMac, unsigned short* peerPort)
	{
		if (infrastructureMode_) return internetRelay_ ? internetRelay_->PtpAccept(socket, peerMac, peerPort) : -1;
		return sceNetAdhocPtpAccept(socket, peerMac, peerPort, 0, 1);
	}

	int PspAdhoc::OpenPtp(const std::uint8_t* localMac, unsigned short localPort, const std::uint8_t* peerMac,
		unsigned short peerPort, int bufferSize)
	{
		if (infrastructureMode_) return internetRelay_ ? internetRelay_->PtpOpen(localMac, localPort, peerMac, peerPort) : -1;
		return sceNetAdhocPtpOpen(const_cast<std::uint8_t*>(localMac), localPort,
			const_cast<std::uint8_t*>(peerMac), peerPort, bufferSize, 100 * 1000, 10, 0);
	}

	int PspAdhoc::ConnectPtp(int socket)
	{
		if (infrastructureMode_) return internetRelay_ ? internetRelay_->PtpConnect(socket) : -1;
		return sceNetAdhocPtpConnect(socket, 0, 1);
	}

	int PspAdhoc::SendPtp(int socket, const void* data, int* length)
	{
		if (infrastructureMode_) return internetRelay_ ? internetRelay_->PtpSend(socket, data, length) : -1;
		return sceNetAdhocPtpSend(socket, const_cast<void*>(data), length, 0, 1);
	}

	int PspAdhoc::ReceivePtp(int socket, void* data, int* length)
	{
		if (infrastructureMode_) return internetRelay_ ? internetRelay_->PtpReceive(socket, data, length) : -1;
		return sceNetAdhocPtpRecv(socket, data, length, 0, 1);
	}

	void PspAdhoc::FlushPtp(int socket)
	{
		if (!infrastructureMode_) sceNetAdhocPtpFlush(socket, 100 * 1000, 0);
	}

	void PspAdhoc::ClosePtp(int socket)
	{
		if (infrastructureMode_) {
			if (internetRelay_) internetRelay_->PtpClose(socket);
		} else sceNetAdhocPtpClose(socket, 0);
	}

	int PspAdhoc::GetLocalMac(std::uint8_t* mac) const
	{
		return infrastructureMode_ ? sceWlanGetEtherAddr(mac) : sceNetGetLocalEtherAddr(mac);
	}

	void PspAdhoc::Stop()
	{
		EndDiscovery();
		if (internetRelay_) internetRelay_->Stop();
		if (adhocctlInitialized_) {
			sceNetAdhocctlTerm();
			adhocctlInitialized_ = false;
		}
		if (adhocInitialized_) {
			sceNetAdhocTerm();
			adhocInitialized_ = false;
		}
		if (apctlInitialized_) {
			sceNetApctlDisconnect();
			sceNetApctlTerm();
			apctlInitialized_ = false;
		}
		if (inetInitialized_) {
			sceNetInetTerm();
			inetInitialized_ = false;
		}
		if (netInitialized_) {
			sceNetTerm();
			netInitialized_ = false;
		}
		if (adhocModuleLoaded_) {
			sceUtilityUnloadNetModule(PSP_NET_MODULE_ADHOC);
			adhocModuleLoaded_ = false;
		}
		if (inetModuleLoaded_) {
			sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
			inetModuleLoaded_ = false;
		}
		if (commonModuleLoaded_) {
			sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
			commonModuleLoaded_ = false;
		}
		if (state_ != State::Failed) {
			state_ = State::Stopped;
			std::strcpy(statusText_, infrastructureMode_ ? "Internet relay is off" : "Ad-hoc is off");
		}
	}

	bool PspAdhoc::Fail(const char* stage, int result)
	{
		state_ = State::Failed;
		std::snprintf(statusText_, sizeof(statusText_), "%s failed: %08X", stage, static_cast<unsigned int>(result));
		Stop();
		return false;
	}

	bool PspAdhoc::EnsureDiscoverySocket()
	{
		if (discoverySocket_ >= 0) return true;
		std::uint8_t localMac[6]{};
		int result = GetLocalMac(localMac);
		if (result < 0) return Fail("local MAC", result);
		discoverySocket_ = CreatePdp(localMac, DiscoveryPort, 4 * 1024);
		if (discoverySocket_ < 0) return Fail("discovery socket", discoverySocket_);
		return true;
	}

	void PspAdhoc::SendDiscoveryRequest()
	{
		DiscoveryPacket packet{};
		std::memcpy(packet.Magic, "J2AD", 4);
		packet.Version = DiscoveryVersion;
		packet.Type = DiscoveryRequest;
		std::uint8_t broadcastMac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
		SendPdp(discoverySocket_, broadcastMac, DiscoveryPort, &packet, sizeof(packet));
		SessionLog("discovery request peers=%d", (infrastructureMode_ && internetRelay_) ? internetRelay_->GetPeerCount() : -1);
	}

	void PspAdhoc::SendDiscoveryResponse(std::uint8_t* destinationMac)
	{
		DiscoveryPacket packet{};
		std::memcpy(packet.Magic, "J2AD", 4);
		packet.Version = DiscoveryVersion;
		packet.Type = DiscoveryResponse;
		packet.PlayerCount = static_cast<std::uint8_t>(1 + GetConnectedPeerCount());
		packet.MaxPlayerCount = static_cast<std::uint8_t>(1 + MaxGuests);
		packet.GameMode = sessionSettings_.GameMode;
		packet.Difficulty = sessionSettings_.Difficulty;
		packet.Flags = (sessionRunning_ ? 0x01 : 0x00) |
			(sessionSettings_.AllowMinimap ? 0x02 : 0x00);
		packet.TotalKills = sessionSettings_.TotalKills;
		packet.TotalLaps = sessionSettings_.TotalLaps;
		packet.TotalTreasureCollected = sessionSettings_.TotalTreasureCollected;
		packet.MaxGameTimeSecs = sessionSettings_.MaxGameTimeSecs;
		packet.OvertimeSecs = sessionSettings_.OvertimeSecs;
		std::snprintf(packet.HostName, sizeof(packet.HostName), "%s", hostName_);
		std::snprintf(packet.LevelName, sizeof(packet.LevelName), "%s", sessionSettings_.LevelName);
		std::snprintf(packet.LevelDisplayName, sizeof(packet.LevelDisplayName), "%s", sessionSettings_.LevelDisplayName);
		SendPdp(discoverySocket_, destinationMac, DiscoveryPort, &packet, sizeof(packet));
		SessionLog("discovery response -> %02X:%02X:%02X:%02X:%02X:%02X", destinationMac[0], destinationMac[1],
			destinationMac[2], destinationMac[3], destinationMac[4], destinationMac[5]);
	}

	void PspAdhoc::ReceiveDiscoveryPackets()
	{
		for (int i = 0; i < 8; ++i) {
			DiscoveryPacket packet{};
			std::uint8_t sourceMac[6]{};
			unsigned short sourcePort = 0;
			unsigned int length = sizeof(packet);
			const int result = ReceivePdp(discoverySocket_, sourceMac, &sourcePort, &packet, &length);
			if (result < 0) break;
			if (length != sizeof(packet) || std::memcmp(packet.Magic, "J2AD", 4) != 0 || packet.Version != DiscoveryVersion) continue;
			SessionLog("discovery recv type=%u src=%02X:%02X:%02X:%02X:%02X:%02X", packet.Type, sourceMac[0], sourceMac[1],
				sourceMac[2], sourceMac[3], sourceMac[4], sourceMac[5]);
			if (discoveryMode_ == DiscoveryMode::Host && packet.Type == DiscoveryRequest) {
				SendDiscoveryResponse(sourceMac);
			} else if (discoveryMode_ == DiscoveryMode::Client && packet.Type == DiscoveryResponse) {
				packet.HostName[sizeof(packet.HostName) - 1] = '\0';
				packet.LevelName[sizeof(packet.LevelName) - 1] = '\0';
				packet.LevelDisplayName[sizeof(packet.LevelDisplayName) - 1] = '\0';
				SessionSettings settings{};
				settings.GameMode = packet.GameMode;
				settings.Difficulty = packet.Difficulty;
				settings.AllowMinimap = (packet.Flags & 0x02) != 0;
				settings.TotalKills = packet.TotalKills;
				settings.TotalLaps = packet.TotalLaps;
				settings.TotalTreasureCollected = packet.TotalTreasureCollected;
				settings.MaxGameTimeSecs = packet.MaxGameTimeSecs;
				settings.OvertimeSecs = packet.OvertimeSecs;
				std::snprintf(settings.LevelName, sizeof(settings.LevelName), "%s", packet.LevelName);
				std::snprintf(settings.LevelDisplayName, sizeof(settings.LevelDisplayName), "%s", packet.LevelDisplayName);
				RememberHost(packet.HostName, sourceMac, packet.PlayerCount, packet.MaxPlayerCount,
					(packet.Flags & 0x01) != 0, settings);
			}
		}
	}

	void PspAdhoc::RememberHost(const char* name, const std::uint8_t* mac, std::uint8_t players, std::uint8_t maxPlayers,
		bool isRunning, const SessionSettings& settings)
	{
		int index = 0;
		while (index < hostCount_ && std::memcmp(hosts_[index].Info.Mac, mac, sizeof(hosts_[index].Info.Mac)) != 0) ++index;
		if (index == hostCount_) {
			if (hostCount_ >= MaxDiscoveredHosts) return;
			++hostCount_;
		}
		auto& host = hosts_[index];
		std::snprintf(host.Info.Name, sizeof(host.Info.Name), "%s", name[0] != '\0' ? name : "Player");
		std::memcpy(host.Info.Mac, mac, sizeof(host.Info.Mac));
		host.Info.PlayerCount = players;
		host.Info.MaxPlayerCount = maxPlayers;
		host.Info.IsRunning = isRunning;
		host.Info.Settings = settings;
		host.LastSeen = discoveryTick_;
	}

	void PspAdhoc::ExpireHosts()
	{
		for (int i = 0; i < hostCount_;) {
			if (discoveryTick_ - hosts_[i].LastSeen <= HostExpiry) {
				++i;
				continue;
			}
			hosts_[i] = hosts_[hostCount_ - 1];
			--hostCount_;
		}
	}

	void PspAdhoc::CloseDiscoverySocket()
	{
		if (discoverySocket_ >= 0) {
			DeletePdp(discoverySocket_);
			discoverySocket_ = -1;
		}
	}

	PspAdhoc::Peer* PspAdhoc::FindFreePeerSlot()
	{
		for (auto& peer : peers_) {
			if (peer.State == PeerState::Disconnected || peer.State == PeerState::Listening) return &peer;
		}
		return nullptr;
	}

	PspAdhoc::Peer* PspAdhoc::FindPeerByMac(const std::uint8_t* mac)
	{
		for (auto& peer : peers_) {
			if (peer.State == PeerState::Connected && std::memcmp(peer.Mac, mac, sizeof(peer.Mac)) == 0) return &peer;
		}
		return nullptr;
	}

	void PspAdhoc::UpdatePeerConnection()
	{
		if (discoveryMode_ == DiscoveryMode::Host) {
			if (!EnsureHostListener()) return;
			for (int i = 0; i < MaxGuests; ++i) {
				Peer* slot = FindFreePeerSlot();
				if (slot == nullptr) break;
				unsigned short peerPort = 0;
				std::uint8_t peerMac[6]{};
				const int accepted = AcceptPtp(listenSocket_, peerMac, &peerPort);
				if (accepted > 0) {
					ResetPeer(*slot);
					slot->Socket = accepted;
					std::memcpy(slot->Mac, peerMac, sizeof(slot->Mac));
					slot->State = PeerState::Connected;
					SessionLog("PTP accepted slot=%d peerPort=%u", static_cast<int>(slot - peers_), peerPort);
					BeginHandshake(*slot, true);
				} else {
					if (accepted < 0 && accepted != AdhocWouldBlock) {
						SessionLog("PTP accept failed result=%08X", static_cast<unsigned>(accepted));
					}
					break;
				}
			}
			for (auto& peer : peers_) {
				if (peer.State == PeerState::Connected) UpdateHandshake(peer);
			}
			return;
		}

		Peer& peer = peers_[0];
		if (peer.State == PeerState::Connecting) {
			const int result = ConnectPtp(peer.Socket);
			if (result == 0) {
				peer.State = PeerState::Connected;
				SessionLog("PTP connected after ticks=%u", peer.ConnectTick);
				BeginHandshake(peer, false);
			} else if (result != AdhocWouldBlock) {
				peer.State = PeerState::Failed;
				std::snprintf(peer.StatusText, sizeof(peer.StatusText), "Connect failed: %08X", static_cast<unsigned int>(result));
				SessionLog("PTP connect failed result=%08X ticks=%u", static_cast<unsigned>(result), peer.ConnectTick);
				ClosePtp(peer.Socket);
				peer.Socket = -1;
			} else if (++peer.ConnectTick >= 600) {
				peer.State = PeerState::Failed;
				std::snprintf(peer.StatusText, sizeof(peer.StatusText), "Connect timed out: %08X", static_cast<unsigned int>(result));
				ClosePtp(peer.Socket);
				peer.Socket = -1;
				SessionLog("PTP connect timed out");
			}
		}
		if (peer.State == PeerState::Connected) UpdateHandshake(peer);
	}

	bool PspAdhoc::EnsureHostListener()
	{
		if (listenSocket_ >= 0) return true;
		std::uint8_t localMac[6]{};
		int result = GetLocalMac(localMac);
		if (result < 0) {
			std::snprintf(peers_[0].StatusText, sizeof(peers_[0].StatusText), "local MAC failed: %08X",
				static_cast<unsigned int>(result));
			return false;
		}
		listenSocket_ = ListenPtp(localMac, HostPort, PtpSocketBufferSize);
		if (listenSocket_ < 0) {
			std::snprintf(peers_[0].StatusText, sizeof(peers_[0].StatusText), "PTP listen failed: %08X",
				static_cast<unsigned int>(listenSocket_));
			return false;
		}
		for (auto& peer : peers_) {
			if (peer.State == PeerState::Disconnected) {
				peer.State = PeerState::Listening;
				std::strcpy(peer.StatusText, "Waiting for peer...");
			}
		}
		return true;
	}

	void PspAdhoc::ResetPeer(Peer& peer)
	{
		ResetNetworkTransport(peer);
		if (peer.Socket >= 0) {
			ClosePtp(peer.Socket);
			peer.Socket = -1;
		}
		peer.State = PeerState::Disconnected;
		peer.ConnectTick = 0;
		std::memset(peer.Mac, 0, sizeof(peer.Mac));
		peer.Handshake = HandshakeStep::Idle;
		peer.HandshakeSendOffset = 0;
		peer.HandshakeReceiveOffset = 0;
		peer.HandshakeTick = 0;
		peer.ReadyTick = 0;
		peer.LastActivityTick = 0;
		peer.PendingControlByte = 0;
		peer.StartAcknowledged = false;
		peer.PeerReady = false;
		peer.LocalReadyDirty = false;
		peer.GracefulDisconnectReceived = false;
		peer.FurColor = 0;
		peer.RosterDirty = false;
		peer.RosterSendOffset = -1;
		peer.ReceivingRoster = false;
		peer.RosterReceiveOffset = 0;
		peer.RemoteGameplaySequence = 0;
		peer.RemotePositionValid = false;
		peer.LastGameplayPacketTick = 0;
		std::strcpy(peer.StatusText, "No peer connected");
	}

	void PspAdhoc::ClosePeerSockets()
	{
		for (auto& peer : peers_) ResetPeer(peer);
		CloseGameplaySocket();
		if (listenSocket_ >= 0) {
			ClosePtp(listenSocket_);
			listenSocket_ = -1;
		}
		startRequested_ = false;
		localReady_ = false;
		localPlayerPositionValid_ = false;
	}

	void PspAdhoc::BeginHandshake(Peer& peer, bool isHost)
	{
		SessionLog("handshake begin role=%s slot=%d", isHost ? "host" : "client", static_cast<int>(&peer - peers_));
		peer.PendingControlByte = 0;
		peer.StartAcknowledged = false;
		peer.PeerReady = false;
		peer.HandshakeSendOffset = 0;
		peer.HandshakeReceiveOffset = 0;
		peer.HandshakeTick = 0;
		if (isHost) {
			peer.Handshake = HandshakeStep::ReceiveHello;
			std::strcpy(peer.StatusText, "Waiting for player info...");
		} else {
			PrepareHandshakePacket(peer, HandshakeHello);
			peer.Handshake = HandshakeStep::SendHello;
			std::strcpy(peer.StatusText, "Sending player info...");
		}
	}

	void PspAdhoc::UpdateHandshake(Peer& peer)
	{
		if (peer.Handshake == HandshakeStep::Ready) {
			UpdateReadySession(peer);
			return;
		}
		if (peer.Handshake == HandshakeStep::Failed || peer.Handshake == HandshakeStep::Idle) return;
		if (++peer.HandshakeTick >= 600) {
			FailHandshake(peer, "Handshake timed out");
			return;
		}

		if (peer.Handshake == HandshakeStep::SendHello || peer.Handshake == HandshakeStep::SendAck) {
			const HandshakeStep sendingStep = peer.Handshake;
			int length = static_cast<int>(sizeof(peer.HandshakeSend)) - peer.HandshakeSendOffset;
			const int result = SendPtp(peer.Socket, peer.HandshakeSend + peer.HandshakeSendOffset, &length);
			if (result == AdhocWouldBlock) return;
			if (result < 0 || length <= 0) {
				SessionLog("handshake send failed step=%d result=%08X length=%d offset=%d", static_cast<int>(sendingStep),
					static_cast<unsigned>(result), length, peer.HandshakeSendOffset);
				FailHandshake(peer, "Handshake send failed");
				return;
			}
			peer.HandshakeSendOffset += length;
			if (peer.HandshakeSendOffset < static_cast<int>(sizeof(peer.HandshakeSend))) return;
			SessionLog("handshake sent step=%d bytes=%d", static_cast<int>(sendingStep), peer.HandshakeSendOffset);
			if (peer.Handshake == HandshakeStep::SendHello) {
				peer.Handshake = HandshakeStep::ReceiveAck;
				peer.HandshakeReceiveOffset = 0;
				std::strcpy(peer.StatusText, "Waiting for host info...");
			} else {
				CompleteHandshake(peer);
			}
			return;
		}

		int length = static_cast<int>(sizeof(peer.HandshakeReceive)) - peer.HandshakeReceiveOffset;
		const int result = ReceivePtp(peer.Socket, peer.HandshakeReceive + peer.HandshakeReceiveOffset, &length);
		if (result == AdhocWouldBlock) return;
		if (result < 0 || length <= 0) {
			SessionLog("handshake receive failed step=%d result=%08X length=%d offset=%d", static_cast<int>(peer.Handshake),
				static_cast<unsigned>(result), length, peer.HandshakeReceiveOffset);
			FailHandshake(peer, "Handshake receive failed");
			return;
		}
		peer.HandshakeReceiveOffset += length;
		if (peer.HandshakeReceiveOffset < static_cast<int>(sizeof(peer.HandshakeReceive))) return;
		SessionLog("handshake received step=%d bytes=%d", static_cast<int>(peer.Handshake), peer.HandshakeReceiveOffset);

		const std::uint8_t expectedType = (peer.Handshake == HandshakeStep::ReceiveHello ? HandshakeHello : HandshakeAck);
		if (!ValidateHandshakePacket(peer, expectedType)) {
			FailHandshake(peer, "Invalid handshake");
			return;
		}
		if (peer.Handshake == HandshakeStep::ReceiveHello) {
			PrepareHandshakePacket(peer, HandshakeAck);
			peer.Handshake = HandshakeStep::SendAck;
			peer.HandshakeSendOffset = 0;
			std::strcpy(peer.StatusText, "Confirming player...");
		} else {
			CompleteHandshake(peer);
		}
	}

	void PspAdhoc::PrepareHandshakePacket(Peer& peer, std::uint8_t type)
	{
		const std::uint8_t flags = (discoveryMode_ == DiscoveryMode::Host && sessionRunning_ ? 0x01 : 0x00);
		HandshakePacket packet{{'J', '2', 'P', 'T'}, HandshakeVersion, type, localCharacter_, flags, {}, localFurColor_,
			localTeam_};
		std::snprintf(packet.PlayerName, sizeof(packet.PlayerName), "%s", localName_);
		std::memcpy(peer.HandshakeSend, &packet, sizeof(packet));
	}

	bool PspAdhoc::ValidateHandshakePacket(Peer& peer, std::uint8_t expectedType)
	{
		HandshakePacket packet{};
		std::memcpy(&packet, peer.HandshakeReceive, sizeof(packet));
		if (std::memcmp(packet.Magic, "J2PT", 4) != 0 || packet.Version != HandshakeVersion ||
			packet.Type != expectedType || packet.Character > 2) return false;
		// An out-of-range team is a malformed peer, not a preference: reject rather than silently coercing,
		// so the protocol mistake surfaces here instead of as a mysterious team assignment later.
		if (packet.Team != PspAdhoc::NoAdhocTeam && packet.Team >= 4) return false;
		packet.PlayerName[sizeof(packet.PlayerName) - 1] = '\0';
		std::snprintf(peer.Name, sizeof(peer.Name), "%s", packet.PlayerName[0] != '\0' ? packet.PlayerName : "Player");
		peer.Character = packet.Character;
		peer.FurColor = packet.FurColor;
		peer.Team = packet.Team;
		if (expectedType == HandshakeAck && discoveryMode_ == DiscoveryMode::Client) {
			sessionRunning_ = (packet.Reserved & 0x01) != 0;
		}
		return true;
	}

	void PspAdhoc::SetLocalFurColor(std::uint32_t furColor)
	{
		if (localFurColor_ == furColor) return;
		localFurColor_ = furColor;
		MarkRosterDirty();
	}

	void PspAdhoc::SetLocalTeam(std::uint8_t team)
	{
		if (localTeam_ == team) return;
		localTeam_ = team;
		MarkRosterDirty();
	}

	void PspAdhoc::LogDiagnostic(const char* format, ...)
	{
#if defined(JAZZ2_PSP_DEBUG)
		char line[256];
		va_list args;
		va_start(args, format);
		std::vsnprintf(line, sizeof(line), format, args);
		va_end(args);
		SessionLog("%s", line);
#else
		(void)format;
#endif
	}

	std::uint8_t PspAdhoc::GetPeerTeam(int peerIndex) const
	{
		if (peerIndex < 0 || peerIndex >= MaxPeers) return NoAdhocTeam;
		return peers_[peerIndex].Team;
	}

	std::uint32_t PspAdhoc::GetPeerFurColor(int peerIndex) const
	{
		if (peerIndex < 0 || peerIndex >= MaxPeers) return 0;
		return peers_[peerIndex].FurColor;
	}

	void PspAdhoc::MarkRosterDirty()
	{
		if (discoveryMode_ != DiscoveryMode::Host) return;
		RebuildRoster();
		for (auto& peer : peers_) {
			if (peer.State == PeerState::Connected) peer.RosterDirty = true;
		}
	}

	// Host only: the roster is the local player followed by every handshaked guest.
	void PspAdhoc::RebuildRoster()
	{
		rosterCount_ = 0;
		auto& local = roster_[rosterCount_++];
		std::snprintf(local.Name, sizeof(local.Name), "%s", localName_);
		local.Character = localCharacter_;
		local.Ready = localReady_;
		local.IsHost = true;
		local.FurColor = localFurColor_;
		local.Team = localTeam_;
		for (const auto& peer : peers_) {
			if (peer.State != PeerState::Connected || peer.Handshake != HandshakeStep::Ready) continue;
			if (rosterCount_ >= MaxRosterEntries) break;
			auto& entry = roster_[rosterCount_++];
			std::snprintf(entry.Name, sizeof(entry.Name), "%s", peer.Name);
			entry.Character = peer.Character;
			entry.Ready = peer.PeerReady;
			entry.IsHost = false;
			entry.FurColor = peer.FurColor;
			entry.Team = peer.Team;
		}
	}

	// Guest only: adopt the host's roster verbatim.
	void PspAdhoc::ApplyRoster(const std::uint8_t* payload)
	{
		RosterWirePacket packet{};
		std::memcpy(&packet, payload, sizeof(packet));
		rosterCount_ = 0;
		const int count = (packet.Count > MaxRosterEntries ? MaxRosterEntries : packet.Count);
		for (int i = 0; i < count; ++i) {
			const RosterWireEntry& wire = packet.Entries[i];
			auto& entry = roster_[rosterCount_++];
			std::memcpy(entry.Name, wire.Name, sizeof(entry.Name));
			entry.Name[sizeof(entry.Name) - 1] = '\0';
			entry.Character = wire.Character;
			entry.Ready = (wire.Ready != 0);
			entry.IsHost = (wire.IsHost != 0);
			entry.FurColor = wire.FurColor;
			entry.Team = wire.Team;
		}
	}

	void PspAdhoc::FailHandshake(Peer& peer, const char* reason)
	{
		SessionLog("handshake failed reason=%s tick=%u send=%d receive=%d", reason, peer.HandshakeTick,
			peer.HandshakeSendOffset, peer.HandshakeReceiveOffset);
		peer.Handshake = HandshakeStep::Failed;
		peer.State = PeerState::Failed;
		std::snprintf(peer.StatusText, sizeof(peer.StatusText), "%s", reason);
		if (peer.Socket >= 0) {
			ClosePtp(peer.Socket);
			peer.Socket = -1;
		}
		// A failed guest must not wedge the host; free its slot for the next join.
		if (discoveryMode_ == DiscoveryMode::Host) ResetPeer(peer);
	}

	void PspAdhoc::CompleteHandshake(Peer& peer)
	{
		SessionLog("handshake ready peer=%s character=%u running=%d", peer.Name, peer.Character, sessionRunning_ ? 1 : 0);
		peer.Handshake = HandshakeStep::Ready;
		peer.ReadyTick = 0;
		peer.LastActivityTick = 0;
		peer.PeerReady = false;
		peer.LocalReadyDirty = true;
		if (discoveryMode_ == DiscoveryMode::Host && sessionRunning_) {
			peer.PendingControlByte = StartGameRequest;
			startRequested_ = true;
			std::strcpy(peer.StatusText, "Inviting player to running game...");
		} else {
			std::snprintf(peer.StatusText, sizeof(peer.StatusText), "Ready: %s (%s)", peer.Name, CharacterName(peer.Character));
		}
		MarkRosterDirty();
	}

	void PspAdhoc::UpdateReadySession(Peer& peer)
	{
		++peer.ReadyTick;

		// In-game liveness. A guest that crashes or loses power leaves a HALF-OPEN PTP connection: the socket
		// never errors and never closes, so nothing else notices and its player stands in the level forever.
		// Gated on EngineTrafficSeen because until the peer speaks after the barrier there is no baseline,
		// and both sides can idle for seconds while the other finishes loading.
		if (peer.EngineTransport && peer.EngineTrafficSeen &&
			NowMs() - peer.LastActivityMs > EnginePeerTimeoutMs) {
			SessionLog("engine peer timed out mode=%d slot=%d gapMs=%u",
				static_cast<int>(discoveryMode_), static_cast<int>(&peer - peers_), NowMs() - peer.LastActivityMs);
			HandlePeerDisconnected(peer);
			return;
		}

		if (peer.EngineTransport) {
			FlushNetworkSend(peer);
			return;
		}
		// A roster message is 0x06 plus a fixed payload: it must be finished before any other byte goes
		// out, or the peer reads the payload as control bytes.
		if (peer.RosterSendOffset >= 0) {
			int length = static_cast<int>(sizeof(peer.RosterSend)) - peer.RosterSendOffset;
			const int result = SendPtp(peer.Socket, peer.RosterSend + peer.RosterSendOffset, &length);
			if (result == 0 && length > 0) {
				peer.RosterSendOffset += length;
				if (peer.RosterSendOffset >= static_cast<int>(sizeof(peer.RosterSend))) peer.RosterSendOffset = -1;
			} else if (result < 0 && result != AdhocWouldBlock) {
				HandlePeerDisconnected(peer);
				return;
			}
			return;
		}
		// Once the engine transport is requested the reliable stream is about to carry engine frames:
		// stop queueing lobby ready/not-ready toggles onto it.
		if (!peer.EngineTransportRequested && peer.PendingControlByte == 0 && peer.LocalReadyDirty) {
			peer.PendingControlByte = (localReady_ ? PlayerReady : PlayerNotReady);
		}
		// Push the roster only while the lobby still owns the stream.
		if (peer.PendingControlByte == 0 && peer.RosterDirty && !peer.EngineTransportRequested &&
			discoveryMode_ == DiscoveryMode::Host && peer.Handshake == HandshakeStep::Ready) {
			RosterWirePacket packet{};
			packet.Count = static_cast<std::uint8_t>(rosterCount_);
			for (int i = 0; i < rosterCount_; ++i) {
				RosterWireEntry& wire = packet.Entries[i];
				std::snprintf(wire.Name, sizeof(wire.Name), "%s", roster_[i].Name);
				wire.Character = roster_[i].Character;
				wire.Ready = roster_[i].Ready ? 1 : 0;
				wire.IsHost = roster_[i].IsHost ? 1 : 0;
				wire.FurColor = roster_[i].FurColor;
				wire.Team = roster_[i].Team;
			}
			peer.RosterSend[0] = RosterUpdate;
			std::memcpy(peer.RosterSend + 1, &packet, sizeof(packet));
			peer.RosterSendOffset = 0;
			peer.RosterDirty = false;
			return;
		}
		if (peer.PendingControlByte != 0) {
			const std::uint8_t sentControlByte = peer.PendingControlByte;
			int length = 1;
			const int result = SendPtp(peer.Socket, &peer.PendingControlByte, &length);
			if (result == 0 && length == 1) {
				SessionLog("control sent byte=%02X tick=%u", sentControlByte, peer.ReadyTick);
				peer.PendingControlByte = 0;
				if (sentControlByte == PlayerReady || sentControlByte == PlayerNotReady) {
					peer.LocalReadyDirty = (sentControlByte != (localReady_ ? PlayerReady : PlayerNotReady));
				}
				if (sentControlByte == EngineTransportReady) {
					peer.LocalEngineReadySent = true;
				} else if (sentControlByte == StartGameRequest && discoveryMode_ == DiscoveryMode::Host) {
					std::strcpy(peer.StatusText, "Waiting for Start acknowledgment...");
				} else if (sentControlByte == StartGameAck) {
					peer.StartAcknowledged = true;
					std::strcpy(peer.StatusText, "Starting game...");
				}
			} else if (result < 0 && result != AdhocWouldBlock) {
				HandlePeerDisconnected(peer);
				return;
			}
		}
		// No heartbeats after our engine-ready 0x03: the reliable stream must contain nothing but engine
		// frames past that point, or the peer mis-frames the stray bytes and hangs on "Synchronizing".
		if (!peer.LocalEngineReadySent && peer.ReadyTick % HeartbeatInterval == 1) {
			std::uint8_t heartbeat = HeartbeatByte;
			int length = 1;
			const int result = SendPtp(peer.Socket, &heartbeat, &length);
			if (result < 0 && result != AdhocWouldBlock) {
				HandlePeerDisconnected(peer);
				return;
			}
		}

		for (int i = 0; i < 8; ++i) {
			// A roster payload is in flight: consume exactly its bytes, not control bytes.
			if (peer.ReceivingRoster) {
				int rosterLength = RosterWireSize - peer.RosterReceiveOffset;
				const int rosterResult = ReceivePtp(peer.Socket, peer.RosterReceive + peer.RosterReceiveOffset, &rosterLength);
				if (rosterResult == AdhocWouldBlock) break;
				if (rosterResult < 0 || rosterLength <= 0) {
					HandlePeerDisconnected(peer);
					return;
				}
				peer.LastActivityTick = peer.ReadyTick;
				peer.RosterReceiveOffset += rosterLength;
				if (peer.RosterReceiveOffset >= RosterWireSize) {
					ApplyRoster(peer.RosterReceive);
					peer.ReceivingRoster = false;
					peer.RosterReceiveOffset = 0;
					SessionLog("roster received entries=%d", rosterCount_);
				}
				continue;
			}
			std::uint8_t controlByte = 0;
			int length = 1;
			const int result = ReceivePtp(peer.Socket, &controlByte, &length);
			if (result == AdhocWouldBlock) break;
			if (result < 0 || length <= 0) {
				HandlePeerDisconnected(peer);
				return;
			}
			peer.LastActivityTick = peer.ReadyTick;
			if (controlByte != HeartbeatByte || peer.ReadyTick <= HeartbeatInterval * 2)
				SessionLog("control received byte=%02X tick=%u", controlByte, peer.ReadyTick);
			if (controlByte == StartGameRequest && discoveryMode_ == DiscoveryMode::Client) {
				startRequested_ = true;
				peer.PendingControlByte = StartGameAck;
				std::strcpy(peer.StatusText, "Host requested Start Game");
			} else if (controlByte == StartGameAck && discoveryMode_ == DiscoveryMode::Host) {
				peer.StartAcknowledged = true;
				sessionRunning_ = true;
				std::strcpy(peer.StatusText, "Start Game acknowledged");
			} else if (controlByte == EngineTransportReady) {
				peer.RemoteEngineReadyReceived = true;
				// Barrier complete once both sides have exchanged 0x03. Everything after the peer's 0x03
				// is engine frames, so stop draining control bytes and leave the stream aligned for
				// ReceiveNetworkPacketFrom, or the first frame's length bytes are eaten and it desyncs.
				if (peer.EngineTransportRequested && peer.LocalEngineReadySent) break;
			} else if (controlByte == RosterUpdate) {
				peer.ReceivingRoster = true;
				peer.RosterReceiveOffset = 0;
			} else if (controlByte == PlayerReady) {
				peer.PeerReady = true;
				MarkRosterDirty();
			} else if (controlByte == PlayerNotReady) {
				peer.PeerReady = false;
				MarkRosterDirty();
			}
		}

		// Lobby liveness (the in-game one is at the top of this function). Skipped once the engine transport
		// is requested: the peer is then silent inside a multi-second synchronous level load while our ticks
		// keep running, and dropping it there would kill precisely the slow console this path exists for.
		if (!peer.EngineTransportRequested && peer.ReadyTick - peer.LastActivityTick > PeerTimeout) {
			SessionLog("peer activity timed out tick=%u last=%u", peer.ReadyTick, peer.LastActivityTick);
			HandlePeerDisconnected(peer);
			return;
		}

		if (peer.EngineTransportRequested && peer.LocalEngineReadySent && peer.RemoteEngineReadyReceived) {
			peer.EngineTransport = true;
			// Our ticks ran through the peer's whole level load, so restart the clock here or the liveness
			// check above drops it on its first in-game frame.
			peer.LastActivityTick = peer.ReadyTick;
			peer.LastActivityMs = NowMs();
			peer.EngineTrafficSeen = false;
			SessionLog("engine transport ready tick=%u slot=%d", peer.ReadyTick, static_cast<int>(&peer - peers_));
			std::strcpy(peer.StatusText, "Authoritative session active");
			return;
		}

	}

	bool PspAdhoc::EnsureGameplaySocket()
	{
		if (gameplaySocket_ >= 0) return true;
		std::uint8_t localMac[6]{};
		if (GetLocalMac(localMac) < 0) return false;
		// Authoritative actor snapshots regularly exceed 2 KiB after compression; a queue smaller than one
		// legal datagram silently drops whole state updates and freezes remote actors until a full resync.
		gameplaySocket_ = CreatePdp(localMac, GameplayPort, NetworkDatagramSize * 2);
		return gameplaySocket_ >= 0;
	}

	void PspAdhoc::UpdateGameplayState()
	{
		if (GetConnectedPeerCount() == 0 || !localPlayerPositionValid_ || !EnsureGameplaySocket()) return;
		if ((discoveryTick_ & 1u) == 0) {
			GameplayPacket packet{};
			std::memcpy(packet.Magic, "J2PS", 4);
			packet.Version = 1;
			packet.Type = 1;
			packet.Sequence = ++gameplaySequence_;
			packet.X = static_cast<std::int32_t>(std::lround(localPlayerX_ * 256.0f));
			packet.Y = static_cast<std::int32_t>(std::lround(localPlayerY_ * 256.0f));
			for (auto& peer : peers_) {
				if (peer.State != PeerState::Connected || !peer.StartAcknowledged) continue;
				SendPdp(gameplaySocket_, peer.Mac, GameplayPort, &packet, sizeof(packet));
			}
		}

		for (int i = 0; i < 8; ++i) {
			GameplayPacket packet{};
			std::uint8_t sourceMac[6]{};
			unsigned short sourcePort = 0;
			unsigned int length = sizeof(packet);
			const int result = ReceivePdp(gameplaySocket_, sourceMac, &sourcePort, &packet, &length);
			if (result < 0) break;
			if (length != sizeof(packet) || sourcePort != GameplayPort ||
				std::memcmp(packet.Magic, "J2PS", 4) != 0 || packet.Version != 1 || packet.Type != 1) continue;
			Peer* peer = FindPeerByMac(sourceMac);
			if (peer == nullptr) continue;
			if (!peer->RemotePositionValid || static_cast<std::int32_t>(packet.Sequence - peer->RemoteGameplaySequence) > 0) {
				peer->RemoteGameplaySequence = packet.Sequence;
				peer->RemoteX = packet.X * (1.0f / 256.0f);
				peer->RemoteY = packet.Y * (1.0f / 256.0f);
				peer->RemotePositionValid = true;
				peer->LastGameplayPacketTick = discoveryTick_;
			}
		}
		for (auto& peer : peers_) {
			if (peer.RemotePositionValid && discoveryTick_ - peer.LastGameplayPacketTick > 120) peer.RemotePositionValid = false;
		}
	}

	void PspAdhoc::CloseGameplaySocket()
	{
		if (gameplaySocket_ >= 0) {
			DeletePdp(gameplaySocket_);
			gameplaySocket_ = -1;
		}
		gameplaySequence_ = 0;
		localPlayerPositionValid_ = false;
		for (auto& peer : peers_) {
			peer.RemoteGameplaySequence = 0;
			peer.LastGameplayPacketTick = 0;
			peer.RemotePositionValid = false;
		}
	}

	void PspAdhoc::HandlePeerDisconnected(Peer& peer)
	{
		SessionLog("peer disconnected mode=%d slot=%d engine=%d", static_cast<int>(discoveryMode_),
			static_cast<int>(&peer - peers_), peer.EngineTransport ? 1 : 0);
		ResetNetworkTransport(peer);
		if (peer.Socket >= 0) {
			ClosePtp(peer.Socket);
			peer.Socket = -1;
		}
		peer.Handshake = HandshakeStep::Idle;
		peer.ReadyTick = 0;
		peer.LastActivityTick = 0;
		peer.PendingControlByte = 0;
		peer.StartAcknowledged = false;
		peer.PeerReady = false;
		peer.LocalReadyDirty = true;
		peer.RemotePositionValid = false;
		peer.RosterDirty = false;
		peer.RosterSendOffset = -1;
		peer.ReceivingRoster = false;
		peer.RosterReceiveOffset = 0;
		if (discoveryMode_ == DiscoveryMode::Host) {
			// Free the slot so another guest can take it; the listener keeps running.
			std::memset(peer.Mac, 0, sizeof(peer.Mac));
			peer.State = PeerState::Listening;
			std::strcpy(peer.StatusText, "Waiting for peer...");
			MarkRosterDirty();
		} else {
			peer.State = PeerState::Failed;
			std::strcpy(peer.StatusText, "Host disconnected");
		}
	}
}
