#include "Integration.h"

#include "Renderer.h"
#include "Settings.h"
#include "Features/Upscaling.h"
#include "Features/Upscaling/FoveatedRender/Bridge.h"
#include "Features/Upscaling/FoveatedRender/Core.h"
#include "Globals.h"
#include "GpuPass.h"
#include "Utils/LazyShader.h"

#include <array>
#include <utility>
#include <vector>

namespace NeuralRendering
{
	namespace
	{
		eastl::unique_ptr<Texture2D> color[2];
		eastl::unique_ptr<Texture2D> colorWork;
		Util::LazyShader<ID3D11ComputeShader> hdrRangeCompressCS;
		Util::LazyShader<ID3D11ComputeShader> hdrRangeExpandCS;
		std::uint32_t colorWidth = 0;
		std::uint32_t colorHeight = 0;
		DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
		std::uint32_t lastAppliedFrame = UINT32_MAX;
		bool writebackLogged = false;
		bool flatRouteWasActive = false;

		ID3D11Texture2D* ResolveRenderTargetTexture(
			const RE::BSGraphics::RenderTargetData& target,
			winrt::com_ptr<ID3D11Texture2D>& holder)
		{
			if (target.texture)
				return target.texture;
			auto resolveView = [&](ID3D11View* view) -> ID3D11Texture2D* {
				if (!view)
					return nullptr;
				winrt::com_ptr<ID3D11Resource> resource;
				view->GetResource(resource.put());
				if (!resource || FAILED(resource->QueryInterface(holder.put())))
					return nullptr;
				return holder.get();
			};
			if (auto* texture = resolveView(target.SRV))
				return texture;
			return resolveView(target.RTV);
		}

		/// <summary>True when kFRAMEBUFFER is the HDR float target, whose highlights run past 1.0.</summary>
		bool NeedsHdrRangeMapping()
		{
			return colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ||
			       colorFormat == DXGI_FORMAT_R32G32B32A32_FLOAT ||
			       colorFormat == DXGI_FORMAT_R11G11B10_FLOAT;
		}

