#pragma once

#include <cstddef>

#include <cstdint>

// PSP GU frame lifecycle. The engine draws through this instead of OpenGL: Viewport begins the frame and
// clears, RenderCommand::Issue appends GU draws, the gfx device ends the frame and swaps.
namespace nCine
{
	void PspGuInit();
	// Presents the previously submitted frame, after giving the CPU a chance to build the next one.
	void PspGuPrepareFrame();
	// Optional. A system utility (savedata, OSK) must draw after the game's list has finished but before
	// its framebuffer is presented.
	void PspGuSetSystemUtilityUpdate(void (*callback)());
	// Requests restoration of GU state on the first game frame after a Sony utility closes.
	void PspGuNotifySystemUtilityFinished();
	std::size_t PspGuDisplayListBytes();
	// Fence without presenting. Required before texture storage is modified or released.
	void PspGuWaitForPreviousFrame();
	// Releases renderer-owned texture storage and container capacity before an allocation-heavy cache rebuild.
	void PspReleaseRendererCaches();
	// Loads a non-paged streamed pack texture synchronously, bypassing the async streamer - for the loading
	// splash, which is on screen too briefly for a background read to land. `key` is Texture::GetGuiTexId().
	void PspForceResident(const void* key);
	// Whether a texture's pixels can be drawn right now. Only a non-paged streamed texture can answer false;
	// eager, paged and untracked ones have nothing to wait for.
	bool PspIsResident(const void* key);
	// Starts/advances the background read for a non-paged streamed texture without drawing it. Streaming is
	// otherwise pull-driven by draw, so a caller holding the previous animation until the new one lands must
	// call this or the texture is never requested and never becomes resident. Poll with PspIsResident.
	void PspRequestResident(const void* key);
	void PspGuBeginFrame(std::uint32_t clearColor);
	void PspGuEndFrame();
	bool PspGuIsInitialized();
}
