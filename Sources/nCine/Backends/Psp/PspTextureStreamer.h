#pragma once

#include <cstdint>
#include <cstddef>
#include <pspkernel.h>

namespace nCine
{
	// Background reader for the baked texture pack (Cache/texture.pak), used on the PSP-1000 where sprite pixels are
	// streamed on first draw and an inline fseek+fread would hitch the render thread. Keeps its OWN raw descriptor:
	// PspAssetPack's single FILE* has a shared seek position and cannot be used from two threads.
	//
	// Threading contract: the main thread owns all memory lifetime (alloc/free) and polls for completion; the worker
	// only seeks, reads and flushes into the caller's buffer, never malloc/free - newlib's allocator stays single-threaded.
	class PspTextureStreamer
	{
	public:
		static PspTextureStreamer& Get();

		// Open `texturePakPath` and start the worker. Idempotent. Callers gate on PspIsLowMemoryModel().
		void Start(const char* texturePakPath);
		void Stop();
		bool IsRunning() const { return running_; }

		// Queue a read of `size` bytes at file `offset` into `dst`. `dst` must stay valid and unfreed until the
		// request completes or is Wait()ed. Returns a handle (>= 0), or -1 if the queue is full / streamer stopped.
		int Submit(std::uint32_t offset, void* dst, std::uint32_t size);
		// Finished, either way? Cheap, lock-free.
		bool IsComplete(int handle) const;
		// Read all its bytes? Only meaningful once IsComplete().
		bool Succeeded(int handle) const;
		// Block until the request completes. Required before freeing a buffer whose read may still be in flight.
		void Wait(int handle);
		// Return the slot to the free pool. Call exactly once per successful Submit after consuming the result.
		void Release(int handle);

	private:
		PspTextureStreamer() = default;
		static int WorkerEntry(SceSize argc, void* argp);
		void WorkerLoop();

		static constexpr int SlotCount = 32;
		enum SlotState : int { Free = 0, Queued, Reading, DoneOk, DoneFail };
		struct Slot {
			volatile int State = Free;
			std::uint32_t Offset = 0;
			std::uint32_t Size = 0;
			void* Dst = nullptr;
			std::uint16_t Generation = 0;
		};
		Slot slots_[SlotCount];

		SceUID fd_ = -1;
		SceUID mutex_ = -1;   // guards slot state transitions (binary semaphore)
		SceUID wake_ = -1;    // counting: signalled on each Submit, awaited by the worker
		SceUID thread_ = -1;
		volatile bool running_ = false;

		static int MakeHandle(int index, std::uint16_t gen) { return (int)(((std::uint32_t)gen << 8) | (std::uint32_t)index); }
		static int HandleIndex(int handle) { return handle & 0xFF; }
		static std::uint16_t HandleGen(int handle) { return (std::uint16_t)((std::uint32_t)handle >> 8); }
	};
}
