// SPDX-License-Identifier: MIT
// Trace genuine native texture export/import from the current Mesa device.
// Native resource FDs alone do not establish dma-buf or compositor support.
#include <wsl/winadapter.h>
#include <wsl/wrladapter.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>
#include <EGL/egl.h>
struct HGLRC__;
using HGLRC = HGLRC__*;
#define _WINDEF_
#include <GL/mesa_glinterop.h>
#undef _WINDEF_
#include <d3d12_interop_public.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

using Microsoft::WRL::ComPtr;

extern "C" int wddm_mesa_resource_fd_test(EGLDisplay display, EGLContext context) {
    std::puts("MESA_RESOURCE_FD_BEGIN width=3840 height=2160 format=28 native=true dmaBuf=false desktop=false");
    const auto failed = [](const char* stage, HRESULT result) {
        std::printf("MESA_RESOURCE_FD_FAILURE stage=%s status=%08x errno=%d\n", stage,
                    static_cast<unsigned>(result), errno);
        return 1;
    };
    d3d12_interop_device_info native{};
    mesa_glinterop_device_info info{};
    info.version = 2; info.driver_data_size = sizeof native; info.driver_data = &native;
    if (MesaGLInteropEGLQueryDeviceInfo(display, context, &info) != MESA_GLINTEROP_SUCCESS ||
        info.driver_data_size != sizeof native || !native.device || !native.queue)
        return failed("MesaDeviceInterop", E_FAIL);
    using Prepare = int (*)(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
    const auto prepare = reinterpret_cast<Prepare>(dlsym(RTLD_DEFAULT, "wddm_bridge_prepare_native_texture"));
    if (!prepare || prepare(3840, 2160, 28, 1)) return failed("NativeTextureProfile", E_FAIL);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = 3840; desc.Height = 2160;
    desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> source;
    auto result = native.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&source));
    if (FAILED(result)) return failed("CreateSharedTexture", result);
    std::puts("MESA_RESOURCE_FD_TEXTURE_CREATED width=3840 height=2160 shared=true");
    HANDLE handle = nullptr;
    result = native.device->CreateSharedHandle(source.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    if (FAILED(result)) return failed("ExportTexture", result);
    const auto value = reinterpret_cast<intptr_t>(handle);
    if (value < 0 || value > INT32_MAX) return failed("ExportDescriptor", E_FAIL);
    const auto fd = static_cast<int>(value);
    if (fcntl(fd, F_GETFD) < 0) return failed("ExportDescriptor", E_FAIL);
    std::puts("MESA_RESOURCE_FD_EXPORTED native=true rawHostHandle=false");
    ComPtr<ID3D12Resource> imported;
    result = native.device->OpenSharedHandle(handle, IID_PPV_ARGS(&imported));
    const auto closeResult = close(fd);
    if (FAILED(result) || !imported || closeResult) return failed("ImportTexture", result);
    const auto importedDesc = imported->GetDesc();
    if (importedDesc.Dimension != desc.Dimension || importedDesc.Width != desc.Width || importedDesc.Height != desc.Height ||
        importedDesc.Format != desc.Format || importedDesc.Flags != desc.Flags)
        return failed("ImportedDescription", E_FAIL);
    std::puts("MESA_RESOURCE_FD_IMPORTED fdClosed=true descriptionMatches=true");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0; UINT64 rowBytes = 0, totalBytes = 0;
    native.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
    if (rows != 2160 || rowBytes != 3840 * 4 || footprint.Offset || footprint.Footprint.RowPitch < rowBytes ||
        footprint.Footprint.RowPitch % 256 || totalBytes > 32u * 1024 * 1024 || !totalBytes)
        return failed("ReadbackFootprint", E_FAIL);
    // Readback is an acceptance diagnostic, not the display path. Reuse one
    // bounded band so all 4K pixels can be checked without a 33 MB contiguous
    // CPU aperture lease competing with the live UMD's many small mappings.
    constexpr unsigned BandRows = 512;
    const auto readbackBytes = static_cast<UINT64>(footprint.Footprint.RowPitch) * BandRows;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = readbackBytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    result = native.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(result)) return failed("CreateReadback", result);
    ComPtr<ID3D12CommandAllocator> allocator;
    result = native.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(result)) return failed("CreateAllocator", result);
    ComPtr<ID3D12GraphicsCommandList> commands;
    result = native.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands));
    if (FAILED(result)) return failed("CreateCommandList", result);
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{}; rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtvDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv;
    result = native.device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtv));
    if (FAILED(result)) return failed("CreateRtv", result);
    ComPtr<ID3D12Fence> retired;
    result = native.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&retired));
    if (FAILED(result)) return failed("CreateRetirementFence", result);
    const auto transition = [&](ID3D12Resource* texture, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before; barrier.Transition.StateAfter = after;
        commands->ResourceBarrier(1, &barrier);
    };
    UINT64 retirementTarget = 0;
