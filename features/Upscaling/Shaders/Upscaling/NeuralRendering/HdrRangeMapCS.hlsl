/// Reversible highlight compression for the neural rendering colour input.
///
/// Under the HDR pipeline kFRAMEBUFFER is the float16 scene target, so it carries
/// values well above 1.0. Feature 18 is trained on display-referred LDR and turns
/// out-of-range input into blocky garbage around bright lights and reflections, so
/// the forward pass folds the highlights into [0,1] and the INVERSE permutation
/// restores them afterwards. Below kKneeThreshold the mapping is the identity,
/// which keeps the range that already worked in SDR bit-identical here.

Texture2D<float4> SourceColor : register(t0);
RWTexture2D<float4> DestinationColor : register(u0);

/// Highlights roll off above this; below it both directions pass values through.
static const float kKneeThreshold = 0.6;
/// Caps the inverse so a network output pinned at 1.0 cannot expand to infinity.
static const float kMaxExpansion = 255.0;

/// Maps [kKneeThreshold, inf) onto [kKneeThreshold, 1) with a C1-continuous knee.
float CompressHighlight(float peak)
{
	const float range = 1.0 - kKneeThreshold;
	const float excess = (peak - kKneeThreshold) / range;
	return kKneeThreshold + range * (excess / (1.0 + excess));
}

/// Exact inverse of CompressHighlight, bounded by kMaxExpansion.
float ExpandHighlight(float peak)
{
	const float range = 1.0 - kKneeThreshold;
	const float excess = min((peak - kKneeThreshold) / range, kMaxExpansion / (1.0 + kMaxExpansion));
	return kKneeThreshold + range * (excess / (1.0 - excess));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint width;
	uint height;
	DestinationColor.GetDimensions(width, height);
	if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
		return;

	const float4 source = SourceColor[dispatchThreadID.xy];
	float3 color = max(0.0, source.rgb);

	// Scaling by the peak channel rather than luminance keeps hue and saturation
	// intact and guarantees every channel lands inside [0,1].
	const float peak = max(color.r, max(color.g, color.b));
	if (peak > kKneeThreshold) {
#if defined(INVERSE)
		color *= ExpandHighlight(peak) / peak;
#else
		color *= CompressHighlight(peak) / peak;
#endif
	}

	DestinationColor[dispatchThreadID.xy] = float4(color, source.a);
}
