#pragma once

namespace Jazz2::Tiles
{
	/** Describes how a layer's parallax scrolling speed is interpreted. */
	enum class LayerSpeedModel {
		Default,
		AlwaysOnTop,
		FitLevel,
		SpeedMultipliers
	};

	/** Specifies how a tile layer is rendered. Values are part of the cache format. */
	enum class LayerRendererType {
		Default,
		Solid,
		Tinted,
		Sky = 10,
		Circle
	};
}