		/// <summary>Runs one direction of the reversible highlight compression that keeps
		/// out-of-range HDR values away from Feature 18. Returns false if the shader is
		/// unavailable, which leaves the framebuffer untouched.</summary>
		bool DispatchHdrRangeMap(ID3D11DeviceContext* context, bool inverse, ID3D11ShaderResourceView* source,
			ID3D11UnorderedAccessView* destination, std::uint32_t width, std::uint32_t height)
		{
			std::vector<std::pair<const char*, const char*>> defines;
			if (inverse)
				defines.emplace_back("INVERSE", "");
			auto& lazyShader = inverse ? hdrRangeExpandCS : hdrRangeCompressCS;
			auto* shader = lazyShader.Get(L"Data\\Shaders\\Upscaling\\NeuralRendering\\HdrRangeMapCS.hlsl",
				defines, "cs_5_0", "main",
				inverse ? "NeuralRendering::HdrRangeExpandCS" : "NeuralRendering::HdrRangeCompressCS");
			if (!shader || !source || !destination)
				return false;
			context->CSSetShader(shader, nullptr, 0);
			context->CSSetShaderResources(0, 1, &source);
			context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
			context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
			ID3D11ShaderResourceView* nullSRV = nullptr;
			ID3D11UnorderedAccessView* nullUAV = nullptr;
			context->CSSetShaderResources(0, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
			return true;
		}

		bool EnsureColorResources(ID3D11Resource* source, std::uint32_t width, std::uint32_t height)
		{
			winrt::com_ptr<ID3D11Texture2D> sourceTexture;
			if (!source || FAILED(source->QueryInterface(sourceTexture.put())))
				return false;
			D3D11_TEXTURE2D_DESC sourceDesc{};
			sourceTexture->GetDesc(&sourceDesc);
			if (color[0] && colorWidth == width && colorHeight == height && colorFormat == sourceDesc.Format)
				return true;
			const std::uint32_t resourceCount = globals::game::isVR ? 2u : 1u;
			for (std::uint32_t eye = 0; eye < resourceCount; ++eye) {
				color[eye] = Upscaling::CreateTextureFromSource(source, width, height, false, true, true,
					eye == 0 ? "NeuralRendering::LdrColorLeft" : "NeuralRendering::LdrColorRight");
				if (!color[eye])
					return false;
			}
			if (!globals::game::isVR)
				color[1].reset();
			colorWidth = width;
			colorHeight = height;
			colorFormat = sourceDesc.Format;
			// The range map reads and writes distinct resources, so the round trip needs a
			// second staging surface. It only exists while the HDR framebuffer is in play.
			colorWork.reset();
			if (NeedsHdrRangeMapping()) {
				colorWork = Upscaling::CreateTextureFromSource(source, width, height, false, true, true,
					"NeuralRendering::HdrRangeWork");
				if (!colorWork)
					return false;
			}
			return true;
		}

		Tuning GetTuning(const Settings& settings)
		{
			return {
				settings.intensity,
				settings.localTone,
				settings.localStructure,
				settings.skinStructure,
				settings.style,
				settings.autoMask,
				settings.uiCorrection,
			};
		}

		bool ApplyFlatLdr(Upscaling& upscaling)
		{
			auto* renderer = globals::game::renderer;
			winrt::com_ptr<ID3D11Texture2D> framebufferHolder;
			ID3D11Texture2D* framebuffer = nullptr;
			if (renderer) {
				auto& target = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
				framebuffer = ResolveRenderTargetTexture(target, framebufferHolder);
			}
			const bool routeActive = upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS &&
				upscaling.neuralRendering.enabled;
			if (!routeActive) {
				if (flatRouteWasActive)
					Reset();
				return false;
			}
			flatRouteWasActive = true;
			// preUpscale already ran the pass in Upscale(), before this frame's DLSS
			// upscale call consumed kMAIN; running it again here would double-apply.
			if (upscaling.neuralRendering.preUpscale)
				return true;

			const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
			if (lastAppliedFrame == frame)
				return true;
			auto* context = globals::d3d::context;
			if (!renderer || !context || !globals::d3d::device || !upscaling.motionVectorCopyTexture)
				return false;

			auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			if (!framebuffer || !depth.texture || !depth.depthSRV || !upscaling.motionVectorCopyTexture->resource)
				return false;

			D3D11_TEXTURE2D_DESC totalDesc{};
			D3D11_TEXTURE2D_DESC motionDesc{};
			framebuffer->GetDesc(&totalDesc);
			upscaling.motionVectorCopyTexture->resource->GetDesc(&motionDesc);
			if (!EnsureColorResources(framebuffer, totalDesc.Width, totalDesc.Height))
				return false;

			CS_GPU_PASS("NeuralRendering::FlatLdrBeforeUI");
			ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* savedDSV = nullptr;
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
			context->OMSetRenderTargets(0, nullptr, nullptr);

			const bool rangeMapped = colorWork != nullptr;
			bool succeeded = true;
			if (rangeMapped) {
				context->CopyResource(colorWork->resource.get(), framebuffer);
				succeeded = DispatchHdrRangeMap(context, false, colorWork->srv.get(), color[0]->uav.get(),
					totalDesc.Width, totalDesc.Height);
			} else {
				context->CopyResource(color[0]->resource.get(), framebuffer);
			}

			for (std::uint32_t pass = 0; succeeded && pass < upscaling.neuralRendering.passes; ++pass)
				succeeded = Renderer::Instance().Apply(globals::d3d::device, context, 0,
					color[0]->resource.get(), depth.texture, depth.depthSRV,
					upscaling.motionVectorCopyTexture->resource.get(), motionDesc.Width, motionDesc.Height,
					totalDesc.Width, totalDesc.Height, static_cast<float>(motionDesc.Width),
					static_cast<float>(motionDesc.Height), GetTuning(upscaling.neuralRendering));

			if (succeeded) {
				if (rangeMapped) {
					succeeded = DispatchHdrRangeMap(context, true, color[0]->srv.get(), colorWork->uav.get(),
						totalDesc.Width, totalDesc.Height);
					if (succeeded)
						context->CopyResource(framebuffer, colorWork->resource.get());
				} else {
					context->CopyResource(framebuffer, color[0]->resource.get());
				}
			}

			if (succeeded) {
				lastAppliedFrame = frame;
				if (!writebackLogged) {
					logger::info("[DLSSNR] Flat LDR kFRAMEBUFFER output written before UI guides={}x{} color={}x{} rangeMapped={}",
						motionDesc.Width, motionDesc.Height, totalDesc.Width, totalDesc.Height, rangeMapped);
					writebackLogged = true;
				}
			}

			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
			for (auto*& rtv : savedRTVs)
				if (rtv) rtv->Release();
			if (savedDSV) savedDSV->Release();
			return succeeded;
		}

		/// <summary>The preUpscale counterpart of ApplyFlatLdr: same guide/tuning inputs, but
		/// runs on the render-resolution kMAIN buffer before Upscale() reads it as DLSS's
		/// colorIn, instead of on the display-resolution post-tonemap kFRAMEBUFFER. Flat only --
		/// VR's post-upscale route has its own per-eye subrect handling this mirrors.</summary>
		bool ApplyPreUpscaleFlat(Upscaling& upscaling)
		{
			if (globals::game::isVR || upscaling.GetUpscaleMethod() != Upscaling::UpscaleMethod::kDLSS ||
				!upscaling.neuralRendering.enabled || !upscaling.neuralRendering.preUpscale)
				return false;

			auto* renderer = globals::game::renderer;
			auto* context = globals::d3d::context;
			if (!renderer || !context || !globals::d3d::device || !upscaling.motionVectorCopyTexture)
				return false;

			auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
			auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			if (!main.texture || !depth.texture || !depth.depthSRV || !upscaling.motionVectorCopyTexture->resource)
				return false;

			D3D11_TEXTURE2D_DESC mainDesc{};
			D3D11_TEXTURE2D_DESC motionDesc{};
			main.texture->GetDesc(&mainDesc);
			upscaling.motionVectorCopyTexture->resource->GetDesc(&motionDesc);
			if (!EnsureColorResources(main.texture, mainDesc.Width, mainDesc.Height))
				return false;

			CS_GPU_PASS("NeuralRendering::PreUpscale");
			ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* savedDSV = nullptr;
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
			context->OMSetRenderTargets(0, nullptr, nullptr);

			const bool rangeMapped = colorWork != nullptr;
			bool succeeded = true;
			if (rangeMapped) {
				context->CopyResource(colorWork->resource.get(), main.texture);
				succeeded = DispatchHdrRangeMap(context, false, colorWork->srv.get(), color[0]->uav.get(),
					mainDesc.Width, mainDesc.Height);
			} else {
				context->CopyResource(color[0]->resource.get(), main.texture);
			}

			for (std::uint32_t pass = 0; succeeded && pass < upscaling.neuralRendering.passes; ++pass)
				succeeded = Renderer::Instance().Apply(globals::d3d::device, context, 0,
					color[0]->resource.get(), depth.texture, depth.depthSRV,
					upscaling.motionVectorCopyTexture->resource.get(), motionDesc.Width, motionDesc.Height,
					mainDesc.Width, mainDesc.Height, static_cast<float>(motionDesc.Width),
					static_cast<float>(motionDesc.Height), GetTuning(upscaling.neuralRendering));

			if (succeeded) {
				if (rangeMapped) {
					succeeded = DispatchHdrRangeMap(context, true, color[0]->srv.get(), colorWork->uav.get(),
						mainDesc.Width, mainDesc.Height);
					if (succeeded)
						context->CopyResource(main.texture, colorWork->resource.get());
				} else {
					context->CopyResource(main.texture, color[0]->resource.get());
				}
			}

			if (succeeded && !writebackLogged) {
				logger::info("[DLSSNR] Pre-upscale kMAIN written before DLSS upscale guides={}x{} color={}x{} rangeMapped={}",
					motionDesc.Width, motionDesc.Height, mainDesc.Width, mainDesc.Height, rangeMapped);
				writebackLogged = true;
			}

			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
			for (auto*& rtv : savedRTVs)
				if (rtv) rtv->Release();
			if (savedDSV) savedDSV->Release();
			return succeeded;
		}
	}

