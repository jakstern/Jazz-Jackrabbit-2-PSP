#include "PspBake.h"

#include <Containers/String.h>
#include <Containers/StringConcatenable.h>
#include <IO/FileSystem.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(DEATH_TARGET_WINDOWS)
#	include <windows.h>
#	include <shellapi.h>
#	include <io.h>
#else
#	include <unistd.h>
#endif

#if !defined(PSP_BAKE_VERSION)
#	define PSP_BAKE_VERSION "0.1.0"
#endif

using Death::Containers::String;
using Death::Containers::StringView;
using namespace Death::Containers::Literals;
using fs = Death::IO::FileSystem;

namespace
{
	struct CliOptions {
		std::string Source;
		std::string Output;
		std::string Content;
		bool MusicOnly = false;
		bool Clean = false;
		bool ValidateOnly = false;
		bool Verbose = false;
	};

	struct ConsoleProgress {
		bool Verbose = false;
		int LastPercent[9] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
		int LastMusicPercent = -1;
		int LastMusicTrack = -1;
	};

	const char* StageName(PspBakeStage stage)
	{
		switch (stage) {
			case PspBakeStage::Animations: return "Animations";
			case PspBakeStage::GameData: return "Game data";
			case PspBakeStage::Episodes: return "Episodes";
			case PspBakeStage::Levels: return "Levels";
			case PspBakeStage::NativeTilesets: return "Tilesets";
			case PspBakeStage::BundledSprites: return "Bundled sprites";
			case PspBakeStage::FinalizingGraphics: return "Graphics pack";
			case PspBakeStage::CacheIndex: return "Cache index";
			case PspBakeStage::Music: return "Music";
		}
		return "Assets";
	}

	void PrintHelp()
	{
		std::printf(
			"psp-bake %s - prepare Jazz Jackrabbit 2 assets for PSP\n\n"
			"Usage:\n"
			"  psp-bake --source <directory> --output <directory> [options]\n\n"
			"Required:\n"
			"  -s, --source <directory>   Original JJ2 Source directory, or its parent\n"
			"  -o, --output <directory>  Destination; generated files go in <directory>/Cache\n\n"
			"Options:\n"
			"  -c, --content <directory> Bundled engine Content directory\n"
			"                             (normally detected beside the executable)\n"
			"      --music-only          Convert/resume music in an existing Cache\n"
			"      --clean               Remove the destination Cache before baking\n"
			"      --validate-only       Check inputs without converting anything\n"
			"  -v, --verbose             Print detailed converter diagnostics\n"
			"  -h, --help                Show this help\n"
			"      --version             Show version information\n\n"
			"Supported original releases: shareware, Jazz Jackrabbit 2, Holiday Hare,\n"
			"The Secret Files, and The Christmas Chronicles. Music is decoded in-process\n"
			"from J2B/IT/XM/S3M/MOD and written as PSP-streamable IMA ADPCM. MO3 is not\n"
			"supported. Existing complete music files are resumed automatically.\n",
			PSP_BAKE_VERSION);
	}

	bool ReadValue(const std::vector<std::string>& args, std::size_t& index, const char* option,
		std::string& destination)
	{
		if (index + 1 >= args.size()) {
			std::fprintf(stderr, "error: %s requires a directory argument\n", option);
			return false;
		}
		destination = args[++index];
		if (destination.empty()) {
			std::fprintf(stderr, "error: %s cannot be empty\n", option);
			return false;
		}
		return true;
	}

