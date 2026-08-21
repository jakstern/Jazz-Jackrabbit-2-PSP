#include "PspAudioBackend.h"

#include "nCine/Audio/AudioBuffer.h"
#include "nCine/Audio/AudioBufferPlayer.h"
#include "nCine/Audio/AudioStream.h"
#include "nCine/Audio/AudioStreamPlayer.h"
#include "nCine/ServiceLocator.h"
#include "Main.h"

#include <IO/FileSystem.h>

#include <pspaudio.h>
#include <pspkernel.h>
#include <pspthreadman.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

using namespace Death::Containers;
using namespace Death::Containers::Literals;
using namespace Death::IO;

namespace nCine
{
	namespace
	{
		constexpr std::int32_t OutputFrequency = 44100;
		constexpr std::int32_t MusicFrequency = 22050;
		constexpr std::int32_t MusicExpansion = OutputFrequency / MusicFrequency;
		static_assert(OutputFrequency % MusicFrequency == 0, "PSP music must expand by an integer ratio");
		constexpr std::int32_t OutputFrames = 1024; // sceAudio requires a multiple of 64
		constexpr std::uint32_t MaxVoices = 16;
		volatile std::uint32_t BuffersLoaded = 0;
		volatile std::uint32_t BuffersFailed = 0;
		volatile std::uint32_t VoicesStarted = 0;
		volatile std::uint32_t MixBlocks = 0;
		volatile std::uint32_t MusicStreamsLoaded = 0;
		volatile std::uint32_t MusicFramesMixed = 0;
		volatile std::uint32_t ActiveSfxVoices = 0;
		volatile std::uint32_t MixMicroseconds = 0;
		volatile std::uint32_t MaxMixMicroseconds = 0;
		volatile std::uint32_t MixDeadlineMisses = 0;
		volatile std::uint32_t PeakAccumulator = 0;
		volatile std::uint32_t SaturatedSamples = 0;
		volatile std::uint32_t OutputErrors = 0;
		constexpr std::int32_t SoftClipKnee = 24576;
		constexpr std::uint32_t OutputDeadlineMicroseconds =
			(std::uint32_t(OutputFrames) * 1000000U) / std::uint32_t(OutputFrequency);

		bool ReadExact(Stream& stream, void* destination, std::int64_t size)
		{
			unsigned char* out = static_cast<unsigned char*>(destination);
			std::int64_t done = 0;
			while (done < size) {
				const std::int64_t read = stream.Read(out + done, size - done);
				if (read <= 0) {
					return false;
				}
				done += read;
			}
			return true;
		}

		bool SkipExact(Stream& stream, std::uint32_t size)
		{
			unsigned char scratch[256];
			while (size > 0) {
				const std::uint32_t chunk = std::min<std::uint32_t>(size, sizeof(scratch));
				if (!ReadExact(stream, scratch, chunk)) return false;
				size -= chunk;
			}
			return true;
		}

		std::uint16_t ReadLe16(const unsigned char* data)
		{
			return std::uint16_t(data[0]) | (std::uint16_t(data[1]) << 8);
		}

		std::uint32_t ReadLe32(const unsigned char* data)
		{
			return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) |
				(std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
		}

		constexpr std::int32_t ImaIndexTable[16] = {
			-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
		};
		constexpr std::int32_t ImaStepTable[89] = {
			7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
			34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
			157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
			598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
			1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
			5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
			16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
		};

		std::int16_t DecodeImaNibble(std::uint8_t nibble, std::int32_t& predictor, std::int32_t& stepIndex)
		{
			const std::int32_t step = ImaStepTable[stepIndex];
			std::int32_t difference = step >> 3;
			if (nibble & 1) difference += step >> 2;
			if (nibble & 2) difference += step >> 1;
			if (nibble & 4) difference += step;
			predictor += (nibble & 8 ? -difference : difference);
			predictor = std::clamp(predictor, -32768, 32767);
			stepIndex = std::clamp(stepIndex + ImaIndexTable[nibble & 15], 0, 88);
			return std::int16_t(predictor);
		}
	}

	class PspAudioDevice final : public IAudioDevice
	{
	public:
		PspAudioDevice()
			: channel_(-1), thread_(-1), mutex_(-1), running_(false), suspended_(false), gain_(1.0f),
			  listenerPosition_(Vector3f::Zero)
		{
			for (Voice& voice : voices_) {
				voice = {};
			}

			channel_ = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, OutputFrames, PSP_AUDIO_FORMAT_STEREO);
			if (channel_ < 0) {
				LOGE("sceAudioChReserve() failed: 0x{:x}", std::uint32_t(channel_));
				return;
			}

			mutex_ = sceKernelCreateSema("Jazz2AudioMutex", 0, 1, 1, nullptr);
			if (mutex_ < 0) {
				LOGE("sceKernelCreateSema() for audio failed: 0x{:x}", std::uint32_t(mutex_));
				sceAudioChRelease(channel_);
				channel_ = -1;
				return;
			}

			running_ = true;
			thread_ = sceKernelCreateThread("Jazz2Audio", &PspAudioDevice::ThreadEntry, 0x12, 32 * 1024,
				PSP_THREAD_ATTR_USER, nullptr);
			if (thread_ < 0) {
				LOGE("sceKernelCreateThread() for audio failed: 0x{:x}", std::uint32_t(thread_));
				running_ = false;
				sceKernelDeleteSema(mutex_);
				mutex_ = -1;
				sceAudioChRelease(channel_);
				channel_ = -1;
				return;
			}

			PspAudioDevice* self = this;
			const int result = sceKernelStartThread(thread_, sizeof(self), &self);
			if (result < 0) {
				LOGE("sceKernelStartThread() for audio failed: 0x{:x}", std::uint32_t(result));
				running_ = false;
				sceKernelDeleteThread(thread_);
				thread_ = -1;
				sceKernelDeleteSema(mutex_);
				mutex_ = -1;
				sceAudioChRelease(channel_);
				channel_ = -1;
				return;
			}

