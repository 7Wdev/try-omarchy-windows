// SPDX-License-Identifier: MIT
#include "driver_coordinator.h"
#include <iostream>
#include <set>
#include <string>
using namespace driver_bridge;
namespace cg=driver_bridge::coordinator;
#define CHECK(...) do { if(!(__VA_ARGS__)) throw std::runtime_error("coordinator control line "+std::to_string(__LINE__)); } while(0)
struct NativeMock : Driver {
    unsigned glCalls=0,syncCalls=0,resourceCalls=0,objectCalls=0,calls=0,destroyed=0,badStage=0;
    std::uint32_t next=100;std::set<std::uint32_t> live,devices;
    std::uint64_t fenceOffset=0;
    bool vmStopped=false,requireVmStop=false;
    bool globalDevicePhase=false;
    bool liveSupported=true,liveReady=true;
    std::uint32_t blockedHandle=0;
    unsigned livePrepares=0;
    std::map<std::uint32_t,std::uint32_t> paging,hardware;
    std::set<std::uint32_t> pagingSyncs,hardwareSyncs;
    unsigned pagingWaits=0;
    std::uint32_t waitedPagingSync=0;
    native_gpu::Capabilities caps{1,AsyncHwSubmitCapability|SyncCapability|GpuReservationCapability|VendorMemoryManagementCapability|
        ExpandedVendorGpuCapability|ExpandedVendorCpuCapability|VendorAllocationCapability|SharedVendorResourceCapability|NativeSharedTextureCapability|ContextCapability|GuestPagingCapability|HwQueueCapability,0x10de,0x1234};
    native_gpu::Capabilities capabilities() const override { return caps; }
    bool supportsLiveRetirement() const override { return liveSupported && !vmStopped; }
    bool prepareLiveRetirement() override { ++livePrepares;return supportsLiveRetirement() && liveReady; }
    SyncResult createSync(std::uint32_t device,SyncDesc desc) override {
        CHECK(devices.count(device) && validSync(desc));return {openAdapter(),{desc.type==5 ? 4096ull : 0ull,desc.type==5 ? 8192ull : 0ull}};
    }
    Result exportSync(std::uint32_t sync) override { CHECK(live.count(sync));return openAdapter(); }
    bool beginCoordinatedObjects(CoordinatedObjectCapacity value) override { CHECK(++objectCalls==1 && validCoordinatedObjectCapacity(value));return badStage!=4; }
    OpenGlProfile beginOpenGl(std::uint32_t version) override { CHECK(++glCalls==1 && (version==10 || version==11 || version==12 || version==13));auto p=version==13 ? GuestOpenGlDesktopReferenceProfile : version==12 ? GuestOpenGlDesktopSlotProfile : version==11 ? GuestOpenGlDesktopFenceProfile : GuestOpenGlDesktopPresentationProfile;if(badStage==1) p.objectLimit=0;return p; }
    SyncSharingProfile beginSyncSharing() override { CHECK(++syncCalls==1);auto p=GuestSyncSharingProfile;if(badStage==2) p.version=1;return p; }
    ResourceSharingProfile beginResourceSharing() override { CHECK(++resourceCalls==1);auto p=GuestResourceSharingProfile;if(badStage==3) p.handleLimit=0;return p; }
    Result openAdapter() override { ++calls;live.insert(++next);return {0,next,0}; }
    Result queryVersion(std::uint32_t adapter) override { ++calls;CHECK(live.count(adapter));return {0,0,31}; }
    Result createDevice(std::uint32_t adapter) override { CHECK(live.count(adapter));const auto result=openAdapter();devices.insert(result.nativeHandle);return result; }
    Result queryAdapter(std::uint32_t,QueryDesc,std::vector<std::uint8_t>&) override { return {-95,0,0}; }
    Result createContext(std::uint32_t device,ContextDesc desc,std::vector<std::uint8_t>& data) override { CHECK(devices.count(device) && validContext(desc) && data.size()==desc.privateBytes);return openAdapter(); }
    GuestPagingResult createGuestPagingQueue(std::uint32_t device) override {
        CHECK(devices.count(device));const auto queue=openAdapter();const auto sync=++next;pagingSyncs.insert(sync);paging.emplace(queue.nativeHandle,sync);return {queue,sync,fenceOffset};
    }
    HwQueueResult createHwQueue(std::uint32_t context,HwQueueDesc desc,std::vector<std::uint8_t>& data) override {
        CHECK(live.count(context) && validHwQueue(desc) && data.size()==desc.privateBytes);
        const auto queue=openAdapter();const auto sync=++next;hardwareSyncs.insert(sync);hardware.emplace(queue.nativeHandle,sync);return {queue,sync,0,4096};
    }
    Result queueContextWait(std::uint32_t context,const std::vector<HwQueueWaitItem>& items) override {
        CHECK(live.count(context) && items.size()==1 && (pagingSyncs.count(items[0].sync) || hardwareSyncs.count(items[0].sync)));++pagingWaits;waitedPagingSync=items[0].sync;return {0,0,0};
    }
    Result createPagingQueue(std::uint32_t) override { return {-95,0,0}; }
    Result readPagingFence(std::uint32_t) override { return {-95,0,0}; }
    Result createAllocation(std::uint32_t,std::uint32_t) override { return {-95,0,0}; }
    Result createSharedAllocation(std::uint32_t,GuestRange) override { return {-95,0,0}; }
    Result copySharedAllocation(std::uint32_t,std::uint32_t,CopyRange) override { return {-95,0,0}; }
    Result writeAllocation(std::uint32_t,Range,const std::uint8_t*) override { return {-95,0,0}; }
    Result readAllocation(std::uint32_t,Range,std::vector<std::uint8_t>&) override { return {-95,0,0}; }
    Result makeResident(std::uint32_t,std::uint32_t) override { return {-95,0,0}; }
    Result mapAllocation(std::uint32_t,std::uint32_t) override { return {-95,0,0}; }
    Result queryResidency(std::uint32_t) override { return {-95,0,0}; }
    Result reserveGpuAddress(std::uint32_t adapter,GpuReservationDesc desc) override {
        CHECK(live.count(adapter) && validGpuReservation(desc));const auto result=openAdapter();
        return {0,result.nativeHandle,0x10000000ull+result.nativeHandle*65536ull};
    }
    Result destroy(Kind kind,std::uint32_t id) override {
        if(id==blockedHandle) return {-16,0,0};
        if(kind==Kind::PagingSync || kind==Kind::HwQueueSync) return {0,0,0};
        CHECK(!requireVmStop || vmStopped);
        if(globalDevicePhase && kind==Kind::Adapter && !devices.empty()) return {-16,0,0};
        if(kind==Kind::Device) CHECK(devices.erase(id)==1);
        if(kind==Kind::PagingQueue) { CHECK(pagingSyncs.erase(paging.at(id))==1);paging.erase(id); }
        if(kind==Kind::HwQueue) { CHECK(hardwareSyncs.erase(hardware.at(id))==1);hardware.erase(id); }
        CHECK(live.erase(id)==1);++destroyed;return {0,0,0};
    }
};
template<class T> static std::vector<std::uint8_t> encoded(const T& value) { std::vector<std::uint8_t> out(sizeof value);std::memcpy(out.data(),&value,sizeof value);return out; }
template<class T> static T at(const std::vector<std::uint8_t>& value,std::size_t offset=0) { CHECK(value.size()>=offset+sizeof(T));T out{};std::memcpy(&out,value.data()+offset,sizeof out);return out; }
static std::vector<std::uint8_t> inner(Op op,std::uint32_t handle=0,const std::vector<std::uint8_t>& body={}) {
    auto out=encoded(Header{static_cast<std::uint32_t>(op),handle,0,0});out.insert(out.end(),body.begin(),body.end());return out;
}
struct Control {
    cg::Runtime& owner;std::uint64_t sequence=1,callSequence=1;
    std::vector<std::uint8_t> control(cg::Operation op,const std::vector<std::uint8_t>& body={}) {
        cg::Frame h;h.operation=op;h.sequence=sequence++;h.bytes=static_cast<std::uint32_t>(body.size());auto out=encoded(h);out.insert(out.end(),body.begin(),body.end());return owner.dispatch(out);
    }
    std::vector<std::uint8_t> call(std::uint64_t actor,const std::vector<std::uint8_t>& body) {
        actors::Envelope h;h.actor=actor;h.sequence=callSequence++;h.bytes=static_cast<std::uint32_t>(body.size());auto out=encoded(h);out.insert(out.end(),body.begin(),body.end());return owner.dispatch(out);
    }
    std::uint64_t reg(cg::Role role=cg::Role::Application) {
        const auto reply=control(cg::Operation::Register,encoded(cg::Registration{role,0}));CHECK(!at<cg::Frame>(reply).status);return at<std::uint64_t>(reply,32);
    }
};
static void liveRetirementControls() {
    {
        NativeMock driver;cg::Runtime owner(driver);Control c{owner};
        c.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopReferenceCapacity,GuestCoordinatedReferenceObjects}));
        const auto actor=c.reg();c.call(actor,hello());c.call(actor,inner(Op::OpenAdapter));
        c.control(cg::Operation::Disconnect,encoded(actor));
        CHECK(at<cg::Frame>(c.control(cg::Operation::RetireExitedActor,encoded(actor))).status==-95 && !driver.destroyed);
        CHECK(at<cg::Frame>(c.control(cg::Operation::BeginLiveRetirement,encoded(LiveRetirementProfile{}))).status==-16);
        CHECK(at<cg::Frame>(c.control(cg::Operation::Release,encoded(actor))).status==-16 && !driver.destroyed);
        CHECK(owner.teardownAfterVmExit(true));
    }
    for(unsigned field=0;field<4;++field) {
        auto profile=LiveRetirementProfile{};std::uint32_t words[4]{};std::memcpy(words,&profile,sizeof words);
        ++words[field];std::memcpy(&profile,words,sizeof words);CHECK(!validLiveRetirementProfile(profile));
    }
    for(unsigned field=0;field<8;++field) {
        LiveRetirementProgress progress{1,1,20,16,16,0,0,0};CHECK(validLiveRetirementProgress(progress));
        if(field==0) progress.version=2;
        if(field==1) progress.phase=5;
        if(field==2) progress.remaining=0;
        if(field==3) progress.attempted=17;
        if(field==4) progress.destroyed=17;
        if(field==5) progress.sharedReferences=21;
        if(field==6) progress.flags=8;
        if(field==7) progress.ntstatus=1;
        CHECK(!validLiveRetirementProgress(progress));
    }
    for(const auto& capacity:{GuestOpenGlDesktopPresentationCapacity,GuestOpenGlDesktopFenceCapacity,GuestOpenGlDesktopSlotCapacity}) {
        NativeMock driver;cg::Runtime owner(driver);Control c{owner};
        const auto objects=capacity.mapping.aperture.version==10 ? CoordinatedObjectCapacity{} : GuestCoordinatedDesktopObjects;
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(cg::Initialization{capacity,objects}))).status);
        CHECK(at<cg::Frame>(c.control(cg::Operation::BeginLiveRetirement,encoded(LiveRetirementProfile{}))).status==-95 && !driver.livePrepares);
    }
    NativeMock driver;cg::Runtime owner(driver);Control c{owner};
    CHECK(!at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopReferenceCapacity,GuestCoordinatedReferenceObjects}))).status);
    driver.liveSupported=false;
    CHECK(at<cg::Frame>(c.control(cg::Operation::BeginLiveRetirement,encoded(LiveRetirementProfile{}))).status==-95);
    driver.liveSupported=true;
    CHECK(validLiveRetirementProfile(at<LiveRetirementProfile>(c.control(cg::Operation::BeginLiveRetirement,encoded(LiveRetirementProfile{})),32)));
    CHECK(at<cg::Frame>(c.control(cg::Operation::BeginLiveRetirement,encoded(LiveRetirementProfile{}))).status==-16);
    const auto primary=c.reg(cg::Role::Compositor),app=c.reg(),spare1=c.reg(),spare2=c.reg();
    (void)spare1;(void)spare2;
    for(const auto actor:{primary,app}) {
        CHECK(!at<Header>(c.call(actor,hello()),40).status);
        CHECK(!at<Header>(c.call(actor,inner(Op::BeginOpenGl,0,encoded(GuestOpenGlDesktopReferenceCapacity))),40).status);
        CHECK(at<Header>(c.call(actor,inner(Op::OpenAdapter)),40).handle==1);
    }
    CHECK(at<cg::Frame>(c.control(cg::Operation::RetireExitedActor,encoded(app))).status==-16 && !driver.destroyed);
    CHECK(at<Header>(c.call(app,inner(Op::CreateDevice,1)),40).handle==2);
    auto context=encoded(ContextDesc{0,1,16,16,4,0});context.resize(context.size()+4);
    CHECK(at<Header>(c.call(app,inner(Op::CreateContext,2,context)),40).handle==3);
    auto queue=encoded(HwQueueDesc{0,4,0,0});queue.resize(queue.size()+4);
    CHECK(at<Header>(c.call(app,inner(Op::CreateHwQueue,3,queue)),40).handle==4);
    driver.blockedHandle=driver.next-1;
    for(unsigned n=0;n<25;++n) CHECK(!at<Header>(c.call(app,inner(Op::ReserveGpuAddress,1,encoded(GpuReservationDesc{0,0,0,65536}))),40).status);
    CHECK(!at<cg::Frame>(c.control(cg::Operation::Disconnect,encoded(app))).status);
    const auto before=owner.objectCount();driver.liveReady=false;
    auto progress=at<LiveRetirementProgress>(c.control(cg::Operation::RetireExitedActor,encoded(app)),32);
    CHECK(validLiveRetirementProgress(progress) && progress.flags==RetirementGpuBusy && !progress.attempted && !driver.destroyed && owner.objectCount()==before);
    driver.liveReady=true;
    progress=at<LiveRetirementProgress>(c.control(cg::Operation::RetireExitedActor,encoded(app)),32);
    CHECK(progress.phase==0 && progress.flags==RetirementNativeBusy && progress.remaining && driver.hardware.size()==1 && driver.devices.size()==1);
    CHECK(at<cg::Frame>(c.control(cg::Operation::Release,encoded(app))).status==-16);
    CHECK(at<cg::Frame>(c.control(cg::Operation::Register,encoded(cg::Registration{}))).status==-24);
    driver.blockedHandle=0;unsigned batches=0;
    do {
        progress=at<LiveRetirementProgress>(c.control(cg::Operation::RetireExitedActor,encoded(app)),32);++batches;
        CHECK(validLiveRetirementProgress(progress) && progress.attempted<=16 && !progress.flags && batches<=8);
        CHECK(!at<Header>(c.call(primary,inner(Op::QueryDriverVersion,1)),40).status);
    } while(progress.remaining);
    CHECK(batches>=2 && progress.phase==4 && !driver.vmStopped && driver.devices.empty() && driver.hardware.empty());
    CHECK(!at<cg::Frame>(c.control(cg::Operation::Release,encoded(app))).status && owner.liveRetiredActorCount()==1);
    CHECK(at<actors::Envelope>(c.call(app,hello())).status==-9);
    for(unsigned cycle=0;cycle<5;++cycle) {
        const auto actor=c.reg();CHECK(actor>4);
        c.call(actor,hello());c.call(actor,inner(Op::OpenAdapter));c.call(actor,inner(Op::CreateDevice,1));
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Disconnect,encoded(actor))).status);
        progress=at<LiveRetirementProgress>(c.control(cg::Operation::RetireExitedActor,encoded(actor)),32);
        CHECK(!progress.remaining && progress.destroyed==2 && !driver.vmStopped);
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Release,encoded(actor))).status);
    }
    CHECK(owner.liveRetiredActorCount()==6 && owner.liveRetiredObjectCount()>=40);
    const auto shared=c.reg();c.call(shared,hello());c.call(shared,inner(Op::BeginSyncSharing));
    c.call(shared,inner(Op::OpenAdapter));c.call(shared,inner(Op::CreateDevice,1));
    const auto sync=at<Header>(c.call(shared,inner(Op::CreateSync,2,encoded(SyncDesc{5,NtSharedGpuSyncFlags,1,0,0}))),40);
    CHECK(!sync.status && sync.handle);
    const auto exported=at<Header>(c.call(shared,inner(Op::ExportSync,sync.handle)),40);CHECK(!exported.status && exported.handle);
    c.control(cg::Operation::Disconnect,encoded(shared));const auto prepares=driver.livePrepares;
    progress=at<LiveRetirementProgress>(c.control(cg::Operation::RetireExitedActor,encoded(shared)),32);
    CHECK(progress.flags==RetirementSharedReferences && progress.sharedReferences==1 && driver.livePrepares==prepares && !progress.attempted);
    CHECK(!at<Header>(c.control(cg::Operation::ReapReference,encoded(cg::Reference{shared,Kind::SharedSync,exported.handle})),32).status);
    progress=at<LiveRetirementProgress>(c.control(cg::Operation::RetireExitedActor,encoded(shared)),32);
    CHECK(!progress.remaining && !progress.flags);
    CHECK(!at<cg::Frame>(c.control(cg::Operation::Release,encoded(shared))).status);
    c.control(cg::Operation::Disconnect,encoded(primary));
    CHECK(at<cg::Frame>(c.control(cg::Operation::RetireExitedActor,encoded(primary))).status==-13);
    CHECK(owner.teardownAfterVmExit(true) && driver.live.empty());
    std::cout<<"LIVE_RETIREMENT_CONTROL negotiatedOnly=true exitedApplicationOnly=true pendingGpuWorkRetained=true sharedReferencesRetained=true failedQueueRetainsParents=true batch16=true actorSlotsReused=true native=mock gpu=false\n";
}
int main() {
    liveRetirementControls();
    for(const auto capacity:{GuestOpenGlDesktopPresentationCapacity,GuestOpenGlDesktopFenceCapacity,GuestOpenGlDesktopSlotCapacity,GuestOpenGlDesktopReferenceCapacity}) {
        NativeMock driver;cg::Runtime owner(driver);Control client{owner};
        const auto objects=capacity.mapping.aperture.version==13 ? GuestCoordinatedReferenceObjects : capacity.mapping.aperture.version>=11 ? GuestCoordinatedDesktopObjects : CoordinatedObjectCapacity{};
        CHECK(!at<cg::Frame>(client.control(cg::Operation::Initialize,encoded(cg::Initialization{capacity,objects}))).status);
        auto info=at<cg::Info>(client.control(cg::Operation::Info),32);
        CHECK(cg::validInfo(info) && info.gl.version==capacity.mapping.aperture.version);
        info.gl.version=info.gl.version==10 ? 11 : 10;CHECK(!cg::validInfo(info));
        const auto actor=client.reg();CHECK(!at<Header>(client.call(actor,hello()),40).status);
        const auto other=capacity.mapping.aperture.version==10 ? GuestOpenGlDesktopFenceCapacity : GuestOpenGlDesktopPresentationCapacity;
        CHECK(at<Header>(client.call(actor,inner(Op::BeginOpenGl,0,encoded(other))),40).status==-95);
        CHECK(!at<Header>(client.call(actor,inner(Op::BeginOpenGl,0,encoded(capacity))),40).status);
        CHECK(at<Header>(client.call(actor,inner(Op::OpenAdapter)),40).handle==1);
        CHECK(at<Header>(client.call(actor,inner(Op::CreateDevice,1)),40).handle==2);
        const unsigned reservationLimit=capacity.mapping.aperture.version>=11 ? 128u : 64u;
        for(unsigned n=0;n<reservationLimit;++n)
            CHECK(!at<Header>(client.call(actor,inner(Op::ReserveGpuAddress,1,encoded(GpuReservationDesc{0,0,0,65536}))),40).status);
        CHECK(at<Header>(client.call(actor,inner(Op::ReserveGpuAddress,1,encoded(GpuReservationDesc{0,0,0,65536}))),40).status==-24);
        driver.fenceOffset=511*4096ull+4088;
        const auto paging=client.call(actor,inner(Op::CreateGuestPagingQueue,2));
        CHECK(at<Header>(paging,40).status==(capacity.mapping.aperture.version>=11 ? 0 : -5));
        if(capacity.mapping.aperture.version>=11) CHECK(at<PagingReply>(paging,72).offset==driver.fenceOffset);
        CHECK(owner.teardownAfterVmExit(true) && driver.live.empty());
    }
    for(const auto original:{GuestOpenGlDesktopFenceCapacity,GuestOpenGlDesktopSlotCapacity,GuestOpenGlDesktopReferenceCapacity}) for(unsigned field=0;field<12;++field) {
        NativeMock driver;cg::Runtime owner(driver);Control client{owner};
        auto capacity=original;std::uint32_t words[12]{};
        std::memcpy(words,&capacity,sizeof words);++words[field];std::memcpy(&capacity,words,sizeof words);
        CHECK(at<cg::Frame>(client.control(cg::Operation::Initialize,encoded(cg::Initialization{capacity,GuestCoordinatedDesktopObjects}))).status==-22 && !driver.glCalls);
    }
    for(unsigned field=0;field<8;++field) {
        NativeMock driver;cg::Runtime owner(driver);Control client{owner};
        auto objects=GuestCoordinatedDesktopObjects;std::uint32_t words[8]{};
        std::memcpy(words,&objects,sizeof words);++words[field];std::memcpy(&objects,words,sizeof words);
        CHECK(at<cg::Frame>(client.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopFenceCapacity,objects}))).status==-22 && !driver.glCalls);
    }
    CHECK(!validCoordinatedAgreement(GuestOpenGlDesktopFenceCapacity,{}));
    CHECK(!validCoordinatedAgreement(GuestOpenGlDesktopPresentationCapacity,GuestCoordinatedDesktopObjects));
    CHECK(validCoordinatedAgreement(GuestOpenGlDesktopFenceCapacity,GuestCoordinatedDesktopObjects));
    CHECK(validCoordinatedAgreement(GuestOpenGlDesktopSlotCapacity,GuestCoordinatedDesktopObjects));
    CHECK(validCoordinatedAgreement(GuestOpenGlDesktopReferenceCapacity,GuestCoordinatedReferenceObjects));
    CHECK(!validCoordinatedAgreement(GuestOpenGlDesktopReferenceCapacity,GuestCoordinatedDesktopObjects));
    CHECK(!validCoordinatedAgreement(GuestOpenGlDesktopSlotCapacity,GuestCoordinatedReferenceObjects));
    CHECK(!validCoordinatedAgreement(GuestOpenGlDesktopFenceCapacity,GuestCoordinatedReferenceObjects));
    for(const auto capacity:{GuestOpenGlDesktopReferenceCapacity,GuestOpenGlDesktopSlotCapacity,GuestOpenGlDesktopFenceCapacity})
        for(const auto objects:{GuestCoordinatedReferenceObjects,GuestCoordinatedDesktopObjects}) {
            const bool matching=capacity.mapping.aperture.version==13 ? objects.version==5 : objects.version==4;
            CHECK(validCoordinatedAgreement(capacity,objects)==matching);
        }
    CHECK(!validCoordinatedAgreement(GuestOpenGlDesktopSlotCapacity,{}));
    CHECK(CoordinatedObjectCapacity{}.version==3 && !CoordinatedObjectCapacity{}.vendorAllocations);
    {
        NativeMock driver;Session legacy(driver);CHECK(!at<Header>(legacy.dispatch(hello())).status);
        CHECK(at<Header>(legacy.dispatch(inner(Op::BeginOpenGl,0,encoded(GuestOpenGlDesktopFenceCapacity)))).status==-93 && !driver.glCalls);
        CHECK(at<Header>(legacy.dispatch(inner(Op::BeginOpenGl,0,encoded(GuestOpenGlDesktopSlotCapacity)))).status==-93 && !driver.glCalls);
        CHECK(at<Header>(legacy.dispatch(inner(Op::BeginOpenGl,0,encoded(GuestOpenGlDesktopReferenceCapacity)))).status==-93 && !driver.glCalls);
    }
    std::cout<<"COORDINATOR_FENCE_CONTROL profile11Exact512=true legacyLastPageRejected=true profileMismatchRejected=true byteBudgetsUnchanged=true native=mock gpu=false\n";
    NativeMock native;
    {
        cg::Runtime owner(native);Control client{owner};
        CHECK(at<cg::Frame>(client.control(cg::Operation::Info)).status==-71 && !native.glCalls);
        CHECK(at<cg::Frame>(client.control(cg::Operation::Initialize,encoded(GuestOpenGlDesktopPresentationCapacity))).status==-22 && !native.glCalls);
        auto bad=GuestOpenGlDesktopPresentationCapacity;++bad.mapping.aperture.cpuSlots;
        CHECK(at<cg::Frame>(client.control(cg::Operation::Initialize,encoded(cg::Initialization{bad,{}}))).status==-22 && !native.glCalls);
        for(unsigned mutation=0;mutation<7;++mutation) {
            cg::Initialization request{GuestOpenGlDesktopPresentationCapacity,{}};
            if(mutation==0) request.objects.version=1;
            if(mutation==1) request.objects.hwQueues=33;
            if(mutation==2) request.objects.syncObjects=385;
            if(mutation==3) request.objects.leaseLimit=769;
            if(mutation==4) request.objects.reservedGpuBytes++;
            if(mutation==5) request.objects.reservations++;
            if(mutation==6) request.objects.vendorAllocations=1;
            CHECK(at<cg::Frame>(client.control(cg::Operation::Initialize,encoded(request))).status==-22 && !native.glCalls);
        }
        CHECK(!at<cg::Frame>(client.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopPresentationCapacity,{}}))).status);
        const auto info=client.control(cg::Operation::Info);CHECK(!at<cg::Frame>(info).status && cg::validInfo(at<cg::Info>(info,32)));
        CHECK(at<cg::Frame>(client.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopPresentationCapacity,{}}))).status==-16);
        const auto a=client.reg(cg::Role::Compositor),b=client.reg();CHECK(a==1 && b==2);
        CHECK(at<cg::Frame>(client.control(cg::Operation::Register,encoded(cg::Registration{cg::Role::Compositor,0}))).status==-24);
        for(const auto actor:{a,b}) {
            CHECK(!at<Header>(client.call(actor,hello()),40).status);
            CHECK(!at<Header>(client.call(actor,inner(Op::BeginOpenGl,0,encoded(GuestOpenGlDesktopPresentationCapacity))),40).status);
            CHECK(at<Header>(client.call(actor,inner(Op::BeginOpenGl,0,encoded(GuestOpenGlDesktopPresentationCapacity))),40).status==-16);
            CHECK(!at<Header>(client.call(actor,inner(Op::BeginSyncSharing)),40).status);
            CHECK(!at<Header>(client.call(actor,inner(Op::BeginResourceSharing)),40).status);
            CHECK(at<Header>(client.call(actor,inner(Op::OpenAdapter)),40).handle==1);
        }
        CHECK(native.glCalls==1 && native.objectCalls==1 && native.syncCalls==1 && native.resourceCalls==1 && owner.objectCount()==2);
        CHECK(at<Header>(client.call(b,inner(Op::QueueCompositorFrame)),40).status==-13);
        CHECK(at<Header>(client.call(a,inner(Op::QueueCompositorFrame)),40).status!=-13);
        CHECK(!at<cg::Frame>(client.control(cg::Operation::Disconnect,encoded(a))).status);
        CHECK(at<actors::Envelope>(client.call(a,inner(Op::OpenAdapter))).status==-32);
        CHECK(at<cg::Frame>(client.control(cg::Operation::Release,encoded(a))).status==-16 && owner.objectCount()==2 && !native.destroyed);
        CHECK(!at<Header>(client.call(b,inner(Op::QueryDriverVersion,1)),40).status);
        CHECK(at<cg::Frame>(client.control(cg::Operation::ReapReference,encoded(cg::Reference{a,Kind::Adapter,1}))).status==-22);
        const auto c=client.reg(),d=client.reg();CHECK(c==3 && d==4);
        CHECK(at<cg::Frame>(client.control(cg::Operation::Register,encoded(cg::Registration{}))).status==-24);
        CHECK(!at<cg::Frame>(client.control(cg::Operation::Disconnect,encoded(c))).status);
        CHECK(!at<cg::Frame>(client.control(cg::Operation::Release,encoded(c))).status && client.reg()==5);
        native.requireVmStop=true;native.vmStopped=true; // Owner reaps VM before nonempty Session destruction.
    }
    CHECK(native.live.empty() && native.destroyed==2);
    for(const bool grouped:{false,true}) {
        NativeMock driver;std::unique_ptr<CoordinatedProfiles> group;
        if(grouped) group=std::make_unique<CoordinatedProfiles>(driver,GuestOpenGlDesktopPresentationCapacity);
        auto session=grouped ? std::make_unique<Session>(driver,*group) : std::make_unique<Session>(driver);
        session->dispatch(hello());session->dispatch(inner(Op::BeginSyncSharing));session->dispatch(inner(Op::OpenAdapter));session->dispatch(inner(Op::CreateDevice,1));
        auto context=encoded(ContextDesc{0,1,16,16,4,0});context.resize(context.size()+4);
        CHECK(at<Header>(session->dispatch(inner(Op::CreateContext,2,context))).handle==3);
        const auto result=session->dispatch(inner(Op::CreateGuestPagingQueue,2));const auto sync=at<PagingReply>(result,32).sync;
        std::uint32_t waitContext=3;
        auto wait=[&](std::uint32_t source) { auto payload=encoded(HwQueueWaitDesc{1,0});const auto item=encoded(HwQueueWaitItem{source,0,7});payload.insert(payload.end(),item.begin(),item.end());return session->dispatch(inner(Op::QueueContextWait,waitContext,payload)); };
        CHECK(at<Header>(wait(sync)).status==(grouped ? 0 : -9));
        CHECK(driver.pagingWaits==(grouped ? 1u : 0u));
        if(grouped) CHECK(driver.pagingSyncs.count(driver.waitedPagingSync));
        const auto foreignDevice=at<Header>(session->dispatch(inner(Op::CreateDevice,1))).handle;
        const auto foreign=session->dispatch(inner(Op::CreateGuestPagingQueue,foreignDevice));
        const auto before=driver.pagingWaits;
        CHECK(at<Header>(wait(at<PagingReply>(foreign,32).sync)).status==-9 && driver.pagingWaits==before);
        auto hardware=encoded(HwQueueDesc{0,4,0,0});hardware.resize(hardware.size()+4);
        const auto ownQueue=session->dispatch(inner(Op::CreateHwQueue,3,hardware));
        CHECK(!at<Header>(ownQueue).status);
        const auto progress=at<HwQueueReply>(ownQueue,32).sync;
        CHECK(at<Header>(wait(progress)).status==(grouped ? 0 : -9));
        if(grouped) CHECK(driver.hardwareSyncs.count(driver.waitedPagingSync));
        const auto foreignContext=at<Header>(session->dispatch(inner(Op::CreateContext,foreignDevice,context))).handle;
        const auto foreignQueue=session->dispatch(inner(Op::CreateHwQueue,foreignContext,hardware));
        const auto count=driver.pagingWaits;
        CHECK(at<Header>(wait(at<HwQueueReply>(foreignQueue,32).sync)).status==-9 && driver.pagingWaits==count);
        CHECK(at<Header>(wait(UINT32_MAX)).status==-9 && driver.pagingWaits==count);
        CHECK(!at<Header>(session->dispatch(inner(Op::DestroyHwQueue,at<Header>(ownQueue).handle))).status);
        CHECK(at<Header>(wait(progress)).status==-9 && driver.pagingWaits==count);
        waitContext=at<Header>(session->dispatch(inner(Op::CreateContext,2,encoded(ContextDesc{0,0,8,0,0,0})))).handle;
        CHECK(waitContext);
        CHECK(at<Header>(wait(sync)).status==(grouped ? 0 : -9));
        const auto after=driver.pagingWaits;
        CHECK(at<Header>(wait(at<PagingReply>(foreign,32).sync)).status==-9 && driver.pagingWaits==after);
        CHECK(at<Header>(session->dispatch(inner(Op::CreateHwQueue,waitContext,hardware))).status==-95);
        auto render=encoded(ContextDesc{0,1,0,12,4,0});render.resize(render.size()+4);
        waitContext=at<Header>(session->dispatch(inner(Op::CreateContext,2,render))).handle;
        CHECK(at<Header>(wait(sync)).status==-9 && driver.pagingWaits==after);
    }
    {
        NativeMock driver;cg::Runtime owner(driver);Control client{owner};
        client.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopPresentationCapacity,{}}));
        for(unsigned n=0;n<2;++n) {
            const auto id=client.reg();client.call(id,hello());client.call(id,inner(Op::OpenAdapter));client.call(id,inner(Op::CreateDevice,1));
        }
        CHECK(owner.objectCount()==4 && driver.devices.size()==2);
        driver.requireVmStop=driver.globalDevicePhase=true;
        bool early=false;try { owner.teardownAfterVmExit(false); } catch(const std::exception&) { early=true; }
        CHECK(early && !driver.destroyed && owner.objectCount()==4);
        driver.vmStopped=true;
        CHECK(owner.teardownAfterVmExit(true) && driver.live.empty() && driver.destroyed==4 && !owner.objectCount());
    }
    for(unsigned stage=1;stage<=3;++stage) {
        NativeMock broken;broken.badStage=stage;cg::Runtime owner(broken);Control client{owner};bool threw=false;
        try { (void)client.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopPresentationCapacity,{}})); } catch(const std::exception&) { threw=true; }
        CHECK(threw && owner.failed() && owner.dispatch({}).empty() && broken.glCalls==1 && broken.syncCalls==(stage>=2 ? 1u : 0u) && broken.resourceCalls==(stage>=3 ? 1u : 0u));
    }
    for(unsigned mutation=0;mutation<3;++mutation) {
        NativeMock driver;
        if(mutation==0) driver.caps.vendor=0x8086;
        if(mutation==1) driver.caps.version=2;
        if(mutation==2) driver.caps.flags&=~ExpandedVendorCpuCapability;
        bool threw=false;
        try { CoordinatedProfiles invalid(driver,GuestOpenGlDesktopPresentationCapacity); } catch(const std::exception&) { threw=true; }
        CHECK(threw && !driver.glCalls && !driver.syncCalls && !driver.resourceCalls);
    }
    {
        NativeMock broken;broken.badStage=4;cg::Runtime owner(broken);Control client{owner};bool threw=false;
        try { client.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopPresentationCapacity,{}})); } catch(const std::exception&) { threw=true; }
        CHECK(threw && owner.failed() && broken.objectCalls==1 && !broken.syncCalls && !broken.resourceCalls);
    }
    {
        NativeMock driver,other;CoordinatedProfiles group(driver,GuestOpenGlDesktopPresentationCapacity);bool wrong=false;
        try { Session invalid(other,group); } catch(const std::exception&) { wrong=true; }CHECK(wrong && !other.calls);
        Session session(driver,group);CHECK(!at<Header>(session.dispatch(hello())).status);
        CHECK(at<Header>(session.dispatch(inner(Op::BeginStreaming))).status==-95);
        CHECK(at<Header>(session.dispatch(inner(Op::BeginOpenGl,0,encoded(std::uint32_t{5})))).status==-95);
        ++driver.caps.device;CHECK(at<Header>(session.dispatch(hello())).status==-95);
    }
    for(unsigned mutation=0;mutation<7;++mutation) {
        NativeMock driver;cg::Runtime owner(driver);cg::Frame h;h.operation=cg::Operation::Info;h.sequence=1;
        if(mutation==0) ++h.magic;
        if(mutation==1) ++h.version;
        if(mutation==2) ++h.sequence;
        if(mutation==3) ++h.bytes;
        if(mutation==4) h.status=-1;
        if(mutation==5) h.reserved=1;
        auto packet=encoded(h);if(mutation==6) packet.resize(31);(void)owner.dispatch(packet);
        CHECK(owner.failed() && !driver.glCalls && owner.dispatch(packet).empty());
    }
    std::cout<<"COORDINATOR_CONTROL exactCapacity=true oneNativeNegotiation=true independentSessions=true singlePresentationOwner=true drainingCharged=true partialInitializationFatal=true native=mock gpu=false\n";
}
