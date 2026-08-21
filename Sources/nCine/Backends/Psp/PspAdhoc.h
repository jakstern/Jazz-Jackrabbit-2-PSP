#pragma once

#include <cstdint>
#include <memory>

namespace nCine
{
	class PspInternetRelayTransport;

	// Ad-hoc/infrastructure session for the PSP multiplayer lobby and the engine transport.
	// Authoritative-host star: the host holds independent state per guest, a guest keeps one
	// connection to the host and uses peer slot 0. Guests never talk to each other; they see
	// one another only through the host's relayed engine state.
	class PspAdhoc
	{
	public:
		static constexpr int MaxDiscoveredHosts = 8;
		// Host capacity: guests in addition to the local player (so 3 => 4 players).
		static constexpr int MaxGuests = 3;
		// Mirrors Jazz2::Multiplayer::NoPreferredTeam; duplicated so this backend does not depend on the
		// game layer. The static_assert in main.cpp pins them together.
		static constexpr std::uint8_t NoAdhocTeam = 0xFF;
		// Per reliable stream: PTP socket buffer plus the send/receive heap buffers. Kept small so
		// listener + N guests at 64 KiB each stay well under the fixed 512 KiB network pool.
		static constexpr int TransportBufferSize = 64 * 1024;
		struct SessionSettings {
			std::uint8_t GameMode = 0;
			std::uint8_t Difficulty = 1;
			bool AllowMinimap = false;
			std::uint32_t TotalKills = 0;
			std::uint32_t TotalLaps = 0;
			std::uint32_t TotalTreasureCollected = 0;
			std::uint32_t MaxGameTimeSecs = 0;
			std::uint32_t OvertimeSecs = 0;
			char LevelName[48]{};
			char LevelDisplayName[32]{};
		};

		enum class State {
			Stopped,
			Connecting,
			Connected,
			Failed
		};
		enum class PeerState {
			Disconnected,
			Listening,
			Connecting,
			Connected,
			Failed
		};
		enum class TransportOperation : std::uint8_t {
			None,
			Send,
			Receive
		};
		struct TransportDiagnostics {
			std::uint32_t SendCalls = 0;
			std::uint32_t ReceiveCalls = 0;
			std::uint32_t Failures = 0;
			TransportOperation LastOperation = TransportOperation::None;
			int LastResult = 0;
			int LastRequestedBytes = 0;
			int LastTransferredBytes = 0;
			int LastQueuedBytes = 0;
		};
		struct DiscoveredHost {
			char Name[32]{};
			std::uint8_t Mac[6]{};
			std::uint8_t PlayerCount = 0;
			std::uint8_t MaxPlayerCount = 0;
			bool IsRunning = false;
			SessionSettings Settings;
		};
		// One lobby roster row. The host builds the roster and pushes it to the guests; a guest has
		// no other way to see the other guests, being connected only to the host.
		struct RosterEntry {
			char Name[32]{};
			std::uint8_t Character = 0;
			bool Ready = false;
			bool IsHost = false;
			std::uint32_t FurColor = 0;
			// Preference, not an assignment: the engine resolves it (and enforces balance) at spawn.
			std::uint8_t Team = NoAdhocTeam;
		};
		static constexpr int MaxRosterEntries = 1 + MaxGuests;
		// Roster wire layout (packed): Count, then MaxRosterEntries x
		// { Name[32], Character, Ready, IsHost, FurColor(4), Team }.
		static constexpr int RosterWireEntrySize = 32 + 1 + 1 + 1 + 4 + 1;
		static constexpr int RosterWireSize = 1 + MaxRosterEntries * RosterWireEntrySize;
		// J2PT identity handshake: magic(4), version, type, character, reserved, name[32], furColor(4), team.
		static constexpr int HandshakeWireSize = 45;

		PspAdhoc();
		~PspAdhoc();

		PspAdhoc(const PspAdhoc&) = delete;
		PspAdhoc& operator=(const PspAdhoc&) = delete;

