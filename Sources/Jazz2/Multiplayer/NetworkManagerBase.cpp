#include "NetworkManagerBase.h"

#if defined(WITH_MULTIPLAYER)

#include "INetworkHandler.h"
#include "Teams.h"
#include "../../nCine/Backends/Psp/PspAdhoc.h"

#include <Containers/StaticArray.h>

#include <cstdlib>
#include <cstring>

namespace Jazz2::Multiplayer
{
	namespace {
		constexpr int MaxPacketSize = 0xffff;
	}

	static_assert(NetworkManagerBase::MaxAdhocGuests <= std::uint32_t(nCine::PspAdhoc::MaxGuests),
		"The bridge must have a slot for every ad-hoc guest");

	// The ad-hoc backend cannot include the game layer, so it declares its own "no team preference" sentinel.
	// A guest's lobby choice is copied straight across, so the two must mean the same thing.
	static_assert(nCine::PspAdhoc::NoAdhocTeam == NoPreferredTeam,
		"Ad-hoc and engine must agree on the no-preference team sentinel");

	NetworkManagerBase::NetworkManagerBase()
		: _adhoc(nullptr), _state(NetworkState::None), _handler(nullptr), _clientData(0), _peerAnnounced{},
		  _packetBuffer(std::make_unique<std::uint8_t[]>(MaxPacketSize))
	{
	}

	NetworkManagerBase::~NetworkManagerBase()
	{
		Dispose();
	}

	void NetworkManagerBase::CreateClient(INetworkHandler* handler, StringView, std::uint16_t, std::uint32_t clientData)
	{
		_handler = handler;
		_clientData = clientData;
		_state = NetworkState::Connecting;
	}

	bool NetworkManagerBase::CreateServer(INetworkHandler* handler, std::uint16_t)
	{
		_handler = handler;
		_state = NetworkState::Listening;
		return true;
	}

	void NetworkManagerBase::AttachAdhoc(nCine::PspAdhoc* adhoc, bool isHost, INetworkHandler* handler, std::uint32_t clientData)
	{
		_adhoc = adhoc;
		_handler = handler;
		_clientData = clientData;
		_state = (isHost ? NetworkState::Listening : NetworkState::Connected);
		for (auto& announced : _peerAnnounced) announced = false;
		// A backlog is meaningful only to the level that queued it; carrying it across a level would replay
		// packets describing a world that no longer exists.
		for (std::uint32_t slot = 0; slot < MaxAdhocGuests; ++slot) ReleasePendingSends(int(slot));
		// A guest may already have finished the barrier by the time the level attaches.
		AnnounceReadyPeers();
	}

	void NetworkManagerBase::DetachAdhocToLocalSession()
	{
		_adhoc = nullptr;
		_state = NetworkState::Local;
		_clientData = 0;
		for (auto& announced : _peerAnnounced) announced = false;
		for (std::uint32_t slot = 0; slot < MaxAdhocGuests; ++slot) ReleasePendingSends(int(slot));
	}

	void NetworkManagerBase::AnnounceReadyPeers()
	{
		if (_adhoc == nullptr || _handler == nullptr) return;
		for (std::uint32_t slot = 0; slot < MaxAdhocGuests; ++slot) {
			const int index = int(slot);
			if (_peerAnnounced[slot] || !_adhoc->IsPeerConnected(index) || !_adhoc->IsPeerEngineReady(index)) continue;
			ConnectionResult result = OnPeerConnected(SlotToPeer(index), _clientData);
			if (result) _peerAnnounced[slot] = true;
			else Kick(SlotToPeer(index), result.FailureReason);
		}
	}

