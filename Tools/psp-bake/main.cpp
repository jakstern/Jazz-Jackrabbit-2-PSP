// psp-bake — host asset pipeline for the lean PSP fork.
//
// Builds the PSP cache from an original JJ2 `Source/` directory using the engine's own conversion code,
// feeding graphics straight into the PSP-GU pack; .j2tpsp sidecars therefore omit the diffuse QOI that
// already lives in texture.pak. Built as a host tool (native g++/clang) against a GL-free engine subset.

#include "Jazz2/Compatibility/JJ2Anims.h"
#include "Jazz2/Compatibility/JJ2Data.h"
#include "Jazz2/Compatibility/JJ2Episode.h"
#include "Jazz2/Compatibility/JJ2Level.h"
#include "Jazz2/Compatibility/JJ2Tileset.h"
#include "Jazz2/Compatibility/JJ2Strings.h"
#include "Jazz2/Compatibility/EventConverter.h"
#include "Jazz2/Compatibility/JJ2Version.h"
#include "TilesetBake.h"
#include "PspBake.h"
#include "Jazz2/EventType.h"
#include "Jazz2/ContentFileType.h"
#include "nCine/Base/Algorithms.h"
#include "nCine/Backends/Psp/PspGpuFormat.h"

#include <string>
#include <unordered_map>

#include "nCine/Base/HashMap.h"
#include <Containers/Pair.h>
#include <Containers/StringConcatenable.h>
#include <IO/PakFile.h>
#include <IO/FileSystem.h>
#include <Containers/DateTime.h>

#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <zlib.h>
#include <xmp.h>

#if defined(DEATH_TARGET_PSP)
#	include <pspsysmem.h>
#endif

using namespace Death::Containers;
using namespace Death::Containers::Literals;
using namespace Death::IO;
using namespace Jazz2;
using namespace Jazz2::Compatibility;
using nCine::HashMap;

#if !defined(NCINE_VERSION)
#	define NCINE_VERSION "3.6.0"
#endif

namespace
{
	using fs = Death::IO::FileSystem;

	const char* VersionName(JJ2Version v)
	{
		const std::uint16_t raw = static_cast<std::uint16_t>(v);
		const bool shareware = (raw & static_cast<std::uint16_t>(JJ2Version::SharewareDemo)) != 0;
		const bool plus = (raw & static_cast<std::uint16_t>(JJ2Version::PlusExtension)) != 0;
		switch (static_cast<JJ2Version>(raw & 0x00ff)) {
			case JJ2Version::BaseGame:
				if (shareware) return "Shareware Demo";
				return plus ? "BaseGame (1.20/1.23) + JJ2+" : "BaseGame (1.20/1.23)";
			case JJ2Version::TSF: return plus ? "TSF (1.24) + JJ2+" : "TSF (1.24)";
			case JJ2Version::HH: return "Holiday Hare";
			case JJ2Version::CC: return "Christmas Chronicles";
			default: return "Unknown";
		}
	}

	String LowercaseExtension(StringView path)
	{
		String extension(fs::GetExtension(path));
		for (char& c : extension) c = char(std::tolower(static_cast<unsigned char>(c)));
		return extension;
	}

	bool WriteCacheDescriptor(StringView path, std::uint64_t currentVersion, std::int64_t animsModified)
	{
		auto so = fs::Open(path, FileAccess::Write);
		if (so == nullptr || !so->IsValid()) return false;
		so->WriteValueAsLE<std::uint64_t>(0x2095A59FF0BFBBEF);	// Signature
		so->WriteValue<std::uint8_t>(ContentFileType::CacheIndex);
		so->WriteValueAsLE<std::uint16_t>(JJ2Anims::CacheVersion);
		so->WriteValue<std::uint8_t>(0x00);	// Flags
		so->WriteValueAsLE<std::int64_t>(animsModified);
		so->WriteValueAsLE<std::uint16_t>(std::uint16_t(EventType::Count));
		so->WriteValueAsLE<std::uint64_t>(currentVersion);
		return so->IsValid();
	}

	bool WriteContentDescriptor(StringView path, JJ2Version version, bool hasLori)
	{
		auto so = fs::Open(path, FileAccess::Write);
		if (so == nullptr || !so->IsValid()) {
			std::fprintf(stderr, "error: cannot write \"%s\"\n", String(path).data());
			return false;
		}
		char line[128];
		int n = std::snprintf(line, sizeof(line), "version=%s\nlori=%d\n", VersionName(version), hasLori ? 1 : 0);
		so->Write(line, n);
		std::printf("Content descriptor: version=%s lori=%d\n", VersionName(version), hasLori ? 1 : 0);
		return so->IsValid();
	}

	struct BakedLevelEntry {
		String Name;
		String DisplayName;
	};

	bool WriteLevelCatalog(StringView path, std::vector<BakedLevelEntry>& levels)
	{
		std::sort(levels.begin(), levels.end(), [](const auto& a, const auto& b) {
			return a.DisplayName < b.DisplayName;
		});
		if (levels.size() > UINT16_MAX) return false;

		const String temporary(path + ".tmp"_s);
		fs::RemoveFile(temporary);
		{
			auto so = fs::Open(temporary, FileAccess::Write);
			if (so == nullptr || !so->IsValid()) return false;
			so->WriteValueAsLE<std::uint64_t>(PspLevelCatalogSignature);
			so->WriteValueAsLE<std::uint16_t>(PspLevelCatalogVersion);
			so->WriteValueAsLE<std::uint16_t>((std::uint16_t)levels.size());
			for (const auto& level : levels) {
				if (level.Name.size() > UINT16_MAX || level.DisplayName.size() > UINT16_MAX) return false;
				so->WriteValueAsLE<std::uint16_t>((std::uint16_t)level.Name.size());
				so->WriteValueAsLE<std::uint16_t>((std::uint16_t)level.DisplayName.size());
				so->Write(level.Name.data(), level.Name.size());
				so->Write(level.DisplayName.data(), level.DisplayName.size());
			}
			if (!so->IsValid()) return false;
		}
		// FAT rename does not replace an existing file reliably.
		if (fs::IsReadableFile(path) && !fs::RemoveFile(path)) return false;
		return fs::Move(temporary, path);
	}

	// Half of the PSP's 44.1 kHz output; the streaming backend expands it back up. Halves both the encode
	// cost and the largest sequential write of an on-device bake.
	constexpr std::int32_t MusicFrequency = 22050;
	constexpr std::int32_t ImaBlockAlign = 1024;
	constexpr std::int32_t ImaSamplesPerBlock = 1017;

	struct PspMusicTrack {
		String Source;
		String Output;
		String Name;
		std::int64_t SourceSize = 0;
		std::uint32_t EstimatedMilliseconds = 0;
	};