		bool Start();
		bool PrepareInfrastructure();
		bool StartInfrastructure(const char* serverAddress);
		void CancelInfrastructure(const char* reason);
		bool IsInfrastructureMode() const { return infrastructureMode_; }
		static bool IsWlanSwitchOn();
		void Update();
		void Stop();
		void BeginDiscovery();
		void BeginHosting(const char* playerName, std::uint8_t character, const SessionSettings& settings);
		void EndDiscovery();
		bool ConnectToHost(int index, const char* playerName, std::uint8_t character);
		void SetLocalCharacter(std::uint8_t character) { localCharacter_ = character; }
		// Must be set before the lobby handshake so the peer learns our fur color.
		void SetLocalFurColor(std::uint32_t furColor);
		// Local team preference (NoAdhocTeam = auto). Marks the roster dirty, so a change made in the
		// lobby propagates to everyone.
		void SetLocalTeam(std::uint8_t team);
		std::uint8_t GetLocalTeam() const { return localTeam_; }
		std::uint8_t GetPeerTeam(int peerIndex) const;
		// Writes a line into the ad-hoc session log - the only log the game layer can reach, since engine
		// LOG*() traces go to stdout, which is nowhere on PSP. Compiled to nothing without PSP_DEBUG.
		void LogDiagnostic(const char* format, ...);
		std::uint32_t GetLocalFurColor() const { return localFurColor_; }
		void SetSessionLevel(const char* levelName, const char* levelDisplayName);

		State GetState() const { return state_; }
		const char* GetStatusText() const { return statusText_; }
		const char* GetLocalName() const { return localName_; }
		std::uint8_t GetLocalCharacter() const { return localCharacter_; }
		void SetLocalReady(bool ready);
		bool IsLocalReady() const { return localReady_; }
		bool CanStartGame() const;
		// At least one guest connected and ready. Required by the lobby's Start button so a host cannot
		// start a fresh match alone; hot-join advertising deliberately does NOT require it.
		bool HasReadyGuest() const;
		bool RequestStartGame();
		bool HasStartRequest() const { return startRequested_; }
		bool HasStartAcknowledged() const;
		bool IsSessionRunning() const { return sessionRunning_; }
		bool IsHosting() const { return discoveryMode_ == DiscoveryMode::Host; }
		bool BeginEngineTransport();
		void DisconnectPeer();
		void SetLocalPlayerPosition(float x, float y);
		const SessionSettings& GetSessionSettings() const { return sessionSettings_; }
		int GetDiscoveredHostCount() const { return hostCount_; }
		const DiscoveredHost& GetDiscoveredHost(int index) const { return hosts_[index].Info; }

		// Per-peer accessors. Slot 0 is the host connection on a guest, or the first guest on a host;
		// slots are stable for the lifetime of a connection.
		static constexpr int MaxPeers = MaxGuests;
		int GetPeerSlotCount() const { return MaxPeers; }
		// Number of peers that completed the identity handshake.
		int GetConnectedPeerCount() const;
		bool IsPeerConnected(int peerIndex) const;
		bool IsPeerSessionReady(int peerIndex) const;
		bool IsPeerEngineReady(int peerIndex) const;
		PeerState GetPeerState(int peerIndex) const;
		const char* GetPeerStatusText(int peerIndex) const;
		const char* GetPeerName(int peerIndex) const;
		std::uint8_t GetPeerCharacter(int peerIndex) const;
		std::uint32_t GetPeerFurColor(int peerIndex) const;
		bool IsPeerReady(int peerIndex) const;

		// Lobby roster, identical on the host and every guest; guests receive it over the lobby stream.
		int GetRosterCount() const { return rosterCount_; }
		const RosterEntry& GetRosterEntry(int index) const { return roster_[index]; }
		bool ConsumeGracefulDisconnect(int peerIndex);
		bool HasRemotePlayerPosition(int peerIndex) const;
		float GetRemotePlayerX(int peerIndex) const;
		float GetRemotePlayerY(int peerIndex) const;
		const TransportDiagnostics& GetTransportDiagnostics(int peerIndex) const;
		bool SendNetworkPacketTo(int peerIndex, std::uint8_t channel, std::uint8_t packetType,
			const std::uint8_t* data, int length);
		// "Nothing pending" from ReceiveNetworkPacketFrom. Cannot be 0: a zero-length payload is a legal
		// frame and must stay distinguishable.
		static constexpr int NoPacketAvailable = -2;
		// Drains the next available packet from any peer; reports its slot in peerIndex.
		// Returns the payload length (>= 0), NoPacketAvailable, or -1 if the peer dropped.
		int ReceiveNetworkPacketFrom(int& peerIndex, std::uint8_t& channel, std::uint8_t& packetType,
			std::uint8_t* data, int capacity);

