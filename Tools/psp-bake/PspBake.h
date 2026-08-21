#pragma once

#include <cstdint>

// Cache/Levels.idx is deliberately tiny and independent of the converted level format. The PSP menu can
// enumerate selectable levels with one sequential read instead of opening and inflating every .j2l at boot.
constexpr std::uint64_t PspLevelCatalogSignature = 0x5844494C32505350ull; // "PSP2LIDX"
constexpr std::uint16_t PspLevelCatalogVersion = 1;

enum class PspBakeStage : int
{
	Animations, GameData, Episodes, Levels, NativeTilesets,
	BundledSprites, FinalizingGraphics, CacheIndex, Music
};

struct PspBakeOptions
{
	const char* SourceDirectory = nullptr;
	const char* OutputDirectory = nullptr;
	const char* ContentDirectory = nullptr;
	bool MusicOnly = false;
	void (*Progress)(void* userData, int completed, int total, const char* currentItem) = nullptr;
	void (*MusicProgress)(void* userData, std::uint32_t completedMilliseconds,
		std::uint32_t totalMilliseconds, int completedTracks, int totalTracks,
		const char* currentTrack) = nullptr;
	void (*Log)(void* userData, const char* message) = nullptr;
	void (*DetailProgress)(void* userData, PspBakeStage stage, int completed, int total) = nullptr;
	void* ProgressUserData = nullptr;
};

// Runs the same conversion pipeline as the host psp-bake executable. Returns zero on success.
int RunPspBake(const PspBakeOptions& options);
