#pragma once

#include <cstddef>
#include <kubridge.h>

namespace nCine
{
	// sceKernelGetModel() is kernel-only; KUBridge exposes it to a user-mode EBOOT and PPSSPP implements it
	// from the configured model. A negative result means unknown - treat it as a PSP-1000.
	inline int PspHardwareModel()
	{
		static const int model = kuKernelGetModel();
		return model;
	}

	inline std::size_t PspHeapCapacityBytes()
	{
		return (PspHardwareModel() > 0 ? 64u : 32u) * 1024u * 1024u;
	}

	inline bool PspIsLowMemoryModel()
	{
#if defined(JAZZ2_PSP_FORCE_LOW_MEMORY)
		// -DPSP_FORCE_LOW_MEMORY=ON: exercise the streaming/bounded-cache profile on 64 MiB hardware.
		return true;
#else
		return PspHardwareModel() <= 0;
#endif
	}
}