		// Single-peer shims (slot 0 / aggregate), kept so the menu and the app lifecycle work unchanged.
		PeerState GetPeerState() const { return GetPeerState(0); }
		const char* GetPeerStatusText() const { return GetPeerStatusText(0); }
		const char* GetPeerName() const { return GetPeerName(0); }
		std::uint8_t GetPeerCharacter() const { return GetPeerCharacter(0); }
		bool IsPeerReady() const { return IsPeerReady(0); }
		bool IsSessionReady() const { return IsPeerSessionReady(0); }
		// True once ANY guest has completed the engine barrier, so the app attaches once.
		bool IsEngineTransportReady() const;
		bool ConsumeGracefulDisconnect() { return ConsumeGracefulDisconnect(0); }
		bool HasRemotePlayerPosition() const { return HasRemotePlayerPosition(0); }
		float GetRemotePlayerX() const { return GetRemotePlayerX(0); }
		float GetRemotePlayerY() const { return GetRemotePlayerY(0); }
		const TransportDiagnostics& GetTransportDiagnostics() const { return GetTransportDiagnostics(0); }
		bool SendNetworkPacket(std::uint8_t channel, std::uint8_t packetType, const std::uint8_t* data, int length)
		{
			return SendNetworkPacketTo(0, channel, packetType, data, length);
		}
		int ReceiveNetworkPacket(std::uint8_t& channel, std::uint8_t& packetType, std::uint8_t* data, int capacity)
		{
			int peerIndex = 0;
			return ReceiveNetworkPacketFrom(peerIndex, channel, packetType, data, capacity);
		}

	private:
		enum class DiscoveryMode { Idle, Client, Host };
		enum class HandshakeStep { Idle, SendHello, ReceiveHello, SendAck, ReceiveAck, Ready, Failed };
		struct HostEntry {
			DiscoveredHost Info;
			std::uint32_t LastSeen = 0;
		};
		// Per-peer state. The host holds one of these per guest; a guest uses only slot 0.
		struct Peer {
			PeerState State = PeerState::Disconnected;
			int Socket = -1;
			std::uint32_t ConnectTick = 0;
			std::uint8_t Mac[6]{};
			char Name[32] = "Host";
			std::uint8_t Character = 0;
			std::uint32_t FurColor = 0;
			std::uint8_t Team = NoAdhocTeam;
			HandshakeStep Handshake = HandshakeStep::Idle;
			// Lobby roster transfer (host -> guest): a framed message on the otherwise byte-oriented
			// lobby stream, so each direction needs its own cursor. Never sent once the engine-ready
			// 0x03 has gone out, which keeps the reliable stream pure engine frames past the barrier.
			bool RosterDirty = false;
			int RosterSendOffset = -1;
			std::uint8_t RosterSend[1 + RosterWireSize]{};
			bool ReceivingRoster = false;
			int RosterReceiveOffset = 0;
			std::uint8_t RosterReceive[RosterWireSize]{};
			std::uint8_t HandshakeSend[HandshakeWireSize]{};
			std::uint8_t HandshakeReceive[HandshakeWireSize]{};
			int HandshakeSendOffset = 0;
			int HandshakeReceiveOffset = 0;
			std::uint32_t HandshakeTick = 0;
			std::uint32_t ReadyTick = 0;
			std::uint32_t LastActivityTick = 0;
			// Wall-clock (ms) counterpart of LastActivityTick for the in-game liveness check. Deliberately
			// NOT frames: the two ends run at wildly different frame rates during synchronization.
			std::uint32_t LastActivityMs = 0;
			// Anything received since the engine barrier completed? Until then there is no baseline for
			// "how long has it been quiet", so the in-game liveness check must not run.
			bool EngineTrafficSeen = false;
			std::uint8_t PendingControlByte = 0;
			bool StartAcknowledged = false;
			bool PeerReady = false;
			bool LocalReadyDirty = false;
			bool EngineTransport = false;
			bool EngineTransportRequested = false;
			bool LocalEngineReadySent = false;
			bool RemoteEngineReadyReceived = false;
			bool GracefulDisconnectReceived = false;
			std::unique_ptr<std::uint8_t[]> NetworkSend;
			std::unique_ptr<std::uint8_t[]> NetworkReceive;
			int NetworkSendOffset = 0;
			int NetworkSendSize = 0;
			int NetworkReceiveSize = 0;
			std::uint32_t RemoteGameplaySequence = 0;
			float RemoteX = 0.0f;
			float RemoteY = 0.0f;
			bool RemotePositionValid = false;
			std::uint32_t LastGameplayPacketTick = 0;
			char StatusText[64] = "No peer connected";
			TransportDiagnostics Diagnostics{};
		};

