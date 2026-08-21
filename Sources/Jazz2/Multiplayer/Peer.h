#pragma once

#if defined(WITH_MULTIPLAYER) || defined(DOXYGEN_GENERATING_OUTPUT)

#include "../../Main.h"

namespace Jazz2::Multiplayer
{
	/** Opaque PSP ad-hoc peer handle. Zero identifies the local peer. */
	struct Peer
	{
		constexpr Peer(std::uint32_t id = 0) : _id(id) {}

		static constexpr Peer Remote() { return Peer(1); }
		static constexpr Peer Local(std::int32_t index) { return Peer(0xffff0000u + std::uint32_t(index)); }

		constexpr bool operator==(const Peer& other) const { return _id == other._id; }
		constexpr bool operator!=(const Peer& other) const { return _id != other._id; }
		explicit constexpr operator bool() const { return IsValid(); }
		constexpr bool IsValid() const { return _id != 0; }
		constexpr std::uint64_t GetId() const { return _id; }

		std::uint32_t _id;
	};
}

namespace Death::Implementation
{
	template<> struct Formatter<Jazz2::Multiplayer::Peer> {
		static std::size_t format(const Containers::MutableStringView& buffer, const Jazz2::Multiplayer::Peer& value, FormatContext& context);
	};
}

#endif
