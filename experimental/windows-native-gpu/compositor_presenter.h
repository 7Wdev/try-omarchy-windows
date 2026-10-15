// SPDX-License-Identifier: MIT
// Three GPU staging slots; ordinary frames have no CPU readback or upload.
// Input returns only after its GPU copy retires. Presentation owns its UI thread.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "binary_capture.h"
#include "compositor_capture_wire.h"
#include "shared_texture_presenter.h"
namespace driver_shared {
class CompositorPresenter {
    using Device = Microsoft::WRL::ComPtr<ID3D12Device>;
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct Slot {
        Resource texture;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> inputAllocator, outputAllocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> inputCommands, outputCommands;
        bool occupied = false;
        std::uint64_t reusedAfter = 0;
    };
    struct Frame { unsigned slot, audit; std::uint64_t sequence; };
    std::array<Slot,3> slots;
    Device device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> inputQueue, outputQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> inputRetired, outputRetired;
    Resource auditReadback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 auditBytes = 0;
    std::map<std::uint32_t,Resource> sources;
    std::mutex guard;
    std::condition_variable ready;
    std::deque<Frame> frames;
    std::thread ui;
    HANDLE work = nullptr, inputEvent = nullptr;
    std::atomic<HRESULT> failure{S_OK};
    bool initialized = false, stopping = false;
    HRESULT initialization = E_PENDING;
    LUID selected{};
    driver_bridge::CompositorCaptureDesc layout{};
    std::uint64_t submitted = 0, presented = 0, outputSubmitted = 0;
    unsigned auditAttempts = 0, completedAudits = 0, peakQueued = 0, peakSources = 0;
    static D3D12_RESOURCE_BARRIER transition(ID3D12Resource* object,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER result{}; result.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        result.Transition.pResource=object; result.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        result.Transition.StateBefore=from; result.Transition.StateAfter=to; return result;
    }
    static void retire(ID3D12Fence* fence,std::uint64_t value) {
        const auto deadline=GetTickCount64()+5000; UINT64 observed=0;
        while ((observed=fence->GetCompletedValue())<value && GetTickCount64()<deadline) Sleep(1);
        if (observed==UINT64_MAX || observed<value) ExitProcess(3);
    }
    HRESULT create() {
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        auto hr=CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr=factory->EnumAdapterByLuid(selected,IID_PPV_ARGS(&adapter));
        if (SUCCEEDED(hr)) hr=D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device));
        D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(hr)) hr=device->CreateCommandQueue(&queue,IID_PPV_ARGS(&inputQueue));
        if (SUCCEEDED(hr)) hr=device->CreateCommandQueue(&queue,IID_PPV_ARGS(&outputQueue));
        if (SUCCEEDED(hr)) hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&inputRetired));
        if (SUCCEEDED(hr)) hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&outputRetired));
        if (FAILED(hr)) return hr;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask=heap.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC texture{}; texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width=layout.width; texture.Height=layout.height; texture.DepthOrArraySize=texture.MipLevels=1;
        texture.Format=static_cast<DXGI_FORMAT>(driver_bridge::compositorDxgiFormat(layout.format)); texture.SampleDesc.Count=1;
        for (auto& slot:slots) {
            hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&slot.texture));
            if (SUCCEEDED(hr)) hr=device->CreateCommandAllocator(queue.Type,IID_PPV_ARGS(&slot.inputAllocator));
            if (SUCCEEDED(hr)) hr=device->CreateCommandAllocator(queue.Type,IID_PPV_ARGS(&slot.outputAllocator));
            if (SUCCEEDED(hr)) hr=device->CreateCommandList(0,queue.Type,slot.inputAllocator.Get(),nullptr,IID_PPV_ARGS(&slot.inputCommands));
            if (SUCCEEDED(hr)) hr=slot.inputCommands->Close();
            if (SUCCEEDED(hr)) hr=device->CreateCommandList(0,queue.Type,slot.outputAllocator.Get(),nullptr,IID_PPV_ARGS(&slot.outputCommands));
            if (SUCCEEDED(hr)) hr=slot.outputCommands->Close();
            if (FAILED(hr)) return hr;
        }
        return S_OK;
    }
    HRESULT prepareAudit() {
        if (auditReadback) return S_OK;
        const auto texture=slots[0].texture->GetDesc(); UINT rows=0; UINT64 rowBytes=0;
        device->GetCopyableFootprints(&texture,0,1,0,&footprint,&rows,&rowBytes,&auditBytes);
        if (rows!=layout.height || rowBytes!=layout.stride || footprint.Offset || footprint.Footprint.RowPitch<rowBytes ||
            footprint.Footprint.RowPitch>16384 || footprint.Footprint.RowPitch%256 ||
            auditBytes<std::uint64_t(layout.height-1)*footprint.Footprint.RowPitch+layout.stride || auditBytes>64ull*1024*1024) return E_INVALIDARG;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_READBACK; heap.CreationNodeMask=heap.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width=auditBytes; buffer.Height=1; buffer.DepthOrArraySize=buffer.MipLevels=1;
        buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&auditReadback));
    }
    HRESULT draw(TexturePresenter& window,Frame frame) {
        auto& slot=slots[frame.slot];
        // Input acknowledges retirement before publishing a ready frame. Its
        // wait on outputRetired also proves the previous output allocator is idle.
        const auto inputValue=inputRetired->GetCompletedValue();
        if (inputValue==UINT64_MAX || inputValue<frame.sequence) return E_FAIL;
        auto hr=slot.outputAllocator->Reset();
        if (SUCCEEDED(hr)) hr=slot.outputCommands->Reset(slot.outputAllocator.Get(),nullptr);
        if (SUCCEEDED(hr) && frame.audit) hr=prepareAudit();
        if (FAILED(hr)) return hr;
        auto before=transition(slot.texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        slot.outputCommands->ResourceBarrier(1,&before);
        hr=window.copy(slot.outputCommands.Get(),slot.texture.Get(),frame.audit ? auditReadback.Get() : nullptr,footprint);
        if (FAILED(hr)) return hr;
        auto after=transition(slot.texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        slot.outputCommands->ResourceBarrier(1,&after);
        hr=slot.outputCommands->Close(); if (FAILED(hr)) return hr;
        ID3D12CommandList* lists[]{slot.outputCommands.Get()}; outputQueue->ExecuteCommandLists(1,lists);
        hr=outputQueue->Signal(outputRetired.Get(),frame.sequence); if (FAILED(hr)) ExitProcess(3);
        outputSubmitted=frame.sequence;
        if (frame.audit) {
            retire(outputRetired.Get(),frame.sequence);
            void* pixels=nullptr; const D3D12_RANGE range{0,static_cast<SIZE_T>(auditBytes)};
            hr=auditReadback->Map(0,&range,&pixels); if (FAILED(hr)) return hr;
            std::uint64_t hash=0;
            hr=driver_capture::writeCompositor(frame.audit,layout.width,layout.height,layout.format,pixels,footprint.Footprint.RowPitch,hash);
            const D3D12_RANGE empty{}; auditReadback->Unmap(0,&empty);
            if (FAILED(hr)) return hr;
            ++completedAudits;
            std::cerr<<"{\"nativeCompositorRingAudited\":true,\"frame\":"<<frame.sequence<<",\"auditRound\":"<<frame.audit
                <<",\"fnv1a\":"<<hash<<",\"width\":"<<layout.width<<",\"height\":"<<layout.height
                <<",\"fourcc\":"<<layout.format<<",\"copyFenceObserved\":"<<outputRetired->GetCompletedValue()<<"}\n";
        }
        {
            std::lock_guard<std::mutex> lock(guard);
            slot.reusedAfter=frame.sequence; slot.occupied=false;
        }
        hr=window.presentQueued(outputQueue.Get(),frame.sequence);
        if (SUCCEEDED(hr)) {
            presented=frame.sequence;
            std::cerr<<"{\"nativeCompositorRingFrame\":true,\"frame\":"<<frame.sequence<<",\"slot\":"<<frame.slot
                <<",\"cpuReadback\":"<<(frame.audit?"true":"false")<<",\"cpuUpload\":false,\"displayVerified\":false}\n";
        }
        return hr;
    }
    void run() noexcept {
        try {
            auto status=create();
            TexturePresenter window;
            struct Drain {
                CompositorPresenter& owner; TexturePresenter& window;
                ~Drain() { window.drainQueued(); if (owner.outputSubmitted) retire(owner.outputRetired.Get(),owner.outputSubmitted); }
            } drain{*this,window};
            if (SUCCEEDED(status)) {
                Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
                status=CreateDXGIFactory1(IID_PPV_ARGS(&factory));
                if (SUCCEEDED(status)) status=window.initialize(factory.Get(),device.Get(),outputQueue.Get(),layout.width,layout.height,
                    static_cast<DXGI_FORMAT>(driver_bridge::compositorDxgiFormat(layout.format)),true);
            }
            { std::lock_guard<std::mutex> lock(guard); initialization=status; initialized=true; }
            ready.notify_one();
            if (FAILED(status)) { failure=status; return; }
            std::cerr<<"{\"nativeCompositorRingOpened\":true,\"slots\":3,\"sourceCacheLimit\":3,\"uiThreadOwned\":true,\"ordinaryCpuReadback\":false}\n";
            for (;;) {
                if (!window.serviceMessages()) { failure=HRESULT_FROM_WIN32(ERROR_CANCELLED); break; }
                Frame frame{}; bool have=false;
                { std::lock_guard<std::mutex> lock(guard);
                  if (!frames.empty()) { frame=frames.front(); frames.pop_front(); have=true; }
                  else if (stopping) break; }
                if (have) { status=draw(window,frame); if (FAILED(status)) { failure=status; break; } continue; }
                const auto wake=MsgWaitForMultipleObjectsEx(1,&work,1000,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                if (wake!=WAIT_OBJECT_0 && wake!=WAIT_OBJECT_0+1 && wake!=WAIT_TIMEOUT) { failure=E_FAIL; break; }
            }
            window.drainQueued();
            if (outputSubmitted) retire(outputRetired.Get(),outputSubmitted);
            if (completedAudits && SUCCEEDED(failure.load())) {
                const auto until=GetTickCount64()+1000;
                do { if (!window.serviceMessages()) break; Sleep(1); } while (GetTickCount64()<until);
                window.reportQueuedDisplay();
            }
        } catch (...) {
            failure=E_FAIL;
            { std::lock_guard<std::mutex> lock(guard); if (!initialized) { initialization=E_FAIL; initialized=true; } }
            ready.notify_one();
        }
    }
public:
    CompositorPresenter()=default;
    ~CompositorPresenter() { stop(); }
    HRESULT initialize(LUID luid,driver_bridge::CompositorCaptureDesc desc) {
        if (ui.joinable() || !driver_bridge::validCompositorFrame(desc)) return E_INVALIDARG;
        selected=luid; layout=desc; layout.flags=0;
        work=CreateEventW(nullptr,FALSE,FALSE,nullptr); if (!work) return HRESULT_FROM_WIN32(GetLastError());
        inputEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if (!inputEvent) { const auto error=GetLastError(); if (!CloseHandle(work)) ExitProcess(3); work=nullptr; return HRESULT_FROM_WIN32(error); }
        try { ui=std::thread([this]{run();}); }
        catch (...) { failure=E_OUTOFMEMORY; return E_OUTOFMEMORY; }
        std::unique_lock<std::mutex> lock(guard);
        if (!ready.wait_for(lock,std::chrono::seconds(10),[this]{return initialized;})) ExitProcess(3);
        return initialization;
    }
    HRESULT submit(std::uint32_t resourceId,HANDLE resource,HANDLE sync,driver_bridge::CompositorCaptureDesc desc,std::uint64_t& value) {
        value=0;
        if (!resourceId || !resource || resource==INVALID_HANDLE_VALUE || !sync || sync==INVALID_HANDLE_VALUE ||
            !driver_bridge::validCompositorFrame(desc) || desc.width!=layout.width || desc.height!=layout.height || desc.format!=layout.format ||
            !initialized || FAILED(initialization) || submitted==UINT64_MAX-1) return E_INVALIDARG;
        if (FAILED(failure.load())) return failure.load();
        unsigned index=0;
        { std::lock_guard<std::mutex> lock(guard);
          if (stopping) return E_FAIL;
          while (index<slots.size() && slots[index].occupied) ++index;
          if (index==slots.size()) return HRESULT_FROM_WIN32(ERROR_RETRY);
          slots[index].occupied=true; }
        auto& slot=slots[index]; bool inputSubmitted=false, inputCompleted=false, published=false;
        struct Rollback { Slot& slot; std::mutex& guard; std::atomic<HRESULT>& failure; bool& submitted; bool& completed; bool& published;
            ~Rollback() {
                if (submitted && !completed) ExitProcess(3);
                if (submitted && !published) failure=E_FAIL;
                std::lock_guard<std::mutex> lock(guard); if (!published) slot.occupied=false;
            }
        } rollback{slot,guard,failure,inputSubmitted,inputCompleted,published};
        auto source=sources.find(resourceId);
        HRESULT hr=S_OK;
        if (source==sources.end()) {
            if (sources.size()>=3) return E_OUTOFMEMORY;
            Resource imported; hr=device->OpenSharedHandle(resource,IID_PPV_ARGS(&imported)); if (FAILED(hr)) return hr;
            const auto actual=imported->GetDesc();
            if (actual.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || actual.Width!=layout.width || actual.Height!=layout.height ||
                actual.Format!=static_cast<DXGI_FORMAT>(driver_bridge::compositorDxgiFormat(layout.format)) || actual.DepthOrArraySize!=1 ||
                actual.MipLevels!=1 || actual.SampleDesc.Count!=1 || actual.SampleDesc.Quality || actual.Flags!=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) return E_INVALIDARG;
            source=sources.emplace(resourceId,std::move(imported)).first;
            peakSources=(std::max)(peakSources,static_cast<unsigned>(sources.size()));
        }
        Microsoft::WRL::ComPtr<ID3D12Fence> producer;
        hr=device->OpenSharedHandle(sync,IID_PPV_ARGS(&producer)); if (FAILED(hr)) return hr;
        hr=slot.inputAllocator->Reset(); if (SUCCEEDED(hr)) hr=slot.inputCommands->Reset(slot.inputAllocator.Get(),nullptr);
        if (FAILED(hr)) return hr;
        const auto a=transition(source->second.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        const auto b=transition(slot.texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        slot.inputCommands->ResourceBarrier(1,&a); slot.inputCommands->ResourceBarrier(1,&b);
        slot.inputCommands->CopyResource(slot.texture.Get(),source->second.Get());
        const auto c=transition(source->second.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        const auto d=transition(slot.texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        slot.inputCommands->ResourceBarrier(1,&c); slot.inputCommands->ResourceBarrier(1,&d);
        hr=slot.inputCommands->Close(); if (FAILED(hr)) return hr;
        unsigned audit=0;
        if (desc.flags & driver_bridge::CompositorFrameAudit) { if (auditAttempts>=2) return E_INVALIDARG; audit=++auditAttempts; }
        hr=inputQueue->Wait(producer.Get(),desc.fence); if (FAILED(hr)) return hr;
        inputSubmitted=true;
        if (slot.reusedAfter) { hr=inputQueue->Wait(outputRetired.Get(),slot.reusedAfter); if (FAILED(hr)) ExitProcess(3); }
        ID3D12CommandList* lists[]{slot.inputCommands.Get()}; inputQueue->ExecuteCommandLists(1,lists);
        const auto frame=submitted+1;
        hr=inputQueue->Signal(inputRetired.Get(),frame); if (FAILED(hr)) ExitProcess(3);
        if (inputRetired->GetCompletedValue()<frame) {
            hr=inputRetired->SetEventOnCompletion(frame,inputEvent);
            if (FAILED(hr) || WaitForSingleObject(inputEvent,5000)!=WAIT_OBJECT_0) ExitProcess(3);
        }
        const auto copied=inputRetired->GetCompletedValue();
        if (copied==UINT64_MAX || copied<frame) ExitProcess(3);
        inputCompleted=true;
        if (producer->GetCompletedValue()<desc.fence || producer->GetCompletedValue()==UINT64_MAX) return E_FAIL;
        std::cerr<<"{\"nativeCompositorRingInput\":true,\"frame\":"<<frame<<",\"slot\":"<<index<<",\"sourceTicket\":"<<resourceId
            <<",\"producerFenceTarget\":"<<desc.fence<<",\"producerFenceObserved\":"<<producer->GetCompletedValue()
            <<",\"inputCopyFenceObserved\":"<<inputRetired->GetCompletedValue()<<",\"sourceReturnedToCommon\":true,\"sourceCopyRetired\":true,\"cpuReadback\":false,\"cpuUpload\":false}\n";
        { std::lock_guard<std::mutex> lock(guard); frames.push_back({index,audit,frame});
          peakQueued=(std::max)(peakQueued,static_cast<unsigned>(frames.size())); }
        submitted=frame; value=frame; published=true;
        if (!SetEvent(work)) ExitProcess(3);
        return S_OK;
    }
    void releaseSource(std::uint32_t resource) { sources.erase(resource); }
    bool stop() {
        if (!ui.joinable()) {
            if (work && !CloseHandle(work)) ExitProcess(3);
            if (inputEvent && !CloseHandle(inputEvent)) ExitProcess(3);
            work=inputEvent=nullptr;
            return SUCCEEDED(failure.load());
        }
        { std::lock_guard<std::mutex> lock(guard); stopping=true; }
        if (!SetEvent(work)) ExitProcess(3);
        ui.join();
        const bool clean=SUCCEEDED(failure.load()) && presented==submitted && frames.empty() &&
            std::none_of(slots.begin(),slots.end(),[](const Slot& slot){return slot.occupied;});
        sources.clear();
        for (auto& slot:slots) { slot.inputCommands.Reset(); slot.outputCommands.Reset(); slot.inputAllocator.Reset(); slot.outputAllocator.Reset(); slot.texture.Reset(); }
        auditReadback.Reset(); inputQueue.Reset(); outputQueue.Reset(); inputRetired.Reset(); outputRetired.Reset(); device.Reset();
        const bool closed=CloseHandle(work)!=FALSE; work=nullptr;
        const bool inputClosed=CloseHandle(inputEvent)!=FALSE; inputEvent=nullptr;
        std::cerr<<"{\"nativeCompositorRingStopped\":"<<(clean&&closed?"true":"false")<<",\"submitted\":"<<submitted<<",\"presented\":"<<presented
            <<",\"completedAudits\":"<<completedAudits<<",\"peakQueued\":"<<peakQueued<<",\"peakSources\":"<<peakSources
            <<",\"workHandleClosed\":"<<(closed?"true":"false")<<",\"inputEventClosed\":"<<(inputClosed?"true":"false")
            <<",\"uiThreadJoined\":true,\"liveGpuResources\":0,\"displayVerified\":false}\n";
        if (!closed || !inputClosed) ExitProcess(3);
        return clean;
    }
};
}