	void NetworkManagerBase::Update()
	{
		if (_adhoc == nullptr || _handler == nullptr || _state == NetworkState::None || _state == NetworkState::Local) return;

		// Retire guests that dropped, then announce the ones that just finished the barrier.
		for (std::uint32_t slot = 0; slot < MaxAdhocGuests; ++slot) {
			const int index = int(slot);
			const bool live = _adhoc->IsPeerConnected(index) && _adhoc->IsPeerEngineReady(index) &&
				!_pendingSends[slot].Overflowed;
			if (!live && _peerAnnounced[slot]) {
				// Names the cause: this retire is what surfaces as "Host closed the game" on a guest, and the
				// three inputs fail for very different reasons.
				_adhoc->LogDiagnostic("bridge retiring peer slot=%d connected=%d engineReady=%d overflowed=%d",
					index, _adhoc->IsPeerConnected(index) ? 1 : 0, _adhoc->IsPeerEngineReady(index) ? 1 : 0,
					_pendingSends[slot].Overflowed ? 1 : 0);
				_peerAnnounced[slot] = false;
				const Reason reason = (_pendingSends[slot].Overflowed ? Reason::ConnectionLost
					: _adhoc->ConsumeGracefulDisconnect(index) ? Reason::Disconnected : Reason::ConnectionLost);
				ReleasePendingSends(index);
				OnPeerDisconnected(SlotToPeer(index), reason);
			}
		}
		AnnounceReadyPeers();

		// Retry anything the transport refused earlier, now that the peer may have drained.
		for (std::uint32_t slot = 0; slot < MaxAdhocGuests; ++slot) {
			if (_peerAnnounced[slot]) FlushPendingSends(int(slot));
		}

		for (int i = 0; i < 64; ++i) {
			int peerIndex = 0;
			std::uint8_t channel = 0;
			std::uint8_t packetType = 0;
			const int length = _adhoc->ReceiveNetworkPacketFrom(peerIndex, channel, packetType, _packetBuffer.get(), MaxPacketSize);
			// A zero-length payload is a valid packet (ForceResyncActors sends one), so only stop on
			// "nothing pending" or a dropped peer. Treating 0 as "no data" swallowed those packets and
			// abandoned the rest of the drain for the frame.
			if (length < 0) break;
			if (peerIndex < 0 || std::uint32_t(peerIndex) >= MaxAdhocGuests || !_peerAnnounced[peerIndex]) continue;
			_handler->OnPacketReceived(SlotToPeer(peerIndex), channel, packetType,
				ArrayView<const std::uint8_t>(_packetBuffer.get(), std::size_t(length)));
		}
	}

	void NetworkManagerBase::Dispose()
	{
		_adhoc = nullptr;
		_handler = nullptr;
		_state = NetworkState::None;
		_clientData = 0;
		for (auto& announced : _peerAnnounced) announced = false;
		for (std::uint32_t slot = 0; slot < MaxAdhocGuests; ++slot) ReleasePendingSends(int(slot));
	}

	void NetworkManagerBase::CreateLocalSession(INetworkHandler* handler)
	{
		_handler = handler;
		_state = NetworkState::Local;
	}

	std::uint32_t NetworkManagerBase::GetConnectedGuestCount() const
	{
		if (_adhoc == nullptr) return 0;
		// Deliberately IsPeerConnected (the lobby-level link) and not IsPeerEngineReady (the per-level barrier):
		// a guest that is still loading is connected but not engine-ready, and it is exactly that guest the
		// round must not start without.
		std::uint32_t count = 0;
		for (std::uint32_t slot = 0; slot < MaxAdhocGuests; ++slot) {
			if (_adhoc->IsPeerConnected(int(slot))) count++;
		}
		return count;
	}

	std::uint32_t NetworkManagerBase::GetRoundTripTimeMs() const { return 0; }
	std::uint32_t NetworkManagerBase::GetRoundTripTimeMs(const Peer&) const { return 0; }
	Array<String> NetworkManagerBase::GetServerEndpoints() const { return {}; }
	std::uint16_t NetworkManagerBase::GetServerPort() const { return 31001; }

