#pragma once

#include <cstdint>

namespace Jazz2
{
	// Type byte stored after the common signature in converted cache files.
	// Kept independent of ContentResolver so conversion tools do not pull in
	// rendering and platform-backend headers just to write the file format.
	struct ContentFileType
	{
		static constexpr std::uint8_t Level = 1;
		static constexpr std::uint8_t Episode = 2;
		static constexpr std::uint8_t CacheIndex = 3;
		static constexpr std::uint8_t Config = 4;
		static constexpr std::uint8_t State = 5;
		static constexpr std::uint8_t SfxList = 6;
		static constexpr std::uint8_t Highscores = 7;
	};
}
