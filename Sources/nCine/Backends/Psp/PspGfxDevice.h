#pragma once

#include "nCine/Graphics/IGfxDevice.h"
#include "PspGu.h"

namespace nCine
{
	// Drawing happens in the GU backend at RenderCommand::Issue, not here, so every operation stays the
	// NullGfxDevice no-op. This exists to report the fixed 480x272 screen to the engine's viewport math.
	class PspGfxDevice : public NullGfxDevice
	{
	public:
		static constexpr int ScreenWidth = 480;
		static constexpr int ScreenHeight = 272;

		PspGfxDevice()
		{
			width_ = ScreenWidth;
			height_ = ScreenHeight;
			drawableWidth_ = ScreenWidth;
			drawableHeight_ = ScreenHeight;
			currentVideoMode_.width = ScreenWidth;
			currentVideoMode_.height = ScreenHeight;
			currentVideoMode_.refreshRate = 60.0f;
		}

		// Engine end-of-frame hook: closes the GU list and queues the swap.
		void update() override { PspGuEndFrame(); }
	};
}
