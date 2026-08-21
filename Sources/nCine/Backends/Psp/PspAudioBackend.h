#pragma once

#include <cstdint>
#include <memory>

namespace nCine
{
	class IAudioDevice;
	struct PspAudioStats
	{
		std::uint32_t buffersLoaded;
		std::uint32_t buffersFailed;
		std::uint32_t voicesStarted;
		std::uint32_t mixBlocks;
		std::uint32_t musicStreamsLoaded;
		std::uint32_t musicFramesMixed;
		std::uint32_t activeSfxVoices;
		std::uint32_t mixMicroseconds;
		std::uint32_t maxMixMicroseconds;
		std::uint32_t deadlineMisses;
		std::uint32_t peakAccumulator;
		std::uint32_t saturatedSamples;
		std::uint32_t outputErrors;
	};

	/** Creates the PSP sceAudio-backed software mixing device. */
	std::unique_ptr<IAudioDevice> CreatePspAudioDevice();
	/** Returns lightweight counters used by the on-device debug heartbeat. */
	PspAudioStats GetPspAudioStats();
}
