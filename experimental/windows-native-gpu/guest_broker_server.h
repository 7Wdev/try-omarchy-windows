// SPDX-License-Identifier: MIT
// Single authenticated GPU client; multiple native workers remain separate work.
#pragma once
#include "guest_broker_views.h"
#include "guest_broker_objects.h"
#include <cstdio>
#include <sys/syscall.h>

namespace guest_broker {
class Kernel {
    Fd control;
    void open() {
        if (control.get() >= 0) return;
        control = Fd(static_cast<int>(syscall(SYS_openat,AT_FDCWD,"/dev/wddm-aperture",O_RDWR|O_CLOEXEC,0)));
        if (control.get() < 0) throw std::system_error(errno,std::generic_category(),"Broker kernel controller open");
    }
public:
    void close() { control=Fd(); }
    wddm_aperture_info info() {
        open(); wddm_aperture_info value{}; value.version=WDDM_APERTURE_VERSION;
        if (syscall(SYS_ioctl,control.get(),WDDM_APERTURE_INFO,&value))
            throw std::system_error(errno,std::generic_category(),"Broker kernel layout");
        if (value.version != WDDM_APERTURE_VERSION || (value.fence_slots != 64 && value.fence_slots != 128) ||
            !driver_bridge::validVendorCpuSlots(value.allocation_slots) || value.cpu_slot_bytes != 1048576 ||
            value.fence_bytes != value.fence_slots*4096ull || value.allocation_bytes != value.allocation_slots*1048576ull ||
            value.active_leases > WDDM_APERTURE_MAX_LEASES || value.active_cpu_bytes > WDDM_APERTURE_CPU_LIMIT)
            throw std::system_error(EPROTO,std::generic_category(),"Broker kernel layout mismatch");
        return value;
    }
    std::pair<Fd,std::uint64_t> acquire(ViewRequest view) {
        const auto layout=info();
        const auto limit=view.kind == WDDM_APERTURE_FENCE ? layout.fence_bytes : layout.allocation_bytes;
        if (!view.bytes || view.offset >= limit || view.bytes > limit-view.offset)
            throw std::system_error(ERANGE,std::generic_category(),"Broker view exceeds configured aperture");
        wddm_aperture_create request{}; request.version=WDDM_APERTURE_VERSION;
        request.kind=view.kind; request.offset=view.offset; request.bytes=view.bytes; request.host_generation=view.generation;
        request.flags=view.kind == WDDM_APERTURE_ALLOCATION ? WDDM_APERTURE_WRITE : 0;
        if (syscall(SYS_ioctl,control.get(),WDDM_APERTURE_CREATE,&request))
            throw std::system_error(errno,std::generic_category(),"Broker kernel view create");
        Fd fd(request.fd);
        if (fd.get() < 0 || !request.cookie) throw std::system_error(EPROTO,std::generic_category(),"Broker kernel view identity");
        return {std::move(fd),request.cookie};
    }
    void release(std::uint64_t cookie) {
        wddm_aperture_revoke request{WDDM_APERTURE_VERSION,0,cookie};
        if (syscall(SYS_ioctl,control.get(),WDDM_APERTURE_REVOKE,&request))
            throw std::system_error(errno,std::generic_category(),"Broker kernel view revoke");
    }
};
class Port {
    const std::string path;
    Fd descriptor;
    void transfer(void* pointer, std::size_t bytes, bool reading,
                  std::chrono::steady_clock::time_point deadline) {
        auto* data=static_cast<std::uint8_t*>(pointer);
        while (bytes) {
            const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
            if (ms <= 0) throw std::system_error(ETIMEDOUT,std::generic_category(),"Broker native transfer deadline");
            pollfd event{descriptor.get(),static_cast<short>(reading ? POLLIN : POLLOUT),0};
            const auto ready=poll(&event,1,static_cast<int>(ms));
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0 || (event.revents & (POLLERR|POLLNVAL)))
                throw std::system_error(ready < 0 ? errno : EIO,std::generic_category(),"Broker native port unavailable");
            const auto count=reading ? read(descriptor.get(),data,bytes) : write(descriptor.get(),data,bytes);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) throw std::system_error(EPIPE,std::generic_category(),"Broker native port disconnected");
            data+=count; bytes-=static_cast<std::size_t>(count);
        }
    }