	int ParseArguments(const std::vector<std::string>& args, CliOptions& options)
	{
		for (std::size_t i = 1; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-h" || arg == "--help") { PrintHelp(); return 1; }
			if (arg == "--version") { std::printf("psp-bake %s\n", PSP_BAKE_VERSION); return 1; }
			if (arg == "-s" || arg == "--source") {
				if (!ReadValue(args, i, arg.c_str(), options.Source)) return -1;
			} else if (arg == "-o" || arg == "--output" || arg == "--out") {
				if (!ReadValue(args, i, arg.c_str(), options.Output)) return -1;
			} else if (arg == "-c" || arg == "--content") {
				if (!ReadValue(args, i, arg.c_str(), options.Content)) return -1;
			} else if (arg == "--music-only") options.MusicOnly = true;
			else if (arg == "--clean") options.Clean = true;
			else if (arg == "--validate-only") options.ValidateOnly = true;
			else if (arg == "-v" || arg == "--verbose") options.Verbose = true;
			else {
				std::fprintf(stderr, "error: unknown option \"%s\"\nTry 'psp-bake --help' for usage.\n", arg.c_str());
				return -1;
			}
		}
		if (options.Source.empty() || options.Output.empty()) {
			std::fprintf(stderr, "error: --source and --output are required\nTry 'psp-bake --help' for usage.\n");
			return -1;
		}
		if (options.MusicOnly && options.Clean) {
			std::fprintf(stderr, "error: --music-only and --clean cannot be used together\n");
			return -1;
		}
		return 0;
	}

	bool HasAnimations(StringView source)
	{
		return fs::IsReadableFile(fs::FindPathCaseInsensitive(fs::CombinePath(source, "Anims.j2a"_s))) ||
			fs::IsReadableFile(fs::FindPathCaseInsensitive(fs::CombinePath(source, "AnimsSw.j2a"_s)));
	}

	bool HasContent(StringView content)
	{
		return fs::DirectoryExists(fs::CombinePath(content, "Animations"_s)) &&
			fs::DirectoryExists(fs::CombinePath(content, "Metadata"_s));
	}

	struct SourceInventory {
		int Music = 0;
		int Levels = 0;
		int Tilesets = 0;
	};

	SourceInventory InspectSource(StringView source)
	{
		SourceInventory inventory;
		for (auto item : fs::Directory(source, fs::EnumerationOptions::SkipDirectories)) {
			String extension(fs::GetExtension(item));
			for (char& c : extension) c = char(std::tolower(static_cast<unsigned char>(c)));
			if (extension == "j2b"_s || extension == "it"_s || extension == "xm"_s ||
				extension == "s3m"_s || extension == "mod"_s) ++inventory.Music;
			else if (extension == "j2l"_s) ++inventory.Levels;
			else if (extension == "j2t"_s) ++inventory.Tilesets;
		}
		return inventory;
	}

	std::string ToStdString(StringView value)
	{
		return std::string(value.data(), value.size());
	}

	String AbsoluteOrOriginal(const std::string& value)
	{
		String absolute = fs::GetAbsolutePath(StringView(value.c_str()));
		return absolute.empty() ? String(value.c_str()) : std::move(absolute);
	}

