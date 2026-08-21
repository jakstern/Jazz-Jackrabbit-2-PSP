#include "JJ2Data.h"
#include "JJ2Anims.h"
#include "JJ2Anims.Palettes.h"
#include "JJ2Block.h"
#include "../ContentFileType.h"

#include "../../nCine/Base/Algorithms.h"

#include <Containers/GrowableArray.h>
#include <Containers/StringConcatenable.h>
#include <IO/FileSystem.h>
#include <IO/MemoryStream.h>
#include <IO/Compression/DeflateStream.h>

#include <cmath>

using namespace Death;
using namespace Death::Containers::Literals;
using namespace Death::IO::Compression;
using namespace nCine;

namespace Jazz2::Compatibility
{
	bool JJ2Data::Open(StringView path, bool strictParser)
	{
		auto s = fs::Open(path, FileAccess::Read);
		if (!s->IsValid()) {
			LOGE("Cannot open file \"{}\" for reading", path);
			return false;
		}

		std::uint32_t magic = s->ReadValueAsLE<std::uint32_t>();
		std::uint32_t signature = s->ReadValueAsLE<std::uint32_t>();
		DEATH_ASSERT(magic == 0x42494C50 /*PLIB*/ && signature == 0xBEBAADDE, "Invalid signature", false);

		/*std::uint32_t version =*/ s->ReadValueAsLE<std::uint32_t>();

		std::uint32_t recordedSize = s->ReadValueAsLE<std::uint32_t>();
		DEATH_ASSERT(!strictParser || s->GetSize() == recordedSize, "Unexpected file size", false);

		/*uint32_t recordedCRC =*/ s->ReadValueAsLE<std::uint32_t>();
		std::int32_t headerBlockPackedSize = s->ReadValueAsLE<std::int32_t>();
		std::int32_t headerBlockUnpackedSize = s->ReadValueAsLE<std::int32_t>();

		JJ2Block headerBlock(s, headerBlockPackedSize, headerBlockUnpackedSize);

		std::int32_t baseOffset = (std::int32_t)s->GetPosition();

		while (s->GetPosition() < s->GetSize()) {
			StringView name = headerBlock.ReadString(32, true);

			std::uint32_t type = headerBlock.ReadUInt32();
			std::uint32_t offset = headerBlock.ReadUInt32();
			/*std::uint32_t fileCRC =*/ headerBlock.ReadUInt32();
			std::int32_t filePackedSize = headerBlock.ReadInt32();
			std::int32_t fileUnpackedSize = headerBlock.ReadInt32();

			s->Seek(baseOffset + offset, SeekOrigin::Begin);

			JJ2Block fileBlock(s, filePackedSize, fileUnpackedSize);
			DEATH_ASSERT(fileBlock.GetLength() == fileUnpackedSize, "Unexpected item size", false);

			Item& item = Items.emplace_back();
			item.Filename = name;
			item.Type = type;
			item.Blob = std::make_unique<std::uint8_t[]>(fileUnpackedSize);
			item.Size = fileUnpackedSize;
			fileBlock.ReadRawBytes(item.Blob.get(), fileUnpackedSize);
		}

		return true;
	}

	void JJ2Data::Extract(StringView targetPath)
	{
		fs::CreateDirectories(targetPath);

		for (auto& item : Items) {
			auto so = fs::Open(fs::CombinePath(targetPath, item.Filename), FileAccess::Write);
			if (!so->IsValid()) {
				LOGE("Cannot open file \"{}\" for writing", item.Filename);
				continue;
			}

			so->Write(item.Blob.get(), item.Size);
		}
	}

	void JJ2Data::Convert(PakWriter& pakWriter, JJ2Version version, JJ2Anims::SpriteSink sink, void* sinkCtx)
	{
		AnimSetMapping animMapping = AnimSetMapping::GetSampleMapping(version);

		// These are LOGICAL asset keys (pak entry names / NameHash / metadata lookup), not host paths - they must be
		// '/'-separated on every host so the device and the Content metadata match. fs::CombinePath would emit '\' on
		// Windows and the '/'-based sink lookup would silently reject these images (the loading screen + menu layers).
		for (auto& item : Items) {
			if (item.Filename == "SoundFXList.Intro"_s) {
				ConvertSfxList(item, pakWriter, "Cinematics/intro.j2sfx"_s, animMapping);
			} else if (item.Filename == "SoundFXList.Ending"_s) {
				ConvertSfxList(item, pakWriter, "Cinematics/ending.j2sfx"_s, animMapping);
			} else if (item.Filename == "Menu.Texture.16x16"_s) {
				ConvertMenuImage(item, pakWriter, "Animations/UI/menu16.aura"_s, 16, 16, sink, sinkCtx);
			} else if (item.Filename == "Menu.Texture.32x32"_s) {
				ConvertMenuImage(item, pakWriter, "Animations/UI/menu32.aura"_s, 32, 32, sink, sinkCtx);
			} else if (item.Filename == "Menu.Texture.128x128"_s) {
				ConvertMenuImage(item, pakWriter, "Animations/UI/menu128.aura"_s, 128, 128, sink, sinkCtx);
			} else if (item.Filename == "Picture.Loading"_s) {
				ConvertPicture(item, pakWriter, "Animations/UI/loading_screen.aura"_s, sink, sinkCtx);
			}
		}
	}