public:
    explicit Port(std::string value):path(std::move(value)) {}
    std::vector<std::uint8_t> exchange(const std::vector<std::uint8_t>& packet) {
        if (descriptor.get() < 0) {
            descriptor=Fd(static_cast<int>(syscall(SYS_openat,AT_FDCWD,path.c_str(),O_RDWR|O_CLOEXEC|O_NONBLOCK,0)));
            if (descriptor.get() < 0) throw std::system_error(errno,std::generic_category(),"Broker native port open");
        }
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        auto bytes=static_cast<std::uint32_t>(packet.size());
        transfer(&bytes,sizeof(bytes),false,deadline);
        transfer(const_cast<std::uint8_t*>(packet.data()),packet.size(),false,deadline);
        transfer(&bytes,sizeof(bytes),true,deadline);
        if (bytes < sizeof(driver_bridge::Header) || bytes > native_gpu::MaxPacket)
            throw std::system_error(EPROTO,std::generic_category(),"Broker native reply bounds");
        std::vector<std::uint8_t> reply(bytes); transfer(reply.data(),reply.size(),true,deadline); return reply;
    }
};
template<class Native,class Authority,class ObjectAuthority> int serve(Peer& peer, Native& native, Authority& kernel, ObjectAuthority& objectKernel) {
    Views views;
    ObjectStore objects;
    std::uint64_t calls=0,created=0,released=0,denied=0,recoverable=0;
    bool failed=false;
    try {
        while (peer.connected()) {
            // Descendants may retain an inherited socket after the original
            // producer exits. Its authenticated pidfd, rather than EOF, owns
            // the memory lifetime and starts capability retention cleanup.
            if (peer.processExited()) { peer.disconnect(); break; }
            auto received=peer.receive();
            if (received.state == ReceiveState::Disconnected) break;
            if (received.state == ReceiveState::Idle) { peer.waitReadable(100); continue; }
            const auto& message=received.message;
            bool expectedObjectError=false;
            try {
                const auto operation=message.frame.operation;
                if (operation!=Operation::DriverCall && operation!=Operation::ResolveObject && !message.fds.empty())
                    throw std::system_error(EPROTO,std::generic_category(),"Unsolicited broker descriptors");
                if (operation == Operation::Hello) {
                    if (!message.payload.empty()) throw std::system_error(EINVAL,std::generic_category(),"Broker hello payload");
                    peer.send(operation,message.frame.sequence,encode(kernel.info()));
                } else if (operation == Operation::DriverCall) {
                    views.approve(message.payload);
                    objects.authorize(message.payload,message.fds,objectKernel);
                    auto reply=native.exchange(message.payload); ++calls;
                    views.observe(message.payload,reply);
                    objects.observe(message.payload,reply);
                    peer.send(operation,message.frame.sequence,reply);
                } else if (operation == Operation::AcquireView) {
                    auto& grant=views.acquire(decode<ViewRequest>(message.payload));
                    auto result=kernel.acquire(grant.view); grant.cookie=result.second; ++created;
                    peer.send(operation,message.frame.sequence,encode(ViewReply{grant.cookie}),{result.first.get()});
                } else if (operation == Operation::ReleaseView) {
                    auto& grant=views.cookie(decode<ViewReply>(message.payload).cookie);
                    kernel.release(grant.cookie); grant.cookie=0; ++released;
                    peer.send(operation,message.frame.sequence,{});
                } else if (operation == Operation::ObjectInfo) {
                    if (!message.payload.empty()) throw std::system_error(EPROTO,std::generic_category(),"Object info payload");
                    peer.send(operation,message.frame.sequence,encode(objectKernel.info()));
                } else if (operation == Operation::CreateObject) {
                    const auto request=decode<ObjectRequest>(message.payload);
                    expectedObjectError=true;
                    auto result=objects.create(request,objectKernel);
                    peer.send(operation,message.frame.sequence,encode(result.second),{result.first.get()});
                } else if (operation == Operation::ResolveObject) {
                    const auto request=decode<ObjectRequest>(message.payload);
                    if (message.fds.size()!=1 || request.ticket || request.reserved ||
                        (request.kind!=WDDM_OBJECT_SYNC && request.kind!=WDDM_OBJECT_RESOURCE))
                        throw std::system_error(EPROTO,std::generic_category(),"Invalid object resolution request");
                    expectedObjectError=true;
                    const auto result=objects.resolve(message.fds[0].get(),request.kind,objectKernel);
                    peer.send(operation,message.frame.sequence,encode(result));
                } else if (operation == Operation::ReapObject) {
                    if (!message.payload.empty()) throw std::system_error(EPROTO,std::generic_category(),"Object reap payload");
                    peer.send(operation,message.frame.sequence,encode(objects.reap(objectKernel)));
                } else throw std::system_error(EOPNOTSUPP,std::generic_category(),"Broker operation not implemented");
            } catch (const std::system_error& error) {
                if (expectedObjectError && recoverableObjectError(message.frame.operation,error.code().value())) {
                    ++recoverable;
                    peer.send(message.frame.operation,message.frame.sequence,{}, {},error.code().value());
                    continue;
                }
                ++denied; failed=true;
                std::fprintf(stderr,"BROKER_ROUTE rejected=true errno=%d operation=%u reason=%s\n",error.code().value(),static_cast<unsigned>(message.frame.operation),error.what());
                peer.send(message.frame.operation,message.frame.sequence,{}, {},error.code().value());
                peer.disconnect();
            }
        }
    } catch (const std::exception& error) {
        failed=true; peer.disconnect(); std::fprintf(stderr,"BROKER_ROUTE failed=true reason=%s\n",error.what());
    }
    const auto retained=views.active();
    // This is deliberately outside the channel loop. Neither EOF nor any
    // rejection destroys the native transport or privileged kernel controller.
    for (;;) {
        try { if (peer.processExited()) break; }
        catch (const std::exception& error) {
            failed=true; std::fprintf(stderr,"BROKER_ROUTE failed=true processLifetimeUnknown=true reason=%s\n",error.what());
        }
        poll(nullptr,0,10);
    }
    // CPU mappings end with their producer. Shared native references use a
    // different controller and survive until the last recipient FD closes.
    kernel.close();
    try { objects.drain(native,objectKernel); }
    catch (const std::exception& error) {
        failed=true; std::fprintf(stderr,"BROKER_OBJECT failed=true producerExited=true reason=%s\n",error.what());
    }
    objects.report();
    if (recoverable) std::fprintf(stderr,"BROKER_OBJECT recoverableErrors=%llu\n",static_cast<unsigned long long>(recoverable));
    std::fprintf(stderr,"BROKER_ROUTE_EXIT processExited=true retainedViews=%zu nativeCalls=%llu viewsCreated=%llu viewsReleased=%llu rejected=%llu multiClient=false\n",
                 retained,static_cast<unsigned long long>(calls),static_cast<unsigned long long>(created),
                 static_cast<unsigned long long>(released),static_cast<unsigned long long>(denied));
    return failed ? 1 : 0;
}
template<class Native,class Authority> int serve(Peer& peer, Native& native, Authority& kernel) {
    KernelObjects objectKernel;
    return serve(peer,native,kernel,objectKernel);
}
} // namespace guest_broker
