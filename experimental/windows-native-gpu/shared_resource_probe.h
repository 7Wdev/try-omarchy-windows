// SPDX-License-Identifier: MIT
// A bounded import probe. It never copies, presents, or retains a GPU resource.
#pragma once
namespace driver_shared {
struct ProbeResult {
    NTSTATUS shareStatus = static_cast<NTSTATUS>(0xc0000001u);
    HRESULT deviceResult = E_FAIL, importResult = E_FAIL;
    HRESULT heapResult = E_FAIL;
    UINT64 heapBytes = 0;
    D3D12_RESOURCE_DESC description{};
    bool handleClosed = false, importedReleased = false;
};
inline ProbeResult probe(D3DKMT_HANDLE resource, LUID luid) {
    ProbeResult result;
    OBJECT_ATTRIBUTES attributes{}; attributes.Length = sizeof attributes;
    HANDLE handle = nullptr;
    result.shareStatus = D3DKMTShareObjects(1, &resource, &attributes, GENERIC_ALL, &handle);
    if (result.shareStatus != 0 || !handle || handle == INVALID_HANDLE_VALUE) {
        if (handle && handle != INVALID_HANDLE_VALUE) result.handleClosed = CloseHandle(handle) != FALSE;
        return result;
    }
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Resource> imported;
    Microsoft::WRL::ComPtr<ID3D12Heap> heap;
    result.deviceResult = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result.deviceResult)) result.deviceResult = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    if (SUCCEEDED(result.deviceResult)) result.deviceResult = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (SUCCEEDED(result.deviceResult)) result.importResult = device->OpenSharedHandle(handle, IID_PPV_ARGS(&imported));
    if (SUCCEEDED(result.importResult) && imported) result.description = imported->GetDesc();
    if (SUCCEEDED(result.deviceResult)) result.heapResult = device->OpenSharedHandle(handle, IID_PPV_ARGS(&heap));
    if (SUCCEEDED(result.heapResult) && heap) result.heapBytes = heap->GetDesc().SizeInBytes;
    heap.Reset();
    imported.Reset(); result.importedReleased = true;
    device.Reset(); adapter.Reset(); factory.Reset();
    result.handleClosed = CloseHandle(handle) != FALSE;
    return result;
}
}
