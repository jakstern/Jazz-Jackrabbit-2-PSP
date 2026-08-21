#include "PspTextureStreamer.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

namespace nCine
{
	PspTextureStreamer& PspTextureStreamer::Get()
	{
		static PspTextureStreamer instance;
		return instance;
	}

	void PspTextureStreamer::Start(const char* texturePakPath)
	{
		if (running_) return;

		// Raw sceIoOpen needs a device-qualified absolute path on real hardware; unlike fopen it does not resolve
		// against the cwd, and a relative "Cache/texture.pak" silently fails, leaving every read synchronous.
		char raw[512];
		if (std::strchr(texturePakPath, ':') != nullptr) {
			std::snprintf(raw, sizeof(raw), "%s", texturePakPath);
		} else {
			char cwd[256];
			cwd[0] = '\0';
			getcwd(cwd, sizeof(cwd));
			std::snprintf(raw, sizeof(raw), "%s/%s", cwd, texturePakPath);
		}
		char absPath[512];
		std::size_t w = 0;
		for (std::size_t r = 0; raw[r] != '\0' && w + 1 < sizeof(absPath); ++r) {
			if (raw[r] == '/' && w > 0 && absPath[w - 1] == '/') continue; // skip duplicate '/', keeps "ms0:/" intact
			absPath[w++] = raw[r];
		}
		absPath[w] = '\0';

		fd_ = sceIoOpen(absPath, PSP_O_RDONLY, 0777);
		if (fd_ < 0) { fd_ = -1; return; }

		for (Slot& s : slots_) { s.State = Free; s.Dst = nullptr; s.Offset = s.Size = 0; }

		mutex_ = sceKernelCreateSema("Jazz2TexStreamMutex", 0, 1, 1, nullptr);
		wake_ = sceKernelCreateSema("Jazz2TexStreamWake", 0, 0, SlotCount + 8, nullptr);
		if (mutex_ < 0 || wake_ < 0) goto fail;

		running_ = true;
		// Priority 0x2E is deliberately below the main thread's, so streaming only runs while it waits or idles.
		thread_ = sceKernelCreateThread("Jazz2TexStream", WorkerEntry, 0x2E, 8 * 1024, PSP_THREAD_ATTR_USER, nullptr);
		if (thread_ < 0) goto fail;
		{
			PspTextureStreamer* self = this;
			if (sceKernelStartThread(thread_, sizeof(self), &self) < 0) goto fail;
		}
		return;

	fail:
		running_ = false;
		if (thread_ >= 0) { sceKernelDeleteThread(thread_); thread_ = -1; }
		if (wake_ >= 0) { sceKernelDeleteSema(wake_); wake_ = -1; }
		if (mutex_ >= 0) { sceKernelDeleteSema(mutex_); mutex_ = -1; }
		if (fd_ >= 0) { sceIoClose(fd_); fd_ = -1; }
	}

	void PspTextureStreamer::Stop()
	{
		if (!running_) {
			if (fd_ >= 0) { sceIoClose(fd_); fd_ = -1; }
			return;
		}
		running_ = false;
		sceKernelSignalSema(wake_, 1); // wake the worker so it observes running_ == false and exits
		if (thread_ >= 0) {
			sceKernelWaitThreadEnd(thread_, nullptr);
			sceKernelDeleteThread(thread_);
			thread_ = -1;
		}
		// Fail whatever the (now dead) worker never finished, or a lingering handle's Wait() spins forever.
		for (Slot& s : slots_) {
			if (s.State == Queued || s.State == Reading) s.State = DoneFail;
		}
		if (wake_ >= 0) { sceKernelDeleteSema(wake_); wake_ = -1; }
		if (mutex_ >= 0) { sceKernelDeleteSema(mutex_); mutex_ = -1; }
		if (fd_ >= 0) { sceIoClose(fd_); fd_ = -1; }
	}

	int PspTextureStreamer::WorkerEntry(SceSize argc, void* argp)
	{
		if (argc == sizeof(PspTextureStreamer*) && argp != nullptr) {
			(*static_cast<PspTextureStreamer**>(argp))->WorkerLoop();
		}
		return 0;
	}

	void PspTextureStreamer::WorkerLoop()
	{
		while (running_) {
			sceKernelWaitSema(wake_, 1, nullptr);
			if (!running_) break;

			// Drain everything queued: a burst of Submits signals wake_ more often than the worker consumes it.
			for (;;) {
				int idx = -1;
				std::uint32_t offset = 0, size = 0;
				void* dst = nullptr;

				sceKernelWaitSema(mutex_, 1, nullptr);
				for (int i = 0; i < SlotCount; ++i) {
					if (slots_[i].State == Queued) {
						slots_[i].State = Reading;
						idx = i; offset = slots_[i].Offset; size = slots_[i].Size; dst = slots_[i].Dst;
						break;
					}
				}
				sceKernelSignalSema(mutex_, 1);
				if (idx < 0) break;

				bool ok = false;
				if (fd_ >= 0 && dst != nullptr) {
					if (sceIoLseek(fd_, (SceOff)offset, PSP_SEEK_SET) == (SceOff)offset) {
						ok = (sceIoRead(fd_, dst, (SceSize)size) == (int)size);
					}
				}
				if (ok) sceKernelDcacheWritebackRange(dst, size);

				sceKernelWaitSema(mutex_, 1, nullptr);
				slots_[idx].State = (ok ? DoneOk : DoneFail);
				sceKernelSignalSema(mutex_, 1);
			}
		}
	}

	int PspTextureStreamer::Submit(std::uint32_t offset, void* dst, std::uint32_t size)
	{
		if (!running_ || dst == nullptr) return -1;
		int handle = -1;
		sceKernelWaitSema(mutex_, 1, nullptr);
		for (int i = 0; i < SlotCount; ++i) {
			if (slots_[i].State == Free) {
				slots_[i].State = Queued;
				slots_[i].Offset = offset;
				slots_[i].Size = size;
				slots_[i].Dst = dst;
				handle = MakeHandle(i, slots_[i].Generation);
				break;
			}
		}
		sceKernelSignalSema(mutex_, 1);
		if (handle >= 0) sceKernelSignalSema(wake_, 1);
		return handle;
	}

	bool PspTextureStreamer::IsComplete(int handle) const
	{
		if (handle < 0) return true;
		const Slot& s = slots_[HandleIndex(handle)];
		if (s.Generation != HandleGen(handle)) return true; // stale handle: treat as complete
		const int state = s.State;
		return (state == DoneOk || state == DoneFail);
	}

	bool PspTextureStreamer::Succeeded(int handle) const
	{
		if (handle < 0) return false;
		const Slot& s = slots_[HandleIndex(handle)];
		return (s.Generation == HandleGen(handle) && s.State == DoneOk);
	}

	void PspTextureStreamer::Wait(int handle)
	{
		if (handle < 0) return;
		while (!IsComplete(handle)) {
			sceKernelDelayThread(80); // waiting is exceptional, so a short poll is fine
		}
	}

	void PspTextureStreamer::Release(int handle)
	{
		if (handle < 0) return;
		const int index = HandleIndex(handle);
		const bool locked = (mutex_ >= 0); // after Stop() the worker is gone, so no lock is needed (or possible)
		if (locked) sceKernelWaitSema(mutex_, 1, nullptr);
		if (slots_[index].Generation == HandleGen(handle)) {
			slots_[index].Generation++;
			slots_[index].State = Free;
			slots_[index].Dst = nullptr;
		}
		if (locked) sceKernelSignalSema(mutex_, 1);
	}
}