			LOGI("PSP audio initialized: {} Hz, {} frames, {} software voices", OutputFrequency, OutputFrames, MaxVoices);
		}

		~PspAudioDevice() override
		{
			running_ = false;
			if (thread_ >= 0) {
				sceKernelWaitThreadEnd(thread_, nullptr);
				sceKernelDeleteThread(thread_);
				thread_ = -1;
			}
			if (mutex_ >= 0) {
				sceKernelDeleteSema(mutex_);
				mutex_ = -1;
			}
			if (channel_ >= 0) {
				sceAudioChRelease(channel_);
				channel_ = -1;
			}
		}

		bool isValid() const override { return channel_ >= 0 && thread_ >= 0; }
		const char* name() const override { return "PSP sceAudio software mixer"; }

		float gain() const override { return gain_; }
		void setGain(float gain) override
		{
			Lock();
			gain_ = std::max(0.0f, gain);
			Unlock();
		}

		std::uint32_t maxNumPlayers() const override { return MaxVoices; }
		std::uint32_t numPlayers() const override
		{
			std::uint32_t count = 0;
			for (const Voice& voice : voices_) {
				if (voice.player != nullptr) ++count;
			}
			return count;
		}

		const IAudioPlayer* player(std::uint32_t index) const override
		{
			for (const Voice& voice : voices_) {
				if (voice.player != nullptr && index-- == 0) return voice.player;
			}
			return nullptr;
		}

		IAudioPlayer* player(std::uint32_t index) override
		{
			for (Voice& voice : voices_) {
				if (voice.player != nullptr && index-- == 0) return voice.player;
			}
			return nullptr;
		}

		void stopPlayers() override
		{
			while (IAudioPlayer* current = player(0)) current->stop();
		}

		void pausePlayers() override
		{
			for (std::uint32_t i = numPlayers(); i > 0; --i) {
				IAudioPlayer* current = player(i - 1);
				if (current != nullptr) current->pause();
			}
		}

		void stopPlayers(PlayerType type) override
		{
			for (std::uint32_t i = numPlayers(); i > 0; --i) {
				IAudioPlayer* current = player(i - 1);
				if (current != nullptr && MatchesType(*current, type)) current->stop();
			}
		}

		void pausePlayers(PlayerType type) override
		{
			for (std::uint32_t i = numPlayers(); i > 0; --i) {
				IAudioPlayer* current = player(i - 1);
				if (current != nullptr && MatchesType(*current, type)) current->pause();
			}
		}

		void freezePlayers() override { pausePlayers(); }
		void unfreezePlayers() override
		{
			for (std::uint32_t i = numPlayers(); i > 0; --i) {
				IAudioPlayer* current = player(i - 1);
				if (current != nullptr && current->isPaused()) current->play();
			}
		}

		std::uint32_t registerPlayer(IAudioPlayer* player) override
		{
			if (!isValid() || player == nullptr ||
				(player->type() != AudioBufferPlayer::sType() && player->type() != AudioStreamPlayer::sType())) {
				return UnavailableSource;
			}

			Lock();
			for (std::uint32_t i = 0; i < MaxVoices; ++i) {
				if (voices_[i].player == player) {
					Unlock();
					return i;
				}
			}
			for (std::uint32_t i = 0; i < MaxVoices; ++i) {
				if (voices_[i].player == nullptr) {
					voices_[i].player = player;
					voices_[i].cursor = 0.0f;
					voices_[i].active = false;
					voices_[i].finished = false;
					Unlock();
					return i;
				}
			}
			Unlock();
			return UnavailableSource;
		}

		void unregisterPlayer(IAudioPlayer* player) override
		{
			if (player == nullptr) return;
			Lock();
			for (Voice& voice : voices_) {
				if (voice.player == player) voice = {};
			}
			Unlock();
		}

		void updatePlayers() override
		{
			Lock();
			for (Voice& voice : voices_) {
				if (voice.player != nullptr && voice.finished) {
					voice.player->state_ = IAudioPlayer::PlayerState::Stopped;
					voice.player->sourceId_ = UnavailableSource;
					voice = {};
				}
			}
			Unlock();
		}

		bool submitStreamDecode(const std::shared_ptr<StreamDecodeRequest>&) override { return false; }
		void drainStreamDecode(const std::shared_ptr<StreamDecodeRequest>& request) override
		{
			if (request != nullptr && request->state.load() == StreamDecodeRequest::State::Pending) {
				request->state.store(StreamDecodeRequest::State::Idle);
			}
		}

		const Vector3f& getListenerPosition() const override { return listenerPosition_; }
		void updateListener(const Vector3f& position, const Vector3f&) override
		{
			Lock();
			listenerPosition_ = position;
			Unlock();
		}

		std::int32_t nativeFrequency() override { return OutputFrequency; }
		void suspendDevice() override { suspended_ = true; }
		void resumeDevice() override { suspended_ = false; }

		void StartSource(std::uint32_t source)
		{
			if (source >= MaxVoices) return;
			Lock();
			if (voices_[source].player != nullptr) voices_[source].active = true;
			Unlock();
		}

		void PauseSource(std::uint32_t source)
		{
			if (source >= MaxVoices) return;
			Lock();
			if (voices_[source].player != nullptr) voices_[source].active = false;
			Unlock();
		}

		std::int32_t SampleOffset(std::uint32_t source) const
		{
			if (source >= MaxVoices || voices_[source].player == nullptr) return 0;
			return std::int32_t(voices_[source].cursor);
		}

		void SetSampleOffset(std::uint32_t source, std::int32_t offset)
		{
			if (source >= MaxVoices) return;
			Lock();
			if (voices_[source].player != nullptr) voices_[source].cursor = float(std::max(0, offset));
			Unlock();
		}

