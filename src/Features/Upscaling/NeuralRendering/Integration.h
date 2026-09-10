#pragma once

namespace NeuralRendering
{
	/** Runs DLSS Neural Rendering on the final LDR scene immediately before UI composite.
	 *  Dispatches to the flat or the VR stereo route. */
	bool ApplyBeforeUI();

	/** Runs DLSS Neural Rendering on the render-resolution kMAIN buffer before the DLSS
	 *  upscale pass consumes it, when Settings::preUpscale opts in. Flat route only. */
	bool ApplyBeforeUpscale();

	/** Releases all runtime and shared-resource state. */
	void Reset();
}