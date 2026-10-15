// SPDX-License-Identifier: MIT
// Host GPU readback of actual owned compositor storage. No guest CPU upload.
#pragma once
#include "binary_capture.h"
#include "compositor_capture_wire.h"
namespace driver_shared {
class CompositorReadback {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Resource> source, readback;
    Microsoft::WRL::ComPtr<ID3D12Fence> producer, retired;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    bool submitted = false, completed = false;
public:
    ~CompositorReadback() {
        if (submitted && !completed) ExitProcess(3);
        const bool opened = device != nullptr;
        commands.Reset(); allocator.Reset(); source.Reset(); readback.Reset();
        producer.Reset(); retired.Reset(); queue.Reset(); device.Reset();
        if (opened) std::cerr << "{\"nativeCompositorCaptureReleased\":true,\"submitted\":" << (submitted ? "true" : "false")
            << ",\"gpuWorkRetired\":" << (completed ? "true" : "false") << "}\n";
    }
    HRESULT capture(HANDLE resource, HANDLE sync, LUID luid, driver_bridge::CompositorCaptureDesc desc,
                    unsigned frame, std::uint64_t& hash) {
        if (!resource || resource == INVALID_HANDLE_VALUE || !sync || sync == INVALID_HANDLE_VALUE ||
            !driver_bridge::validCompositorCapture(desc) || !frame || frame > 2 || device) return E_INVALIDARG;
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        auto hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
        if (SUCCEEDED(hr)) hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr)) hr = device->OpenSharedHandle(resource, IID_PPV_ARGS(&source));
        if (SUCCEEDED(hr)) hr = device->OpenSharedHandle(sync, IID_PPV_ARGS(&producer));
        if (FAILED(hr)) return hr;
        const auto layout = source->GetDesc();
        if (layout.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || layout.Width != desc.width || layout.Height != desc.height ||
            layout.Format != static_cast<DXGI_FORMAT>(driver_bridge::compositorDxgiFormat(desc.format)) ||
            layout.DepthOrArraySize != 1 || layout.MipLevels != 1 || layout.SampleDesc.Count != 1 || layout.SampleDesc.Quality ||
            layout.Flags != D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) return E_INVALIDARG;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0; UINT64 rowBytes = 0, total = 0;
        device->GetCopyableFootprints(&layout, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
        if (rows != desc.height || rowBytes != desc.stride || footprint.Offset || footprint.Footprint.RowPitch < rowBytes ||
            footprint.Footprint.RowPitch > 16384 || footprint.Footprint.RowPitch % 256 ||
            total < std::uint64_t(desc.height-1)*footprint.Footprint.RowPitch+desc.stride || total > 64ull * 1024 * 1024)
            return E_INVALIDARG;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total; buffer.Height = 1; buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(hr)) hr = device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
        if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&commands));
        if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&retired));
        if (FAILED(hr)) return hr;
        const auto before = producer->GetCompletedValue();
        if (before == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source.Get(); barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commands->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = source.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        commands->ResourceBarrier(1, &barrier);
        hr = commands->Close(); if (FAILED(hr)) return hr;
        hr = queue->Wait(producer.Get(), desc.fence); if (FAILED(hr)) return hr;
        submitted = true;
        ID3D12CommandList* lists[]{commands.Get()}; queue->ExecuteCommandLists(1, lists);
        hr = queue->Signal(retired.Get(), 1);
        UINT64 observed = 0; const auto deadline = GetTickCount64() + 5000;
        while (SUCCEEDED(hr) && (observed = retired->GetCompletedValue()) < 1 && GetTickCount64() < deadline) Sleep(1);
        if (FAILED(hr) || observed == UINT64_MAX || observed < 1) ExitProcess(3);
        completed = true;
        const auto producerObserved = producer->GetCompletedValue();
        if (producerObserved < desc.fence || producerObserved == UINT64_MAX) return E_FAIL;
        void* mapped = nullptr; const D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
        hr = readback->Map(0, &range, &mapped); if (FAILED(hr)) return hr;
        struct Unmap { ID3D12Resource* resource; ~Unmap() { const D3D12_RANGE empty{}; resource->Unmap(0, &empty); } } unmap{readback.Get()};
        hr = driver_capture::writeCompositor(frame, desc.width, desc.height, desc.format, mapped, footprint.Footprint.RowPitch, hash);
        if (FAILED(hr)) return hr;
        std::cerr << "{\"nativeCompositorCaptured\":true,\"frame\":" << frame << ",\"width\":" << desc.width << ",\"height\":" << desc.height
            << ",\"fourcc\":" << desc.format << ",\"dxgiFormat\":" << layout.Format << ",\"producerFenceTarget\":" << desc.fence
            << ",\"producerFenceBefore\":" << before << ",\"producerFenceObserved\":" << producerObserved
            << ",\"copyFenceObserved\":" << observed << ",\"fnv1a\":" << hash
            << ",\"sourceReturnedToCommon\":true,\"cpuUpload\":false,\"windowsPresentation\":false}\n";
        return S_OK;
    }
};
}