	private:
		struct Voice
		{
			IAudioPlayer* player = nullptr;
			float cursor = 0.0f;
			float filteredLeft = 0.0f;
			float filteredRight = 0.0f;
			bool active = false;
			bool finished = false;
			bool filterPrimed = false;
		};

		static int ThreadEntry(SceSize args, void* argp)
		{
			if (args != sizeof(PspAudioDevice*) || argp == nullptr) return -1;
			PspAudioDevice* self = *static_cast<PspAudioDevice**>(argp);
			self->OutputLoop();
			return 0;
		}

		void OutputLoop()
		{
			// sceAudioOutputBlocking() returns while the hardware may still be reading the buffer it queued,
			// so alternate two stable buffers (as PSPSDK's reference loop does).
			alignas(64) std::int16_t output[2][OutputFrames * 2];
			std::uint32_t bufferIndex = 0;
			while (running_) {
				Mix(output[bufferIndex]);
				const int result = sceAudioOutputBlocking(channel_, PSP_AUDIO_VOLUME_MAX, output[bufferIndex]);
				if (result < 0 && running_) {
					++OutputErrors;
					sceKernelDelayThread(1000);
				}
				bufferIndex ^= 1;
			}
		}

		void Mix(std::int16_t* output)
		{
			const std::uint64_t started = sceKernelGetSystemTimeWide();
			std::int32_t accumulator[OutputFrames * 2]{};
			std::uint32_t activeSfxVoices = 0;
			Lock();
			if (!suspended_) {
				for (Voice& voice : voices_) {
					if (voice.active && !voice.finished && voice.player != nullptr &&
						voice.player->type() == AudioBufferPlayer::sType()) ++activeSfxVoices;
					MixVoice(voice, accumulator);
				}
			}
			Unlock();

			std::uint32_t peak = 0;
			std::uint32_t saturated = 0;
			for (std::int32_t i = 0; i < OutputFrames * 2; ++i) {
				const std::int32_t sample = accumulator[i];
				const std::uint32_t magnitude = std::uint32_t(sample < 0 ?
					(sample == INT32_MIN ? INT32_MAX : -sample) : sample);
				peak = std::max(peak, magnitude);
				if (magnitude > std::uint32_t(SoftClipKnee)) ++saturated;
				output[i] = SoftClip(accumulator[i]);
			}
			const std::uint32_t elapsed = std::uint32_t(sceKernelGetSystemTimeWide() - started);
			ActiveSfxVoices = activeSfxVoices;
			MixMicroseconds = elapsed;
			if (elapsed > MaxMixMicroseconds) MaxMixMicroseconds = elapsed;
			if (elapsed >= OutputDeadlineMicroseconds) ++MixDeadlineMisses;
			PeakAccumulator = peak;
			SaturatedSamples = saturated;
			++MixBlocks;
		}

		void MixVoice(Voice& voice, std::int32_t* output)
		{
			if (!voice.active || voice.finished || voice.player == nullptr) return;
			if (voice.player->type() == AudioStreamPlayer::sType()) {
				MixStreamVoice(voice, output);
				return;
			}
			auto* bufferPlayer = static_cast<AudioBufferPlayer*>(voice.player);
			const AudioBuffer* buffer = bufferPlayer->audioBuffer();
			if (buffer == nullptr || buffer->data() == nullptr || buffer->numSamples() <= 0 ||
				(buffer->bytesPerSample() != 1 && buffer->bytesPerSample() != 2) ||
				(buffer->numChannels() != 1 && buffer->numChannels() != 2)) {
				voice.finished = true;
				voice.active = false;
				return;
			}

			const IAudioPlayer& player = *voice.player;
			float voiceGain = std::max(0.0f, player.gain_) * gain_;
			float pan = 0.0f;
			if (!player.GetFlags(IAudioPlayer::PlayerFlags::SourceRelative)) {
				const float dx = player.position_.X - listenerPosition_.X;
				const float dy = player.position_.Y - listenerPosition_.Y;
				const float distance = std::sqrt(dx * dx + dy * dy);
				pan = std::clamp(dx / 320.0f, -1.0f, 1.0f);
				if (distance > 200.0f) {
					voiceGain *= std::clamp(1.0f - (distance - 200.0f) / 700.0f, 0.0f, 1.0f);
				}
			} else if (player.GetFlags(IAudioPlayer::PlayerFlags::As2D)) {
				pan = std::clamp(player.position_.X, -1.0f, 1.0f);
			}
			const float leftGain = voiceGain * (pan > 0.0f ? 1.0f - pan : 1.0f);
			const float rightGain = voiceGain * (pan < 0.0f ? 1.0f + pan : 1.0f);
			const float step = std::max(0.01f, player.pitch_) * float(buffer->frequency()) / float(OutputFrequency);
			// Mild reconstruction filter even at lowPass_ == 1: upsampled 11/22 kHz samples otherwise keep their
			// ultrasonic images. Lower values are the engine's underwater/occlusion effect.
			const float cutoff = 800.0f + 13200.0f * std::clamp(player.lowPass_, 0.0f, 1.0f);
			const float filterAlpha = 1.0f - std::exp(-6.283185307f * cutoff / float(OutputFrequency));

			for (std::int32_t frame = 0; frame < OutputFrames; ++frame) {
				std::int32_t sourceFrame = std::int32_t(voice.cursor);
				if (sourceFrame >= buffer->numSamples()) {
					if (player.GetFlags(IAudioPlayer::PlayerFlags::Looping)) {
						voice.cursor = std::fmod(voice.cursor, float(buffer->numSamples()));
						sourceFrame = std::int32_t(voice.cursor);
					} else {
						voice.finished = true;
						voice.active = false;
						break;
					}
				}

				std::int32_t nextFrame = sourceFrame + 1;
				if (nextFrame >= buffer->numSamples()) {
					nextFrame = (player.GetFlags(IAudioPlayer::PlayerFlags::Looping) ? 0 : sourceFrame);
				}
				const float fraction = voice.cursor - float(sourceFrame);
				const float left0 = float(ReadSample(*buffer, sourceFrame, 0));
				const float left1 = float(ReadSample(*buffer, nextFrame, 0));
				const float left = left0 + (left1 - left0) * fraction;
				float right = left;
				if (buffer->numChannels() == 2) {
					const float right0 = float(ReadSample(*buffer, sourceFrame, 1));
					const float right1 = float(ReadSample(*buffer, nextFrame, 1));
					right = right0 + (right1 - right0) * fraction;
				}
				if (!voice.filterPrimed) {
					voice.filteredLeft = left;
					voice.filteredRight = right;
					voice.filterPrimed = true;
				} else {
					voice.filteredLeft += filterAlpha * (left - voice.filteredLeft);
					voice.filteredRight += filterAlpha * (right - voice.filteredRight);
				}
				output[frame * 2] += std::int32_t(voice.filteredLeft * leftGain);
				output[frame * 2 + 1] += std::int32_t(voice.filteredRight * rightGain);
				voice.cursor += step;
			}
		}

