// SPDX-License-Identifier: MIT
// Bounded diagnostic presentation; not the Omarchy display integration.
#pragma once
#include <dwmapi.h>
namespace driver_shared {
class TexturePresenter {
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> buffers;
    Microsoft::WRL::ComPtr<ID3D12Fence> retirement;
    HWND window = nullptr;
    ATOM windowClass = 0;
    bool opened = false;
    UINT index = 0;
    static constexpr const wchar_t* ClassName = L"7WdevGuestShaderPresentationLab";
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        // This short owned test destroys its window after GPU retirement.
        if (message == WM_CLOSE) return 0;
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    void pump() const {
        MSG message{};
        while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
public:
    ~TexturePresenter() {
        for (auto& buffer : buffers) buffer.Reset();
        swapchain.Reset(); retirement.Reset();
        const bool destroyed = !window || DestroyWindow(window) != FALSE;
        const bool unregistered = !windowClass || UnregisterClassW(ClassName, GetModuleHandleW(nullptr)) != FALSE;
        if (opened) std::cerr << "{\"nativeSharedTexturePresenterReleased\":true,\"gpuWorkRetired\":true,\"windowDestroyed\":"
            << (destroyed ? "true" : "false") << ",\"classUnregistered\":" << (unregistered ? "true" : "false") << "}\n";
        if (!destroyed || !unregistered) ExitProcess(3);
    }
    HRESULT initialize(IDXGIFactory4* factory, ID3D12Device* device, ID3D12CommandQueue* queue) {
        WNDCLASSEXW type{}; type.cbSize = sizeof type; type.lpfnWndProc = procedure;
        type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = ClassName;
        windowClass = RegisterClassExW(&type);
        if (!windowClass) return HRESULT_FROM_WIN32(GetLastError());
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        constexpr DWORD extended = WS_EX_NOACTIVATE;
        RECT bounds{0, 0, 780, 438};
        if (!AdjustWindowRectEx(&bounds, style, FALSE, extended)) return HRESULT_FROM_WIN32(GetLastError());
        window = CreateWindowExW(extended, ClassName, L"NVIDIA guest shader - QEMU Windows GPU bridge lab", style,
            CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, type.hInstance, nullptr);
        if (!window) return HRESULT_FROM_WIN32(GetLastError());
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = 130; desc.Height = 73;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> created;
        auto hr = factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &created);
        if (FAILED(hr)) return hr;
        hr = created.As(&swapchain); if (FAILED(hr)) return hr;
        hr = factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        if (FAILED(hr)) return hr;
        for (UINT i = 0; i < buffers.size(); ++i) {
            hr = swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i])); if (FAILED(hr)) return hr;
        }
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&retirement)); if (FAILED(hr)) return hr;
        UINT previous = UINT_MAX;
        hr = swapchain->GetLastPresentCount(&previous); if (hr != S_OK || previous != 0) return E_FAIL;
        ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window); pump();
        RECT client{};
        if (!IsWindowVisible(window) || !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) return E_FAIL;
        opened = true;
        std::cerr << "{\"nativeSharedTexturePresenterOpened\":true,\"width\":130,\"height\":73,\"bufferCount\":2"
            << ",\"flipModel\":true,\"clientWidth\":" << client.right << ",\"clientHeight\":" << client.bottom
            << ",\"windowVisible\":true,\"sameAdapter\":true,\"cpuUpload\":false}\n";
        return S_OK;
    }
    HRESULT copy(ID3D12GraphicsCommandList* commands, ID3D12Resource* source,
                 ID3D12Resource* readback, D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint) {
        if (!opened) return E_FAIL;
        pump(); index = swapchain->GetCurrentBackBufferIndex(); if (index >= buffers.size()) return E_FAIL;
        const auto barrier = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = buffers[index].Get(); b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before; b.Transition.StateAfter = after; commands->ResourceBarrier(1, &b);
        };
        barrier(D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        commands->CopyResource(buffers[index].Get(), source);
        barrier(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = buffers[index].Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = readback; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        barrier(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        return S_OK;
    }
    HRESULT present(ID3D12CommandQueue* queue, UINT64 round, UINT64 hash) {
        if (!opened || !round || round > 2) return E_INVALIDARG;
        pump();
        const auto presented = swapchain->Present(1, 0);
        // Retire the queue even if Present reports an error or occlusion.
        auto hr = queue->Signal(retirement.Get(), round);
        const auto deadline = GetTickCount64() + 5000; UINT64 observed = 0;
        while (SUCCEEDED(hr) && (observed = retirement->GetCompletedValue()) < round && GetTickCount64() < deadline) { pump(); Sleep(1); }
        if (FAILED(hr) || observed == UINT64_MAX || observed < round) {
            std::cerr << "FAIL: Presentation queue did not retire; backing retained until process teardown\n" << std::flush;
            ExitProcess(3);
        }
        if (presented != S_OK) return E_FAIL;
        UINT count = 0; hr = swapchain->GetLastPresentCount(&count); if (hr != S_OK || count != round) return E_FAIL;
        const auto flushed = DwmFlush();
        DXGI_FRAME_STATISTICS stats{};
        const auto statsDeadline = GetTickCount64() + 2500;
        do {
            pump(); hr = swapchain->GetFrameStatistics(&stats);
            if (hr == S_OK && stats.PresentCount == count && stats.SyncQPCTime.QuadPart > 0) break;
            if (FAILED(hr) && hr != DXGI_ERROR_FRAME_STATISTICS_DISJOINT) break;
            Sleep(10);
        } while (GetTickCount64() < statsDeadline);
        const bool visible = IsWindowVisible(window) != FALSE && !IsIconic(window);
        const bool complete = hr == S_OK && stats.PresentCount == count && stats.SyncQPCTime.QuadPart > 0 && visible;
        std::cerr << "{\"nativeSharedTexturePresented\":" << (complete ? "true" : "false") << ",\"round\":" << round
            << ",\"width\":130,\"height\":73,\"backBufferIndex\":" << index << ",\"backBufferFnv1a\":" << hash
            << ",\"presentHresult\":" << presented << ",\"lastPresentCount\":" << count << ",\"statisticsHresult\":" << hr
            << ",\"statisticsPresentCount\":" << stats.PresentCount << ",\"syncQpc\":" << stats.SyncQPCTime.QuadPart
            << ",\"dwmFlushHresult\":" << flushed << ",\"windowVisible\":" << (visible ? "true" : "false")
            << ",\"presentQueueFenceTarget\":" << round << ",\"presentQueueFenceObserved\":" << observed
            << ",\"backBufferState\":\"PRESENT\",\"cpuUpload\":false}\n";
        // Leave the verified frame visible briefly, pumping its window messages.
        if (complete) { const auto hold = GetTickCount64() + 3000; do { pump(); Sleep(10); } while (GetTickCount64() < hold); }
        return complete ? S_OK : E_FAIL;
    }
};
}
