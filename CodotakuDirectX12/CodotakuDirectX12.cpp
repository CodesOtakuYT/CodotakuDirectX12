#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <system_error>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dx12.h>
#include <memory>
#include <format>
#include <ranges>

using Microsoft::WRL::ComPtr;

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 616; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

struct WindowDeleter {
	void operator()(HWND hwnd) {
		if (hwnd) DestroyWindow(hwnd);
	}
};

inline std::string HrToString(HRESULT hr) {
	return std::format("HRESULT of 0x{:08X}", static_cast<UINT>(hr));
}

class HrException : public std::runtime_error {
public:
	HrException(HRESULT hr) : std::runtime_error(HrToString(hr)), m_hr(hr) {}
	HRESULT Error() const { return m_hr; }
private:
	const HRESULT m_hr;
};

inline void ThrowIfFailed(HRESULT hr) {
	if (FAILED(hr))
		throw HrException(hr);
}

class App {
public:
	App(HINSTANCE hInstance, int nCmdShow) {
		auto className = L"CodotakuDirectX12";
		WNDCLASSEXW windowClass{
			.cbSize = sizeof(WNDCLASSEXW),
			.style = CS_HREDRAW | CS_VREDRAW,
			.lpfnWndProc = WindowProc,
			.hInstance = hInstance,
			.lpszClassName = className,
		};
		if (RegisterClassExW(&windowClass) == 0)
			throw std::system_error(
				std::error_code(GetLastError(), std::system_category())
			);
		RECT windowRect{
			.right = 800,
			.bottom = 600,
		};
		DWORD dwStyle = WS_OVERLAPPEDWINDOW;
		DWORD dwExStyle = 0;

		if (AdjustWindowRect(&windowRect, dwStyle, false) == 0)
			throw std::system_error(
				std::error_code(GetLastError(), std::system_category())
			);

		auto windowWidth = windowRect.right - windowRect.left;
		auto windowHeight = windowRect.bottom - windowRect.top;

		m_window.reset(CreateWindowExW(
			dwExStyle,
			className,
			L"Codotaku DirectX12",
			dwStyle,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			windowWidth,
			windowHeight,
			nullptr,
			nullptr,
			hInstance,
			this
		));
		if (!m_window)
			throw std::system_error(
				std::error_code(GetLastError(), std::system_category())
			);

		UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&m_debugController)))) {
			m_debugController->EnableDebugLayer();
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		}
#endif
		ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_factory)));
		ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));

		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));

		DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {
			.Width = static_cast<UINT>(windowWidth),
			.Height = static_cast<UINT>(windowHeight),
			.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
			.SampleDesc = {
				.Count = 1,
			},
			.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
			.BufferCount = FrameCount,
			.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
		};

		ComPtr<IDXGISwapChain1> swapChain;
		ThrowIfFailed(m_factory->CreateSwapChainForHwnd(m_commandQueue.Get(), m_window.get(), &swapChainDesc, nullptr, nullptr, &swapChain));

		ThrowIfFailed(m_factory->MakeWindowAssociation(m_window.get(), DXGI_MWA_NO_ALT_ENTER));

		ThrowIfFailed(swapChain.As(&m_swapChain));

		m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

		{
			D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{
				.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
				.NumDescriptors = FrameCount,
			};
			ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));

			m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		}

		{
			CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
			for (UINT frameIndex : std::views::iota(0u, FrameCount)) {
				ThrowIfFailed(m_swapChain->GetBuffer(frameIndex, IID_PPV_ARGS(&m_renderTargets[frameIndex])));
				m_device->CreateRenderTargetView(m_renderTargets[frameIndex].Get(), nullptr, rtvHandle);
				rtvHandle.Offset(1, m_rtvDescriptorSize);
			}
		}

		ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));

		ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)));
		m_commandList->Close();
		ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));

		m_fenceEvent = CreateEventW(nullptr, false, false, nullptr);
		if (m_fenceEvent == nullptr)
			throw std::system_error(
				std::error_code(GetLastError(), std::system_category())
			);

		ShowWindow(m_window.get(), nCmdShow);
	}

	void Render() {
		ThrowIfFailed(m_commandAllocator->Reset());
		ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr));
		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
			m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
			m_frameIndex,
			m_rtvDescriptorSize
		);

		const float clearColor[] = { 1.0f, 0.0f, 0.0f, 1.0f };
		{
			auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				m_renderTargets[m_frameIndex].Get(),
				D3D12_RESOURCE_STATE_PRESENT,
				D3D12_RESOURCE_STATE_RENDER_TARGET
			);
			m_commandList->ResourceBarrier(1, &barrier);
		}

		m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

		{
			auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				m_renderTargets[m_frameIndex].Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PRESENT
			);
			m_commandList->ResourceBarrier(1, &barrier);
		}

		ThrowIfFailed(m_commandList->Close());
		ID3D12CommandList* lists[] = { m_commandList.Get() };
		m_commandQueue->ExecuteCommandLists(_countof(lists), lists);

		ThrowIfFailed(m_swapChain->Present(1, 0));

		const UINT64 fence = m_fenceValue;
		ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fence));
		m_fenceValue++;

		if (m_fence->GetCompletedValue() < fence) {
			ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
			WaitForSingleObject(m_fenceEvent, INFINITE);
		}

		m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
	}

private:
	static const UINT FrameCount = 2;

	std::unique_ptr<HWND__, WindowDeleter> m_window;
	ComPtr<ID3D12Debug> m_debugController;
	ComPtr<IDXGIFactory4> m_factory;
	ComPtr<ID3D12Device> m_device;
	ComPtr<ID3D12CommandQueue> m_commandQueue;
	ComPtr<IDXGISwapChain3> m_swapChain;
	UINT m_frameIndex;
	ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	UINT m_rtvDescriptorSize;
	std::array<ComPtr<ID3D12Resource>, FrameCount> m_renderTargets;
	ComPtr<ID3D12CommandAllocator> m_commandAllocator;
	ComPtr<ID3D12GraphicsCommandList> m_commandList;
	ComPtr<ID3D12Fence> m_fence;
	HANDLE m_fenceEvent;
	UINT64 m_fenceValue = 0;
};

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	switch (uMsg) {
	case WM_CLOSE:
		PostQuitMessage(0);
		return 0;
	case WM_CREATE: {
		LPCREATESTRUCT pCreateStruct = reinterpret_cast<LPCREATESTRUCT>(lParam);
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pCreateStruct->lpCreateParams));
		return 0;
	}
	case WM_PAINT: {
		auto app = reinterpret_cast<App*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		app->Render();
		return 0;
	}
	default:
		return DefWindowProcW(hwnd, uMsg, wParam, lParam);
	}
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
	App app{ hInstance , nCmdShow };

	MSG message{};
	while (message.message != WM_QUIT) {
		if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}

	return 0; 
}