	void NetworkManagerBase::ReleasePendingSends(int slot)
	{
		// A backlog only ever holds frames for the peer that is going away, so it dies with it. Freeing the
		// buffer also means a session that hiccups once does not hold 8KB per peer for the rest of its life.
		_pendingSends[slot].Buffer = nullptr;
		_pendingSends[slot].Head = _pendingSends[slot].Tail = 0;
		_pendingSends[slot].Overflowed = false;
	}

	void NetworkManagerBase::FlushPendingSends(int slot)
	{
		PendingSends& pending = _pendingSends[slot];
		if (pending.Buffer == nullptr || pending.IsEmpty()) return;

		while (pending.Head < pending.Tail) {
			const std::uint8_t* frame = pending.Buffer.get() + pending.Head;
			const std::uint8_t packetType = frame[0];
			const std::uint32_t length = std::uint32_t(frame[1]) | (std::uint32_t(frame[2]) << 8);
			if (!_adhoc->SendNetworkPacketTo(slot, std::uint8_t(NetworkChannel::Main), packetType,
					frame + PendingSendHeaderSize, int(length))) {
				// Still refused. Stop here rather than skipping ahead - the peer must see these in order.
				return;
			}
			pending.Head += PendingSendHeaderSize + length;
		}

		pending.Head = pending.Tail = 0;
	}

	bool NetworkManagerBase::EnqueuePendingSend(int slot, std::uint8_t packetType, ArrayView<const std::uint8_t> data)
	{
		PendingSends& pending = _pendingSends[slot];
		const std::uint32_t length = std::uint32_t(data.size());
		if (length > 0xffff) return false;
		const std::uint32_t needed = PendingSendHeaderSize + length;
		if (needed > PendingSendCapacity) return false;

		if (pending.Buffer == nullptr) {
			pending.Buffer = std::make_unique<std::uint8_t[]>(PendingSendCapacity);
			if (pending.Buffer == nullptr) return false;
			pending.Head = pending.Tail = 0;
		}

		if (pending.Tail + needed > PendingSendCapacity) {
			// Reclaim the space already sent before giving up
			const std::uint32_t remaining = pending.Tail - pending.Head;
			if (remaining > 0 && pending.Head > 0) {
				std::memmove(pending.Buffer.get(), pending.Buffer.get() + pending.Head, remaining);
			}
			pending.Head = 0;
			pending.Tail = remaining;
			if (pending.Tail + needed > PendingSendCapacity) return false;
		}

		std::uint8_t* frame = pending.Buffer.get() + pending.Tail;
		frame[0] = packetType;
		frame[1] = std::uint8_t(length & 0xff);
		frame[2] = std::uint8_t((length >> 8) & 0xff);
		if (length > 0) std::memcpy(frame + PendingSendHeaderSize, data.data(), length);
		pending.Tail += needed;
		return true;
	}

	void NetworkManagerBase::SendTo(const Peer& peer, NetworkChannel channel, std::uint8_t packetType, ArrayView<const std::uint8_t> data)
	{
		if (_adhoc == nullptr || !peer) return;
		const int slot = PeerToSlot(peer);
		if (slot < 0 || std::uint32_t(slot) >= MaxAdhocGuests || !_peerAnnounced[slot]) return;

		if (channel != NetworkChannel::Main) {
			// Unreliable updates are soft state - each one supersedes the last, so a lost frame is corrected by
			// the next one ~33ms later. Retrying them would only deliver a stale position late.
			_adhoc->SendNetworkPacketTo(slot, std::uint8_t(channel), packetType, data.data(), int(data.size()));
			return;
		}

		// Drain anything already backed up first; a new frame must never overtake an older one.
		FlushPendingSends(slot);

		if (_pendingSends[slot].IsEmpty() &&
			_adhoc->SendNetworkPacketTo(slot, std::uint8_t(channel), packetType, data.data(), int(data.size()))) {
			return;
		}

		if (!EnqueuePendingSend(slot, packetType, data)) {
			// The peer has 64KB in flight plus a full backlog: it stopped draining entirely and is not coming
			// back. Losing a Main packet silently is how a client ends up permanently stale (a missed
			// DestroyRemoteActor leaves a ghost actor forever), so fail loudly instead. Retired in Update(),
			// not here, because OnPeerDisconnected re-enters the handler and may itself send.
			if (!_pendingSends[slot].Overflowed) {
				_pendingSends[slot].Overflowed = true;
				LOGE("[MP] Peer {} stopped draining; reliable backlog overflowed on packet {}", slot, packetType);
			}
		}
	}

