#pragma once

#include "ActorBase.h"

namespace Jazz2::Actors
{
	/**
		@brief Represents a dead corpse of a player
		
		The leftover body of a defeated player that remains in the level as decoration after the player dies.
	*/
	class PlayerCorpse : public ActorBase
	{
		DEATH_RUNTIME_OBJECT(ActorBase);

	public:
		/** @brief Creates a new instance */
		PlayerCorpse();
		~PlayerCorpse();

		/** @brief Effective fur color of the player this corpse came from (0 = native palette), replicated so
			guests can tint the corpse - it reaches them as a plain remote actor that has no other way to know */
		std::uint32_t GetReplicatedFurColor() const override { return _furColor; }

	protected:
		Task<bool> OnActivatedAsync(const ActorActivationDetails& details) override;

	private:
		// Allocated palette offset into the shared palette texture for the corpse recolor (-1 = none)
		std::int32_t _paletteOffset;
		// Kept so the server can replicate it: a corpse reaches clients as a plain RemoteActor, which has no
		// other way to learn which palette to load
		std::uint32_t _furColor;
	};
}