#ifdef WDDM_BRIDGE_RESOURCE_FD_BAD_COLOR
    std::puts("MESA_RESOURCE_FD_NEGATIVE_CONTROL kind=sourceColor expectedFailure=SharedPixels");
#endif
    for (unsigned round = 0; round < 2; ++round) {
        if (round) source.Reset();
        auto writer = round ? imported.Get() : source.Get();
        FLOAT color[4] = {round ? 0.0f : 1.0f, round ? 1.0f : 0.0f, 0.0f, 1.0f};
#ifdef WDDM_BRIDGE_RESOURCE_FD_BAD_COLOR
        if (!round) { color[0] = 0.0f; color[2] = 1.0f; }
#endif
        const std::uint8_t expected[4] = {round ? std::uint8_t{0} : std::uint8_t{255}, round ? std::uint8_t{255} : std::uint8_t{0}, 0, 255};
        std::uint64_t digest = UINT64_C(14695981039346656037);
        unsigned bands = 0;
        for (unsigned firstRow = 0; firstRow < 2160; firstRow += BandRows) {
            if (retirementTarget) {
                result = allocator->Reset(); if (FAILED(result)) return failed("ResetAllocator", result);
                result = commands->Reset(allocator.Get(), nullptr); if (FAILED(result)) return failed("ResetCommands", result);
            }
            const unsigned remaining = 2160 - firstRow;
            const unsigned bandRows = remaining < BandRows ? remaining : BandRows;
            if (!firstRow) {
                const auto descriptor = rtv->GetCPUDescriptorHandleForHeapStart();
                native.device->CreateRenderTargetView(writer, nullptr, descriptor);
                transition(writer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
                commands->ClearRenderTargetView(descriptor, color, 0, nullptr);
                transition(writer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            }
            transition(imported.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = footprint;
            destination.PlacedFootprint.Footprint.Height = bandRows;
            D3D12_TEXTURE_COPY_LOCATION origin{}; origin.pResource = imported.Get(); origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const D3D12_BOX box{0, firstRow, 0, 3840, firstRow + bandRows, 1};
            commands->CopyTextureRegion(&destination, 0, 0, 0, &origin, &box);
            transition(imported.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            result = commands->Close(); if (FAILED(result)) return failed("CloseCommands", result);
            ID3D12CommandList* submitted[] = {commands.Get()}; native.queue->ExecuteCommandLists(1, submitted);
            ++retirementTarget;
            result = native.queue->Signal(retired.Get(), retirementTarget); if (FAILED(result)) return failed("SignalRetirement", result);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (retired->GetCompletedValue() < retirementTarget && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (retired->GetCompletedValue() != retirementTarget) return failed("Retirement", E_FAIL);
            void* mapped = nullptr; const D3D12_RANGE readable{0, static_cast<SIZE_T>(bandRows) * footprint.Footprint.RowPitch};
            result = readback->Map(0, &readable, &mapped); if (FAILED(result) || !mapped) return failed("ReadbackMap", result);
            bool matches = true;
            for (unsigned y = 0; y < bandRows && matches; ++y) {
                const auto row = static_cast<const std::uint8_t*>(mapped) + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch;
                for (unsigned x = 0; x < 3840 && matches; ++x)
                    for (unsigned channel = 0; channel < 4; ++channel) {
                        const auto value = row[x * 4 + channel];
                        if (value != expected[channel]) { matches = false; break; }
                        digest = (digest ^ value) * UINT64_C(1099511628211);
                    }
            }
            const D3D12_RANGE written{0,0}; readback->Unmap(0, &written);
            if (!matches) return failed("SharedPixels", E_FAIL);
            ++bands;
        }
        std::printf("MESA_RESOURCE_FD_SHARED_STORAGE round=%u writer=%s importedRead=true sourceReleased=%s fdClosed=true width=3840 height=2160 pixels=8294400 rgba=%s bands=%u readbackBytes=%llu fnv1a=%016llx\n",
            round + 1, round ? "imported" : "source", round ? "true" : "false", round ? "00ff00ff" : "ff0000ff", bands,
            static_cast<unsigned long long>(readbackBytes), static_cast<unsigned long long>(digest));
    }
    std::puts("MESA_RESOURCE_FD_COMPLETE rounds=2 pixelsPerRound=8294400 native=true dmaBuf=false desktop=false");
    return 0;
}