		bool Fail(const char* stage, int result);
		bool EnsureDiscoverySocket();
		void SendDiscoveryRequest();
		void SendDiscoveryResponse(std::uint8_t* destinationMac);
		void ReceiveDiscoveryPackets();
		void RememberHost(const char* name, const std::uint8_t* mac, std::uint8_t players, std::uint8_t maxPlayers,
			bool isRunning, const SessionSettings& settings);
		void ExpireHosts();
		void CloseDiscoverySocket();
		void UpdatePeerConnection();
		bool EnsureHostListener();
		void ClosePeerSockets();
		void ResetPeer(Peer& peer);
		Peer* FindFreePeerSlot();
		Peer* FindPeerByMac(const std::uint8_t* mac);
		void BeginHandshake(Peer& peer, bool isHost);
		void UpdateHandshake(Peer& peer);
		void PrepareHandshakePacket(Peer& peer, std::uint8_t type);
		bool ValidateHandshakePacket(Peer& peer, std::uint8_t expectedType);
		void FailHandshake(Peer& peer, const char* reason);
		void CompleteHandshake(Peer& peer);
		void UpdateReadySession(Peer& peer);
		void RebuildRoster();
		void ApplyRoster(const std::uint8_t* payload);
		void MarkRosterDirty();
		bool EnsureGameplaySocket();
		void UpdateGameplayState();
		void CloseGameplaySocket();
		void HandlePeerDisconnected(Peer& peer);
		void FlushNetworkSend(Peer& peer);
		void ResetNetworkTransport(Peer& peer);
		void RecordTransportOperation(Peer& peer, TransportOperation operation, int result, int requestedBytes,
			int transferredBytes, int queuedBytes);
		int CreatePdp(const std::uint8_t* localMac, unsigned short localPort, int bufferSize);
		int SendPdp(int socket, const std::uint8_t* destinationMac, unsigned short destinationPort,
			const void* data, int length);
		int ReceivePdp(int socket, std::uint8_t* sourceMac, unsigned short* sourcePort, void* data, unsigned int* length);
		void DeletePdp(int socket);
		int ListenPtp(const std::uint8_t* localMac, unsigned short localPort, int bufferSize);
		int AcceptPtp(int socket, std::uint8_t* peerMac, unsigned short* peerPort);
		int OpenPtp(const std::uint8_t* localMac, unsigned short localPort, const std::uint8_t* peerMac,
			unsigned short peerPort, int bufferSize);
		int ConnectPtp(int socket);
		int SendPtp(int socket, const void* data, int* length);
		int ReceivePtp(int socket, void* data, int* length);
		void FlushPtp(int socket);
		void ClosePtp(int socket);
		int GetLocalMac(std::uint8_t* mac) const;

		State state_ = State::Stopped;
		bool commonModuleLoaded_ = false;
		bool adhocModuleLoaded_ = false;
		bool inetModuleLoaded_ = false;
		bool netInitialized_ = false;
		bool adhocInitialized_ = false;
		bool adhocctlInitialized_ = false;
		bool inetInitialized_ = false;
		bool apctlInitialized_ = false;
		bool infrastructureMode_ = false;
		std::unique_ptr<PspInternetRelayTransport> internetRelay_;
		DiscoveryMode discoveryMode_ = DiscoveryMode::Idle;
		int discoverySocket_ = -1;
		std::uint32_t discoveryTick_ = 0;
		HostEntry hosts_[MaxDiscoveredHosts]{};
		int hostCount_ = 0;
		char hostName_[32] = "Player";
		SessionSettings sessionSettings_{};
		// One listener accepts every guest; one shared gameplay PDP socket serves them all, with
		// inbound datagrams demultiplexed to a peer slot by source MAC.
		int listenSocket_ = -1;
		int gameplaySocket_ = -1;
		Peer peers_[MaxPeers]{};
		char localName_[32] = "Player";
		std::uint8_t localCharacter_ = 0;
		std::uint32_t localFurColor_ = 0;
		std::uint8_t localTeam_ = NoAdhocTeam;
		RosterEntry roster_[MaxRosterEntries]{};
		int rosterCount_ = 0;
		bool startRequested_ = false;
		bool sessionRunning_ = false;
		std::uint32_t gameplaySequence_ = 0;
		float localPlayerX_ = 0.0f;
		float localPlayerY_ = 0.0f;
		bool localPlayerPositionValid_ = false;
		bool localReady_ = false;
		std::unique_ptr<std::uint8_t[]> networkDatagram_;
		char statusText_[64] = "Ad-hoc is off";
	};
}
