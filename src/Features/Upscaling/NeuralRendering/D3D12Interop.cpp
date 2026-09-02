#include "D3D12Interop.h"

#include "Utils/D3D.h"

#include <utility>

#include <Windows.h>

namespace NeuralRendering
{
	namespace
	{
		constexpr std::uint64_t kSyncTimeoutMs = 250;
	}

	D3D12Interop::~D3D12Interop() { Shutdown(); }

	bool D3D12Interop::RecordFailure(const char* operation, HRESULT result)
	{
		lastOperation_ = operation;
		lastError_ = result;
		return false;
	}

	bool D3D12Interop::AdoptFenceContext(ID3D11Device* device, ID3D11DeviceContext* context,
		ID3D11DeviceContext4* proxyContext, HRESULT queryResult)
	{
		Microsoft::WRL::ComPtr<ID3D11Device> contextDevice;
		context->GetDevice(&contextDevice);
		logger::warn("[DLSSNR] render context lacks ID3D11DeviceContext4 hr=0x{:08X} type={} sameDevice={} proxyContext={}",
			static_cast<std::uint32_t>(queryResult), static_cast<int>(context->GetType()),
			contextDevice.Get() == device, proxyContext != nullptr);

		// A third-party wrapper can stop at ID3D11DeviceContext3 while still forwarding
		// device queries untouched. Every view of the immediate context records into the
		// same command stream, so any v4-capable one of them can carry the fence.
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediateContext;
		device->GetImmediateContext(&immediateContext);
		if (immediateContext && SUCCEEDED(immediateContext.As(&context11_)))
			return true;
		// Frame generation captured the immediate context at device creation, before any
		// wrapper had a chance to attach.
		if (proxyContext) {
			context11_ = proxyContext;
			logger::info("[DLSSNR] using the frame-generation immediate context for fence sync");
			return true;
		}

		// Nothing here exposes ID3D11Fence. CPU round-trips are more expensive 
		// GPU side but don't need a new context
		logger::warn("[DLSSNR] no ID3D11DeviceContext4 available; synchronising on the CPU instead");
		gpuFenceSync_ = false;
		return true;
	}