	std::uint32_t ReadLittle32(const std::uint8_t* p)
	{
		return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
			(std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
	}

	void WriteLittle16(std::uint8_t* p, std::uint16_t value)
	{
		p[0] = std::uint8_t(value);
		p[1] = std::uint8_t(value >> 8);
	}

	void WriteLittle32(std::uint8_t* p, std::uint32_t value)
	{
		p[0] = std::uint8_t(value);
		p[1] = std::uint8_t(value >> 8);
		p[2] = std::uint8_t(value >> 16);
		p[3] = std::uint8_t(value >> 24);
	}

	bool LoadJ2bModule(StringView path, std::vector<std::uint8_t>& decoded)
	{
		std::FILE* f = std::fopen(String(path).data(), "rb");
		if (f == nullptr) return false;
		std::fseek(f, 0, SEEK_END);
		const long fileSize = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		if (fileSize < 24) { std::fclose(f); return false; }
		std::uint8_t header[24];
		if (std::fread(header, 1, sizeof(header), f) != sizeof(header) ||
			std::memcmp(header, "MUSE", 4) != 0 || ReadLittle32(header + 8) != std::uint32_t(fileSize)) {
			std::fclose(f);
			return false;
		}
		const std::uint32_t expectedCrc = ReadLittle32(header + 12);
		const std::uint32_t packedSize = ReadLittle32(header + 16);
		const std::uint32_t decodedSize = ReadLittle32(header + 20);
		if (packedSize != std::uint32_t(fileSize - sizeof(header)) || decodedSize == 0) {
			std::fclose(f);
			return false;
		}

		// Inflate streaming rather than holding packed and unpacked modules at once: a retail J2B would
		// otherwise cost almost 2 MiB before libxmp even allocates its parsed form.
		decoded.resize(decodedSize);
		z_stream stream{};
		if (inflateInit(&stream) != Z_OK) { std::fclose(f); return false; }
		stream.next_out = decoded.data();
		stream.avail_out = decodedSize;
		std::uint8_t input[16 * 1024];
		std::uint32_t remaining = packedSize;
		uLong actualCrc = crc32(0, Z_NULL, 0);
		int inflateResult = Z_OK;
		while (remaining > 0 && inflateResult == Z_OK) {
			const std::size_t chunk = std::min<std::size_t>(remaining, sizeof(input));
			if (std::fread(input, 1, chunk, f) != chunk) { inflateResult = Z_ERRNO; break; }
			actualCrc = crc32(actualCrc, input, (uInt)chunk);
			remaining -= (std::uint32_t)chunk;
			stream.next_in = input;
			stream.avail_in = (uInt)chunk;
			while (stream.avail_in > 0 && inflateResult == Z_OK)
				inflateResult = inflate(&stream, Z_NO_FLUSH);
		}
		const bool success = inflateResult == Z_STREAM_END && remaining == 0 && stream.avail_in == 0 &&
			stream.total_in == packedSize && stream.total_out == decodedSize && actualCrc == expectedCrc;
		inflateEnd(&stream);
		std::fclose(f);
		if (!success) decoded.clear();
		return success;
	}

	bool OpenPspMusicModule(const PspMusicTrack& track, xmp_context context,
		std::vector<std::uint8_t>& decoded)
	{
		String extension = LowercaseExtension(track.Source);
		int result;
		if (extension == "j2b"_s) {
			if (!LoadJ2bModule(track.Source, decoded)) return false;
			result = xmp_load_module_from_memory(context, decoded.data(), (long)decoded.size());
			// libxmp owns the parsed module once load returns, so drop the decompressed source now: keeping
			// it would overlap two large copies of the same track and fragment the PSP heap between tracks.
			std::vector<std::uint8_t>().swap(decoded);
		} else {
			result = xmp_load_module(context, track.Source.data());
		}
		if (result != 0 || xmp_start_player(context, MusicFrequency, 0) != 0) return false;
		xmp_set_player(context, XMP_PLAYER_INTERP, XMP_INTERP_LINEAR);
		return true;
	}

	bool ReadCompleteImaDuration(StringView path, std::uint32_t& milliseconds)
	{
		std::FILE* file = std::fopen(String(path).data(), "rb");
		if (file == nullptr) return false;
		std::uint8_t header[60]{};
		const bool read = std::fread(header, 1, sizeof(header), file) == sizeof(header);
		std::fseek(file, 0, SEEK_END);
		const long fileSize = std::ftell(file);
		std::fclose(file);
		if (!read || std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVEfmt ", 8) != 0 ||
			ReadLittle32(header + 16) != 20 || header[20] != 0x11 || header[22] != 2 ||
			ReadLittle32(header + 24) != MusicFrequency ||
			std::memcmp(header + 40, "fact", 4) != 0 || std::memcmp(header + 52, "data", 4) != 0) return false;
		const std::uint32_t frames = ReadLittle32(header + 48);
		const std::uint32_t dataBytes = ReadLittle32(header + 56);
		if (frames == 0 || fileSize != long(sizeof(header) + dataBytes)) return false;
		milliseconds = std::uint32_t(std::max<std::uint64_t>(1, std::uint64_t(frames) * 1000 / MusicFrequency));
		return true;
	}

	bool ReplaceMusicFile(StringView temporary, StringView output)
	{
		// FAT/PSP rename does not reliably replace an existing destination. Safe to unlink first: the
		// temporary is already fully written and closed.
		if (fs::IsReadableFile(output) && !fs::RemoveFile(output)) return false;
		return fs::Move(temporary, output);
	}

	void LogMusicMemory(const PspBakeOptions& options, const char* event, const PspMusicTrack& track)
	{
		if (options.Log == nullptr) return;
		char message[224];
#if defined(DEATH_TARGET_PSP)
		std::snprintf(message, sizeof(message), "music %s: %s; free=%u maxblock=%u source=%lld",
			track.Name.data(), event, (unsigned)sceKernelTotalFreeMemSize(),
			(unsigned)sceKernelMaxFreeMemSize(), (long long)track.SourceSize);
#else
		std::snprintf(message, sizeof(message), "music %s: %s; source=%lld",
			track.Name.data(), event, (long long)track.SourceSize);
#endif
		options.Log(options.ProgressUserData, message);
	}

	class ImaWavWriter
	{
	public:
		explicit ImaWavWriter(StringView path) : file_(std::fopen(String(path).data(), "wb"))
		{
			if (file_ == nullptr) return;
			blockBuffer_ = std::make_unique<std::uint8_t[]>(BlockBufferSize);
			// Already batched in blockBuffer_; a second stdio buffer would copy the whole music cache again.
			std::setvbuf(file_, nullptr, _IONBF, 0);
			std::uint8_t placeholder[60]{};
			if (std::fwrite(placeholder, 1, sizeof(placeholder), file_) != sizeof(placeholder)) {
				std::fclose(file_);
				file_ = nullptr;
			}
		}

		~ImaWavWriter() { if (file_ != nullptr) std::fclose(file_); }
		bool IsValid() const { return file_ != nullptr && blockBuffer_ != nullptr; }

		bool Write(const std::int16_t* samples, std::int32_t frames)
		{
			if (file_ == nullptr || frames <= 0 || frames > ImaSamplesPerBlock) return false;
			// Deliberately uninitialised: every payload byte is assigned below, and clearing 1 KiB per
			// block is pure cost. Only the two reserved header bytes need explicit zeroing.
			std::uint8_t* const block = blockBuffer_.get() + bufferedBytes_;
			for (std::int32_t channel = 0; channel < 2; ++channel) {
				std::int32_t predictor = samples[channel];
				std::int32_t stepIndex = stepIndex_[channel];
				block[channel * 4] = std::uint8_t(predictor);
				block[channel * 4 + 1] = std::uint8_t(predictor >> 8);
				block[channel * 4 + 2] = std::uint8_t(stepIndex);
				block[channel * 4 + 3] = 0;

				// Predictor and step index stay in locals for the whole block: a half-rate retail bake
				// encodes roughly 190 million nibbles, so the reloads matter.
				const std::int16_t* input = samples + 2 + channel;
				std::uint8_t* output = block + 8 + channel * 4;
				if (frames == ImaSamplesPerBlock) {
					// Full-block fast path (every block but a track's last): no per-nibble bounds checks.
					for (std::int32_t group = 0; group < 127; ++group, output += 8) {
						for (std::int32_t byte = 0; byte < 4; ++byte) {
							const std::uint8_t lo = EncodeNibble(*input, predictor, stepIndex);
							input += 2;
							const std::uint8_t hi = EncodeNibble(*input, predictor, stepIndex);
							input += 2;
							output[byte] = std::uint8_t(lo | (hi << 4));
						}
					}
				} else {
					std::int32_t remaining = frames - 1;
					for (std::int32_t group = 0; group < 127; ++group, output += 8) {
						for (std::int32_t byte = 0; byte < 4; ++byte) {
							const std::int32_t a = (remaining > 0 ? *input : predictor);
							if (remaining > 0) { input += 2; --remaining; }
							const std::uint8_t lo = EncodeNibble(a, predictor, stepIndex);
							const std::int32_t b = (remaining > 0 ? *input : predictor);
							if (remaining > 0) { input += 2; --remaining; }
							const std::uint8_t hi = EncodeNibble(b, predictor, stepIndex);
							output[byte] = std::uint8_t(lo | (hi << 4));
						}
					}
				}
				stepIndex_[channel] = stepIndex;
			}
			bufferedBytes_ += ImaBlockAlign;
			if (bufferedBytes_ == BlockBufferSize && !FlushBlocks()) return false;
			dataBytes_ += ImaBlockAlign;
			frames_ += std::uint32_t(frames);
			return true;
		}

		bool Finish()
		{
			if (file_ == nullptr || std::ferror(file_) || !FlushBlocks()) return false;
			std::uint8_t header[60]{};
			std::memcpy(header, "RIFF", 4); WriteLittle32(header + 4, 52 + dataBytes_);
			std::memcpy(header + 8, "WAVEfmt ", 8); WriteLittle32(header + 16, 20);
			WriteLittle16(header + 20, 0x11); WriteLittle16(header + 22, 2);
			WriteLittle32(header + 24, MusicFrequency);
			WriteLittle32(header + 28, MusicFrequency * ImaBlockAlign / ImaSamplesPerBlock);
			WriteLittle16(header + 32, ImaBlockAlign); WriteLittle16(header + 34, 4);
			WriteLittle16(header + 36, 2); WriteLittle16(header + 38, ImaSamplesPerBlock);
			std::memcpy(header + 40, "fact", 4); WriteLittle32(header + 44, 4);
			WriteLittle32(header + 48, frames_);
			std::memcpy(header + 52, "data", 4); WriteLittle32(header + 56, dataBytes_);
			if (std::fseek(file_, 0, SEEK_SET) != 0 ||
				std::fwrite(header, 1, sizeof(header), file_) != sizeof(header)) return false;
			const bool ok = std::fflush(file_) == 0 && !std::ferror(file_);
			std::fclose(file_);
			file_ = nullptr;
			return ok;
		}

	private:
		static constexpr std::uint32_t BlockBufferSize = 64 * 1024;

		bool FlushBlocks()
		{
			if (bufferedBytes_ == 0) return true;
			if (std::fwrite(blockBuffer_.get(), 1, bufferedBytes_, file_) != bufferedBytes_) return false;
			bufferedBytes_ = 0;
			return true;
		}

		static std::uint8_t EncodeNibble(std::int32_t sample, std::int32_t& predictor, std::int32_t& index)
		{
			static constexpr std::int32_t StepTable[89] = {
				7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
				50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253,
				279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
				1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
				4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
				16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
			};
			static constexpr std::int32_t IndexTable[16] = {
				-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
			};
			const std::int32_t step = StepTable[index];
			std::int32_t difference = sample - predictor;
			std::uint8_t nibble = 0;
			if (difference < 0) { nibble = 8; difference = -difference; }
			std::int32_t delta = step >> 3;
			if (difference >= step) { nibble |= 4; difference -= step; delta += step; }
			if (difference >= (step >> 1)) { nibble |= 2; difference -= step >> 1; delta += step >> 1; }
			if (difference >= (step >> 2)) { nibble |= 1; delta += step >> 2; }
			predictor += ((nibble & 8) != 0 ? -delta : delta);
			if (predictor < -32768) predictor = -32768;
			else if (predictor > 32767) predictor = 32767;
			index += IndexTable[nibble];
			if (index < 0) index = 0;
			else if (index > 88) index = 88;
			return nibble;
		}

		std::FILE* file_ = nullptr;
		std::unique_ptr<std::uint8_t[]> blockBuffer_;
		std::uint32_t bufferedBytes_ = 0;
		std::uint32_t dataBytes_ = 0;
		std::uint32_t frames_ = 0;
		std::int32_t stepIndex_[2]{};
	};

	bool BakeMusic(StringView sourceDir, StringView cachePath, const PspBakeOptions& options)
	{
		const String musicPath = fs::CombinePath(cachePath, "Music"_s);
		fs::CreateDirectories(musicPath);
		std::vector<PspMusicTrack> tracks;
		for (auto item : fs::Directory(sourceDir, fs::EnumerationOptions::SkipDirectories)) {
			String extension = LowercaseExtension(item);
			if (extension != "j2b"_s && extension != "it"_s && extension != "xm"_s &&
				extension != "s3m"_s && extension != "mod"_s) continue;
			String stem(fs::GetFileNameWithoutExtension(item));
			for (char& c : stem) c = char(std::tolower(static_cast<unsigned char>(c)));
			tracks.push_back({ String(item), fs::CombinePath(musicPath, String(stem + ".wav"_s)),
				String(fs::GetFileName(item)), fs::GetFileSize(item), 0 });
		}
		if (tracks.empty()) {
			std::fprintf(stderr,
				"error: no supported music files found under \"%.*s\" (expected J2B/IT/XM/S3M/MOD)\n",
				(int)sourceDir.size(), sourceDir.data());
			return false;
		}
		// Convert the largest modules first while the heap still has its largest contiguous blocks.
		std::sort(tracks.begin(), tracks.end(), [](const PspMusicTrack& a, const PspMusicTrack& b) {
			return a.SourceSize > b.SourceSize;
		});

		// Estimate ETA from already-complete WAVs, guessing for the rest and correcting as each module
		// loads. A separate libxmp inspection pass would double every large allocation/free cycle.
		constexpr std::uint32_t UnknownTrackEstimateMs = 120000;
		std::uint32_t totalMilliseconds = 0;
		for (auto& track : tracks) {
			if (!ReadCompleteImaDuration(track.Output, track.EstimatedMilliseconds))
				track.EstimatedMilliseconds = UnknownTrackEstimateMs;
			totalMilliseconds += track.EstimatedMilliseconds;
		}

		auto musicProgress = [&](std::uint32_t completed, int completedTracks, const char* current) {
			if (options.MusicProgress != nullptr) options.MusicProgress(options.ProgressUserData,
				std::min(completed, totalMilliseconds), totalMilliseconds, completedTracks,
				(int)tracks.size(), current);
		};
		musicProgress(0, 0, tracks[0].Name.data());
		std::uint32_t completedMilliseconds = 0;
		for (std::size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
			auto& track = tracks[trackIndex];
			std::uint32_t existingDuration = 0;
			if (ReadCompleteImaDuration(track.Output, existingDuration)) {
				completedMilliseconds += existingDuration;
				musicProgress(completedMilliseconds, (int)trackIndex + 1,
					(trackIndex + 1 < tracks.size() ? tracks[trackIndex + 1].Name.data() : "Music complete"));
				continue;
			}
			LogMusicMemory(options, "before context", track);
			xmp_context context = xmp_create_context();
			std::vector<std::uint8_t> decoded;
			if (context == nullptr || !OpenPspMusicModule(track, context, decoded)) {
				if (context != nullptr) xmp_free_context(context);
				LogMusicMemory(options, "module load failed", track);
				return false;
			}
			xmp_frame_info moduleInfo{};
			xmp_get_frame_info(context, &moduleInfo);
			const std::uint32_t actualMilliseconds = std::uint32_t(std::max(moduleInfo.total_time, 1));
			totalMilliseconds = totalMilliseconds - track.EstimatedMilliseconds + actualMilliseconds;
			track.EstimatedMilliseconds = actualMilliseconds;
			LogMusicMemory(options, "module loaded", track);

			const String temporary = String(track.Output + ".tmp"_s);
			fs::RemoveFile(temporary);
			ImaWavWriter writer(temporary);
			// libxmp's frame size varies with module tempo and can exceed 2048 stereo frames (Ending.j2b
			// does), so buffer exactly one ADPCM block and consume each mixer frame in chunks.
			std::vector<std::int16_t> pending(std::size_t(ImaSamplesPerBlock) * 2);
			std::int32_t pendingFrames = 0;
			std::uint64_t renderedFrames = 0;
			const std::uint64_t targetFrames = std::uint64_t(track.EstimatedMilliseconds) * MusicFrequency / 1000;
			std::uint32_t lastReported = 0;
			bool success = writer.IsValid();
			while (success && renderedFrames < targetFrames && xmp_play_frame(context) == 0) {
				xmp_frame_info info{};
				xmp_get_frame_info(context, &info);
				const std::int32_t availableFrames = info.buffer_size / (2 * (std::int32_t)sizeof(std::int16_t));
				const std::int32_t frames = std::int32_t(std::min<std::uint64_t>(availableFrames,
					targetFrames - renderedFrames));
				if (frames <= 0 || info.buffer == nullptr) { success = false; break; }
				const auto* input = static_cast<const std::int16_t*>(info.buffer);
				std::int32_t consumedFrames = 0;
				while (success && consumedFrames < frames) {
					const std::int32_t copiedFrames = std::min(frames - consumedFrames,
						ImaSamplesPerBlock - pendingFrames);
					std::memcpy(pending.data() + pendingFrames * 2, input + consumedFrames * 2,
						std::size_t(copiedFrames) * 2 * sizeof(std::int16_t));
					pendingFrames += copiedFrames;
					consumedFrames += copiedFrames;
					if (pendingFrames == ImaSamplesPerBlock) {
						success = writer.Write(pending.data(), ImaSamplesPerBlock);
						pendingFrames = 0;
					}
				}
				renderedFrames += frames;
				const std::uint32_t currentMs = std::uint32_t(renderedFrames * 1000 / MusicFrequency);
				if (currentMs - lastReported >= 250) {
					musicProgress(completedMilliseconds + std::min(currentMs, track.EstimatedMilliseconds),
						(int)trackIndex, track.Name.data());
					lastReported = currentMs;
				}
			}
			if (success && pendingFrames > 0) success = writer.Write(pending.data(), pendingFrames);
			if (success) success = writer.Finish();
			xmp_end_player(context); xmp_release_module(context); xmp_free_context(context);
			LogMusicMemory(options, "module released", track);
			bool moved = false;
			if (success) moved = ReplaceMusicFile(temporary, track.Output);
			if (options.Log != nullptr) {
				char resultMessage[192];
				std::snprintf(resultMessage, sizeof(resultMessage),
					"music %s: render=%s rename=%s frames=%llu target=%llu", track.Name.data(),
					success ? "ok" : "failed", moved ? "ok" : "failed",
					(unsigned long long)renderedFrames, (unsigned long long)targetFrames);
				options.Log(options.ProgressUserData, resultMessage);
			}
			if (!success || !moved) {
				fs::RemoveFile(temporary);
				std::fprintf(stderr, "error: failed to convert music file %s\n", track.Source.data());
				return false;
			}
			completedMilliseconds += track.EstimatedMilliseconds;
			musicProgress(completedMilliseconds, (int)trackIndex + 1,
				(trackIndex + 1 < tracks.size() ? tracks[trackIndex + 1].Name.data() : "Music complete"));
		}
		return true;
	}

	void WriteBigEndian32(std::FILE* file, std::uint32_t value)
	{
		const std::uint8_t bytes[4] = {
			std::uint8_t(value >> 24), std::uint8_t(value >> 16),
			std::uint8_t(value >> 8), std::uint8_t(value)
		};
		std::fwrite(bytes, 1, sizeof(bytes), file);
	}

	bool WritePngChunk(std::FILE* file, const char type[4], const std::uint8_t* data, std::size_t size)
	{
		WriteBigEndian32(file, (std::uint32_t)size);
		if (std::fwrite(type, 1, 4, file) != 4) return false;
		if (size != 0 && std::fwrite(data, 1, size, file) != size) return false;
		uLong crc = crc32(0, Z_NULL, 0);
		crc = crc32(crc, reinterpret_cast<const Bytef*>(type), 4);
		if (size != 0) crc = crc32(crc, data, (uInt)size);
		WriteBigEndian32(file, (std::uint32_t)crc);
		return !std::ferror(file);
	}

	bool WriteMenuSaveIcon(StringView path, const std::uint8_t* pixels,
		std::int32_t width, std::int32_t height, std::int32_t channels)
	{
		// PSP savedata ICON0.PNG is fixed at 144x80. Unwrap the retail logo out of Menu.Texture.128x128,
		// drop the adjacent tiled fragments, and composite over a menu-palette purple backdrop.
		constexpr std::int32_t IconWidth = 144;
		constexpr std::int32_t IconHeight = 80;
		if (pixels == nullptr || width <= 0 || height <= 0 || channels != 4) return false;

		std::vector<std::uint8_t> scanlines((IconWidth * 4 + 1) * IconHeight);
		for (std::int32_t y = 0; y < IconHeight; ++y) {
			std::uint8_t* row = scanlines.data() + std::size_t(y) * (IconWidth * 4 + 1);
			*row++ = 0; // PNG filter: None
			for (std::int32_t x = 0; x < IconWidth; ++x) {
				const std::int32_t sourceX = (x + width / 2 - 2) % width;
				const std::int32_t sourceY = (y + height - IconHeight + 9) % height;
				const std::uint8_t* source = pixels + (std::size_t(sourceY) * width + sourceX) * 4;
				const bool insideLogo = (x >= 10 && x < IconWidth - 10 && y >= 10 && y < IconHeight - 10);
				const std::uint8_t alpha = (insideLogo ? source[3] : 0);
				const int verticalShade = y * 22 / (IconHeight - 1);
				const std::uint8_t background[3] = {
					std::uint8_t(48 + verticalShade),
					std::uint8_t(4 + verticalShade / 5),
					std::uint8_t(82 + verticalShade)
				};
				for (int channel = 0; channel < 3; ++channel)
					*row++ = std::uint8_t((source[channel] * alpha + background[channel] * (255 - alpha)) / 255);
				*row++ = 255;
			}
		}

		uLongf compressedSize = compressBound((uLong)scanlines.size());
		std::vector<std::uint8_t> compressed(compressedSize);
		if (compress2(compressed.data(), &compressedSize, scanlines.data(), (uLong)scanlines.size(), Z_BEST_COMPRESSION) != Z_OK)
			return false;
		compressed.resize(compressedSize);

		std::FILE* file = std::fopen(String(path).data(), "wb");
		if (file == nullptr) return false;
		static constexpr std::uint8_t Signature[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
		std::fwrite(Signature, 1, sizeof(Signature), file);
		std::uint8_t header[13] = {
			0, 0, 0, IconWidth, 0, 0, 0, IconHeight,
			8, 6, 0, 0, 0 // 8-bit RGBA, deflate, adaptive filtering, no interlace
		};
		const bool success = WritePngChunk(file, "IHDR", header, sizeof(header)) &&
			WritePngChunk(file, "IDAT", compressed.data(), compressed.size()) &&
			WritePngChunk(file, "IEND", nullptr, 0);
		std::fclose(file);
		return success;
	}

	// State for the JJ2Anims::Convert sink, which bakes each converted sprite's GU texture in-place (so
	// Source.pak is never read back) and skips anything the game's metadata does not reference.
	struct SpriteSinkCtx {
		const std::unordered_map<std::string, int>* meta;
		TexturePackStream* stream;
		std::unordered_set<std::uint64_t>* keys;
		StringView cachePath;
		std::size_t count = 0;
		bool failed = false;
		bool hasLori = false;
		bool saveIconWritten = false;
	};
	struct DetailProgressCtx {
		const PspBakeOptions* options;
		PspBakeStage stage;
	};
	struct TilesetPackSinkCtx {
		TexturePackStream* stream;
		const char* name;
		const PspBakeOptions* options;
	};
	bool StoreConvertedTileset(void* context, const std::uint32_t* palette,
		const std::uint8_t* pixels, std::int32_t width, std::int32_t height,
		std::int32_t channelCount, std::int32_t tileCount)
	{
		auto* sink = static_cast<TilesetPackSinkCtx*>(context);
		return AppendTilesetPixels(sink->stream, sink->name, palette, pixels, width, height,
			channelCount, tileCount, sink->options->Log, sink->options->ProgressUserData);
	}
	void ForwardDetailProgress(void* context, int completed, int total)
	{
		auto* detail = static_cast<DetailProgressCtx*>(context);
		if (detail->options->DetailProgress != nullptr)
			detail->options->DetailProgress(detail->options->ProgressUserData, detail->stage, completed, total);
	}
	bool StoreBakedTexture(void* context, BakedTexture&& texture)
	{
		auto* c = static_cast<SpriteSinkCtx*>(context);
		const std::uint64_t key = texture.key;
		if (!AppendTexturePack(c->stream, std::move(texture))) { c->failed = true; return false; }
		c->keys->insert(key);
		c->hasLori = c->hasLori || key == nCine::PspGpu::NameHash("Lori/idle.aura");
		++c->count;
		return true;
	}
	void ConverterSpriteSink(void* ctx, StringView filename, const std::uint8_t* data,
		std::int32_t width, std::int32_t height, std::int32_t channelCount, bool notIndexed,
		const std::uint8_t* auraHeader)
	{
		auto* c = static_cast<SpriteSinkCtx*>(ctx);
		StringView rel = filename;
		if (rel.hasPrefix("Animations/"_s)) rel = rel.exceptPrefix(sizeof("Animations/") - 1);
		std::string key(rel.data(), rel.size());
		auto it = c->meta->find(key);
		if (it == c->meta->end()) return; // not referenced by the game -> don't bake
		if (!c->saveIconWritten && key == "UI/menu128.aura") {
			const String iconPath = fs::CombinePath(c->cachePath, "ICON0.PNG"_s);
			c->saveIconWritten = WriteMenuSaveIcon(iconPath, data, width, height, channelCount);
			if (!c->saveIconWritten) {
				c->failed = true;
				std::fprintf(stderr, "error: failed to bake savedata icon %s\n", iconPath.data());
			}
		}
		const bool isFireShield = (key == "Common/shield_fire.aura");
		const bool isLightningShield = (key == "Common/shield_lightning.aura");
		const bool isWaterShield = (key == "Common/shield_water.aura");
		if ((isFireShield || isLightningShield) && channelCount == 4) {
			BakedTexture bt;
			if (BakeShieldAnimationFromPixels(nCine::PspGpu::NameHash(key.c_str()), data, width, height,
				channelCount, isLightningShield, auraHeader, &bt)) {
				StoreBakedTexture(c, std::move(bt));
			}
			return;
		}
		if (isWaterShield && channelCount == 4) {
			BakedTexture bt;
			if (BakeWaterShieldFromPixels(nCine::PspGpu::NameHash(key.c_str()), data, width, height,
				channelCount, it->second, auraHeader, &bt)) {
				StoreBakedTexture(c, std::move(bt));
			}
			return;
		}
		std::vector<std::uint8_t> filteredPixels;
		if (key == "UI/loading_font_large.aura" && channelCount == 4) {
			// Turn the light part of the retail glyph into coverage; the loading screen draws it solid black.
			constexpr int LoadingFontThreshold = 100;
			filteredPixels.resize((std::size_t)width * height * 4);
			for (std::int32_t i = 0; i < width * height; ++i) {
				const std::uint8_t* src = data + (std::size_t)i * 4;
				std::uint8_t* dst = filteredPixels.data() + (std::size_t)i * 4;
				const int luminance = (54 * src[0] + 183 * src[1] + 19 * src[2]) >> 8;
				const int coverage = std::clamp((luminance - LoadingFontThreshold) * 255 /
					(255 - LoadingFontThreshold), 0, 255);
				dst[0] = dst[1] = dst[2] = 255;
				dst[3] = (std::uint8_t)(src[3] * coverage / 255);
			}
			// Glyphs render at about half this resolution, so box-filter 2x down (identical to the GU's
			// bilinear at a 2x reduction, hence still crisp). Takes the atlas from six 512-pages (~6 MiB) to
			// one non-paged page (~1 MiB); at 6 MiB this transient OOMed level transitions on a 24 MiB heap.
			const std::int32_t dstW = width / 2, dstH = height / 2;
			std::vector<std::uint8_t> scaled((std::size_t)dstW * dstH * 4);
			for (std::int32_t y = 0; y < dstH; ++y) {
				for (std::int32_t x = 0; x < dstW; ++x) {
					std::int32_t a = 0;
					for (std::int32_t dy = 0; dy < 2; ++dy)
						for (std::int32_t dx = 0; dx < 2; ++dx)
							a += filteredPixels[(((std::size_t)(y * 2 + dy) * width) + (x * 2 + dx)) * 4 + 3];
					std::uint8_t* d = scaled.data() + ((std::size_t)y * dstW + x) * 4;
					d[0] = d[1] = d[2] = 255;
					d[3] = (std::uint8_t)(a / 4);
				}
			}
			filteredPixels = std::move(scaled);
			data = filteredPixels.data();
			width = dstW;
			height = dstH;
		}
		BakedTexture bt;
		const bool isMenuLayer = (key == "UI/menu16.aura" || key == "UI/menu32.aura" || key == "UI/menu128.aura");
		std::uint32_t frameWidth = 0, frameHeight = 0;
		std::memcpy(&frameWidth, auraHeader + 13, sizeof(frameWidth));
		std::memcpy(&frameHeight, auraHeader + 17, sizeof(frameHeight));
		const bool includeMask = (auraHeader[11] & 0x02) == 0;
		if (BakeSpriteFromPixels(nCine::PspGpu::NameHash(key.c_str()), data, width, height, channelCount,
			notIndexed, it->second, (int)frameWidth, (int)frameHeight, &bt, isMenuLayer, includeMask)) {
			bt.meta.assign(auraHeader, auraHeader + 39); // frame metadata, parsed verbatim by the device
			StoreBakedTexture(c, std::move(bt));
		}
	}
	bool ShouldBakeConverterSprite(void* ctx, StringView filename)
	{
		auto* c = static_cast<SpriteSinkCtx*>(ctx);
		StringView relative = filename;
		if (relative.hasPrefix("Animations/"_s)) relative = relative.exceptPrefix(sizeof("Animations/") - 1);
		return c->meta->find(std::string(relative.data(), relative.size())) != c->meta->end();
	}
}

int RunPspBake(const PspBakeOptions& options)
{
	const PspBakeOptions& bakeOptions = options;
	String sourceDir(options.SourceDirectory ? options.SourceDirectory : "");
	String outDir(options.OutputDirectory ? options.OutputDirectory : "");
	String contentDir(options.ContentDirectory ? options.ContentDirectory : "");
	const bool musicOnly = options.MusicOnly;
	auto report = [&](int completed, int total, const char* item) {
		if (bakeOptions.Log != nullptr) {
			char message[192];
			std::snprintf(message, sizeof(message), "progress %d/%d: %s", completed, total, item ? item : "");
			bakeOptions.Log(bakeOptions.ProgressUserData, message);
		}
		if (bakeOptions.Progress != nullptr)
			bakeOptions.Progress(bakeOptions.ProgressUserData, completed, total, item);
	};
	auto detail = [&](PspBakeStage stage, int completed, int total) {
		if (bakeOptions.DetailProgress != nullptr)
			bakeOptions.DetailProgress(bakeOptions.ProgressUserData, stage, completed, std::max(total, 1));
	};
	if (sourceDir.empty() || outDir.empty()) {
		std::fprintf(stderr,
			"error: source and output directories are required\n");
		return 2;
	}

	String cachePath = fs::CombinePath(StringView(outDir), "Cache"_s);
	if (!fs::DirectoryExists(cachePath) && !fs::CreateDirectories(cachePath)) {
		std::fprintf(stderr, "error: cannot create cache directory \"%s\"\n", cachePath.data());
		return 1;
	}
	if (musicOnly) return (BakeMusic(sourceDir, cachePath, bakeOptions) ? 0 : 1);

	int progressCompleted = 0;
	int progressTotal = 5; // animations, data/audio, music, descriptors, final texture pack
	int episodeTotal = 0, levelTotal = 0;
	std::size_t bakedTilesetCount = 0;
	for (auto item : fs::Directory(fs::FindPathCaseInsensitive(StringView(sourceDir)), fs::EnumerationOptions::SkipDirectories)) {
		String extension = LowercaseExtension(item);
		if (extension == "j2e"_s || extension == "j2pe"_s) { ++progressTotal; ++episodeTotal; }
		else if (extension == "j2l"_s) { ++progressTotal; ++levelTotal; }
	}
	report(progressCompleted, progressTotal, "Scanning original game files");

	// ================================================================================================
	//  Animations / sprites / data -> texture.pak + audio.pak
	// ================================================================================================
	String animsPath = fs::FindPathCaseInsensitive(fs::CombinePath(StringView(sourceDir), "Anims.j2a"_s));
	if (!fs::IsReadableFile(animsPath)) {
		animsPath = fs::FindPathCaseInsensitive(fs::CombinePath(StringView(sourceDir), "AnimsSw.j2a"_s));
	}
	if (!fs::IsReadableFile(animsPath)) {
		std::fprintf(stderr, "error: no Anims.j2a / AnimsSw.j2a under \"%s\"\n", sourceDir.data());
		return 1;
	}

	// Sprite metadata (asset path -> palette offset) must be read up-front so the converter sink can bake
	// each sprite with the right format/offset as it is generated, avoiding any Source.pak read-back.
	String packPath = fs::CombinePath(StringView(cachePath), "texture.pak"_s);
	TexturePackStream* textureStream = CreateTexturePackStream(packPath.data());
	if (textureStream == nullptr) {
		std::fprintf(stderr, "error: cannot create texture pack %s\n", packPath.data());
		return 1;
	}
	std::unique_ptr<TexturePackStream, void(*)(TexturePackStream*)> textureStreamOwner(textureStream, DestroyTexturePackStream);
	std::unordered_set<std::uint64_t> bakedKeys;
	std::unordered_map<std::string, int> spriteMeta;
	if (!contentDir.empty()) {
		CollectSpriteMetadata(contentDir.data(), spriteMeta);
		if (spriteMeta.empty()) {
			std::fprintf(stderr, "error: no sprite metadata found under \"%s\"\n", contentDir.data());
			return 1;
		}
		// The PSP-native menu draws these JJ2 animations directly, bypassing the desktop menu's metadata
		// tree, so nothing else pulls them into the pack.
		for (const char* path : { "UI/multiplayer_char.aura", "UI/multiplayer_color.aura",
			"UI/multiplayer_mode.aura", "UI/character_art_jazz.aura", "UI/character_art_lori.aura",
			"UI/character_art_spaz.aura", "UI/logo_large.aura" }) {
			spriteMeta.emplace(path, 0);
		}
		std::printf("Sprite metadata: %zu animations\n", (std::size_t)spriteMeta.size());
	}
	SpriteSinkCtx sinkCtx{ &spriteMeta, textureStream, &bakedKeys, cachePath };

	// No Source.pak is generated: sprites go through the sink into texture.pak, audio into audio.pak, and
	// the device reads nothing else.
	JJ2Anims::SpriteSink sink = (contentDir.empty() ? nullptr : ConverterSpriteSink);
	JJ2Version version;
	{
		String audioPakPath = fs::CombinePath(StringView(cachePath), "audio.pak"_s);
		std::printf("Converting animations (sprites -> texture pack, audio -> %s)\n", audioPakPath.data());
		PakWriter audioPak(audioPakPath, /*useHashIndex*/ true, false, false, false, 64 * 1024);
		if (!audioPak.IsValid()) {
			std::fprintf(stderr, "error: cannot open \"%s\" for writing\n", audioPakPath.data());
			return 1;
		}

		DetailProgressCtx animationProgress{ &bakeOptions, PspBakeStage::Animations };
		version = JJ2Anims::Convert(animsPath, audioPak, false, sink, &sinkCtx,
			ForwardDetailProgress, &animationProgress,
			(contentDir.empty() ? nullptr : ShouldBakeConverterSprite), &sinkCtx);
		if (version == JJ2Version::Unknown) {
			std::fprintf(stderr, "error: unsupported JJ2 version in \"%s\"\n", animsPath.data());
			return 1;
		}
		report(++progressCompleted, progressTotal, "Animations and sound effects");

		detail(PspBakeStage::GameData, 0, 1);
		const String dataPath = fs::FindPathCaseInsensitive(fs::CombinePath(StringView(sourceDir), "Data.j2d"_s));
		JJ2Data data;
		if (!fs::IsReadableFile(dataPath) || !data.Open(dataPath, false)) {
			std::fprintf(stderr, "error: missing or invalid Data.j2d under \"%s\"\n", sourceDir.data());
			return 1;
		}
		std::printf("Converting Data.j2d\n");
		data.Convert(audioPak, version, sink, &sinkCtx);
		audioPak.Finalize();
		detail(PspBakeStage::GameData, 1, 1);
		report(++progressCompleted, progressTotal, "Game sound data");
	}
	std::printf("Detected %s\n", VersionName(version));

	// ================================================================================================
	//  Levels / episodes / tilesets  (lifted from GameEventHandler::RefreshCacheLevels)
	// ================================================================================================
	int nativeTilesetTotal = 0;
	{
	EventConverter eventConverter;

	bool hasChristmasChronicles = fs::IsReadableFile(fs::FindPathCaseInsensitive(fs::CombinePath(StringView(sourceDir), "xmas99.j2e"_s)));
	const HashMap<String, Pair<String, String>> knownLevels = {
		{ "trainer"_s, { "prince"_s, {} } },
		{ "castle1"_s, { "prince"_s, "01"_s } }, { "castle1n"_s, { "prince"_s, "02"_s } },
		{ "carrot1"_s, { "prince"_s, "03"_s } }, { "carrot1n"_s, { "prince"_s, "04"_s } },
		{ "labrat1"_s, { "prince"_s, "05"_s } }, { "labrat2"_s, { "prince"_s, "06"_s } },
		{ "labrat3"_s, { "prince"_s, "bonus"_s } },
		{ "colon1"_s, { "rescue"_s, "01"_s } }, { "colon2"_s, { "rescue"_s, "02"_s } },
		{ "psych1"_s, { "rescue"_s, "03"_s } }, { "psych2"_s, { "rescue"_s, "04"_s } },
		{ "beach"_s, { "rescue"_s, "05"_s } }, { "beach2"_s, { "rescue"_s, "06"_s } },
		{ "psych3"_s, { "rescue"_s, "bonus"_s } },
		{ "diam1"_s, { "flash"_s, "01"_s } }, { "diam3"_s, { "flash"_s, "02"_s } },
		{ "tube1"_s, { "flash"_s, "03"_s } }, { "tube2"_s, { "flash"_s, "04"_s } },
		{ "medivo1"_s, { "flash"_s, "05"_s } }, { "medivo2"_s, { "flash"_s, "06"_s } },
		{ "garglair"_s, { "flash"_s, "bonus"_s } }, { "tube3"_s, { "flash"_s, "bonus"_s } },
		{ "jung1"_s, { "monk"_s, "01"_s } }, { "jung2"_s, { "monk"_s, "02"_s } },
		{ "hell"_s, { "monk"_s, "03"_s } }, { "hell2"_s, { "monk"_s, "04"_s } },
		{ "damn"_s, { "monk"_s, "05"_s } }, { "damn2"_s, { "monk"_s, "06"_s } },
		{ "share1"_s, { "share"_s, "01"_s } }, { "share2"_s, { "share"_s, "02"_s } },
		{ "share3"_s, { "share"_s, "03"_s } },
		{ "xmas1"_s, { "xmas99"_s, "01"_s } }, { "xmas2"_s, { "xmas99"_s, "02"_s } },
		{ "xmas3"_s, { "xmas99"_s, "03"_s } },
		{ "easter1"_s, { "secretf"_s, "01"_s } }, { "easter2"_s, { "secretf"_s, "02"_s } },
		{ "easter3"_s, { "secretf"_s, "03"_s } }, { "haunted1"_s, { "secretf"_s, "04"_s } },
		{ "haunted2"_s, { "secretf"_s, "05"_s } }, { "haunted3"_s, { "secretf"_s, "06"_s } },
		{ "town1"_s, { "secretf"_s, "07"_s } }, { "town2"_s, { "secretf"_s, "08"_s } },
		{ "town3"_s, { "secretf"_s, "09"_s } },
		{ "end"_s, { {}, ":end"_s } }, { "endepis"_s, { {}, ":end"_s } },
		{ "ending"_s, { {}, ":credits"_s } }
	};

	auto LevelTokenConversion = [&knownLevels](StringView levelToken) -> JJ2Level::LevelToken {
		auto it = knownLevels.find(String::nullTerminatedView(levelToken));
		if (it != knownLevels.end()) {
			if (it->second.second().empty()) {
				return { it->second.first(), levelToken };
			}
			return { it->second.first(), (it->second.second()[0] == ':' ? it->second.second() : (it->second.second() + "_"_s + levelToken)) };
		}
		return { {}, levelToken };
	};

	auto EpisodeNameConversion = [](JJ2Episode* episode) -> String {
		if (episode->Name == "share"_s && episode->DisplayName == "#Shareware@Levels"_s) {
			return "Shareware Demo"_s;
		} else if (episode->Name == "xmas98"_s && episode->DisplayName == "#Xmas 98@Levels"_s) {
			return "Holiday Hare '98"_s;
		} else if (episode->Name == "xmas99"_s && episode->DisplayName == "#Xmas 99@Levels"_s) {
			return "The Christmas Chronicles"_s;
		} else if (episode->Name == "secretf"_s && episode->DisplayName == "#Secret@Files"_s) {
			return "The Secret Files"_s;
		} else {
			return JJ2Strings::RecodeString(episode->DisplayName, true);
		}
	};

	auto EpisodePrevNext = [](JJ2Episode* episode) -> Pair<String, String> {
		if (episode->Name == "prince"_s) { return { {}, "rescue"_s }; }
		else if (episode->Name == "rescue"_s) { return { "prince"_s, "flash"_s }; }
		else if (episode->Name == "flash"_s) { return { "rescue"_s, "monk"_s }; }
		else if (episode->Name == "monk"_s) { return { "flash"_s, {} }; }
		else { return { {}, {} }; }
	};

	String episodesPath = fs::CombinePath(StringView(cachePath), "Episodes"_s);
	fs::RemoveDirectoryRecursive(episodesPath);
	fs::CreateDirectories(episodesPath);

	HashMap<String, bool> usedTilesets;
	std::vector<BakedLevelEntry> bakedLevels;
	std::printf("Converting levels and episodes...\n");
	int episodesConverted = 0, levelsConverted = 0;
	if (episodeTotal > 0) detail(PspBakeStage::Episodes, 0, episodeTotal);

	for (auto item : fs::Directory(fs::FindPathCaseInsensitive(StringView(sourceDir)), fs::EnumerationOptions::SkipDirectories)) {
		String extension = LowercaseExtension(item);
		if (extension == "j2e"_s || extension == "j2pe"_s) {
			JJ2Episode episode;
			if (!episode.Open(item)) {
				std::fprintf(stderr, "error: cannot parse episode \"%s\"\n", String(item).data());
				return 1;
			}
			if (hasChristmasChronicles && episode.Name == "xmas98"_s) {
				continue;
			}
			if (episode.Name == "home"_s) {
				episode.FirstLevel = ":custom-levels"_s;
				episode.Position = UINT16_MAX;
			} else if (episode.Position >= UINT32_MAX) {
				episode.Position = UINT16_MAX - 1;
			}
			String fullPath = fs::CombinePath(episodesPath, String((episode.Name == "xmas98"_s ? "xmas99"_s : StringView(episode.Name)) + ".j2e"_s));
			episode.Convert(fullPath, std::move(LevelTokenConversion), std::move(EpisodeNameConversion), std::move(EpisodePrevNext));
			if (!fs::IsReadableFile(fullPath)) {
				std::fprintf(stderr, "error: episode conversion did not produce \"%s\"\n", fullPath.data());
				return 1;
			}
			report(++progressCompleted, progressTotal, "Episodes");
			detail(PspBakeStage::Episodes, ++episodesConverted, episodeTotal);
		} else if (extension == "j2l"_s) {
			if (levelsConverted == 0) detail(PspBakeStage::Levels, 0, levelTotal);
			String levelName = fs::GetFileNameWithoutExtension(item);
			if (levelName.find("-MLLE-Data-"_s) == nullptr) {
				JJ2Level level;
				if (!level.Open(item, false)) {
					std::fprintf(stderr, "error: cannot parse level \"%s\"\n", String(item).data());
					return 1;
				}
				String fullPath;
				String canonicalName;
				auto it = knownLevels.find(level.LevelName);
				if (it != knownLevels.end()) {
					if (it->second.second().empty()) {
						fullPath = fs::CombinePath({ episodesPath, it->second.first(), String(level.LevelName + ".j2l"_s) });
						if (!it->second.first().empty()) canonicalName = String(it->second.first() + '/' + level.LevelName);
					} else {
						fullPath = fs::CombinePath({ episodesPath, it->second.first(), String(it->second.second() + '_' + level.LevelName + ".j2l"_s) });
						if (!it->second.first().empty()) canonicalName = String(it->second.first() + '/' + it->second.second() + '_' + level.LevelName);
					}
				} else {
					fullPath = fs::CombinePath({ episodesPath, "unknown"_s, String(level.LevelName + ".j2l"_s) });
					canonicalName = String("unknown/"_s + level.LevelName);
				}
				String displayName = JJ2Strings::RecodeString(level.DisplayName, true);
				if (displayName.empty()) displayName = level.LevelName;

				fs::CreateDirectories(fs::GetDirectoryName(fullPath));
				level.Convert(fullPath, eventConverter, LevelTokenConversion);
				if (!fs::IsReadableFile(fullPath)) {
					std::fprintf(stderr, "error: level conversion did not produce \"%s\"\n", fullPath.data());
					return 1;
				}
				if (!canonicalName.empty() && !level.IsHidden())
					bakedLevels.push_back({std::move(canonicalName), std::move(displayName)});

				usedTilesets.emplace(level.Tileset, true);
				for (auto& extraTileset : level.ExtraTilesets) {
					usedTilesets.emplace(extraTileset.Name, true);
				}

				StringView foundDot = item.findLastOr('.', item.end());
				String scriptPath = item.prefix(foundDot.begin()) + ".j2as"_s;
				auto adjustedPath = fs::FindPathCaseInsensitive(scriptPath);
				if (fs::IsReadableFile(adjustedPath)) {
					foundDot = fullPath.findLastOr('.', fullPath.end());
					if (!fs::Copy(adjustedPath, String(fullPath.prefix(foundDot.begin()) + ".j2as"_s))) {
						std::fprintf(stderr, "error: cannot copy level script \"%s\"\n", adjustedPath.data());
						return 1;
					}
				}
			}
			report(++progressCompleted, progressTotal, "Levels");
			detail(PspBakeStage::Levels, ++levelsConverted, levelTotal);
		}
	}

	// Write descriptors before the two-part tileset stage so its detail counter can run continuously.
	detail(PspBakeStage::CacheIndex, 0, 1);
	std::int64_t animsModified = fs::GetLastModificationTime(animsPath).ToUnixMilliseconds();
	const String cacheIndexPath = fs::CombinePath(StringView(cachePath), "Source.idx"_s);
	const String contentDescriptorPath = fs::CombinePath(StringView(cachePath), "Source.version"_s);
	const String levelCatalogPath = fs::CombinePath(StringView(cachePath), "Levels.idx"_s);
	if (!WriteCacheDescriptor(cacheIndexPath, nCine::parseVersion(NCINE_VERSION ""_s), animsModified) ||
		!WriteContentDescriptor(contentDescriptorPath, version, sinkCtx.hasLori) ||
		!WriteLevelCatalog(levelCatalogPath, bakedLevels)) {
		std::fprintf(stderr, "error: failed to write cache descriptors under \"%s\"\n", cachePath.data());
		return 1;
	}
	detail(PspBakeStage::CacheIndex, 1, 1);
	report(++progressCompleted, progressTotal, "Writing cache index");

	// Convert only the tilesets actually used by the converted levels
	{
		std::printf("Converting %zu used tilesets...\n", (std::size_t)usedTilesets.size());
		nativeTilesetTotal = (int)usedTilesets.size();
		progressTotal += (int)usedTilesets.size();
		detail(PspBakeStage::NativeTilesets, 0, nativeTilesetTotal);
		report(progressCompleted, progressTotal, "Preparing tilesets");
		String tilesetsPath = fs::CombinePath(StringView(cachePath), "Tilesets"_s);
		fs::RemoveDirectoryRecursive(tilesetsPath);
		fs::CreateDirectories(tilesetsPath);
		int tilesetsConverted = 0;
		for (auto& pair : usedTilesets) {
			String tilesetPath = fs::CombinePath(StringView(sourceDir), String(pair.first + ".j2t"_s));
			auto adjustedPath = fs::FindPathCaseInsensitive(tilesetPath);
			if (!fs::IsReadableFile(adjustedPath)) {
				std::fprintf(stderr, "error: level references missing tileset \"%s\"\n", tilesetPath.data());
				return 1;
			}
			JJ2Tileset tileset;
			if (!tileset.Open(adjustedPath, false)) {
				std::fprintf(stderr, "error: cannot parse tileset \"%s\"\n", adjustedPath.data());
				return 1;
			}
			const String convertedPath = fs::CombinePath({ tilesetsPath, String(pair.first + ".j2tpsp"_s) });
			TilesetPackSinkCtx packSink{ textureStream, pair.first.data(), &bakeOptions };
			if (!tileset.Convert(convertedPath, false, StoreConvertedTileset, &packSink) ||
				!fs::IsReadableFile(convertedPath)) {
				std::fprintf(stderr, "error: tileset conversion did not produce \"%s\"\n", convertedPath.data());
				return 1;
			}
			++bakedTilesetCount;
			report(++progressCompleted, progressTotal, "Tilesets");
			detail(PspBakeStage::NativeTilesets, ++tilesetsConverted, nativeTilesetTotal);
		}
	}
	} // Release level maps, conversion callbacks, and the used-tileset set before graphics decoding.

	// ================================================================================================
	//  Stage 2: finish the PSP-GU texture pack. Converter sprites and tilesets are already spooled;
	//  append the bundled Content/Animations sprites and close it out.
	// ================================================================================================
	{
		const std::size_t spriteFromConverter = sinkCtx.count;

		int skipped = 0;
		const std::size_t tilesetCount = bakedTilesetCount;

		// Bundled sprites (Content/Animations) not produced by the converter.
		if (!contentDir.empty()) {
			DetailProgressCtx bundledProgress{ &bakeOptions, PspBakeStage::BundledSprites };
			const int bundledResult = BakeBundledSpritesToSink(contentDir.data(), spriteMeta, bakedKeys,
				StoreBakedTexture, &sinkCtx, ForwardDetailProgress, &bundledProgress);
			if (bundledResult < 0) {
				std::fprintf(stderr, "error: failed to decode %d bundled sprite asset(s)\n", -bundledResult);
				return 1;
			}
		} else {
			std::printf("NOTE: --content not given; only converter sprites baked (pass --content <repo Content dir>)\n");
		}
		const std::size_t spriteCount = sinkCtx.count;
		// Return the buckets to the heap before pack finalization and libxmp; clear() would keep them.
		std::unordered_map<std::string, int>().swap(spriteMeta);
		std::unordered_set<std::uint64_t>().swap(bakedKeys);

		detail(PspBakeStage::FinalizingGraphics, 0, 1);
		if (!sinkCtx.failed && FinishTexturePack(textureStream, bakeOptions.Log, bakeOptions.ProgressUserData)) {
			std::printf("Baked PSP texture pack: %zu tilesets + %zu sprites (%zu via converter) (%d tilesets skipped) -> %s\n",
				tilesetCount, spriteCount, spriteFromConverter, skipped, packPath.data());
		} else {
			std::fprintf(stderr, "error: failed to write %s\n", packPath.data());
			return 1;
		}
		detail(PspBakeStage::FinalizingGraphics, 1, 1);
		report(++progressCompleted, progressTotal, "Finalizing graphics pack");
	}
	// FinishTexturePack closes the file, so release its entry/page vectors before libxmp starts allocating.
	textureStreamOwner.reset();

	// Music runs last on purpose: libxmp allocates module/sample state and a full retail cache writes ~190
	// MiB, so doing it after every other converter keeps heap fragmentation and a nearly-full Memory Stick
	// from taking down stages that cannot recover as cleanly.
	if (!BakeMusic(sourceDir, cachePath, bakeOptions)) return 1;
	report(++progressCompleted, progressTotal, "Music");

	std::printf("Done. PSP Cache/ written to %s\n", cachePath.data());
	return 0;
}
