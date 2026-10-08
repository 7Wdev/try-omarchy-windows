// SPDX-License-Identifier: MIT
// Local WSL reference only; this program still uses the genuine /dev/dxg.
#include <wsl/winadapter.h>
#include <directx/d3d12.h>
#include <directx/dxcore.h>
#include <wsl/wrladapter.h>
#include <dxguids/dxguids.h>
#include <cstdio>
using Microsoft::WRL::ComPtr;
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
        std::printf("device=%08x\n",static_cast<unsigned>(hr)); if(FAILED(hr)) return 1;
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_COPY;
        ComPtr<ID3D12CommandQueue> queue; hr=device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue));
        std::printf("copyQueue=%08x\n",static_cast<unsigned>(hr)); return FAILED(hr)?1:0;
    }
    return 2;
}