		void MixStreamVoice(Voice& voice, std::int32_t* output)
		{
			auto* streamPlayer = static_cast<AudioStreamPlayer*>(voice.player);
			alignas(16) std::int16_t decoded[OutputFrames * 2];
			const std::int32_t frames = streamPlayer->audioStream_.decodePspFrames(decoded, OutputFrames,
				streamPlayer->GetFlags(IAudioPlayer::PlayerFlags::Looping));
			if (frames <= 0) {
				voice.finished = true;
				voice.active = false;
				return;
			}

			const float voiceGain = std::max(0.0f, streamPlayer->gain_) * gain_;
			const float cutoff = 800.0f + 13200.0f * std::clamp(streamPlayer->lowPass_, 0.0f, 1.0f);
			const float alpha = 1.0f - std::exp(-6.283185307f * cutoff / float(OutputFrequency));
			for (std::int32_t frame = 0; frame < frames; ++frame) {
				const float left = float(decoded[frame * 2]);
				const float right = float(decoded[frame * 2 + 1]);
				if (!voice.filterPrimed) {
					voice.filteredLeft = left;
					voice.filteredRight = right;
					voice.filterPrimed = true;
				} else {
					voice.filteredLeft += alpha * (left - voice.filteredLeft);
					voice.filteredRight += alpha * (right - voice.filteredRight);
				}
				output[frame * 2] += std::int32_t(voice.filteredLeft * voiceGain);
				output[frame * 2 + 1] += std::int32_t(voice.filteredRight * voiceGain);
			}
			if (frames < OutputFrames && !streamPlayer->GetFlags(IAudioPlayer::PlayerFlags::Looping)) {
				voice.finished = true;
				voice.active = false;
			}
			MusicFramesMixed += frames;
		}

		static std::int16_t SoftClip(std::int32_t sample)
		{
			const std::int32_t magnitude = (sample < 0 ? (sample == INT32_MIN ? INT32_MAX : -sample) : sample);
			if (magnitude <= SoftClipKnee) return std::int16_t(sample);
			const std::int32_t excess = magnitude - SoftClipKnee;
			const std::int32_t compressed = SoftClipKnee + std::int32_t((std::int64_t(excess) * 8191) / (excess + 8191));
			return std::int16_t(sample < 0 ? -compressed : compressed);
		}

		static std::int32_t ReadSample(const AudioBuffer& buffer, std::int32_t frame, std::int32_t channel)
		{
			const std::int32_t index = frame * buffer.numChannels() + channel;
			if (buffer.bytesPerSample() == 1) {
				return (std::int32_t(buffer.data()[index]) - 128) << 8;
			}
			const unsigned char* sample = buffer.data() + index * 2;
			return std::int16_t(ReadLe16(sample));
		}

		static bool MatchesType(const IAudioPlayer& player, PlayerType type)
		{
			return (type == PlayerType::Buffer && player.type() == AudioBufferPlayer::sType()) ||
				(type == PlayerType::Stream && player.type() == AudioStreamPlayer::sType());
		}

		void Lock() const
		{
			if (mutex_ >= 0) sceKernelWaitSema(mutex_, 1, nullptr);
		}
		void Unlock() const
		{
			if (mutex_ >= 0) sceKernelSignalSema(mutex_, 1);
		}

