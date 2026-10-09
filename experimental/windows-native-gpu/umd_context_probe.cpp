// SPDX-License-Identifier: MIT
// Local diagnostic: genuine NVIDIA Linux UMD, optionally through the QEMU bridge.
#include <wsl/winadapter.h>
#include <directx/d3d12.h>
#include <directx/dxcore.h>
#include <wsl/wrladapter.h>
#include <dxguids/dxguids.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <link.h>
using Microsoft::WRL::ComPtr;
static constexpr std::size_t CopyBytes = 65536;
static constexpr unsigned CopyRounds = 2;
static std::uint8_t pattern(std::size_t offset, unsigned round) {
    return static_cast<std::uint8_t>((offset * 37 + (offset >> 8) * 11 + round * 73) & 255);
}
static std::uint64_t hashBytes(const std::uint8_t* data, std::size_t bytes) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (std::size_t i = 0; i < bytes; ++i) { hash ^= data[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}
static bool succeeded(const char* stage, HRESULT hr) {
    std::printf("gpuCopy%s=%08x\n", stage, static_cast<unsigned>(hr));
    return SUCCEEDED(hr);
}
static int copyWorkload(ID3D12Device* device, ID3D12CommandQueue* queue) {
    std::printf("GPU_COPY_TEST_BEGIN bytes=%zu rounds=%u\n", CopyBytes, CopyRounds);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = CopyBytes;
    desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{}; heap.CreationNodeMask = 1; heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> upload, gpu, readback;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (!succeeded("Upload", device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) return 3;
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (!succeeded("Default", device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&gpu)))) return 3;
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    if (!succeeded("Readback", device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return 3;
    ComPtr<ID3D12CommandAllocator> allocator;
    if (!succeeded("Allocator", device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator)))) return 3;
    ComPtr<ID3D12GraphicsCommandList> commands;
    if (!succeeded("CommandList", device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)))) return 3;
    ComPtr<ID3D12Fence> fence;
    if (!succeeded("Fence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return 3;
    std::uint8_t expected[CopyBytes];
    for (unsigned round = 1; round <= CopyRounds; ++round) {
        if (round > 1 && (!succeeded("AllocatorReset", allocator->Reset()) ||
                         !succeeded("CommandListReset", commands->Reset(allocator.Get(), nullptr)))) return 3;
        void* pointer = nullptr;
        const D3D12_RANGE empty{};
        if (!succeeded("UploadMap", upload->Map(0, &empty, &pointer))) return 3;
        if (!pointer) return 3;
        for (std::size_t i = 0; i < CopyBytes; ++i) expected[i] = pattern(i, round);
        std::memcpy(pointer, expected, CopyBytes);
        const D3D12_RANGE written{0, CopyBytes}; upload->Unmap(0, &written);
        commands->CopyBufferRegion(gpu.Get(), 0, upload.Get(), 0, CopyBytes);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = gpu.Get(); barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commands->ResourceBarrier(1, &barrier);
        commands->CopyBufferRegion(readback.Get(), 0, gpu.Get(), 0, CopyBytes);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        commands->ResourceBarrier(1, &barrier);
        if (!succeeded("Close", commands->Close())) return 3;
        ID3D12CommandList* lists[] = {commands.Get()}; queue->ExecuteCommandLists(1, lists);
        if (!succeeded("Signal", queue->Signal(fence.Get(), round))) return 3;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::uint64_t observed = 0;
        do {
            observed = fence->GetCompletedValue();
            if (observed == UINT64_MAX) { succeeded("DeviceRemoved", device->GetDeviceRemovedReason()); return 3; }
            if (observed >= round) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (observed < round) { std::printf("GPU_COPY_TIMEOUT round=%u observed=%llu\n", round, static_cast<unsigned long long>(observed)); return 3; }
        if (!succeeded("ReadbackMap", readback->Map(0, &written, &pointer))) return 3;
        if (!pointer) return 3;
        const bool matches = std::memcmp(pointer, expected, CopyBytes) == 0;
        const auto actualHash = hashBytes(static_cast<const std::uint8_t*>(pointer), CopyBytes);
        readback->Unmap(0, &empty);
        std::printf("GPU_COPY_ROUND round=%u verifiedBytes=%zu expectedHash=%016llx observedHash=%016llx fenceTarget=%u fenceObserved=%llu\n",
            round, matches ? CopyBytes : 0, static_cast<unsigned long long>(hashBytes(expected, CopyBytes)),
            static_cast<unsigned long long>(actualHash), round, static_cast<unsigned long long>(observed));
        if (!matches) return 3;
    }
    std::printf("GPU_COPY_TEST_COMPLETE verified=true bytes=%zu rounds=%u\n", CopyBytes, CopyRounds);
    return 0;
}
static int moduleLoaded(struct dl_phdr_info* info, size_t bytes, void* data) {
    (void)bytes;
    if (info->dlpi_name && std::strstr(info->dlpi_name, "/libnvwgf2umx.so")) *static_cast<bool*>(data) = true;
    return 0;
}
int main() {
    ComPtr<IDXCoreAdapterFactory> factory;
    auto hr=DXCoreCreateAdapterFactory(IID_PPV_ARGS(&factory));
    std::printf("factory=%08x\n",static_cast<unsigned>(hr)); if (FAILED(hr)) return 1;
    ComPtr<IDXCoreAdapterList> list;
    hr=factory->CreateAdapterList(1,&DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS,IID_PPV_ARGS(&list));
    std::printf("list=%08x\n",static_cast<unsigned>(hr)); if (FAILED(hr)) return 1;
    for (UINT i=0;i<list->GetAdapterCount();++i) {
        ComPtr<IDXCoreAdapter> adapter; hr=list->GetAdapter(i,IID_PPV_ARGS(&adapter)); if(FAILED(hr)) return 1;
        DXCoreHardwareID id{}; hr=adapter->GetProperty(DXCoreAdapterProperty::HardwareID,sizeof id,&id); if(FAILED(hr)) return 1;
        if(id.vendorID != 0x10de) continue;
        std::printf("nvidia vendor=%u device=%u\n",id.vendorID,id.deviceID);
        ComPtr<ID3D12Device> device; hr=D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device));
        bool loaded = false; dl_iterate_phdr(moduleLoaded, &loaded);
        std::printf("LINUX_RUNTIME_NVIDIA_UMD_STILL_LOADED=%s\n", loaded ? "true" : "false");
        std::printf("device=%08x\n",static_cast<unsigned>(hr)); if(FAILED(hr)) return 1;
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_COPY;
        ComPtr<ID3D12CommandQueue> queue; hr=device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue));
        std::printf("copyQueue=%08x\n",static_cast<unsigned>(hr)); if (FAILED(hr)) return 1;
        const char* workload = std::getenv("WDDM_BRIDGE_RUNTIME_WORKLOAD");
        return workload && !std::strcmp(workload, "copy") ? copyWorkload(device.Get(), queue.Get()) : 0;
    }
    return 2;
}
