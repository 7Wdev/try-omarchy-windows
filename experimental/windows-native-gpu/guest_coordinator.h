// SPDX-License-Identifier: MIT
// Privileged controller transport and cross-client capability registry.
#pragma once
#include "guest_broker_server.h"
#include "driver_coordinator.h"
#include "guest_broker_receipts.h"
#include <deque>
#include <set>
#include <atomic>

namespace guest_broker {
namespace cg=driver_bridge::coordinator;
namespace ar=driver_bridge::actors;
class CoordinatorPort {
    Port port;
    std::mutex lock;
    std::uint64_t sequence=1,actorSequence=1;
    std::atomic<bool> failed{false};
    cg::Info agreement{};
    template<class T> static T at(const std::vector<std::uint8_t>& packet,std::size_t offset=0) {
        if(offset>packet.size() || sizeof(T)>packet.size()-offset) throw std::runtime_error("Truncated coordinator response");
        T value{};std::memcpy(&value,packet.data()+offset,sizeof value);return value;
    }
    [[noreturn]] void reject(const char* message) { failed=true;throw std::runtime_error(message); }
public:
    explicit CoordinatorPort(std::string path):port(std::move(path),cg::MaxPacket) {}
    std::vector<std::uint8_t> control(cg::Operation operation,const std::vector<std::uint8_t>& payload={}) {
        std::lock_guard<std::mutex> guard(lock);
        if(failed || sequence==UINT64_MAX) reject("Coordinator control unavailable");
        cg::Frame h;h.operation=operation;h.sequence=sequence++;h.bytes=static_cast<std::uint32_t>(payload.size());
        auto packet=encode(h);packet.insert(packet.end(),payload.begin(),payload.end());
        std::vector<std::uint8_t> reply;
        try { reply=port.exchange(packet); } catch(...) { failed=true;throw; }
        const auto r=at<cg::Frame>(reply);
        if(r.magic!=cg::Magic || r.version!=cg::ProtocolVersion || r.operation!=operation || r.sequence!=h.sequence || r.reserved ||
            r.bytes!=reply.size()-sizeof r || r.status>0 || r.status< -4095 || (r.status && r.bytes)) reject("Coordinator reply identity mismatch");
        if(r.status) throw std::system_error(-r.status,std::generic_category(),"Coordinator control refused");
        return {reply.begin()+sizeof r,reply.end()};
    }
    void initialize(const wddm_aperture_info& layout,const wddm_aperture_caps& caps,const wddm_aperture_coordinator_caps& leases,
                    const wddm_aperture_desktop_fences& fences={},const wddm_aperture_desktop_slots& slots={},bool references=false) {
        const bool wide=layout.fence_slots==512;
        const bool expanded=layout.allocation_slots==1024;
        const driver_bridge::OpenGlDesktopPresentationCapacity wanted{{{expanded ? references ? 13u : 12u : wide ? 11u : 10u,caps.version,caps.cpu_byte_limit,layout.allocation_slots,layout.fence_slots,0},
            driver_bridge::OpenGlDesktopLargeMapBytes},driver_bridge::OpenGlDesktopPresentationReportedBytes};
        if((references && !expanded) || !wddm_aperture_valid_coordinator_caps(leases) || !leases.enabled || leases.lease_limit!=driver_bridge::CoordinatedObjectCapacity{}.leaseLimit ||
            !wddm_aperture_valid_caps(caps) || caps.flags!=(WDDM_APERTURE_DESKTOP_CPU_ENABLED|WDDM_APERTURE_SPARSE_ENABLED) ||
            !driver_bridge::validCoordinatedPresentationCapacity(wanted) ||
            (wide && (!wddm_aperture_valid_desktop_fences(fences) || !fences.enabled || fences.slots!=layout.fence_slots || fences.bytes!=layout.fence_bytes)) ||
            (expanded && (!wddm_aperture_valid_desktop_slots(slots) || !slots.enabled || slots.slots!=layout.allocation_slots || slots.bytes!=layout.allocation_bytes))) reject("Coordinator kernel capacity mismatch");
        const auto objects=references ? driver_bridge::GuestCoordinatedReferenceObjects : wide ? driver_bridge::GuestCoordinatedDesktopObjects : driver_bridge::CoordinatedObjectCapacity{};
        if(!control(cg::Operation::Initialize,encode(cg::Initialization{wanted,objects})).empty()) reject("Coordinator initialization payload");
        agreement=decode<cg::Info>(control(cg::Operation::Info));
        if(!cg::validInfo(agreement) || std::memcmp(&wanted,&agreement.capacity,sizeof wanted)) reject("Coordinator agreement mismatch");
        receipt("BROKER_COORDINATOR_KERNEL_CAPACITY verified=true leaseLimit=%u cpuBytes=%llu flags=%u actualIoctl=true\n",
            leases.lease_limit,static_cast<unsigned long long>(caps.cpu_byte_limit),caps.flags);
        if(wide) receipt("BROKER_COORDINATOR_FENCE_CAPACITY profile=%u slots=%u bytes=%llu actualIoctl=true\n",wanted.mapping.aperture.version,fences.slots,static_cast<unsigned long long>(fences.bytes));
        if(expanded) receipt("BROKER_COORDINATOR_SLOT_CAPACITY profile=%u slots=%u bytes=%llu cpuByteLimit=%llu actualIoctl=true\n",wanted.mapping.aperture.version,slots.slots,static_cast<unsigned long long>(slots.bytes),static_cast<unsigned long long>(caps.cpu_byte_limit));
        if(references) receipt("BROKER_COORDINATOR_REFERENCE_CAPACITY profile=13 objectsVersion=5 resources=64 syncs=64 perActor=16 byteBudgetsMultiplied=false\n");
    }
    cg::Info info() const { return agreement; }
    std::uint64_t attach(bool primary) {
        const auto id=decode<std::uint64_t>(control(cg::Operation::Register,encode(cg::Registration{primary ? cg::Role::Compositor : cg::Role::Application,0})));
        if(!id) reject("Coordinator actor identity absent");
        return id;
    }
    std::vector<std::uint8_t> call(std::uint64_t actor,const std::vector<std::uint8_t>& payload) {
        std::lock_guard<std::mutex> guard(lock);
        if(failed || actorSequence==UINT64_MAX || payload.size()<sizeof(driver_bridge::Header) || payload.size()>native_gpu::MaxPacket)
            reject("Coordinator actor call unavailable");
        ar::Envelope h;h.actor=actor;h.sequence=actorSequence++;h.bytes=static_cast<std::uint32_t>(payload.size());
        auto packet=encode(h);packet.insert(packet.end(),payload.begin(),payload.end());
        std::vector<std::uint8_t> reply;
        try { reply=port.exchange(packet); } catch(...) { failed=true;throw; }
        const auto r=at<ar::Envelope>(reply);
        if(r.magic!=ar::Magic || r.version!=ar::ProtocolVersion || r.operation!=h.operation || r.actor!=actor || r.sequence!=h.sequence ||
            r.reserved || r.padding || r.status || r.bytes!=reply.size()-sizeof r || r.bytes<sizeof(driver_bridge::Header) || r.bytes>native_gpu::MaxPacket)
            reject("Coordinator actor reply mismatch");
        return {reply.begin()+sizeof r,reply.end()};
    }
    driver_bridge::SharedReferenceAlias alias(cg::Alias value) {
        const auto result=decode<driver_bridge::SharedReferenceAlias>(control(cg::Operation::Alias,encode(value)));
        if(result.ntstatus>0 || (result.ntstatus ? result.ticket!=0 : !result.ticket)) reject("Coordinator alias reply mismatch");
        if(result.ntstatus) {
            receipt("BROKER_COORDINATOR_ALIAS_REJECTED kind=%u ntstatus=%d nativeReplyPreserved=true\n",static_cast<unsigned>(value.kind),result.ntstatus);
            throw std::system_error(ENOMEM,std::generic_category(),"Native recipient alias failed");
        }
        return result;
    }
    void disconnect(std::uint64_t actor) { if(!control(cg::Operation::Disconnect,encode(actor)).empty()) reject("Coordinator disconnect payload"); }
    bool release(std::uint64_t actor) {
        try { if(!control(cg::Operation::Release,encode(actor)).empty()) reject("Coordinator release payload");return true; }
        catch(const std::system_error& error) { if(error.code().value()==EBUSY) return false;throw; }
    }
};
class ActorPort {
    CoordinatorPort& owner;
    std::uint64_t id=0;
    const bool primary;
    const pid_t process;
    std::set<std::uint32_t> adapters;
    bool dead=false;
public:
    ActorPort(CoordinatorPort& controller,bool presentationOwner,pid_t pid):owner(controller),primary(presentationOwner),process(pid) {}
    std::uint64_t actor() const { return id; }
    cg::Info info() const { return owner.info(); }
    std::vector<std::uint8_t> exchange(const std::vector<std::uint8_t>& packet) {
        using namespace driver_bridge;
        if(packet.size()<sizeof(Header)) throw std::runtime_error("Actor native request truncated");
        if(!id && !dead) {
            id=owner.attach(primary);
            receipt("BROKER_COORDINATOR_ACTOR actor=%llu pid=%d primary=%s pidfdAuthenticated=true nativeDriverCount=1\n",
                static_cast<unsigned long long>(id),process,primary ? "true" : "false");
        }
        Header h{};std::memcpy(&h,packet.data(),sizeof h);std::vector<std::uint8_t> reply;
        if(dead) {
            if(packet.size()!=sizeof h || (h.type!=static_cast<std::uint32_t>(Op::CloseSharedResource) && h.type!=static_cast<std::uint32_t>(Op::CloseSharedSync)))
                throw std::runtime_error("Disconnected actor cannot submit native calls");
            reply=owner.control(cg::Operation::ReapReference,encode(cg::Reference{id,h.type==static_cast<std::uint32_t>(Op::CloseSharedSync) ? Kind::SharedSync : Kind::SharedResource,h.handle}));
        } else reply=owner.call(id,packet);
        if(reply.size()>=sizeof(Header)+sizeof(Reply)) {
            Header r{};Reply value{};std::memcpy(&r,reply.data(),sizeof r);std::memcpy(&value,reply.data()+sizeof r,sizeof value);
            if(!r.status && !value.ntstatus) {
                if(h.type==static_cast<std::uint32_t>(Op::OpenAdapter)) adapters.insert(r.handle);
                if(h.type==static_cast<std::uint32_t>(Op::CloseAdapter)) adapters.erase(h.handle);
            }
        }
        return reply;
    }
    std::uint32_t alias(std::uint64_t donor,std::uint32_t ticket,std::uint32_t kind) {
        if(dead || adapters.empty()) throw std::system_error(EBADF,std::generic_category(),"Recipient has no adapter");
        return owner.alias(cg::Alias{donor,id,kind==WDDM_OBJECT_SYNC ? driver_bridge::Kind::SharedSync : driver_bridge::Kind::SharedResource,ticket,*adapters.begin(),0}).ticket;
    }
    void processExited() { if(id) owner.disconnect(id);dead=true; }
    void finish() {
        if(!id) { receipt("BROKER_COORDINATOR_METADATA_EXIT pid=%d nativeActorAllocated=false\n",process);return; }
        const auto empty=owner.release(id);
        receipt("BROKER_COORDINATOR_ACTOR_EXIT actor=%llu empty=%s retainedUntilVmExit=%s\n",static_cast<unsigned long long>(id),empty ? "true" : "false",empty ? "false" : "true");
    }
};
// One kernel object controller gives every capability a globally unique
// ticket/cookie. Per-client views use separate controllers and pidfd lifetimes.
template<class Native,class Authority> class ActorObjects;
template<class Authority=KernelObjects> class ObjectRegistry {
    template<class Native,class A> friend class ActorObjects;
    struct Entry {
        ObjectRef global;
        std::uint64_t producer=0;
        std::uint32_t source=0;
        std::map<std::uint64_t,std::uint32_t> aliases;
        bool reaped=false;
    };
    Authority kernel;
    std::mutex mutex;
    std::map<std::uint32_t,Entry> entries;
    std::map<std::uint64_t,std::deque<ObjectRef>> pending;
    std::uint32_t nextTicket=1;
    void collect() {
        for(;;) {
            const auto ref=kernel.reap();if(!ref.ticket) break;
            const auto found=entries.find(ref.ticket);
            if(found==entries.end() || found->second.reaped || found->second.global.kind!=ref.kind || found->second.global.cookie!=ref.cookie)
                throw std::runtime_error("Coordinator global capability reap mismatch");
            auto& entry=found->second;entry.reaped=true;
            for(const auto& alias:entry.aliases) pending[alias.first].push_back(ObjectRef{ref.kind,alias.second,ref.cookie});
        }
    }
public:
    explicit ObjectRegistry(Authority value={}) : kernel(std::move(value)) {}
};
template<class Native,class Authority> class ActorObjects {
    ObjectRegistry<Authority>& registry;
    Native& native;
    ObjectStore local;
    static void require(bool ok,const char* message) { if(!ok) throw std::system_error(EPROTO,std::generic_category(),message); }
public:
    ActorObjects(ObjectRegistry<Authority>& objects,Native& port):registry(objects),native(port) {}
    std::vector<std::uint8_t> coordinatedProfile() const { return encode(native.info()); }
    bool multiClient() const { return true; }
    // Authority methods translate kernel-global identities into this actor's
    // independent wire namespace. Callers never see or choose actor IDs.
    std::pair<Fd,ObjectRef> create(std::uint32_t ticket,std::uint32_t kind) {
        std::lock_guard<std::mutex> guard(registry.mutex);
        require(native.actor() && registry.nextTicket!=UINT32_MAX,"Global capability actor or ID absent");
        const auto global=registry.nextTicket++;auto result=registry.kernel.create(global,kind);
        typename ObjectRegistry<Authority>::Entry entry{result.second,native.actor(),ticket,{{native.actor(),ticket}},false};
        require(registry.entries.emplace(global,std::move(entry)).second,"Global capability collision");
        result.second.ticket=ticket;return result;
    }
    ObjectRef resolve(int fd,std::uint32_t kind) {
        std::lock_guard<std::mutex> guard(registry.mutex);
        const auto ref=registry.kernel.resolve(fd,kind);const auto found=registry.entries.find(ref.ticket);
        require(found!=registry.entries.end() && !found->second.reaped && ref.cookie && found->second.global.cookie==ref.cookie &&
                ref.kind==kind && found->second.global.kind==kind && native.actor(),"Foreign kernel capability identity mismatch");
        auto& entry=found->second;auto alias=entry.aliases.find(native.actor());
        if(alias==entry.aliases.end()) {
            const auto ticket=native.alias(entry.producer,entry.source,kind);
            local.retainAlias(ObjectRef{kind,ticket,ref.cookie});
            alias=entry.aliases.emplace(native.actor(),ticket).first;
            receipt("BROKER_COORDINATOR_ALIAS donor=%llu recipient=%llu kind=%u sourceTicket=%u recipientTicket=%u cookie=%llu realFd=true\n",
                static_cast<unsigned long long>(entry.producer),static_cast<unsigned long long>(native.actor()),kind,entry.source,ticket,static_cast<unsigned long long>(ref.cookie));
        }
        return {kind,alias->second,ref.cookie};
    }
    ObjectRef reap() {
        std::lock_guard<std::mutex> guard(registry.mutex);registry.collect();
        auto& pending=registry.pending[native.actor()];if(pending.empty()) return {};
        const auto result=pending.front();pending.pop_front();return result;
    }
    wddm_object_stats info() { std::lock_guard<std::mutex> guard(registry.mutex);return registry.kernel.info(); }
    std::pair<Fd,std::uint64_t> resolveSyncFile(int fd) { std::lock_guard<std::mutex> guard(registry.mutex);return registry.kernel.resolveSyncFile(fd); }
    Fd nativeBuffer(int fd,std::uint64_t bytes) { std::lock_guard<std::mutex> guard(registry.mutex);return registry.kernel.nativeBuffer(fd,bytes); }
    std::uint64_t nativeBufferInfo(int fd) { std::lock_guard<std::mutex> guard(registry.mutex);return registry.kernel.nativeBufferInfo(fd); }
    wddm_native_buffer_stats nativeBufferStats() { std::lock_guard<std::mutex> guard(registry.mutex);return registry.kernel.nativeBufferStats(); }
    wddm_native_buffer_caps nativeBufferCaps() { std::lock_guard<std::mutex> guard(registry.mutex);return registry.kernel.nativeBufferCaps(); }
    template<class A> void authorize(const std::vector<std::uint8_t>& packet,const std::vector<Fd>& fds,A&) { local.authorize(packet,fds,*this); }
    void observe(const std::vector<std::uint8_t>& packet,const std::vector<std::uint8_t>& reply) {
        local.observe(packet,reply);
        using namespace driver_bridge;
        Header h{},r{};std::memcpy(&h,packet.data(),sizeof h);std::memcpy(&r,reply.data(),sizeof r);
        if(r.status || reply.size()!=sizeof(Header)+sizeof(Reply) ||
            (h.type!=static_cast<std::uint32_t>(Op::CloseSharedSync) && h.type!=static_cast<std::uint32_t>(Op::CloseSharedResource))) return;
        Reply status{};std::memcpy(&status,reply.data()+sizeof r,sizeof status);if(status.ntstatus) return;
        std::lock_guard<std::mutex> guard(registry.mutex);
        for(auto i=registry.entries.begin();i!=registry.entries.end();++i) {
            const auto alias=i->second.aliases.find(native.actor());
            if(alias==i->second.aliases.end() || alias->second!=h.handle) continue;
            require(i->second.reaped,"Native alias closed before global FD reap");i->second.aliases.erase(alias);
            if(i->second.aliases.empty()) registry.entries.erase(i);
            break;
        }
    }
    template<class A> std::pair<Fd,ObjectRef> create(ObjectRequest request,A&) { return local.create(request,*this); }
    template<class A> ObjectRef resolve(int fd,std::uint32_t kind,A&) { return local.resolve(fd,kind,*this); }
    template<class A> void validate(int fd,std::uint32_t kind,A&) {
        std::lock_guard<std::mutex> guard(registry.mutex);
        const auto ref=registry.kernel.resolve(fd,kind);const auto found=registry.entries.find(ref.ticket);
        require(found!=registry.entries.end() && !found->second.reaped && ref.cookie && found->second.global.cookie==ref.cookie &&
                ref.kind==kind && found->second.global.kind==kind,"Kernel capability validation mismatch");
    }
    template<class A> ObjectRef reap(A&) { return local.reap(*this); }
    template<class A> void syncLineage(int fd,std::uint32_t source,A&) { local.syncLineage(fd,source,*this); }
    template<class N,class A> void drain(N&,A&) {
        native.processExited();local.drain(native,*this);
        {
            std::lock_guard<std::mutex> guard(registry.mutex);
            for(auto i=registry.entries.begin();i!=registry.entries.end();) {
                if(i->second.aliases.erase(native.actor())) require(i->second.reaped,"Actor drained before FD lifetime ended");
                if(i->second.aliases.empty()) i=registry.entries.erase(i);else ++i;
            }
            registry.pending.erase(native.actor());
        }
        native.finish();
    }
    void report() const { local.report(); }
};
} // namespace guest_broker
