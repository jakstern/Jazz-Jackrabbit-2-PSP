#pragma once

#include <cstdint>

namespace nCine
{
	// PDP/PTP transport wire-compatible with PPSSPP's native ad-hoc emulation (the coldbird
	// "proAdhoc"/aemu protocol). Logs in to a proAdhocServer (TCP 27312), learns peers' real
	// IPs from its peer list, then talks peer-to-peer exactly as PPSSPP does: plain UDP for
	// PDP, plain TCP for PTP, every virtual ad-hoc port shifted by PPSSPP's default +10000
	// offset. Virtual ports are kept at the API boundary, so callers are identical to the
	// real sceNetAdhoc path.
	//
	// Direct peer-to-peer only despite the name: there is no relay.
	class PspInternetRelayTransport
	{
	public:
		static constexpr int MaxPeers = 8;
		static constexpr int WouldBlock = static_cast<int>(0x80410709u);
		// PPSSPP default g_Config.iPortOffset; both endpoints must agree (0 here if PPSSPP uses 0).
		static constexpr unsigned short PortOffset = 10000;

		enum class State {
			Stopped,
			Connecting,
			Connected,
			Failed
		};

		struct Peer {
			char Name[32]{};
			std::uint8_t Mac[6]{};
			std::uint32_t Address = 0;
		};

		PspInternetRelayTransport() = default;
		~PspInternetRelayTransport();

		PspInternetRelayTransport(const PspInternetRelayTransport&) = delete;
		PspInternetRelayTransport& operator=(const PspInternetRelayTransport&) = delete;

		bool Start(const char* serverAddress);
		void Update();
		void Stop();

		State GetState() const { return state_; }
		const char* GetStatusText() const { return statusText_; }
		const std::uint8_t* GetLocalMac() const { return localMac_; }
		int GetPeerCount() const { return peerCount_; }
		const Peer& GetPeer(int index) const { return peers_[index]; }

		int PdpCreate(const std::uint8_t* localMac, unsigned short localPort);
		int PdpSend(int handle, const std::uint8_t* destinationMac, unsigned short destinationPort,
			const void* data, int length);
		int PdpReceive(int handle, std::uint8_t* sourceMac, unsigned short* sourcePort, void* data, unsigned int* length);
		void PdpDelete(int handle);
		int PtpListen(const std::uint8_t* localMac, unsigned short localPort);
		int PtpAccept(int handle, std::uint8_t* peerMac, unsigned short* peerPort);
		int PtpOpen(const std::uint8_t* localMac, unsigned short localPort,
			const std::uint8_t* peerMac, unsigned short peerPort);
		int PtpConnect(int handle);
		int PtpSend(int handle, const void* data, int* length);
		int PtpReceive(int handle, void* data, int* length);
		void PtpClose(int handle);

		// Maps a PSP virtual ad-hoc port onto the real UDP/TCP port PPSSPP uses.
		static unsigned short RealPort(unsigned short virtualPort);
		static unsigned short VirtualPort(unsigned short realPort);

	private:
		bool ResolveServer(const char* serverAddress);
		bool ConnectControlServer();
		bool SendInitialPackets();
		bool SendAll(const void* data, int length);
		bool Fail(const char* stage, int result);
		bool PreserveDirectSessionAfterControlLoss(const char* stage, int result);
		void ReceiveControlPackets();
		void ParseControlPackets();
		void AddPeer(const char* name, const std::uint8_t* mac, std::uint32_t address);
		void RemovePeer(std::uint32_t address);
		const Peer* FindPeerByMac(const std::uint8_t* mac) const;
		const Peer* FindPeerByAddress(std::uint32_t address) const;

		struct PdpSession {
			int Socket = -1;
			unsigned short Port = 0;
		};
		struct PtpSession {
			int Socket = -1;
			bool Connected = false;
			std::uint32_t PeerAddress = 0;
			unsigned short PeerPort = 0;
		};
		struct PtpListener {
			int Socket = -1;
			std::uint8_t Mac[6]{};
			unsigned short Port = 0;
		};

		State state_ = State::Stopped;
		int controlSocket_ = -1;
		bool resolverInitialized_ = false;
		std::uint32_t serverAddress_ = 0;
		std::uint8_t localMac_[6]{};
		std::uint8_t receiveBuffer_[1024]{};
		int receiveSize_ = 0;
		std::uint32_t updateTick_ = 0;
		std::uint32_t lastUnknownDropTick_ = 0;
		std::uint32_t lastSendLogTick_ = 0;
		Peer peers_[MaxPeers]{};
		int peerCount_ = 0;
		PdpSession pdpSessions_[2]{};
		PtpSession ptpSession_{};
		PtpListener ptpListener_{};
		char statusText_[96] = "Ad-hoc server is off";
	};
}