	bool ResolveAndValidate(CliOptions& options)
	{
		String source = AbsoluteOrOriginal(options.Source);
		if (!HasAnimations(source)) {
			String child = fs::FindPathCaseInsensitive(fs::CombinePath(source, "Source"_s));
			if (HasAnimations(child)) source = std::move(child);
		}
		if (!fs::DirectoryExists(source)) {
			std::fprintf(stderr, "error: source directory does not exist: \"%s\"\n", source.data());
			return false;
		}
		if (!HasAnimations(source)) {
			std::fprintf(stderr,
				"error: \"%s\" is not a JJ2 Source directory\n"
				"       Expected Anims.j2a or AnimsSw.j2a there (or in a Source child directory).\n",
				source.data());
			return false;
		}
		options.Source = ToStdString(source);
		const SourceInventory inventory = InspectSource(source);
		if (inventory.Music == 0) {
			std::fprintf(stderr,
				"error: source contains no supported music (expected J2B/IT/XM/S3M/MOD): \"%s\"\n",
				source.data());
			return false;
		}
		if (!options.MusicOnly) {
			const String dataPath = fs::FindPathCaseInsensitive(fs::CombinePath(source, "Data.j2d"_s));
			if (!fs::IsReadableFile(dataPath)) {
				std::fprintf(stderr, "error: source is missing Data.j2d: \"%s\"\n", source.data());
				return false;
			}
			if (inventory.Levels == 0 || inventory.Tilesets == 0) {
				std::fprintf(stderr,
					"error: source is incomplete (found %d levels and %d tilesets): \"%s\"\n",
					inventory.Levels, inventory.Tilesets, source.data());
				return false;
			}
		}

		String output = AbsoluteOrOriginal(options.Output);
		if (fs::Exists(output) && !fs::DirectoryExists(output)) {
			std::fprintf(stderr, "error: output path is not a directory: \"%s\"\n", output.data());
			return false;
		}
		if (!fs::DirectoryExists(output) && !options.ValidateOnly && !fs::CreateDirectories(output)) {
			std::fprintf(stderr, "error: cannot create output directory: \"%s\"\n", output.data());
			return false;
		}
		if (fs::DirectoryExists(output) && !fs::IsWritable(output)) {
			std::fprintf(stderr, "error: output directory is not writable: \"%s\"\n", output.data());
			return false;
		}
		options.Output = ToStdString(output);

		if (options.MusicOnly) {
			String cache = fs::CombinePath(output, "Cache"_s);
			if (!fs::DirectoryExists(cache)) {
				std::fprintf(stderr, "error: --music-only requires an existing cache at \"%s\"\n", cache.data());
				return false;
			}
			return true;
		}

		std::vector<String> candidates;
		if (!options.Content.empty()) candidates.emplace_back(AbsoluteOrOriginal(options.Content));
		else {
			const String executableDir = fs::GetDirectoryName(fs::GetExecutablePath());
			const String workingDir = fs::GetWorkingDirectory();
			candidates.emplace_back(fs::CombinePath(executableDir, "Content"_s));
			candidates.emplace_back(fs::CombinePath({ executableDir, ".."_s, "share"_s, "psp-bake"_s, "Content"_s }));
			candidates.emplace_back(fs::CombinePath({ executableDir, ".."_s, ".."_s, ".."_s, "Content"_s }));
			candidates.emplace_back(fs::CombinePath(workingDir, "Content"_s));
		}
		for (const String& candidate : candidates) {
			if (HasContent(candidate)) {
				options.Content = ToStdString(fs::GetAbsolutePath(candidate));
				break;
			}
		}
		if (options.Content.empty() || !HasContent(StringView(options.Content.c_str()))) {
			std::fprintf(stderr,
				"error: engine Content assets were not found\n"
				"       Keep the packaged Content directory beside psp-bake, or pass --content <directory>.\n");
			return false;
		}
		return true;
	}

	void ReportProgress(void*, int completed, int total, const char* item)
	{
		if (completed == 0 && item != nullptr && item[0] != '\0') std::printf("Preparing: %s\n", item);
		(void)total;
	}

	void ReportDetail(void* userData, PspBakeStage stage, int completed, int total)
	{
		auto* state = static_cast<ConsoleProgress*>(userData);
		const int safeTotal = std::max(total, 1);
		const int percent = std::clamp(completed * 100 / safeTotal, 0, 100);
		const int slot = static_cast<int>(stage);
		if (completed != 0 && completed != total && percent < state->LastPercent[slot] + 10) return;
		if (percent == state->LastPercent[slot]) return;
		state->LastPercent[slot] = percent;
		std::printf("[%3d%%] %-17s %d / %d\n", percent, StageName(stage), completed, total);
	}