	void NetworkManagerBase::SendTo(Function<bool(const Peer&)>&& predicate, NetworkChannel channel,
		std::uint8_t packetType, ArrayView<const std::uint8_t> data)
	{
		for (std::uint32_t slot = 0; slot < MaxAdhocGuests; ++slot) {
			if (!_peerAnnounced[slot]) continue;
			const Peer peer = SlotToPeer(int(slot));
			if (predicate(peer)) SendTo(peer, channel, packetType, data);
		}
	}

	void NetworkManagerBase::SendTo(AllPeersT, NetworkChannel channel, std::uint8_t packetType, ArrayView<const std::uint8_t> data)
	{
		for (std::uint32_t slot = 0; slot < MaxAdhocGuests; ++slot) {
			if (!_peerAnnounced[slot]) continue;
			SendTo(SlotToPeer(int(slot)), channel, packetType, data);
		}
	}

	void NetworkManagerBase::Kick(const Peer& peer, Reason reason)
	{
		if (!peer) return;
		const int slot = PeerToSlot(peer);
		if (slot < 0 || std::uint32_t(slot) >= MaxAdhocGuests || !_peerAnnounced[slot]) return;
		// Drop just this guest; the remaining guests and the listener keep running.
		_peerAnnounced[slot] = false;
		ReleasePendingSends(slot);
		OnPeerDisconnected(peer, reason);
	}

	String NetworkManagerBase::AddressToString(const Peer& peer) const
	{
		return (peer ? String("ad-hoc peer") : String("local"));
	}

	bool NetworkManagerBase::IsAddressValid(StringView address) { return !address.empty(); }
	bool NetworkManagerBase::IsDomainValid(StringView domain) { return !domain.empty(); }

	bool NetworkManagerBase::TrySplitAddressAndPort(StringView input, StringView& address, std::uint16_t& port)
	{
		auto parts = input.partition(':');
		address = parts[0];
		if (!parts[1]) return !address.empty();
		String portString = String::nullTerminatedView(parts[2]);
		char* end = nullptr;
		const long value = std::strtol(portString.data(), &end, 10);
		if (end == portString.data() || *end != '\0' || value < 1 || value > 65535) return false;
		port = std::uint16_t(value);
		return !address.empty();
	}

	const char* NetworkManagerBase::ReasonToString(Reason reason)
	{
		switch (reason) {
			case Reason::Disconnected: return "Disconnected";
			case Reason::IncompatibleVersion: return "Incompatible version";
			case Reason::ProtocolViolation: return "Protocol violation";
			case Reason::InvalidPassword: return "Invalid password";
			case Reason::InvalidPlayerName: return "Invalid player name";
			case Reason::ServerIsFull: return "Server is full";
			case Reason::ConnectionLost: return "Connection lost";
			case Reason::ConnectionTimedOut: return "Connection timed out";
			case Reason::Kicked: return "Kicked";
			case Reason::Banned: return "Banned";
			default: return "Unknown reason";
		}
	}

	ConnectionResult NetworkManagerBase::OnPeerConnected(const Peer& peer, std::uint32_t clientData)
	{
		return (_handler != nullptr ? _handler->OnPeerConnected(peer, clientData) : ConnectionResult(true));
	}

	void NetworkManagerBase::OnPeerDisconnected(const Peer& peer, Reason reason)
	{
		if (_handler != nullptr) _handler->OnPeerDisconnected(peer, reason);
	}
}

#endif
