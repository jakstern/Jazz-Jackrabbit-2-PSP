#include "PspInternetRelayTransport.h"

#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <psputility_sysparam.h>
#include <pspwlan.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

namespace nCine
{
	namespace
	{
		// coldbird / PPSSPP proAdhocServer control protocol.
		constexpr unsigned short ControlPort = 27312;
		constexpr char ProductId[] = "JAZZ2PSP1";
		constexpr char GroupName[] = "JAZZ2MP";
		constexpr std::uint8_t OpcodePing = 0;
		constexpr std::uint8_t OpcodeLogin = 1;
		constexpr std::uint8_t OpcodeConnect = 2;
		constexpr std::uint8_t OpcodeDisconnect = 3;
		constexpr std::uint8_t OpcodeConnectBssid = 6;
		// PPSSPP pings every 2 s (PSP_ADHOCCTL_PING_TIMEOUT) and the server drops silent users after
		// 15 s. Update() runs ~60x/s, so 120 ticks is ~2 s.
		constexpr std::uint32_t PingInterval = 120;
		constexpr int PdpHandleBase = 100;
		constexpr int PtpListenHandle = 200;
		constexpr int PtpHandle = 300;
		constexpr int MaxDatagramSize = 20 * 1024;
		// Supported by the PSP kernel but missing from older PSPSDK headers.
		constexpr int PspSocketNonBlocking = 0x1009;
		// PSP inet uses its own flag values; host libc MSG_DONTWAIT does not survive PPSSPP's HLE translation.
		constexpr int PspMessageDontWait = 0x80;
		constexpr short PspPollRead = 0x0001;

#pragma pack(push, 1)
		struct LoginPacket {
			std::uint8_t Opcode;
			std::uint8_t Mac[6];
			char Name[128];
			char Product[9];
		};
		struct JoinGroupPacket {
			std::uint8_t Opcode;
			char Group[8];
		};
		struct PeerConnectedPacket {
			std::uint8_t Opcode;
			char Name[128];
			std::uint8_t Mac[6];
			std::uint32_t Address;
		};
		struct PeerDisconnectedPacket {
			std::uint8_t Opcode;
			std::uint32_t Address;
		};
		struct BssidPacket {
			std::uint8_t Opcode;
			std::uint8_t Mac[6];
		};
#pragma pack(pop)

		static_assert(sizeof(LoginPacket) == 144, "adhocctl login packet layout changed");
		static_assert(sizeof(JoinGroupPacket) == 9, "adhocctl group packet layout changed");
		static_assert(sizeof(PeerConnectedPacket) == 139, "adhocctl peer packet layout changed");
		static_assert(sizeof(PeerDisconnectedPacket) == 5, "adhocctl disconnect packet layout changed");
		static_assert(sizeof(BssidPacket) == 7, "adhocctl bssid packet layout changed");

#if defined(JAZZ2_PSP_DEBUG)
		void RelayLog(const char* format, ...)
		{
			// Separate from the main debug stream: two handles appending to debug.log can overwrite
			// each other on real PSP.
			FILE* file = std::fopen("aemu.log", "a");
			if (file == nullptr) return;
			std::fputs("aemu: ", file);
			va_list args;
			va_start(args, format);
			std::vfprintf(file, format, args);
			va_end(args);
			std::fputc('\n', file);
			std::fclose(file);
		}
#else
		// Compiled out unless -DPSP_DEBUG=ON, see SessionLog in PspAdhoc.cpp
		inline void RelayLog(const char*, ...) {}
#endif