	bool ApplyBeforeUpscale()
	{
		return ApplyPreUpscaleFlat(globals::features::upscaling);
	}

	bool ApplyBeforeUI()
	{
		auto& upscaling = globals::features::upscaling;
		auto& foveated = upscaling.foveatedRender;
		if (!globals::game::isVR)
			return ApplyFlatLdr(upscaling);
		if (!globals::game::isVR || !FoveatedRenderImpl::Bridge::IsRouteActive() ||
			upscaling.GetUpscaleMethod() != Upscaling::UpscaleMethod::kDLSS ||
			foveated.GetDlssMode() != FoveatedRender::DlssMode::kDefault ||
			!upscaling.neuralRendering.enabled)
			return false;

		const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
		const std::uint32_t guideFrame = FoveatedRenderImpl::Core::neuralGuidesFrame;
		if (lastAppliedFrame == frame || (guideFrame != frame && !(frame > 0 && guideFrame == frame - 1)))
			return false;

		auto* renderer = globals::game::renderer;
		auto* context = globals::d3d::context;
		if (!renderer || !context || !globals::d3d::device ||
			!FoveatedRenderImpl::Core::vrSubrectDepth[0] || !FoveatedRenderImpl::Core::vrSubrectDepth[1] ||
			!FoveatedRenderImpl::Core::vrSubrectMotionVectors[0] || !FoveatedRenderImpl::Core::vrSubrectMotionVectors[1])
			return false;
		auto& total = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTOTAL];
		if (!total.texture)
			return false;