	void JJ2Data::ConvertSfxList(const Item& item, PakWriter& pakWriter, StringView targetPath, AnimSetMapping& animMapping)
	{
#pragma pack(push, 1)
		struct SoundFXList {
			std::uint32_t Frame;
			std::uint32_t Sample;
			std::uint32_t Volume;
			std::uint32_t Panning;
		};
#pragma pack(pop)

		MemoryStream so(16384);

		so.WriteValueAsLE<std::uint64_t>(0x2095A59FF0BFBBEF);	// Signature
		so.WriteValue<std::uint8_t>(ContentFileType::SfxList);
		so.WriteValueAsLE<std::uint16_t>(1);

		HashMap<std::uint32_t, std::uint32_t> sampleToIndex;
		SmallVector<std::uint32_t> indexToSample;

		std::int32_t itemCount = item.Size / sizeof(SoundFXList);
		std::int32_t itemRealCount = 0;
		std::int32_t sampleCount = 0;
		for (std::int32_t i = 0; i < itemCount; i++) {
			SoundFXList sfx;
			std::memcpy(&sfx, &item.Blob[i * sizeof(SoundFXList)], sizeof(SoundFXList));
			if (sfx.Frame == UINT32_MAX) {
				break;
			}

			auto it = sampleToIndex.find(sfx.Sample);
			if (it == sampleToIndex.end()) {
				sampleToIndex.emplace(sfx.Sample, sampleCount);
				indexToSample.emplace_back(sfx.Sample);
				sampleCount++;
			}

			itemRealCount++;
		}

		so.WriteValueAsLE<std::uint16_t>(sampleCount);
		for (std::int32_t i = 0; i < sampleCount; i++) {
			auto sample = animMapping.GetByOrdinal(indexToSample[i]);
			if (sample == nullptr) {
				so.WriteValue<std::uint8_t>(0);
			} else {
				String samplePath = sample->Category + '/' + sample->Name + ".wav"_s;
				so.WriteValue<std::uint8_t>((std::uint8_t)samplePath.size());
				so.Write(samplePath.data(), (std::uint32_t)samplePath.size());
			}
		}

		so.WriteValueAsLE<std::uint16_t>(itemRealCount);
		for (std::int32_t i = 0; i < itemRealCount; i++) {
			SoundFXList sfx;
			std::memcpy(&sfx, &item.Blob[i * sizeof(SoundFXList)], sizeof(SoundFXList));
					
			so.WriteVariableUint32(sfx.Frame);

			auto it = sampleToIndex.find(sfx.Sample);
			if (it != sampleToIndex.end()) {
				so.WriteValueAsLE<std::uint16_t>(it->second);
			} else {
				so.WriteValueAsLE<std::uint16_t>(0);
			}

			so.WriteValue<std::uint8_t>((std::uint8_t)std::min(sfx.Volume * 255 / 0x40, (std::uint32_t)UINT8_MAX));
			so.WriteValue<std::int8_t>((std::int8_t)std::clamp(((std::int32_t)sfx.Panning - 0x20) * INT8_MAX / /*0x20*/0x40, -(std::int32_t)INT8_MAX, (std::int32_t)INT8_MAX));
		}

		so.Seek(0, SeekOrigin::Begin);
		bool success = pakWriter.AddFile(so, targetPath, PakPreferredCompression::Deflate);
		DEATH_ASSERT(success, "Cannot add file to .pak container", );
	}

	void JJ2Data::ConvertMenuImage(const Item& item, PakWriter& pakWriter, StringView targetPath, std::int32_t width, std::int32_t height, JJ2Anims::SpriteSink sink, void* sinkCtx)
	{
		std::int32_t pixelCount = width * height;
		DEATH_ASSERT(item.Size == pixelCount, "Image has unexpected size", );

		std::unique_ptr<uint8_t[]> pixels = std::make_unique<uint8_t[]>(pixelCount * 4);
		for (std::int32_t i = 0; i < pixelCount; i++) {
			std::uint8_t colorIdx = item.Blob[i];
			const Color& src = MenuPalette[colorIdx];
			std::uint8_t a = (colorIdx == 0 ? 0 : src.A);
			
			pixels[(i * 4)] = src.R;
			pixels[(i * 4) + 1] = src.G;
			pixels[(i * 4) + 2] = src.B;
			pixels[(i * 4) + 3] = a;
		}
		ConvertRgbaImage(pixels.get(), width, height, pakWriter, targetPath, sink, sinkCtx);
	}

