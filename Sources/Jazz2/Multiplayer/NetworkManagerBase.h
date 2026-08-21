#pragma once

#if defined(WITH_MULTIPLAYER) || defined(DOXYGEN_GENERATING_OUTPUT)

#include "ConnectionResult.h"
#include "Peer.h"
#include "Reason.h"

#include <Base/IDisposable.h>
#include <Containers/Array.h>
#include <Containers/Function.h>
#include <Containers/String.h>
#include <Containers/StringView.h>

#include <memory>

using namespace Death::Containers;

namespace nCine { class PspAdhoc; }

namespace Jazz2::Multiplayer
{
	class INetworkHandler;

	enum class NetworkChannel : std::uint8_t { Main, UnreliableUpdates, Count };
	enum class NetworkState { None, Listening, Connecting, Connected, Local };

	struct AllPeersT { struct Init {}; constexpr explicit AllPeersT(Init) {} };
	struct LocalPeerT { struct Init {}; constexpr explicit LocalPeerT(Init) {} };
	constexpr AllPeersT AllPeers{AllPeersT::Init{}};
	constexpr LocalPeerT LocalPeer{LocalPeerT::Init{}};

	/** PSP-only bridge between the engine multiplayer protocol and the ad-hoc peers.
		The host exposes each connected guest as a distinct Peer; guest slot g maps to
		Peer(1 + g), so slot 0 is Peer::Remote() and a two-player session is unchanged.
		A guest only ever has slot 0 (its link to the host). */
	class NetworkManagerBase : public Death::IDisposable
	{
	public:
		/** Guest slots the host can hold (must not exceed nCine::PspAdhoc::MaxGuests). */
		static constexpr std::uint32_t MaxAdhocGuests = 3;
		/** Local player plus the guest slots. Used as the default ServerConfiguration::MaxPlayerCount. */
		static constexpr std::uint32_t MaxPeerCount = MaxAdhocGuests + 1;

		NetworkManagerBase();
		~NetworkManagerBase();
		NetworkManagerBase(const NetworkManagerBase&) = delete;
		NetworkManagerBase& operator=(const NetworkManagerBase&) = delete;

		virtual void CreateClient(INetworkHandler* handler, StringView endpoints, std::uint16_t defaultPort, std::uint32_t clientData);
		virtual bool CreateServer(INetworkHandler* handler, std::uint16_t port);
		virtual void Dispose();

		void AttachAdhoc(nCine::PspAdhoc* adhoc, bool isHost, INetworkHandler* handler, std::uint32_t clientData = 0);
		void DetachAdhocToLocalSession();
		void Update();

		NetworkState GetState() const { return _state; }
		/**
			Guests present in the ad-hoc session, whether or not they have reached this level yet.

			This is the durable roster: the ad-hoc layer outlives every level load, and it is the ONLY place that
			knows about a guest which is still loading. Such a guest has not completed the engine-transport
			barrier, so it is not announced and has no PeerDescriptor - it is invisible to GetPeers() precisely
			while it is the thing everyone is waiting for.
		*/
		std::uint32_t GetConnectedGuestCount() const;
		std::uint32_t GetRoundTripTimeMs() const;
		std::uint32_t GetRoundTripTimeMs(const Peer& peer) const;
		Array<String> GetServerEndpoints() const;
		std::uint16_t GetServerPort() const;

		void SendTo(const Peer& peer, NetworkChannel channel, std::uint8_t packetType, ArrayView<const std::uint8_t> data);
		void SendTo(Function<bool(const Peer&)>&& predicate, NetworkChannel channel, std::uint8_t packetType, ArrayView<const std::uint8_t> data);
		void SendTo(AllPeersT, NetworkChannel channel, std::uint8_t packetType, ArrayView<const std::uint8_t> data);
		void Kick(const Peer& peer, Reason reason);

		String AddressToString(const Peer& peer) const;
		static bool IsAddressValid(StringView address);
		static bool IsDomainValid(StringView domain);
		static bool TrySplitAddressAndPort(StringView input, StringView& address, std::uint16_t& port);
		static const char* ReasonToString(Reason reason);

	protected:
		void CreateLocalSession(INetworkHandler* handler);
		virtual ConnectionResult OnPeerConnected(const Peer& peer, std::uint32_t clientData);
		virtual void OnPeerDisconnected(const Peer& peer, Reason reason);

	private:
		// Backlog for reliable frames the transport refused. The Main channel is a PTP (TCP-like) stream, so a
		// frame the transport accepted is delivered, in order, or the peer is dropped outright - there is no
		// partial loss to acknowledge. The one real failure is a synchronous refusal (send buffer full), which
		// the transport reports by returning false. That return value IS the acknowledgement, and it is instant
		// and local: a refused frame never reached the wire, so it cannot arrive twice and needs no dedup. All
		// that was missing is holding on to it. Allocated on the first refusal, so a healthy session pays nothing.
		static constexpr std::uint32_t PendingSendCapacity = 8 * 1024;
		static constexpr std::uint32_t PendingSendHeaderSize = 3; // packetType, length low, length high

		struct PendingSends {
			std::unique_ptr<std::uint8_t[]> Buffer;
			std::uint32_t Head = 0;
			std::uint32_t Tail = 0;
			bool Overflowed = false;

			bool IsEmpty() const { return Head >= Tail; }
		};

		static constexpr int PeerToSlot(const Peer& peer) { return int(peer.GetId()) - 1; }
		static constexpr Peer SlotToPeer(int slot) { return Peer(std::uint32_t(slot) + 1); }
		void AnnounceReadyPeers();
		// Retries the backlog head-first; stops at the first frame the transport still refuses, so order holds
		void FlushPendingSends(int slot);
		bool EnqueuePendingSend(int slot, std::uint8_t packetType, ArrayView<const std::uint8_t> data);
		void ReleasePendingSends(int slot);

		nCine::PspAdhoc* _adhoc;
		NetworkState _state;
		INetworkHandler* _handler;
		std::uint32_t _clientData;
		bool _peerAnnounced[MaxAdhocGuests];
		PendingSends _pendingSends[MaxAdhocGuests];
		std::unique_ptr<std::uint8_t[]> _packetBuffer;
	};
}

#endif
