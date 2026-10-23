// SPDX-License-Identifier: MIT
// Real Unix FD duplication/last-close; kernel object and native GPU APIs mock.
#include "guest_coordinator.h"
#include <condition_variable>
#include <iostream>
#include <thread>
using namespace guest_broker;
using namespace driver_bridge;
#define CHECK(...) do { if(!(__VA_ARGS__)) throw std::runtime_error("guest coordinator control line "+std::to_string(__LINE__)); } while(0)
struct KernelState {
    struct Record { Fd watch;ObjectRef ref;std::uint64_t identity; };
    std::map<std::uint32_t,Record> records;
    bool corrupt=false;
};
struct KernelMock {
    std::shared_ptr<KernelState> state;
    static std::uint64_t identity(int fd) {
        std::uint64_t value=0;socklen_t bytes=sizeof value;
        if(getsockopt(fd,SOL_SOCKET,SO_COOKIE,&value,&bytes) || bytes!=sizeof value) throw std::system_error(EBADF,std::generic_category());
        return value;
    }
    std::pair<Fd,ObjectRef> create(std::uint32_t ticket,std::uint32_t kind) {
        int sockets[2];CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sockets));
        Fd recipient(sockets[0]);ObjectRef ref{kind,ticket,1000ull+ticket};
        CHECK(state->records.emplace(ticket,KernelState::Record{Fd(sockets[1]),ref,identity(recipient.get())}).second);
        return {std::move(recipient),ref};
    }
    ObjectRef resolve(int fd,std::uint32_t kind) {
        const auto cookie=identity(fd);
        for(const auto& entry:state->records) if(entry.second.identity==cookie) {
            if(kind!=entry.second.ref.kind) throw std::system_error(EINVAL,std::generic_category());
            auto ref=entry.second.ref;if(state->corrupt) ++ref.cookie;return ref;
        }
        throw std::system_error(EBADF,std::generic_category());
    }
    ObjectRef reap() {
        for(auto i=state->records.begin();i!=state->records.end();++i) {
            pollfd event{i->second.watch.get(),POLLIN,0};CHECK(poll(&event,1,0)>=0);
            if(event.revents&POLLHUP) { const auto ref=i->second.ref;state->records.erase(i);return ref; }
        }
        return {};
    }
};
struct NativeMock {
    std::uint64_t id=0;
    std::uint32_t nextAlias=100;
    std::atomic<unsigned> aliases{0},closed{0};
    std::atomic<bool> exited{false},finished{false};
    std::condition_variable ready;
    std::mutex mutex;
    std::uint64_t actor() const { return id; }
    cg::Info info() const { return {}; }
    std::uint32_t alias(std::uint64_t donor,std::uint32_t ticket,std::uint32_t kind) {
        CHECK(donor && donor!=id && ticket==3 && (kind==WDDM_OBJECT_RESOURCE || kind==WDDM_OBJECT_SYNC));++aliases;return nextAlias++;
    }
    std::vector<std::uint8_t> exchange(const std::vector<std::uint8_t>& packet) {
        CHECK(exited);Header h{};std::memcpy(&h,packet.data(),sizeof h);
        CHECK(h.type==static_cast<std::uint32_t>(Op::CloseSharedResource) || h.type==static_cast<std::uint32_t>(Op::CloseSharedSync));
        ++closed;auto reply=encode(h);const auto status=encode(Reply{});reply.insert(reply.end(),status.begin(),status.end());return reply;
    }
    void processExited() { std::lock_guard<std::mutex> guard(mutex);exited=true;ready.notify_all(); }
    void finish() { finished=true; }
};
static std::vector<std::uint8_t> packet(Op op,std::uint32_t handle) { return encode(Header{static_cast<std::uint32_t>(op),handle,0,0}); }
template<class Store> static void exported(Store& store,Op op,std::uint32_t ticket) {
    auto reply=packet(op,ticket);const auto result=encode(Reply{});reply.insert(reply.end(),result.begin(),result.end());store.observe(packet(op,1),reply);
}
int main() {
    CHECK(wddm_aperture_valid_caps({1,0,WDDM_APERTURE_CPU_LIMIT,{0,0}}));
    CHECK(wddm_aperture_valid_caps({1,3,WDDM_APERTURE_DESKTOP_CPU_LIMIT,{0,0}}));
    CHECK(wddm_aperture_valid_caps({1,7,WDDM_APERTURE_MULTIAPP_CPU_LIMIT,{0,0}}));
    CHECK(!wddm_aperture_valid_caps({1,3,WDDM_APERTURE_MULTIAPP_CPU_LIMIT,{0,0}}));
    CHECK(!wddm_aperture_valid_caps({1,7,WDDM_APERTURE_DESKTOP_CPU_LIMIT,{0,0}}));
    CHECK(!wddm_aperture_valid_caps({1,4,WDDM_APERTURE_MULTIAPP_CPU_LIMIT,{0,0}}));
    CHECK(!wddm_aperture_valid_caps({1,15,WDDM_APERTURE_MULTIAPP_CPU_LIMIT,{0,0}}));
    CHECK(wddm_aperture_valid_coordinator_caps({1,0,256,0}));
    CHECK(wddm_aperture_valid_coordinator_caps({1,0,512,0}));
    CHECK(wddm_aperture_valid_coordinator_caps({1,1,768,0}));
    CHECK(wddm_aperture_valid_coordinator_caps({1,1,1536,0}));
    CHECK(!wddm_aperture_valid_coordinator_caps({1,0,1536,0}));
    CHECK(!wddm_aperture_valid_coordinator_caps({1,1,1537,0}));
    CHECK(!wddm_aperture_valid_coordinator_caps({2,1,768,0}));
    CHECK(!wddm_aperture_valid_coordinator_caps({1,2,768,0}));
    CHECK(!wddm_aperture_valid_coordinator_caps({1,1,512,0}));
    CHECK(!wddm_aperture_valid_coordinator_caps({1,1,768,1}));
    {
        CoordinatorPort unavailable("/no-native-coordinator-for-metadata-test");
        ActorPort metadata(unavailable,false,getpid());
        metadata.processExited();metadata.finish();CHECK(!metadata.actor());
    }
    for(const std::uint32_t kind:{WDDM_OBJECT_SYNC,WDDM_OBJECT_RESOURCE}) {
        auto state=std::make_shared<KernelState>();ObjectRegistry<KernelMock> registry(KernelMock{state});
        NativeMock first,second;first.id=1;second.id=2;
        ActorObjects source(registry,first),recipient(registry,second);
        const auto exporting=kind==WDDM_OBJECT_SYNC ? Op::ExportSync : Op::ExportResource;
        const auto importing=kind==WDDM_OBJECT_SYNC ? Op::ImportSync : Op::ImportResource;
        const auto closing=kind==WDDM_OBJECT_SYNC ? Op::CloseSharedSync : Op::CloseSharedResource;
        exported(source,exporting,3);exported(recipient,exporting,3);
        auto original=source.create(ObjectRequest{kind,3,0},source);
        auto own=recipient.create(ObjectRequest{kind,3,0},recipient);
        CHECK(state->records.size()==2 && original.second.ticket==3 && own.second.ticket==3 && original.second.cookie!=own.second.cookie);
        Fd inherited(fcntl(original.first.get(),F_DUPFD_CLOEXEC,0));CHECK(inherited.get()>=0);
        original.first=Fd();
        bool bad=false;state->corrupt=true;
        try { (void)recipient.resolve(inherited.get(),kind,recipient); } catch(const std::system_error&) { bad=true; }
        CHECK(bad && !second.aliases);state->corrupt=false;
        std::exception_ptr failed;
        std::thread producer([&] { try { source.drain(first,source); } catch(...) { failed=std::current_exception(); } });
        {
            std::unique_lock<std::mutex> guard(first.mutex);CHECK(first.ready.wait_for(guard,std::chrono::seconds(2),[&]{return first.exited.load();}));
        }
        CHECK(!first.closed && !first.finished); // Real recipient duplicate keeps source capability alive.
        recipient.validate(inherited.get(),kind,recipient);CHECK(!second.aliases);
        const auto foreign=recipient.resolve(inherited.get(),kind,recipient);
        CHECK(foreign.ticket==100 && foreign.cookie==original.second.cookie && second.aliases==1);
        CHECK(recipient.resolve(inherited.get(),kind,recipient).ticket==100 && second.aliases==1);
        std::vector<Fd> auth;auth.emplace_back(fcntl(inherited.get(),F_DUPFD_CLOEXEC,0));
        recipient.authorize(packet(importing,foreign.ticket),auth,recipient);
        bad=false;try { recipient.authorize(packet(importing,3),auth,recipient); } catch(const std::system_error&) { bad=true; }CHECK(bad);
        bad=false;try { recipient.authorize(packet(closing,foreign.ticket),{},recipient); } catch(const std::system_error&) { bad=true; }CHECK(bad);
        auth.clear();inherited=Fd();producer.join();if(failed) std::rethrow_exception(failed);
        CHECK(first.closed==1 && first.finished && !second.closed); // Recipient alias owns a separate native reference.
        own.first=Fd();recipient.drain(second,recipient);
        CHECK(second.closed==2 && second.finished && state->records.empty());
    }
    std::cout<<"GUEST_COORDINATOR realUnixFdLifetime=true collidingLocalTickets=true kernelGlobalTicketsUnique=true forgedCookieRejected=true foreignTicketRejected=true aliasCached=true producerExitRetained=true nativeAndKernel=mock gpu=false\n";
}