		bool ParseIpv4Address(const char* text, std::uint32_t& address)
		{
			unsigned int a = 0, b = 0, c = 0, d = 0;
			char trailing = '\0';
			if (text == nullptr || std::sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &trailing) != 4 ||
				a > 255 || b > 255 || c > 255 || d > 255) return false;
			address = htonl((a << 24) | (b << 16) | (c << 8) | d);
			return true;
		}
	}

	// Mirrors PPSSPP's offset_port_simple(), including the wrap guard that avoids binding port 0 (random).
	unsigned short PspInternetRelayTransport::RealPort(unsigned short virtualPort)
	{
		const unsigned short real = static_cast<unsigned short>(virtualPort + PortOffset);
		if (real == 0 && virtualPort != 0) return 65535;
		return real;
	}

	unsigned short PspInternetRelayTransport::VirtualPort(unsigned short realPort)
	{
		return static_cast<unsigned short>(realPort - PortOffset);
	}

	PspInternetRelayTransport::~PspInternetRelayTransport()
	{
		Stop();
	}

	bool PspInternetRelayTransport::Start(const char* serverAddress)
	{
		Stop();
		RelayLog("start server=%s", (serverAddress != nullptr && serverAddress[0] != '\0') ? serverAddress : "192.168.0.0");
		if (sceWlanGetSwitchState() == 0) return Fail("WLAN switch is off", 0);
		if (sceWlanGetEtherAddr(localMac_) < 0) return Fail("local MAC", -1);
		state_ = State::Connecting;
		std::strcpy(statusText_, "Resolving ad-hoc server...");
		if (!ResolveServer(serverAddress)) return false;
		std::strcpy(statusText_, "Connecting to ad-hoc server...");
		if (!ConnectControlServer()) return false;
		if (!SendInitialPackets()) return false;
		std::strcpy(statusText_, "Joining internet party group...");
		return true;
	}

	void PspInternetRelayTransport::Update()
	{
		if (state_ != State::Connecting && state_ != State::Connected) return;
		if (controlSocket_ < 0) return;
		ReceiveControlPackets();
		if (state_ == State::Failed) return;
		if (controlSocket_ < 0) return;
		if (++updateTick_ % PingInterval == 0) {
			const std::uint8_t ping = OpcodePing;
			if (SendAll(&ping, sizeof(ping))) {
				RelayLog("control ping tick=%u peers=%d", updateTick_, peerCount_);
			} else {
				const int error = sceNetInetGetErrno();
				if (!PreserveDirectSessionAfterControlLoss("adhoc heartbeat", error)) Fail("adhoc heartbeat", error);
			}
		}
	}

	void PspInternetRelayTransport::Stop()
	{
		if (controlSocket_ >= 0 || ptpSession_.Socket >= 0 || ptpListener_.Socket >= 0) {
			RelayLog("stop state=%d control=%d ptp=%d listener=%d", static_cast<int>(state_), controlSocket_,
				ptpSession_.Socket, ptpListener_.Socket);
		}
		for (auto& session : pdpSessions_) {
			if (session.Socket >= 0) sceNetInetClose(session.Socket);
			session = {};
		}
		if (ptpSession_.Socket >= 0) sceNetInetClose(ptpSession_.Socket);
		ptpSession_ = {};
		if (ptpListener_.Socket >= 0) sceNetInetClose(ptpListener_.Socket);
		ptpListener_ = {};
		if (controlSocket_ >= 0) {
			sceNetInetClose(controlSocket_);
			controlSocket_ = -1;
		}
		if (resolverInitialized_) {
			sceNetResolverTerm();
			resolverInitialized_ = false;
		}
		serverAddress_ = 0;
		receiveSize_ = 0;
		updateTick_ = 0;
		peerCount_ = 0;
		if (state_ != State::Failed) {
			state_ = State::Stopped;
			std::strcpy(statusText_, "Ad-hoc server is off");
		}
	}

	bool PspInternetRelayTransport::ResolveServer(const char* serverAddress)
	{
		const char* addressText = (serverAddress != nullptr && serverAddress[0] != '\0') ? serverAddress : "192.168.0.0";
		if (ParseIpv4Address(addressText, serverAddress_)) {
			RelayLog("using numeric address=%s", addressText);
			return true;
		}
		RelayLog("resolving hostname=%s", addressText);
		int result = sceNetResolverInit();
		if (result < 0) return Fail("resolver init", result);
		resolverInitialized_ = true;
		alignas(64) std::uint8_t resolverBuffer[1024]{};
		int resolverId = -1;
		result = sceNetResolverCreate(&resolverId, resolverBuffer, sizeof(resolverBuffer));
		if (result < 0) return Fail("resolver create", result);
		in_addr address{};
		result = sceNetResolverStartNtoA(resolverId, addressText, &address, 500 * 1000, 2);
		sceNetResolverDelete(resolverId);
		if (result < 0) return Fail("server lookup", result);
		serverAddress_ = address.s_addr;
		RelayLog("resolved address=%08X", static_cast<unsigned>(serverAddress_));
		return true;
	}

	bool PspInternetRelayTransport::ConnectControlServer()
	{
		controlSocket_ = sceNetInetSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (controlSocket_ < 0) return Fail("control socket", sceNetInetGetErrno());
		int enabled = 1;
		sceNetInetSetsockopt(controlSocket_, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
		sceNetInetSetsockopt(controlSocket_, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
		sockaddr_in address{};
		address.sin_len = sizeof(address);
		address.sin_family = AF_INET;
		address.sin_port = htons(ControlPort);
		address.sin_addr.s_addr = serverAddress_;
		const int result = sceNetInetConnect(controlSocket_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
		if (result < 0) return Fail("server connect", sceNetInetGetErrno());
		RelayLog("control connected port=%u", ControlPort);
		return true;
	}

	bool PspInternetRelayTransport::SendInitialPackets()
	{
		LoginPacket login{};
		login.Opcode = OpcodeLogin;
		std::memcpy(login.Mac, localMac_, sizeof(localMac_));
		if (sceUtilityGetSystemParamString(PSP_SYSTEMPARAM_ID_STRING_NICKNAME, login.Name, sizeof(login.Name)) < 0 || login.Name[0] == '\0') {
			std::strcpy(login.Name, "Player");
		}
		std::memcpy(login.Product, ProductId, sizeof(login.Product));
		if (!SendAll(&login, sizeof(login))) return Fail("adhoc login", sceNetInetGetErrno());

		JoinGroupPacket group{};
		group.Opcode = OpcodeConnect;
		std::memcpy(group.Group, GroupName, sizeof(group.Group));
		if (!SendAll(&group, sizeof(group))) return Fail("adhoc group join", sceNetInetGetErrno());
		return true;
	}

	bool PspInternetRelayTransport::SendAll(const void* data, int length)
	{
		const auto* bytes = static_cast<const std::uint8_t*>(data);
		int offset = 0;
		while (offset < length) {
			const int result = static_cast<int>(sceNetInetSend(controlSocket_, bytes + offset, length - offset, 0));
			if (result <= 0) return false;
			offset += result;
		}
		return true;
	}

	bool PspInternetRelayTransport::Fail(const char* stage, int result)
	{
		RelayLog("failure stage=%s result=%08X", stage, static_cast<unsigned>(result));
		state_ = State::Failed;
		if (result == 0) std::snprintf(statusText_, sizeof(statusText_), "%s", stage);
		else std::snprintf(statusText_, sizeof(statusText_), "%s failed: %08X", stage, static_cast<unsigned>(result));
		Stop();
		return false;
	}

	bool PspInternetRelayTransport::PreserveDirectSessionAfterControlLoss(const char* stage, int result)
	{
		// A long synchronous level load can stall the main-loop-driven ping past the server's 15 s timeout.
		// If a PTP stream is already established, keep it: the reliable channel survives control loss, but
		// new discovery/joins do not, and PPSSPP drops us from its friend list so its UDP/PDP path to us
		// may go quiet until the control session is restarted.
		if (ptpSession_.Socket < 0 || !ptpSession_.Connected) return false;
		RelayLog("control lost stage=%s result=%08X, preserving direct session", stage, static_cast<unsigned>(result));
		if (controlSocket_ >= 0) {
			sceNetInetClose(controlSocket_);
			controlSocket_ = -1;
		}
		state_ = State::Connected;
		std::strcpy(statusText_, "Direct session active");
		return true;
	}

	void PspInternetRelayTransport::ReceiveControlPackets()
	{
		while (receiveSize_ < static_cast<int>(sizeof(receiveBuffer_))) {
			const int result = static_cast<int>(sceNetInetRecv(controlSocket_, receiveBuffer_ + receiveSize_,
				sizeof(receiveBuffer_) - receiveSize_, PspMessageDontWait));
			if (result > 0) {
				receiveSize_ += result;
				continue;
			}
			if (result == 0) {
				RelayLog("control closed by server tick=%u peers=%d pending=%d", updateTick_, peerCount_, receiveSize_);
				if (!PreserveDirectSessionAfterControlLoss("adhoc server closed connection", 0))
					Fail("adhoc server closed connection", 0);
				return;
			}
			const int error = sceNetInetGetErrno();
			if (error != EAGAIN && error != EWOULDBLOCK) {
				RelayLog("control recv error=%d tick=%u", error, updateTick_);
				if (!PreserveDirectSessionAfterControlLoss("adhoc receive", error))
					Fail("adhoc receive", error);
			}
			break;
		}
		ParseControlPackets();
	}

	void PspInternetRelayTransport::ParseControlPackets()
	{
		while (receiveSize_ > 0) {
			int packetSize = 0;
			switch (receiveBuffer_[0]) {
				case OpcodeConnect: packetSize = sizeof(PeerConnectedPacket); break;
				case OpcodeDisconnect: packetSize = sizeof(PeerDisconnectedPacket); break;
				case OpcodeConnectBssid: packetSize = sizeof(BssidPacket); break;
				default:
					Fail("unsupported adhoc packet", receiveBuffer_[0]);
					return;
			}
			if (receiveSize_ < packetSize) return;
			if (receiveBuffer_[0] == OpcodeConnect) {
				PeerConnectedPacket packet{};
				std::memcpy(&packet, receiveBuffer_, sizeof(packet));
				packet.Name[sizeof(packet.Name) - 1] = '\0';
				AddPeer(packet.Name, packet.Mac, packet.Address);
			} else if (receiveBuffer_[0] == OpcodeDisconnect) {
				PeerDisconnectedPacket packet{};
				std::memcpy(&packet, receiveBuffer_, sizeof(packet));
				RemovePeer(packet.Address);
			} else {
				state_ = State::Connected;
				std::strcpy(statusText_, "Ad-hoc server connected");
			}
			const int remaining = receiveSize_ - packetSize;
			if (remaining > 0) std::memmove(receiveBuffer_, receiveBuffer_ + packetSize, remaining);
			receiveSize_ = remaining;
		}
	}

	void PspInternetRelayTransport::AddPeer(const char* name, const std::uint8_t* mac, std::uint32_t address)
	{
		for (int i = 0; i < peerCount_; ++i) {
			if (peers_[i].Address != address) continue;
			std::snprintf(peers_[i].Name, sizeof(peers_[i].Name), "%s", name);
			std::memcpy(peers_[i].Mac, mac, sizeof(peers_[i].Mac));
			return;
		}
		if (peerCount_ >= MaxPeers) return;
		auto& peer = peers_[peerCount_++];
		std::snprintf(peer.Name, sizeof(peer.Name), "%s", name);
		std::memcpy(peer.Mac, mac, sizeof(peer.Mac));
		peer.Address = address;
		const std::uint32_t host = ntohl(address);
		RelayLog("peer joined name=%s mac=%02X:%02X:%02X:%02X:%02X:%02X addr=%u.%u.%u.%u", peer.Name,
			peer.Mac[0], peer.Mac[1], peer.Mac[2], peer.Mac[3], peer.Mac[4], peer.Mac[5],
			(host >> 24) & 0xff, (host >> 16) & 0xff, (host >> 8) & 0xff, host & 0xff);
	}

	void PspInternetRelayTransport::RemovePeer(std::uint32_t address)
	{
		for (int i = 0; i < peerCount_; ++i) {
			if (peers_[i].Address != address) continue;
			if (ptpSession_.Socket >= 0 && ptpSession_.Connected && ptpSession_.PeerAddress == address) {
				RelayLog("control peer left address=%08X, retaining active direct mapping", static_cast<unsigned>(address));
				return;
			}
			peers_[i] = peers_[peerCount_ - 1];
			--peerCount_;
			RelayLog("peer left address=%08X", static_cast<unsigned>(address));
			return;
		}
	}

	const PspInternetRelayTransport::Peer* PspInternetRelayTransport::FindPeerByMac(const std::uint8_t* mac) const
	{
		for (int i = 0; i < peerCount_; ++i) {
			if (std::memcmp(peers_[i].Mac, mac, sizeof(peers_[i].Mac)) == 0) return &peers_[i];
		}
		return nullptr;
	}

	const PspInternetRelayTransport::Peer* PspInternetRelayTransport::FindPeerByAddress(std::uint32_t address) const
	{
		for (int i = 0; i < peerCount_; ++i) {
			if (peers_[i].Address == address) return &peers_[i];
		}
		return nullptr;
	}

	int PspInternetRelayTransport::PdpCreate(const std::uint8_t* localMac, unsigned short localPort)
	{
		(void)localMac;
		for (int i = 0; i < static_cast<int>(sizeof(pdpSessions_) / sizeof(pdpSessions_[0])); ++i) {
			auto& session = pdpSessions_[i];
			if (session.Socket >= 0) continue;
			const int socket = sceNetInetSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (socket < 0) return -1;
			int enabled = 1;
			sceNetInetSetsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
			sceNetInetSetsockopt(socket, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled));
			sockaddr_in address{};
			address.sin_len = sizeof(address);
			address.sin_family = AF_INET;
			address.sin_port = htons(RealPort(localPort));
			address.sin_addr.s_addr = htonl(INADDR_ANY);
			if (sceNetInetBind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
				sceNetInetClose(socket);
				return -1;
			}
			session.Socket = socket;
			session.Port = localPort;
			RelayLog("PDP open vport=%u realport=%u handle=%d", localPort, RealPort(localPort), PdpHandleBase + i);
			return PdpHandleBase + i;
		}
		return -1;
	}

	int PspInternetRelayTransport::PdpSend(int handle, const std::uint8_t* destinationMac,
		unsigned short destinationPort, const void* data, int length)
	{
		const int index = handle - PdpHandleBase;
		if (index < 0 || index >= static_cast<int>(sizeof(pdpSessions_) / sizeof(pdpSessions_[0])) ||
			pdpSessions_[index].Socket < 0 || length < 0 || length > MaxDatagramSize) return -1;
		auto sendTo = [&](const Peer& peer) {
			sockaddr_in address{};
			address.sin_len = sizeof(address);
			address.sin_family = AF_INET;
			address.sin_port = htons(RealPort(destinationPort));
			address.sin_addr.s_addr = peer.Address;
			const int result = static_cast<int>(sceNetInetSendto(pdpSessions_[index].Socket, data, length,
				PspMessageDontWait, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
			const int error = (result < 0) ? sceNetInetGetErrno() : 0;
			// Proves whether the datagram left the socket and to which address; ~once/second.
			if (updateTick_ - lastSendLogTick_ >= 60) {
				lastSendLogTick_ = updateTick_;
				const std::uint32_t host = ntohl(peer.Address);
				RelayLog("PDP send sock=%d -> %u.%u.%u.%u:%u len=%d result=%d errno=%d", pdpSessions_[index].Socket,
					(host >> 24) & 0xff, (host >> 16) & 0xff, (host >> 8) & 0xff, host & 0xff,
					RealPort(destinationPort), length, result, error);
			}
			if (result == length) return 0;
			return (error == EAGAIN || error == EWOULDBLOCK) ? WouldBlock : -1;
		};
		const std::uint8_t broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
		if (std::memcmp(destinationMac, broadcast, sizeof(broadcast)) == 0) {
			// PPSSPP emulates ad-hoc broadcast by unicasting to every known peer.
			for (int i = 0; i < peerCount_; ++i) {
				const int result = sendTo(peers_[i]);
				if (result < 0 && result != WouldBlock) return result;
			}
			return 0;
		}
		const Peer* peer = FindPeerByMac(destinationMac);
		return peer != nullptr ? sendTo(*peer) : -1;
	}

	int PspInternetRelayTransport::PdpReceive(int handle, std::uint8_t* sourceMac, unsigned short* sourcePort,
		void* data, unsigned int* length)
	{
		const int index = handle - PdpHandleBase;
		if (index < 0 || index >= static_cast<int>(sizeof(pdpSessions_) / sizeof(pdpSessions_[0])) ||
			pdpSessions_[index].Socket < 0 || length == nullptr) return -1;
		auto& session = pdpSessions_[index];
		sockaddr_in address{};
		unsigned int addressLength = sizeof(address);
		const int result = static_cast<int>(sceNetInetRecvfrom(session.Socket, data, *length, PspMessageDontWait,
			reinterpret_cast<sockaddr*>(&address), &addressLength));
		if (result < 0) {
			const int error = sceNetInetGetErrno();
			return (error == EAGAIN || error == EWOULDBLOCK) ? WouldBlock : -1;
		}
		// Like PPSSPP, identify the sender by IP from the control-server peer list and drop datagrams
		// from peers we have not been told about.
		const Peer* peer = FindPeerByAddress(address.sin_addr.s_addr);
		if (peer == nullptr) {
			// When a peer that is genuinely sending "can't be seen", this is usually where it dies:
			// the server evicted the sender from our list. Rate-limited to ~once/second.
			if (updateTick_ - lastUnknownDropTick_ >= 60) {
				lastUnknownDropTick_ = updateTick_;
				RelayLog("PDP recv unknown addr=%08X port=%u dropped peers=%d",
					static_cast<unsigned>(address.sin_addr.s_addr), ntohs(address.sin_port), peerCount_);
			}
			return WouldBlock;
		}
		if (sourceMac != nullptr) std::memcpy(sourceMac, peer->Mac, sizeof(peer->Mac));
		if (sourcePort != nullptr) *sourcePort = VirtualPort(ntohs(address.sin_port));
		*length = result;
		return 0;
	}

	void PspInternetRelayTransport::PdpDelete(int handle)
	{
		const int index = handle - PdpHandleBase;
		if (index < 0 || index >= static_cast<int>(sizeof(pdpSessions_) / sizeof(pdpSessions_[0]))) return;
		auto& session = pdpSessions_[index];
		if (session.Socket >= 0) sceNetInetClose(session.Socket);
		session = {};
	}

	int PspInternetRelayTransport::PtpListen(const std::uint8_t* localMac, unsigned short localPort)
	{
		if (ptpListener_.Socket >= 0) return PtpListenHandle;
		const int socket = sceNetInetSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (socket < 0) return -1;
		int enabled = 1;
		sceNetInetSetsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
		sceNetInetSetsockopt(socket, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled));
		sockaddr_in address{};
		address.sin_len = sizeof(address);
		address.sin_family = AF_INET;
		address.sin_port = htons(RealPort(localPort));
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		if (sceNetInetBind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
			sceNetInetListen(socket, 4) < 0) {
			sceNetInetClose(socket);
			return -1;
		}
		sceNetInetSetsockopt(socket, SOL_SOCKET, PspSocketNonBlocking, &enabled, sizeof(enabled));
		ptpListener_.Socket = socket;
		std::memcpy(ptpListener_.Mac, localMac, sizeof(ptpListener_.Mac));
		ptpListener_.Port = localPort;
		RelayLog("PTP listen vport=%u realport=%u", localPort, RealPort(localPort));
		return PtpListenHandle;
	}

	int PspInternetRelayTransport::PtpAccept(int handle, std::uint8_t* peerMac, unsigned short* peerPort)
	{
		if (handle != PtpListenHandle || ptpListener_.Socket < 0 || ptpSession_.Socket >= 0) return WouldBlock;
		sockaddr_in address{};
		unsigned int addressLength = sizeof(address);
		const int socket = sceNetInetAccept(ptpListener_.Socket, reinterpret_cast<sockaddr*>(&address), &addressLength);
		if (socket < 0) {
			const int error = sceNetInetGetErrno();
			return (error == EAGAIN || error == EWOULDBLOCK) ? WouldBlock : -1;
		}
		// PPSSPP rejects TCP connections whose source IP is not a known friend.
		const Peer* peer = FindPeerByAddress(address.sin_addr.s_addr);
		if (peer == nullptr) {
			sceNetInetClose(socket);
			return WouldBlock;
		}
		int enabled = 1;
		sceNetInetSetsockopt(socket, SOL_SOCKET, PspSocketNonBlocking, &enabled, sizeof(enabled));
		sceNetInetSetsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
		sceNetInetSetsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
		ptpSession_.Socket = socket;
		ptpSession_.Connected = true;
		ptpSession_.PeerAddress = address.sin_addr.s_addr;
		ptpSession_.PeerPort = VirtualPort(ntohs(address.sin_port));
		if (peerMac != nullptr) std::memcpy(peerMac, peer->Mac, sizeof(peer->Mac));
		if (peerPort != nullptr) *peerPort = ptpSession_.PeerPort;
		RelayLog("PTP accepted direct peer=%s", peer->Name);
		return PtpHandle;
	}

	int PspInternetRelayTransport::PtpOpen(const std::uint8_t* localMac, unsigned short localPort,
		const std::uint8_t* peerMac, unsigned short peerPort)
	{
		(void)localMac;
		if (ptpSession_.Socket >= 0) return -1;
		const Peer* peer = FindPeerByMac(peerMac);
		if (peer == nullptr) return -1;
		const int socket = sceNetInetSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (socket < 0) return -1;
		int enabled = 1;
		sceNetInetSetsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
		sceNetInetSetsockopt(socket, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled));
		sceNetInetSetsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
		sceNetInetSetsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
		sceNetInetSetsockopt(socket, SOL_SOCKET, PspSocketNonBlocking, &enabled, sizeof(enabled));
		sockaddr_in local{};
		local.sin_len = sizeof(local);
		local.sin_family = AF_INET;
		local.sin_port = htons(RealPort(localPort));
		local.sin_addr.s_addr = htonl(INADDR_ANY);
		if (sceNetInetBind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
			sceNetInetClose(socket);
			return -1;
		}
		ptpSession_.Socket = socket;
		ptpSession_.PeerAddress = peer->Address;
		ptpSession_.PeerPort = peerPort;
		RelayLog("PTP connecting peer=%02X:%02X:%02X:%02X:%02X:%02X vport=%u", peerMac[0], peerMac[1], peerMac[2],
			peerMac[3], peerMac[4], peerMac[5], peerPort);
		return PtpHandle;
	}

	int PspInternetRelayTransport::PtpConnect(int handle)
	{
		if (handle != PtpHandle || ptpSession_.Socket < 0) return -1;
		if (ptpSession_.Connected) return 0;
		auto finishIfConnected = [&]() {
			sockaddr_in peer{};
			unsigned int peerLength = sizeof(peer);
			if (sceNetInetGetpeername(ptpSession_.Socket, reinterpret_cast<sockaddr*>(&peer), &peerLength) != 0 ||
				peer.sin_addr.s_addr != ptpSession_.PeerAddress) return false;
			ptpSession_.Connected = true;
			RelayLog("PTP connected direct");
			return true;
		};
		// Real PSP firmware does not consistently report EISCONN when connect() is polled after
		// EINPROGRESS; the established peer name is authoritative instead.
		if (finishIfConnected()) return 0;
		sockaddr_in address{};
		address.sin_len = sizeof(address);
		address.sin_family = AF_INET;
		address.sin_port = htons(RealPort(ptpSession_.PeerPort));
		address.sin_addr.s_addr = ptpSession_.PeerAddress;
		const int result = sceNetInetConnect(ptpSession_.Socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
		const int error = sceNetInetGetErrno();
		if (result == 0 || error == EISCONN || finishIfConnected()) {
			ptpSession_.Connected = true;
			if (result == 0 || error == EISCONN) RelayLog("PTP connected direct");
			return 0;
		}
		if (error == EINPROGRESS || error == EALREADY || error == EAGAIN || error == EWOULDBLOCK) return WouldBlock;
		return -1;
	}

	int PspInternetRelayTransport::PtpSend(int handle, const void* data, int* length)
	{
		if (handle != PtpHandle || ptpSession_.Socket < 0 || !ptpSession_.Connected || length == nullptr || *length < 0) return -1;
		const int result = static_cast<int>(sceNetInetSend(ptpSession_.Socket, data, *length, PspMessageDontWait));
		if (result >= 0) {
			*length = result;
			return 0;
		}
		const int error = sceNetInetGetErrno();
		if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) return WouldBlock;
		RelayLog("PTP send failed error=%d", error);
		return -1;
	}

	int PspInternetRelayTransport::PtpReceive(int handle, void* data, int* length)
	{
		if (handle != PtpHandle || ptpSession_.Socket < 0 || !ptpSession_.Connected || length == nullptr || *length < 0) return -1;
		// PPSSPP can ignore both SO_NBIO and PSP's per-call nonblocking flag on accept()ed sockets. A
		// zero-time poll keeps recv out of the emulator's blocking path, EOF/error handling intact.
		SceNetInetPollfd descriptor{};
		descriptor.fd = ptpSession_.Socket;
		descriptor.events = PspPollRead;
		const int pollResult = sceNetInetPoll(&descriptor, 1, 0);
		if (pollResult == 0) return WouldBlock;
		if (pollResult < 0) {
			RelayLog("PTP poll failed error=%d", sceNetInetGetErrno());
			return -1;
		}
		const int result = static_cast<int>(sceNetInetRecv(ptpSession_.Socket, data, *length, PspMessageDontWait));
		if (result > 0) {
			*length = result;
			return 0;
		}
		if (result == 0) {
			RelayLog("PTP peer closed stream");
			return -1;
		}
		const int error = sceNetInetGetErrno();
		if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) return WouldBlock;
		RelayLog("PTP receive failed error=%d", error);
		return -1;
	}

	void PspInternetRelayTransport::PtpClose(int handle)
	{
		if (handle == PtpListenHandle) {
			RelayLog("PTP close listener socket=%d", ptpListener_.Socket);
			if (ptpListener_.Socket >= 0) sceNetInetClose(ptpListener_.Socket);
			ptpListener_ = {};
		} else if (handle == PtpHandle) {
			RelayLog("PTP close session socket=%d connected=%d", ptpSession_.Socket, ptpSession_.Connected ? 1 : 0);
			if (ptpSession_.Socket >= 0) sceNetInetClose(ptpSession_.Socket);
			ptpSession_ = {};
		}
	}
}
