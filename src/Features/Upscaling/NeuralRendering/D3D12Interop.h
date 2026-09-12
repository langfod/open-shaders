#pragma once

#include <array>
#include <cstdint>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace NeuralRendering
{
	struct SharedTexture
	{
		Microsoft::WRL::ComPtr<ID3D11Texture2D> resource11;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav11;
		Microsoft::WRL::ComPtr<ID3D12Resource> resource12;
		D3D11_TEXTURE2D_DESC desc{};
	};

	class D3D12Interop
	{
	public:
		~D3D12Interop();

		D3D12Interop(const D3D12Interop&) = delete;
		D3D12Interop& operator=(const D3D12Interop&) = delete;

		/// <summary>Brings up the D3D11/D3D12 bridge. Pass the frame-generation device in
		/// <paramref name="existingDevice"/> to share it; a null value creates a private one.
		/// <paramref name="proxyDevice"/> and <paramref name="proxyContext"/> are the
		/// frame-generation views of the D3D11 device and immediate context, used when the
		/// render pair will not carry a shared fence.</summary>
		bool Initialize(IDXGIAdapter* adapter, ID3D11Device* device, ID3D11DeviceContext* context,
			ID3D12Device* existingDevice = nullptr, ID3D11Device5* proxyDevice = nullptr,
			ID3D11DeviceContext4* proxyContext = nullptr);
		void Shutdown();
		bool CreateSharedTexture(const D3D11_TEXTURE2D_DESC& desc, SharedTexture& texture, const char* name);
		bool BeginD3D12(ID3D12GraphicsCommandList** commandList);
		bool EndD3D12();
		bool WaitForIdle();

		[[nodiscard]] bool IsInitialized() const { return initialized_; }
		[[nodiscard]] HRESULT LastError() const { return lastError_; }
		[[nodiscard]] const char* LastOperation() const { return lastOperation_; }
		[[nodiscard]] std::uint32_t LastResourceFlags() const { return lastResourceFlags_; }
		[[nodiscard]] ID3D12Device* Device() const { return device12_.Get(); }

	private:
		static constexpr std::size_t kCommandContextCount = 3;

		struct CommandContext
		{
			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
			std::uint64_t fenceValue = 0;
		};

		D3D12Interop() = default;
		friend class Renderer;
		bool RecordFailure(const char* operation, HRESULT result);
		/// <summary>Finds a fence-capable view of the immediate context when the supplied
		/// render context will not expose ID3D11DeviceContext4, or falls back to CPU
		/// synchronisation. Returns false only if no usable command stream exists.</summary>
		bool AdoptFenceContext(ID3D11Device* device, ID3D11DeviceContext* context,
			ID3D11DeviceContext4* proxyContext, HRESULT queryResult);
		/// <summary>Blocks until the D3D11 command stream has retired, for the CPU sync path.</summary>
		bool FlushD3D11();
		/// <summary>Builds the queue, command contexts and shared fence on <c>device12_</c>.</summary>
		bool CreateDeviceObjects();
		/// <summary>Opens <c>fence12_</c> on the D3D11 device. Failure is recoverable.</summary>
		bool ShareFenceWithD3D11();
		/// <summary>Repoints fence sync at the frame-generation device/context pair, captured
		/// at device creation and so unwrapped. A fence belongs to the device that opened it,
		/// so both move together. Returns false when that pair is absent or already in use.</summary>
		bool AdoptProxyFenceDevice();
		/// <summary>Drops to CPU round-trip synchronisation when D3D11 will not carry a
		/// shared fence, rebuilding <c>fence12_</c> unshared. Costs more than a GPU-side
		/// wait but needs no ID3D11Fence at all.</summary>
		bool DegradeToCpuSync();
		/// <summary>Creates the event query the CPU sync path polls, once.</summary>
		bool EnsureFlushQuery();
		void ReleaseDeviceObjects();
		bool WaitForFence(std::uint64_t value);

		Microsoft::WRL::ComPtr<ID3D11Device> device11_;
		/// Only OpenSharedFence needs the v5 device; null forces the CPU sync path.
		Microsoft::WRL::ComPtr<ID3D11Device5> device5_;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> contextBase_;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context11_;
		/// Unwrapped fallback pair for the shared fence; both or neither are used.
		Microsoft::WRL::ComPtr<ID3D11Device5> proxyDevice_;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext4> proxyContext_;
		Microsoft::WRL::ComPtr<ID3D11Query> flushQuery_;
		Microsoft::WRL::ComPtr<ID3D11Fence> fence11_;
		Microsoft::WRL::ComPtr<ID3D12Device> device12_;
		Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue12_;
		std::array<CommandContext, kCommandContextCount> commandContexts_;
		Microsoft::WRL::ComPtr<ID3D12Fence> fence12_;
		HANDLE fenceEvent_ = nullptr;
		std::uint64_t fenceValue_ = 0;
		HRESULT lastError_ = S_OK;
		const char* lastOperation_ = "none";
		std::uint32_t lastResourceFlags_ = 0;
		std::size_t commandContextCursor_ = 0;
		std::size_t recordingContext_ = kCommandContextCount;
		bool backpressureLogged_ = false;
		bool initialized_ = false;
		bool recording_ = false;
		bool gpuFenceSync_ = true;
	};
}