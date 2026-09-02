#pragma once

#include <cstdint>

namespace NeuralRendering
{
	/**
	 * @brief User-facing tuning for the DLSS Neural Rendering pass.
	 *
	 * Shared by the flat and VR routes, so it is owned by Upscaling rather than by
	 * FoveatedRender and persisted under Upscaling's "neuralRendering" key.
	 */
	struct Settings
	{
		bool enabled = false;      ///< Opt-in: the nvngx_dlssnr runtime is user-supplied.
		std::uint32_t preset = 3;  ///< 0=Custom, 1=Balanced, 2=Fabric Detail, 3=Natural, 4=Strong.
		float intensity = 0.8f;
		float localTone = 0.75f;
		float localStructure = 0.9f;
		float skinStructure = 0.9f;
		std::uint32_t style = 3;
		bool autoMask = true;
		bool uiCorrection = false;
	};
}
