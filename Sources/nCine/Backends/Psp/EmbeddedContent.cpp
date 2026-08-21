#include "EmbeddedContent.h"

// Content is only linked into the EBOOT when -DPSP_EMBED_CONTENT=ON. Off by default: the ~742 KiB blob is
// part of the module image, which the kernel carves out of the user partition before _sbrk takes the heap,
// and it is redundant once Content/ exists on the Memory Stick next to the EBOOT.
#if defined(JAZZ2_PSP_EMBED_CONTENT)
#include "EmbeddedContentVersion.h"

#include "Shared/IO/FileSystem.h"
#include <Containers/StringConcatenable.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" const unsigned char g_embeddedContentStart[];
extern "C" const unsigned char g_embeddedContentEnd[];

namespace
{
	using namespace Death::Containers;
	using namespace Death::Containers::Literals;
	using fs = Death::IO::FileSystem;

	std::size_t ReadOctal(const char* value, std::size_t length)
	{
		std::size_t result = 0;
		for (std::size_t i = 0; i < length && value[i] != '\0' && value[i] != ' '; ++i) {
			if (value[i] < '0' || value[i] > '7') continue;
			result = result * 8 + (std::size_t)(value[i] - '0');
		}
		return result;
	}
}

bool EnsureEmbeddedPspContent()
{
	const unsigned char* cursor = g_embeddedContentStart;
	const unsigned char* const end = g_embeddedContentEnd;

	const String markerPath = fs::CombinePath("Content"_s, ".psp-embedded-version"_s);
	{
		std::FILE* marker = std::fopen(markerPath.data(), "rb");
		char installed[sizeof(PspEmbeddedContentVersion)]{};
		const bool current = marker != nullptr &&
			std::fread(installed, 1, sizeof(installed) - 1, marker) == sizeof(installed) - 1 &&
			std::memcmp(installed, PspEmbeddedContentVersion, sizeof(installed) - 1) == 0;
		if (marker != nullptr) std::fclose(marker);
		if (current) return true;
	}

	fs::CreateDirectories("Content"_s);
	while ((std::size_t)(end - cursor) >= 512) {
		const char* header = reinterpret_cast<const char*>(cursor);
		bool empty = true;
		for (int i = 0; i < 512; ++i) empty = empty && header[i] == '\0';
		if (empty) break;
		char path[256]{};
		std::snprintf(path, sizeof(path), "%.100s", header);
		const std::size_t dataLength = ReadOctal(header + 124, 12);
		cursor += 512;
		if ((std::size_t)(end - cursor) < dataLength) return false;
		if (std::strncmp(path, "Content/", 8) != 0 && std::strcmp(path, "Content") != 0) return false;
		const String outputPath(path);
		if (header[156] == '5') {
			fs::CreateDirectories(outputPath);
			cursor += (dataLength + 511) & ~std::size_t(511);
			continue;
		}
		fs::CreateDirectories(fs::GetDirectoryName(outputPath));
		std::FILE* output = std::fopen(outputPath.data(), "wb");
		if (output == nullptr || (dataLength != 0 && std::fwrite(cursor, 1, dataLength, output) != dataLength)) {
			if (output != nullptr) std::fclose(output);
			return false;
		}
		std::fclose(output);
		cursor += (dataLength + 511) & ~std::size_t(511);
	}

	std::FILE* marker = std::fopen(markerPath.data(), "wb");
	if (marker == nullptr) return false;
	const bool written = std::fwrite(PspEmbeddedContentVersion, 1,
		sizeof(PspEmbeddedContentVersion) - 1, marker) == sizeof(PspEmbeddedContentVersion) - 1;
	std::fclose(marker);
	return written;
}

#else

// No blob linked in - Content/ is expected to already sit next to the EBOOT.
bool EnsureEmbeddedPspContent() { return true; }

#endif