		D3D11_TEXTURE2D_DESC totalDesc{};
		total.texture->GetDesc(&totalDesc);
		const auto& leftUV = foveated.subrectController.GetUV();
		const auto& rightUV = foveated.subrectController.GetRightEyeUV();
		if (leftUV.w != rightUV.w || leftUV.h != rightUV.h)
			return false;
		const std::uint32_t eyeWidth = totalDesc.Width / 2;
		const std::uint32_t outWidth = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(eyeWidth * leftUV.w));
		const std::uint32_t outHeight = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(totalDesc.Height * leftUV.h));

		CS_GPU_PASS("NeuralRendering::FoveatedLdrBeforeUI");
		ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* savedDSV = nullptr;
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
		context->OMSetRenderTargets(0, nullptr, nullptr);

		const Util::Subrect::UVRegion* eyeUVs[2]{ &leftUV, &rightUV };
		std::array<Renderer::StereoEyeInput, 2> inputs{};
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			const auto& uv = *eyeUVs[eye];
			const std::uint32_t x = (eye ? eyeWidth : 0) + static_cast<std::uint32_t>(eyeWidth * uv.x);
			const std::uint32_t y = static_cast<std::uint32_t>(totalDesc.Height * uv.y);
			float motionScaleX = 1.0f;
			float motionScaleY = 1.0f;
			FoveatedRenderImpl::Bridge::ComputeMvecScale(eye, motionScaleX, motionScaleY);
			inputs[eye] = {
				.depth = FoveatedRenderImpl::Core::vrSubrectDepth[eye]->resource.get(),
				.depthSRV = FoveatedRenderImpl::Core::vrSubrectDepth[eye]->srv.get(),
				.motionVectors = FoveatedRenderImpl::Core::vrSubrectMotionVectors[eye]->resource.get(),
				.sourceX = x,
				.sourceY = y,
				.motionVectorScaleX = motionScaleX * FoveatedRenderImpl::Core::vrSubrectInW,
				.motionVectorScaleY = motionScaleY * FoveatedRenderImpl::Core::vrSubrectInH,
			};
		}
		bool succeeded = true;
		for (std::uint32_t pass = 0; succeeded && pass < upscaling.neuralRendering.passes; ++pass)
			succeeded = Renderer::Instance().ApplyStereo(globals::d3d::device, context,
				total.texture, inputs, FoveatedRenderImpl::Core::vrSubrectInW, FoveatedRenderImpl::Core::vrSubrectInH,
				outWidth, outHeight, GetTuning(upscaling.neuralRendering));
		if (succeeded) {
			lastAppliedFrame = frame;
			if (!writebackLogged) {
				logger::info("[DLSSNR] LDR output written before UI composite size={}x{} batchedAsync=true", outWidth, outHeight);
				writebackLogged = true;
			}
		}

		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
		for (auto*& rtv : savedRTVs)
			if (rtv) rtv->Release();
		if (savedDSV) savedDSV->Release();
		return succeeded;
	}

	void Reset()
	{
		Renderer::Instance().Reset();
		color[0].reset();
		color[1].reset();
		colorWork.reset();
		hdrRangeCompressCS.Reset();
		hdrRangeExpandCS.Reset();
		colorWidth = colorHeight = 0;
		colorFormat = DXGI_FORMAT_UNKNOWN;
		lastAppliedFrame = UINT32_MAX;
		writebackLogged = false;
		flatRouteWasActive = false;
	}
}
