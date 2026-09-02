#pragma once

namespace NeuralRendering
{
	/** Runs DLSS Neural Rendering on the final LDR scene immediately before UI composite.
	 *  Dispatches to the flat or the VR stereo route. */
	bool ApplyBeforeUI();

	/** Releases all runtime and shared-resource state. */
	void Reset();
}