		int channel_;
		SceUID thread_;
		SceUID mutex_;
		volatile bool running_;
		volatile bool suspended_;
		float gain_;
		Vector3f listenerPosition_;
		Voice voices_[MaxVoices];
	};

	std::unique_ptr<IAudioDevice> CreatePspAudioDevice()
	{
		return std::make_unique<PspAudioDevice>();
	}

	PspAudioStats GetPspAudioStats()
	{
		return { BuffersLoaded, BuffersFailed, VoicesStarted, MixBlocks, MusicStreamsLoaded, MusicFramesMixed,
			ActiveSfxVoices, MixMicroseconds, MaxMixMicroseconds, MixDeadlineMisses, PeakAccumulator,
			SaturatedSamples, OutputErrors };
	}

	// AudioBuffer: the PSP owns decoded PCM instead of an OpenAL buffer handle.
	AudioBuffer::AudioBuffer()
		: Object(ObjectType::AudioBuffer), bufferId_(0), bytesPerSample_(0), numChannels_(0), frequency_(0),
		  numSamples_(0), duration_(0.0f)
	{
	}

	AudioBuffer::AudioBuffer(StringView filename) : AudioBuffer()
	{
		if (!loadFromFile(filename)) ++BuffersFailed;
	}

	AudioBuffer::AudioBuffer(std::unique_ptr<Stream> fileHandle, StringView filename) : AudioBuffer()
	{
		if (!loadFromStream(std::move(fileHandle), filename)) {
			++BuffersFailed;
			LOGW("Audio file \"{}\" cannot be loaded", filename);
		}
	}

	AudioBuffer::~AudioBuffer()
	{
		IAudioDevice& device = theServiceLocator().GetAudioDevice();
		for (std::uint32_t i = device.numPlayers(); i > 0; --i) {
			IAudioPlayer* current = device.player(i - 1);
			if (current != nullptr && current->type() == AudioBufferPlayer::sType()) {
				auto* bufferPlayer = static_cast<AudioBufferPlayer*>(current);
				if (bufferPlayer->audioBuffer() == this) bufferPlayer->setAudioBuffer(nullptr);
			}
		}
	}

	AudioBuffer::AudioBuffer(AudioBuffer&& other) noexcept
		: Object(std::move(other)), bufferId_(other.bufferId_), bytesPerSample_(other.bytesPerSample_),
		  numChannels_(other.numChannels_), frequency_(other.frequency_), numSamples_(other.numSamples_),
		  duration_(other.duration_), samples_(std::move(other.samples_))
	{
		other.bufferId_ = 0;
		other.numSamples_ = 0;
	}

	AudioBuffer& AudioBuffer::operator=(AudioBuffer&& other) noexcept
	{
		if (this == &other) return *this;
		Object::operator=(std::move(other));
		bufferId_ = other.bufferId_;
		bytesPerSample_ = other.bytesPerSample_;
		numChannels_ = other.numChannels_;
		frequency_ = other.frequency_;
		numSamples_ = other.numSamples_;
		duration_ = other.duration_;
		samples_ = std::move(other.samples_);
		other.bufferId_ = 0;
		other.numSamples_ = 0;
		return *this;
	}

	void AudioBuffer::init(Format format, std::int32_t frequency)
	{
		bytesPerSample_ = (format == Format::Mono8 || format == Format::Stereo8 ? 1 : 2);
		numChannels_ = (format == Format::Mono8 || format == Format::Mono16 ? 1 : 2);
		frequency_ = frequency;
		numSamples_ = 0;
		duration_ = 0.0f;
		samples_.reset();
		bufferId_ = 0;
	}

	bool AudioBuffer::loadFromFile(StringView filename)
	{
		return loadFromStream(fs::Open(filename, FileAccess::Read), filename);
	}

	bool AudioBuffer::loadFromStream(std::unique_ptr<Stream> fileHandle, StringView filename)
	{
		if (fileHandle == nullptr || !fileHandle->IsValid()) return false;
		const auto extension = fs::GetExtension(filename);
		if (extension != "wav"_s) {
			LOGW("PSP audio currently supports PCM WAV only: {}", filename);
			return false;
		}

		unsigned char riff[12];
		if (!ReadExact(*fileHandle, riff, sizeof(riff)) || std::memcmp(riff, "RIFF", 4) != 0 ||
			std::memcmp(riff + 8, "WAVE", 4) != 0) return false;

		bool foundFormat = false;
		std::uint16_t audioFormat = 0;
		std::uint16_t channels = 0;
		std::uint16_t bits = 0;
		std::uint32_t frequency = 0;
		while (true) {
			unsigned char chunk[8];
			if (!ReadExact(*fileHandle, chunk, sizeof(chunk))) break;
			const std::uint32_t chunkSize = ReadLe32(chunk + 4);
			if (std::memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16) {
				unsigned char format[16];
				if (!ReadExact(*fileHandle, format, sizeof(format))) return false;
				audioFormat = ReadLe16(format);
				channels = ReadLe16(format + 2);
				frequency = ReadLe32(format + 4);
				bits = ReadLe16(format + 14);
				foundFormat = true;
				if (!SkipExact(*fileHandle, chunkSize - sizeof(format) + (chunkSize & 1U))) return false;
			} else if (std::memcmp(chunk, "data", 4) == 0) {
				// PakFile's deflate stream is forward-only; PCM WAV puts "fmt " before "data", so one pass works.
				if (!foundFormat || audioFormat != 1 || (channels != 1 && channels != 2) ||
					(bits != 8 && bits != 16) || frequency == 0 || chunkSize == 0 || chunkSize > std::uint32_t(INT32_MAX)) {
					return false;
				}
				bytesPerSample_ = bits / 8;
				numChannels_ = channels;
				frequency_ = std::int32_t(frequency);
				if (!loadFromSamples(nullptr, std::int32_t(chunkSize)) ||
					!ReadExact(*fileHandle, samples_.get(), chunkSize)) return false;
				++BuffersLoaded;
				return true;
			} else {
				if (!SkipExact(*fileHandle, chunkSize + (chunkSize & 1U))) return false;
			}
		}
		return false;
	}

	bool AudioBuffer::loadFromSamples(const unsigned char* data, std::int32_t size)
	{
		if (bytesPerSample_ == 0 || numChannels_ == 0 || frequency_ <= 0 || size <= 0 ||
			size % (bytesPerSample_ * numChannels_) != 0) return false;
		auto samples = std::make_unique<unsigned char[]>(size);
		if (data != nullptr) std::memcpy(samples.get(), data, size);
		samples_ = std::move(samples);
		numSamples_ = size / (bytesPerSample_ * numChannels_);
		duration_ = float(numSamples_) / float(frequency_);
		bufferId_ = 1;
		return true;
	}

	bool AudioBuffer::load(IAudioLoader&) { return false; }

	IAudioPlayer::IAudioPlayer(ObjectType type)
		: Object(type), sourceId_(IAudioDevice::UnavailableSource), state_(PlayerState::Stopped), flags_(PlayerFlags::None),
		  gain_(1.0f), pitch_(1.0f), lowPass_(1.0f), position_(Vector3f::Zero), filterHandle_(0)
	{
	}

	IAudioPlayer::~IAudioPlayer() = default;

	std::int32_t IAudioPlayer::sampleOffset() const
	{
		if (sourceId_ == IAudioDevice::UnavailableSource) return 0;
		return static_cast<const PspAudioDevice&>(theServiceLocator().GetAudioDevice()).SampleOffset(sourceId_);
	}

	void IAudioPlayer::setSampleOffset(std::int32_t offset)
	{
		if (sourceId_ != IAudioDevice::UnavailableSource)
			static_cast<PspAudioDevice&>(theServiceLocator().GetAudioDevice()).SetSampleOffset(sourceId_, offset);
	}

	void IAudioPlayer::setSourceRelative(bool value) { SetFlags(PlayerFlags::SourceRelative, value); }
	void IAudioPlayer::setGain(float gain) { gain_ = std::max(0.0f, gain); }
	void IAudioPlayer::setPitch(float pitch) { pitch_ = std::max(0.01f, pitch); }
	void IAudioPlayer::setLowPass(float value) { lowPass_ = std::clamp(value, 0.0f, 1.0f); }
	void IAudioPlayer::setPosition(const Vector3f& position) { position_ = position; }
	void IAudioPlayer::updateFilters() {}
	void IAudioPlayer::setPositionInternal(const Vector3f&) {}
	Vector3f IAudioPlayer::getAdjustedPosition(IAudioDevice&, const Vector3f& pos, bool, bool) { return pos; }

	AudioBufferPlayer::AudioBufferPlayer() : IAudioPlayer(ObjectType::AudioBufferPlayer), audioBuffer_(nullptr) {}
	AudioBufferPlayer::AudioBufferPlayer(AudioBuffer* buffer) : IAudioPlayer(ObjectType::AudioBufferPlayer), audioBuffer_(buffer) {}
	AudioBufferPlayer::~AudioBufferPlayer() { stop(); }
	std::uint32_t AudioBufferPlayer::bufferId() const { return audioBuffer_ != nullptr ? audioBuffer_->bufferId() : 0; }
	std::int32_t AudioBufferPlayer::bytesPerSample() const { return audioBuffer_ != nullptr ? audioBuffer_->bytesPerSample() : 0; }
	std::int32_t AudioBufferPlayer::numChannels() const { return audioBuffer_ != nullptr ? audioBuffer_->numChannels() : 0; }
	std::int32_t AudioBufferPlayer::frequency() const { return audioBuffer_ != nullptr ? audioBuffer_->frequency() : 0; }
	std::int32_t AudioBufferPlayer::numSamples() const { return audioBuffer_ != nullptr ? audioBuffer_->numSamples() : 0; }
	float AudioBufferPlayer::duration() const { return audioBuffer_ != nullptr ? audioBuffer_->duration() : 0.0f; }
	std::int32_t AudioBufferPlayer::bufferSize() const { return audioBuffer_ != nullptr ? audioBuffer_->bufferSize() : 0; }

	void AudioBufferPlayer::setAudioBuffer(AudioBuffer* buffer)
	{
		stop();
		audioBuffer_ = buffer;
	}

	void AudioBufferPlayer::play()
	{
		auto& device = static_cast<PspAudioDevice&>(theServiceLocator().GetAudioDevice());
		if (state_ == PlayerState::Paused && sourceId_ != IAudioDevice::UnavailableSource) {
			state_ = PlayerState::Playing;
			device.StartSource(sourceId_);
			return;
		}
		if (audioBuffer_ == nullptr || audioBuffer_->data() == nullptr) return;
		const std::uint32_t source = device.registerPlayer(this);
		if (source == IAudioDevice::UnavailableSource) return;
		sourceId_ = source;
		state_ = PlayerState::Playing;
		device.StartSource(sourceId_);
		++VoicesStarted;
	}

	void AudioBufferPlayer::pause()
	{
		if (state_ != PlayerState::Playing || sourceId_ == IAudioDevice::UnavailableSource) return;
		static_cast<PspAudioDevice&>(theServiceLocator().GetAudioDevice()).PauseSource(sourceId_);
		state_ = PlayerState::Paused;
	}

	void AudioBufferPlayer::stop()
	{
		if (sourceId_ != IAudioDevice::UnavailableSource) theServiceLocator().GetAudioDevice().unregisterPlayer(this);
		sourceId_ = IAudioDevice::UnavailableSource;
		state_ = PlayerState::Stopped;
	}

	void AudioBufferPlayer::setLooping(bool value) { IAudioPlayer::setLooping(value); }
	void AudioBufferPlayer::updateState() {}

	// Music is host-baked 22.05 kHz IMA ADPCM; one block (~46 ms) is decoded and expanded to 44.1 kHz at a time.
	AudioStream::AudioStream()
		: nextAvailableBufferIndex_(0), currentBufferId_(0), bytesPerSample_(0), numChannels_(0), frequency_(0),
		  numSamples_(-1), duration_(0.0f), isLooping_(false), format_(0), pspDataOffset_(0), pspDataSize_(0),
		  pspDataRemaining_(0), pspBlockAlign_(0), pspSamplesPerBlock_(0), pspSourceSamplesRemaining_(0),
		  pspDecodedFrames_(0), pspDecodedCursor_(0), pspDecodeFailed_(false)
	{
	}
	AudioStream::AudioStream(StringView filename) : AudioStream() { loadFromFile(filename); }
	AudioStream::~AudioStream() = default;
	AudioStream::AudioStream(AudioStream&& other)
		: buffersIds_(std::move(other.buffersIds_)), nextAvailableBufferIndex_(other.nextAvailableBufferIndex_),
		  decodeRequest_(std::move(other.decodeRequest_)), currentBufferId_(other.currentBufferId_),
		  bytesPerSample_(other.bytesPerSample_), numChannels_(other.numChannels_), frequency_(other.frequency_),
		  numSamples_(other.numSamples_), duration_(other.duration_), isLooping_(other.isLooping_), format_(other.format_),
		  audioReader_(std::move(other.audioReader_)), pspFile_(std::move(other.pspFile_)),
		  pspBlock_(std::move(other.pspBlock_)), pspDecoded_(std::move(other.pspDecoded_)),
		  pspDataOffset_(other.pspDataOffset_), pspDataSize_(other.pspDataSize_),
		  pspDataRemaining_(other.pspDataRemaining_), pspBlockAlign_(other.pspBlockAlign_),
		  pspSamplesPerBlock_(other.pspSamplesPerBlock_),
		  pspSourceSamplesRemaining_(other.pspSourceSamplesRemaining_),
		  pspDecodedFrames_(other.pspDecodedFrames_), pspDecodedCursor_(other.pspDecodedCursor_),
		  pspDecodeFailed_(other.pspDecodeFailed_)
	{
	}
	AudioStream& AudioStream::operator=(AudioStream&& other)
	{
		if (this != &other) {
			this->~AudioStream();
			new (this) AudioStream(std::move(other));
		}
		return *this;
	}
	std::int32_t AudioStream::numStreamSamples() const
	{
		return pspSamplesPerBlock_ * MusicExpansion;
	}
	bool AudioStream::enqueue(std::uint32_t, bool) { return false; }
	void AudioStream::stop(std::uint32_t) { rewindPsp(); }
	void AudioStream::setLooping(bool value) { isLooping_ = value; }
	bool AudioStream::loadFromFile(StringView filename)
	{
		pspFile_ = fs::Open(filename, FileAccess::Read, 16 * 1024);
		if (pspFile_ == nullptr || !pspFile_->IsValid()) return false;
		unsigned char riff[12];
		if (!ReadExact(*pspFile_, riff, sizeof(riff)) || std::memcmp(riff, "RIFF", 4) != 0 ||
			std::memcmp(riff + 8, "WAVE", 4) != 0) return false;

		bool foundFormat = false;
		std::uint32_t factSamples = 0;
		while (true) {
			unsigned char chunk[8];
			if (!ReadExact(*pspFile_, chunk, sizeof(chunk))) return false;
			const std::uint32_t chunkSize = ReadLe32(chunk + 4);
			if (std::memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 20) {
				unsigned char fmt[20];
				if (!ReadExact(*pspFile_, fmt, sizeof(fmt))) return false;
				format_ = ReadLe16(fmt);
				numChannels_ = ReadLe16(fmt + 2);
				frequency_ = std::int32_t(ReadLe32(fmt + 4));
				pspBlockAlign_ = ReadLe16(fmt + 12);
				bytesPerSample_ = 2;
				pspSamplesPerBlock_ = ReadLe16(fmt + 18);
				foundFormat = true;
				if (!SkipExact(*pspFile_, chunkSize - sizeof(fmt) + (chunkSize & 1U))) return false;
			} else if (std::memcmp(chunk, "fact", 4) == 0 && chunkSize >= 4) {
				unsigned char fact[4];
				if (!ReadExact(*pspFile_, fact, sizeof(fact))) return false;
				factSamples = ReadLe32(fact);
				if (!SkipExact(*pspFile_, chunkSize - sizeof(fact) + (chunkSize & 1U))) return false;
			} else if (std::memcmp(chunk, "data", 4) == 0) {
				if (!foundFormat || format_ != 0x11 || numChannels_ != 2 || frequency_ != MusicFrequency ||
					pspBlockAlign_ < 8 || pspSamplesPerBlock_ <= 1 || chunkSize > std::uint32_t(INT32_MAX) ||
					chunkSize == 0 || chunkSize % pspBlockAlign_ != 0) return false;
				pspDataOffset_ = std::int32_t(pspFile_->GetPosition());
				pspDataSize_ = std::int32_t(chunkSize);
				const std::int64_t fileSize = pspFile_->GetSize();
				if (fileSize < std::int64_t(pspDataOffset_) + pspDataSize_) return false;
				pspDataRemaining_ = pspDataSize_;
				pspBlock_ = std::make_unique<unsigned char[]>(pspBlockAlign_);
				pspDecoded_ = std::make_unique<std::int16_t[]>(pspSamplesPerBlock_ * numChannels_);
				const std::int32_t blockCount = pspDataSize_ / pspBlockAlign_;
				const std::int64_t blockCapacity = std::int64_t(blockCount) * pspSamplesPerBlock_;
				if (factSamples == 0 || factSamples > std::uint32_t(INT32_MAX) ||
					factSamples > std::uint64_t(blockCapacity) ||
					factSamples <= std::uint64_t(blockCapacity - pspSamplesPerBlock_)) return false;
				numSamples_ = std::int32_t(factSamples);
				pspSourceSamplesRemaining_ = numSamples_;
				duration_ = float(numSamples_) / float(frequency_);
				++MusicStreamsLoaded;
				return true;
			} else if (!SkipExact(*pspFile_, chunkSize + (chunkSize & 1U))) {
				return false;
			}
		}
	}
	void AudioStream::createReader(IAudioLoader&) {}

	bool AudioStream::rewindPsp()
	{
		if (pspFile_ == nullptr || pspFile_->Seek(pspDataOffset_, SeekOrigin::Begin) < 0) return false;
		pspDataRemaining_ = pspDataSize_;
		pspSourceSamplesRemaining_ = numSamples_;
		pspDecodedFrames_ = 0;
		pspDecodedCursor_ = 0;
		pspDecodeFailed_ = false;
		return true;
	}

	bool AudioStream::decodePspBlock()
	{
		if (pspDataRemaining_ <= 0 || pspSourceSamplesRemaining_ <= 0 || pspFile_ == nullptr || pspDecodeFailed_)
			return false;
		const std::int32_t bytes = std::min(pspBlockAlign_, pspDataRemaining_);
		if (bytes < numChannels_ * 4 || !ReadExact(*pspFile_, pspBlock_.get(), bytes)) {
			pspDecodeFailed_ = true;
			return false;
		}
		pspDataRemaining_ -= bytes;

		std::int32_t predictor[2];
		std::int32_t stepIndex[2];
		for (std::int32_t channel = 0; channel < 2; ++channel) {
			const unsigned char* header = pspBlock_.get() + channel * 4;
			predictor[channel] = std::int16_t(ReadLe16(header));
			stepIndex[channel] = std::clamp<std::int32_t>(header[2], 0, 88);
			pspDecoded_[channel] = std::int16_t(predictor[channel]);
		}

		std::int32_t frame = 1;
		const unsigned char* encoded = pspBlock_.get() + 8;
		const unsigned char* end = pspBlock_.get() + bytes;
		while (encoded + 8 <= end && frame < pspSamplesPerBlock_) {
			for (std::int32_t channel = 0; channel < 2; ++channel) {
				const unsigned char* group = encoded + channel * 4;
				for (std::int32_t i = 0; i < 4; ++i) {
					const std::uint8_t value = group[i];
					const std::int32_t outFrame = frame + i * 2;
					if (outFrame < pspSamplesPerBlock_)
						pspDecoded_[outFrame * 2 + channel] = DecodeImaNibble(value & 15, predictor[channel], stepIndex[channel]);
					if (outFrame + 1 < pspSamplesPerBlock_)
						pspDecoded_[(outFrame + 1) * 2 + channel] = DecodeImaNibble(value >> 4, predictor[channel], stepIndex[channel]);
				}
			}
			encoded += 8;
			frame += 8;
		}
		pspDecodedFrames_ = std::min({ frame, pspSamplesPerBlock_, pspSourceSamplesRemaining_ });
		pspSourceSamplesRemaining_ -= pspDecodedFrames_;
		pspDecodedCursor_ = 0;
		return pspDecodedFrames_ > 0;
	}

	std::int32_t AudioStream::decodePspFrames(std::int16_t* destination, std::int32_t frames, bool looping)
	{
		std::int32_t written = 0;
		while (written < frames) {
			if (pspDecodedCursor_ >= pspDecodedFrames_ * MusicExpansion) {
				if (!decodePspBlock()) {
					// A decode/I/O failure is not EOF: rewinding on one replays the readable prefix as a short loop.
					const bool reachedEnd = !pspDecodeFailed_ && pspSourceSamplesRemaining_ <= 0;
					if (!looping || !reachedEnd || !rewindPsp() || !decodePspBlock()) break;
				}
			}
			const std::int32_t outputFrames = pspDecodedFrames_ * MusicExpansion;
			const std::int32_t available = outputFrames - pspDecodedCursor_;
			const std::int32_t count = std::min(available, frames - written);
			// Frame counts stay in the source domain; tracking the cursor in output frames re-decodes blocks.
			for (std::int32_t i = 0; i < count; ++i) {
				const std::int32_t outputFrame = pspDecodedCursor_ + i;
				const std::int32_t sourceFrame = outputFrame / MusicExpansion;
				const std::int16_t* current = pspDecoded_.get() + sourceFrame * 2;
				std::int16_t* output = destination + (written + i) * 2;
				if ((outputFrame % MusicExpansion) != 0 && sourceFrame + 1 < pspDecodedFrames_) {
					const std::int16_t* next = current + 2;
					output[0] = std::int16_t((std::int32_t(current[0]) + next[0]) / 2);
					output[1] = std::int16_t((std::int32_t(current[1]) + next[1]) / 2);
				} else {
					output[0] = current[0];
					output[1] = current[1];
				}
			}
			pspDecodedCursor_ += count;
			written += count;
		}
		return written;
	}

	AudioStreamPlayer::AudioStreamPlayer() : IAudioPlayer(ObjectType::AudioStreamPlayer), audioStream_() {}
	AudioStreamPlayer::AudioStreamPlayer(StringView filename) : IAudioPlayer(ObjectType::AudioStreamPlayer), audioStream_(filename) {}
	AudioStreamPlayer::~AudioStreamPlayer() { stop(); }
	bool AudioStreamPlayer::loadFromFile(const char* filename) { return audioStream_.loadFromFile(filename); }
	void AudioStreamPlayer::play()
	{
		auto& device = static_cast<PspAudioDevice&>(theServiceLocator().GetAudioDevice());
		if (state_ == PlayerState::Paused && sourceId_ != IAudioDevice::UnavailableSource) {
			state_ = PlayerState::Playing;
			device.StartSource(sourceId_);
			return;
		}
		if (audioStream_.pspFile_ == nullptr) return;
		const std::uint32_t source = device.registerPlayer(this);
		if (source == IAudioDevice::UnavailableSource) return;
		sourceId_ = source;
		state_ = PlayerState::Playing;
		device.StartSource(sourceId_);
		++VoicesStarted;
	}
	void AudioStreamPlayer::pause()
	{
		if (state_ != PlayerState::Playing || sourceId_ == IAudioDevice::UnavailableSource) return;
		static_cast<PspAudioDevice&>(theServiceLocator().GetAudioDevice()).PauseSource(sourceId_);
		state_ = PlayerState::Paused;
	}
	void AudioStreamPlayer::stop()
	{
		if (sourceId_ != IAudioDevice::UnavailableSource) theServiceLocator().GetAudioDevice().unregisterPlayer(this);
		sourceId_ = IAudioDevice::UnavailableSource;
		state_ = PlayerState::Stopped;
		audioStream_.rewindPsp();
	}
	void AudioStreamPlayer::setLooping(bool value) { audioStream_.setLooping(value); IAudioPlayer::setLooping(value); }
	void AudioStreamPlayer::updateState() {}
}
