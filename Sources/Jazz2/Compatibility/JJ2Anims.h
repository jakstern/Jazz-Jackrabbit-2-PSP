#pragma once

#include "../../Main.h"
#include "JJ2Version.h"
#include "AnimSetMapping.h"

#include <memory>

#include <Containers/SmallVector.h>
#include <Containers/StringView.h>
#include <IO/Stream.h>
#include <IO/PakFile.h>

using namespace Death::Containers;
using namespace Death::IO;
using namespace nCine;

namespace Jazz2::Compatibility
{
	/**
		@brief Parses original `.j2a` animation files
		
		Reads the original game's combined animation archive (`Anims.j2a`), decoding its animation sets,
		frames and embedded audio samples, and writes the converted sprites and sounds into the engine's
		`.pak` format. Asset naming and palette handling are driven by @ref AnimSetMapping.
	*/
	class JJ2Anims
	{
	public:
#ifndef DOXYGEN_GENERATING_OUTPUT
		static constexpr std::uint16_t CacheVersion = 36;
#endif

		/** @brief Optional per-sprite sink invoked as each sprite is converted, with its packed pixels.
			Used by the PSP asset baker to generate GU textures during conversion (no read-back of the .pak).
			`filename` is the "Animations/…/name.aura" path; `notIndexed` = the sprite is pre-coloured RGBA. */
		using SpriteSink = void (*)(void* ctx, StringView filename, const std::uint8_t* data,
			std::int32_t width, std::int32_t height, std::int32_t channelCount, bool notIndexed,
			const std::uint8_t* auraHeader);
		using ConversionProgress = void (*)(void* ctx, std::int32_t completed, std::int32_t total);
		using SpritePredicate = bool (*)(void* ctx, StringView filename);

		/** @brief Converts the specified animation file and writes the result to a `.pak` file */
		static JJ2Version Convert(StringView path, PakWriter& pakWriter, bool isPlus = false,
			SpriteSink sink = nullptr, void* sinkCtx = nullptr,
			ConversionProgress progress = nullptr, void* progressCtx = nullptr,
			SpritePredicate predicate = nullptr, void* predicateCtx = nullptr);

		/** @brief Writes raw image content to the specified stream */
		static void WriteImageContent(Stream& so, const std::uint8_t* data, std::int32_t width, std::int32_t height, std::int32_t channelCount);

	private:
		static constexpr int32_t AddBorder = 2;

#ifndef DOXYGEN_GENERATING_OUTPUT
		// Doxygen 1.12.0 outputs also private structs/unions even if it shouldn't
		struct AnimFrameSection {
			std::int16_t SizeX, SizeY;
			std::int16_t ColdspotX, ColdspotY;
			std::int16_t HotspotX, HotspotY;
			std::int16_t GunspotX, GunspotY;

			std::unique_ptr<std::uint8_t[]> ImageData;
			// TODO: Sprite mask
			//std::unique_ptr<std::uint8_t[]> MaskData;
			std::int32_t ImageAddr;
			std::int32_t MaskAddr;
			bool DrawTransparent;
		};

		struct AnimSection {
			std::uint16_t FrameCount;
			std::uint16_t FrameRate;
			SmallVector<AnimFrameSection, 0> Frames;
			std::int32_t Set;
			std::uint16_t Anim;

			std::int16_t AdjustedSizeX, AdjustedSizeY;
			std::int16_t LargestOffsetX, LargestOffsetY;
			std::int16_t NormalizedHotspotX, NormalizedHotspotY;
			std::int8_t FrameConfigurationX, FrameConfigurationY;
		};

		struct SampleSection {
			std::int32_t Set;
			std::uint16_t IdInSet;
			std::uint32_t SampleRate;
			std::uint32_t DataSize;
			std::unique_ptr<std::uint8_t[]> Data;
			std::uint16_t Multiplier;
		};
#endif

		JJ2Anims();

		static void ImportAnimations(PakWriter& pakWriter, JJ2Version version, SmallVectorImpl<AnimSection>& anims,
			SpriteSink sink, void* sinkCtx, ConversionProgress progress, void* progressCtx,
			std::int32_t& progressCompleted, std::int32_t progressTotal,
			SpritePredicate predicate, void* predicateCtx);
		static void ImportAudioSamples(PakWriter& pakWriter, JJ2Version version, SmallVectorImpl<SampleSection>& samples);

		static void WriteImageToFile(StringView targetPath, const std::uint8_t* data, std::int32_t width, std::int32_t height, std::int32_t channelCount, const AnimSection& anim, AnimSetMapping::Entry* entry);
		static void WriteImageToStream(Stream& targetStream, const std::uint8_t* data, std::int32_t width, std::int32_t height, std::int32_t channelCount, const AnimSection& anim, AnimSetMapping::Entry* entry);
	};
}