	void ReportMusic(void* userData, std::uint32_t completedMilliseconds, std::uint32_t totalMilliseconds,
		int completedTracks, int totalTracks, const char* currentTrack)
	{
		auto* state = static_cast<ConsoleProgress*>(userData);
		const int percent = totalMilliseconds == 0 ? 0 :
			int(std::min<std::uint64_t>(100, std::uint64_t(completedMilliseconds) * 100 / totalMilliseconds));
		if (completedTracks == state->LastMusicTrack && percent != 100 && percent < state->LastMusicPercent + 5) return;
		state->LastMusicTrack = completedTracks;
		state->LastMusicPercent = percent;
		std::printf("[%3d%%] Music             %d / %d tracks%s%s\n", percent, completedTracks, totalTracks,
			currentTrack != nullptr ? " - " : "", currentTrack != nullptr ? currentTrack : "");
	}

	void ReportLog(void* userData, const char* message)
	{
		if (static_cast<ConsoleProgress*>(userData)->Verbose && message != nullptr)
			std::printf("  %s\n", message);
	}

	int RunCli(const std::vector<std::string>& args)
	{
		CliOptions cli;
		const int parseResult = ParseArguments(args, cli);
		if (parseResult > 0) return 0;
		if (parseResult < 0) return 2;
		if (!ResolveAndValidate(cli)) return 2;

		std::printf("psp-bake %s\nSource:  %s\nOutput:  %s/Cache\n",
			PSP_BAKE_VERSION, cli.Source.c_str(), cli.Output.c_str());
		if (!cli.MusicOnly) std::printf("Content: %s\n", cli.Content.c_str());
		if (cli.ValidateOnly) {
			std::printf("Validation successful. No files were written.\n");
			return 0;
		}

		const String cachePath = fs::CombinePath(StringView(cli.Output.c_str()), "Cache"_s);
		if (cli.Clean && fs::DirectoryExists(cachePath)) {
			std::printf("Removing existing cache: %s\n", cachePath.data());
			if (!fs::RemoveDirectoryRecursive(cachePath)) {
				std::fprintf(stderr, "error: cannot remove existing cache: \"%s\"\n", cachePath.data());
				return 1;
			}
		}

		ConsoleProgress progress;
		progress.Verbose = cli.Verbose;
		PspBakeOptions options{};
		options.SourceDirectory = cli.Source.c_str();
		options.OutputDirectory = cli.Output.c_str();
		options.ContentDirectory = cli.MusicOnly ? nullptr : cli.Content.c_str();
		options.MusicOnly = cli.MusicOnly;
		options.Progress = ReportProgress;
		options.MusicProgress = ReportMusic;
		options.DetailProgress = ReportDetail;
		options.Log = cli.Verbose ? ReportLog : nullptr;
		options.ProgressUserData = &progress;

		const auto started = std::chrono::steady_clock::now();
		const int result = RunPspBake(options);
		const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::steady_clock::now() - started).count();
		if (result == 0) {
			std::printf("Bake completed successfully in %lld:%02lld.\n",
				(long long)(seconds / 60), (long long)(seconds % 60));
			return 0;
		}
		std::fprintf(stderr,
			"psp-bake failed after %lld:%02lld. The message above identifies the failing asset or stage.\n",
			(long long)(seconds / 60), (long long)(seconds % 60));
		return result == 2 ? 2 : 1;
	}
}

#if defined(DEATH_TARGET_WINDOWS)
int main()
{
	int count = 0;
	wchar_t** wideArguments = CommandLineToArgvW(GetCommandLineW(), &count);
	if (wideArguments == nullptr) {
		std::fprintf(stderr, "error: Windows could not decode the command line\n");
		return 2;
	}
	std::vector<std::string> arguments;
	arguments.reserve(count);
	for (int i = 0; i < count; ++i) {
		String utf8 = Death::Utf8::FromUtf16(wideArguments[i]);
		arguments.emplace_back(utf8.data(), utf8.size());
	}
	LocalFree(wideArguments);
	return RunCli(arguments);
}
#else
int main(int argc, char** argv)
{
	std::vector<std::string> arguments;
	arguments.reserve(argc);
	for (int i = 0; i < argc; ++i) arguments.emplace_back(argv[i]);
	return RunCli(arguments);
}
#endif