	void JJ2Data::ConvertPicture(const Item& item, PakWriter& pakWriter, StringView targetPath, JJ2Anims::SpriteSink sink, void* sinkCtx)
	{
		// Original Data.j2d pictures are: width, height, bit depth, a 256-entry
		// RGBA palette, then one 8-bit palette index per pixel.
		if (item.Size < 12 + 256 * 4) return;
		std::uint32_t width, height, depth;
		std::memcpy(&width, item.Blob.get(), sizeof(width));
		std::memcpy(&height, item.Blob.get() + 4, sizeof(height));
		std::memcpy(&depth, item.Blob.get() + 8, sizeof(depth));
		if (depth != 8 || width == 0 || height == 0 ||
			item.Size != std::int32_t(12 + 256 * 4 + width * height)) return;

		const std::uint8_t* palette = item.Blob.get() + 12;
		const std::uint8_t* indices = palette + 256 * 4;
		constexpr std::uint32_t OutputWidth = 480, OutputHeight = 272;
		auto pixels = std::make_unique<std::uint8_t[]>(std::size_t(OutputWidth) * OutputHeight * 4);
		for (std::uint32_t y = 0; y < OutputHeight; ++y) {
			const float sourceY = (y + 0.5f) * height / OutputHeight - 0.5f;
			const std::uint32_t y0 = std::uint32_t(std::max(0.0f, std::floor(sourceY)));
			const std::uint32_t y1 = std::min(y0 + 1, height - 1);
			const float fy = std::clamp(sourceY - y0, 0.0f, 1.0f);
			for (std::uint32_t x = 0; x < OutputWidth; ++x) {
				const float sourceX = (x + 0.5f) * width / OutputWidth - 0.5f;
				const std::uint32_t x0 = std::uint32_t(std::max(0.0f, std::floor(sourceX)));
				const std::uint32_t x1 = std::min(x0 + 1, width - 1);
				const float fx = std::clamp(sourceX - x0, 0.0f, 1.0f);
				const std::uint8_t* c00 = palette + std::size_t(indices[y0 * width + x0]) * 4;
				const std::uint8_t* c10 = palette + std::size_t(indices[y0 * width + x1]) * 4;
				const std::uint8_t* c01 = palette + std::size_t(indices[y1 * width + x0]) * 4;
				const std::uint8_t* c11 = palette + std::size_t(indices[y1 * width + x1]) * 4;
				std::uint8_t* dst = pixels.get() + (std::size_t(y) * OutputWidth + x) * 4;
				for (int channel = 0; channel < 3; ++channel) {
					const float top = c00[channel] + (c10[channel] - c00[channel]) * fx;
					const float bottom = c01[channel] + (c11[channel] - c01[channel]) * fx;
					dst[channel] = std::uint8_t(std::clamp(top + (bottom - top) * fy, 0.0f, 255.0f));
				}
				dst[3] = 255;
			}
		}
		ConvertRgbaImage(pixels.get(), OutputWidth, OutputHeight, pakWriter, targetPath, sink, sinkCtx);
	}

	void JJ2Data::ConvertRgbaImage(const std::uint8_t* pixels, std::int32_t width, std::int32_t height,
		PakWriter& pakWriter, StringView targetPath, JJ2Anims::SpriteSink sink, void* sinkCtx)
	{

		MemoryStream so(16384);

		constexpr std::uint8_t flags = 0x80 | 0x01 | 0x02;

		so.WriteValueAsLE<std::uint64_t>(0xB8EF8498E2BFBBEF);
		so.WriteValueAsLE<std::uint16_t>(0x208F);
		so.WriteValue<std::uint8_t>(0x02); // Version 2 is reserved for sprites (or bigger images)
		so.WriteValue<std::uint8_t>(flags);

		so.WriteValue<std::uint8_t>(4);
		so.WriteValueAsLE<std::uint32_t>(width);
		so.WriteValueAsLE<std::uint32_t>(height);

		// Include Sprite extension
		so.WriteValue<std::uint8_t>(1); // FrameConfigurationX
		so.WriteValue<std::uint8_t>(1); // FrameConfigurationY
		so.WriteValueAsLE<std::uint16_t>(1); // FrameCount
		so.WriteValueAsLE<std::uint16_t>(0); // FrameRate
		so.WriteValueAsLE<std::uint16_t>(UINT16_MAX); // NormalizedHotspotX
		so.WriteValueAsLE<std::uint16_t>(UINT16_MAX); // NormalizedHotspotY
		so.WriteValueAsLE<std::uint16_t>(UINT16_MAX); // ColdspotX
		so.WriteValueAsLE<std::uint16_t>(UINT16_MAX); // ColdspotY
		so.WriteValueAsLE<std::uint16_t>(UINT16_MAX); // GunspotX
		so.WriteValueAsLE<std::uint16_t>(UINT16_MAX); // GunspotY

		JJ2Anims::WriteImageContent(so, pixels, width, height, 4);

		so.Seek(0, SeekOrigin::Begin);

		// Feed the menu image (already RGBA / pre-coloured) to the PSP asset baker. When a sink is present it goes
		// into the PSP texture pack and is NOT written to the .pak.
		if (sink != nullptr) {
			sink(sinkCtx, targetPath, pixels, width, height, 4, /*notIndexed*/ true, so.GetBuffer());
		} else {
			bool success = pakWriter.AddFile(so, targetPath, PakPreferredCompression::Deflate);
			DEATH_ASSERT(success, "Cannot add file to .pak container", );
		}
	}
}