	bool D3D12Interop::FlushD3D11()
	{
		contextBase_->End(flushQuery_.Get());
		contextBase_->Flush();
		const std::uint64_t deadline = GetTickCount64() + kSyncTimeoutMs;
		for (;;) {
			BOOL retired = FALSE;
			const HRESULT result = contextBase_->GetData(flushQuery_.Get(), &retired, sizeof(retired), 0);
			if (FAILED(result)) return RecordFailure("D3D11FlushQuery", result);
			if (result == S_OK && retired) {
				lastError_ = S_OK;
				return true;
			}
			if (GetTickCount64() >= deadline)
				return RecordFailure("D3D11FlushTimeout", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
			SwitchToThread();
		}
	}

	bool D3D12Interop::CreateDeviceObjects()
	{
		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		HRESULT result = device12_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue12_));
		if (FAILED(result)) return RecordFailure("CreateCommandQueue", result);
		for (auto& commandContext : commandContexts_) {
			result = device12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
				IID_PPV_ARGS(&commandContext.allocator));
			if (FAILED(result)) return RecordFailure("CreateCommandAllocator", result);
			result = device12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				commandContext.allocator.Get(), nullptr, IID_PPV_ARGS(&commandContext.commandList));
			if (FAILED(result)) return RecordFailure("CreateCommandList", result);
			result = commandContext.commandList->Close();
			if (FAILED(result)) return RecordFailure("CommandListClose", result);
		}

		const D3D12_FENCE_FLAGS fenceFlags = gpuFenceSync_ ? D3D12_FENCE_FLAG_SHARED : D3D12_FENCE_FLAG_NONE;
		result = device12_->CreateFence(0, fenceFlags, IID_PPV_ARGS(&fence12_));
		if (FAILED(result)) return RecordFailure("CreateFence", result);
		if (gpuFenceSync_) {
			HANDLE sharedFence = nullptr;
			result = device12_->CreateSharedHandle(fence12_.Get(), nullptr, GENERIC_ALL, nullptr, &sharedFence);
			if (FAILED(result)) return RecordFailure("CreateSharedHandle(Fence)", result);
			result = device11_->OpenSharedFence(sharedFence, IID_PPV_ARGS(&fence11_));
			CloseHandle(sharedFence);
			if (FAILED(result)) return RecordFailure("OpenSharedFence", result);
		}
		lastError_ = S_OK;
		return true;
	}

	void D3D12Interop::ReleaseDeviceObjects()
	{
		fence11_.Reset();
		fence12_.Reset();
		commandContexts_ = {};
		queue12_.Reset();
		device12_.Reset();
	}

	bool D3D12Interop::Initialize(IDXGIAdapter* adapter, ID3D11Device* device, ID3D11DeviceContext* context,
		ID3D12Device* existingDevice, ID3D11DeviceContext4* proxyContext)
	{
		Shutdown();
		if (!adapter || !device || !context)
			return RecordFailure("InitializeArguments", E_INVALIDARG);

		HRESULT result = device->QueryInterface(IID_PPV_ARGS(&device11_));
		if (FAILED(result)) return RecordFailure("QueryInterface(ID3D11Device5)", result);
		contextBase_ = context;
		result = context->QueryInterface(IID_PPV_ARGS(&context11_));
		if (FAILED(result) && !AdoptFenceContext(device, context, proxyContext, result))
			return RecordFailure("QueryInterface(ID3D11DeviceContext4)", result);
		if (!gpuFenceSync_) {
			const D3D11_QUERY_DESC queryDesc{ D3D11_QUERY_EVENT, 0 };
			result = device11_->CreateQuery(&queryDesc, &flushQuery_);
			if (FAILED(result)) return RecordFailure("CreateQuery(Event)", result);
			Util::SetResourceName(flushQuery_.Get(), "NeuralRendering::FlushQuery");
		}

		// NGX keys its session off the D3D12 device, use existing instead of makinga new one that may break the session
		bool sharedDevice = false;
		if (existingDevice) {
			device12_ = existingDevice;
			sharedDevice = CreateDeviceObjects();
			if (!sharedDevice) {
				// Frame generation hands us a Streamline interposer proxy. If it refuses any
				// object we need, a private device still beats losing neural rendering.
				logger::warn("[DLSSNR] shared frame-generation D3D12 device rejected op={} hr=0x{:08X}; falling back to a private device",
					lastOperation_, static_cast<std::uint32_t>(lastError_));
				ReleaseDeviceObjects();
			}
		}
		if (!sharedDevice) {
			result = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device12_));
			if (FAILED(result)) return RecordFailure("D3D12CreateDevice", result);
			if (!CreateDeviceObjects()) return false;
		}

		fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!fenceEvent_) return RecordFailure("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
		initialized_ = true;
		lastOperation_ = "Initialize";
		lastError_ = S_OK;
		logger::info("[DLSSNR] D3D12 interop initialized commandContexts={} sharedDevice={} sync={}",
			kCommandContextCount, sharedDevice, gpuFenceSync_ ? "gpu-fence" : "cpu-roundtrip");
		return true;
	}

	void D3D12Interop::Shutdown()
	{
		if (fenceEvent_) CloseHandle(fenceEvent_);
		fenceEvent_ = nullptr;
		recording_ = false;
		recordingContext_ = kCommandContextCount;
		commandContextCursor_ = 0;
		backpressureLogged_ = false;
		initialized_ = false;
		gpuFenceSync_ = true;
		fenceValue_ = 0;
		ReleaseDeviceObjects();
		flushQuery_.Reset();
		context11_.Reset();
		contextBase_.Reset();
		device11_.Reset();
	}

	bool D3D12Interop::CreateSharedTexture(const D3D11_TEXTURE2D_DESC& sourceDesc, SharedTexture& texture, const char* name)
	{
		if (!initialized_ || sourceDesc.Width == 0 || sourceDesc.Height == 0 ||
			sourceDesc.ArraySize == 0 || sourceDesc.ArraySize > UINT16_MAX || sourceDesc.MipLevels > UINT16_MAX)
			return RecordFailure("CreateSharedTextureArguments", E_INVALIDARG);

		D3D11_TEXTURE2D_DESC desc = sourceDesc;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		if ((desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) == 0)
			return RecordFailure("CreateSharedTextureRequiresUAV", E_INVALIDARG);

		SharedTexture replacement;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		HRESULT result = device11_->CreateTexture2D(&desc, nullptr, &replacement.resource11);
		if (FAILED(result)) return RecordFailure("D3D11CreateTexture2D", result);
		Util::SetResourceName(replacement.resource11.Get(), name);
		result = device11_->CreateUnorderedAccessView(replacement.resource11.Get(), nullptr, &replacement.uav11);
		if (FAILED(result)) return RecordFailure("D3D11CreateUnorderedAccessView", result);
		Util::SetResourceName(replacement.uav11.Get(), "%s UAV", name);

		Microsoft::WRL::ComPtr<IDXGIResource1> dxgiResource;
		result = replacement.resource11.As(&dxgiResource);
		if (FAILED(result)) return RecordFailure("QueryInterface(IDXGIResource1)", result);
		HANDLE sharedTexture = nullptr;
		result = dxgiResource->CreateSharedHandle(nullptr,
			DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedTexture);
		if (FAILED(result)) return RecordFailure("DXGICreateSharedHandle", result);
		result = device12_->OpenSharedHandle(sharedTexture, IID_PPV_ARGS(&replacement.resource12));
		CloseHandle(sharedTexture);
		if (FAILED(result)) return RecordFailure("OpenSharedHandle", result);

		lastResourceFlags_ = static_cast<std::uint32_t>(replacement.resource12->GetDesc().Flags);
		replacement.desc = desc;
		texture = std::move(replacement);
		lastOperation_ = "CreateSharedTexture(D3D11First)";
		lastError_ = S_OK;
		return true;
	}

	bool D3D12Interop::BeginD3D12(ID3D12GraphicsCommandList** commandList)
	{
		if (!initialized_ || recording_ || !commandList) return RecordFailure("BeginD3D12State", E_UNEXPECTED);

		const std::uint64_t completedValue = fence12_->GetCompletedValue();
		std::size_t contextIndex = kCommandContextCount;
		for (std::size_t offset = 0; offset < kCommandContextCount; ++offset) {
			const std::size_t candidate = (commandContextCursor_ + offset) % kCommandContextCount;
			const auto pendingValue = commandContexts_[candidate].fenceValue;
			if (pendingValue == 0 || completedValue >= pendingValue) {
				contextIndex = candidate;
				break;
			}
		}
		if (contextIndex == kCommandContextCount) {
			contextIndex = commandContextCursor_;
			if (!backpressureLogged_) {
				logger::warn("[DLSSNR] D3D12 command contexts saturated; applying CPU backpressure");
				backpressureLogged_ = true;
			}
			if (!WaitForFence(commandContexts_[contextIndex].fenceValue))
				return false;
		}

		auto& commandContext = commandContexts_[contextIndex];
		commandContext.fenceValue = 0;
		HRESULT result = S_OK;
		if (gpuFenceSync_) {
			const std::uint64_t readyValue = ++fenceValue_;
			result = context11_->Signal(fence11_.Get(), readyValue);
			if (FAILED(result)) return RecordFailure("D3D11FenceSignal", result);
			result = queue12_->Wait(fence12_.Get(), readyValue);
			if (FAILED(result)) return RecordFailure("D3D12QueueWait", result);
		} else if (!FlushD3D11()) {
			return false;
		}
		result = commandContext.allocator->Reset();
		if (FAILED(result)) return RecordFailure("CommandAllocatorReset", result);
		result = commandContext.commandList->Reset(commandContext.allocator.Get(), nullptr);
		if (FAILED(result)) return RecordFailure("CommandListReset", result);
		recording_ = true;
		recordingContext_ = contextIndex;
		commandContextCursor_ = (contextIndex + 1) % kCommandContextCount;
		*commandList = commandContext.commandList.Get();
		return true;
	}

	bool D3D12Interop::EndD3D12()
	{
		if (!initialized_ || !recording_ || recordingContext_ >= kCommandContextCount)
			return RecordFailure("EndD3D12State", E_UNEXPECTED);
		recording_ = false;
		auto& commandContext = commandContexts_[recordingContext_];
		recordingContext_ = kCommandContextCount;
		HRESULT result = commandContext.commandList->Close();
		if (FAILED(result)) return RecordFailure("CommandListClose", result);
		ID3D12CommandList* lists[] = { commandContext.commandList.Get() };
		queue12_->ExecuteCommandLists(1, lists);
		const std::uint64_t completeValue = ++fenceValue_;
		result = queue12_->Signal(fence12_.Get(), completeValue);
		if (FAILED(result)) return RecordFailure("D3D12QueueSignal", result);
		commandContext.fenceValue = completeValue;

		// This queues a GPU-side dependency. Subsequent D3D11 output copies wait
		// for Feature 18 without stalling the render thread on the CPU.
		if (gpuFenceSync_) {
			result = context11_->Wait(fence11_.Get(), completeValue);
			if (FAILED(result)) return RecordFailure("D3D11FenceWait", result);
		} else if (!WaitForFence(completeValue)) {
			return false;
		}
		lastError_ = S_OK;
		return true;
	}

	bool D3D12Interop::WaitForFence(std::uint64_t value)
	{
		if (!value || fence12_->GetCompletedValue() >= value)
			return true;
		const HRESULT result = fence12_->SetEventOnCompletion(value, fenceEvent_);
		if (FAILED(result)) return RecordFailure("SetEventOnCompletion", result);
		const DWORD waitResult = WaitForSingleObject(fenceEvent_, static_cast<DWORD>(kSyncTimeoutMs));
		if (waitResult != WAIT_OBJECT_0)
			return RecordFailure("WaitForFence", waitResult == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) :
			                                                                        HRESULT_FROM_WIN32(GetLastError()));
		lastError_ = S_OK;
		return true;
	}

	bool D3D12Interop::WaitForIdle()
	{
		if (!initialized_)
			return true;
		std::uint64_t lastSubmittedValue = 0;
		for (const auto& commandContext : commandContexts_)
			lastSubmittedValue = std::max(lastSubmittedValue, commandContext.fenceValue);
		return WaitForFence(lastSubmittedValue);
	}
}
