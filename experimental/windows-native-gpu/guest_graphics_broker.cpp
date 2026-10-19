// SPDX-License-Identifier: MIT
#include "guest_broker_server.h"
#include "guest_coordinator.h"
#include <sys/stat.h>
#include <sys/un.h>
#include <limits>
#include <thread>

static int coordinated(guest_broker::Fd& listener,const char* path,unsigned uid,unsigned gid,guest_broker::Fd notification,guest_broker::Fd primaryPipe) {
    using namespace guest_broker;
    CoordinatorPort controller(path);Kernel layout;
    const auto info=layout.info();const auto caps=layout.capacity();
    const auto references=std::getenv("WDDM_BRIDGE_COORDINATED_REFERENCES");
    if(references && std::strcmp(references,"1")) throw std::runtime_error("Invalid coordinated reference opt-in");
    controller.initialize(info,caps,layout.coordinatedCapacity(),info.fence_slots==512 ? layout.desktopFenceCapacity() : wddm_aperture_desktop_fences{},
        info.allocation_slots==1024 ? layout.desktopSlotCapacity() : wddm_aperture_desktop_slots{},references!=nullptr);layout.close();
    if(const auto retirement=std::getenv("WDDM_BRIDGE_COORDINATED_RETIREMENT")) {
        if(std::strcmp(retirement,"1") || !references) throw std::runtime_error("Invalid coordinated live retirement opt-in");
        if(mkdir("/run/wddm-retired",0755) && errno!=EEXIST) throw std::runtime_error("Retirement witness directory");
        struct stat status{};
        if(lstat("/run/wddm-retired",&status) || !S_ISDIR(status.st_mode) || status.st_uid || (status.st_mode&0022))
            throw std::runtime_error("Untrusted retirement witness directory");
        Fd directory(open("/run/wddm-retired",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
        if(directory.get()<0 || fchmod(directory.get(),0755)) throw std::runtime_error("Retirement witness directory mode");
        controller.beginLiveRetirement();
    }
    const char ready='R';if(write(notification.get(),&ready,1)!=1) throw std::runtime_error("Coordinator readiness failed");
    notification=Fd();
    pollfd trusted{primaryPipe.get(),POLLIN,0};
    if(poll(&trusted,1,10000)!=1) throw std::runtime_error("Coordinator trusted primary identity timeout");
    std::uint64_t primary=0;
    if(read(primaryPipe.get(),&primary,sizeof primary)!=sizeof primary || !primary || primary>INT32_MAX)
        throw std::runtime_error("Coordinator primary identity missing");
    primaryPipe=Fd();
    ObjectRegistry<> objects;
    std::atomic<unsigned> active{0},failures{0};std::atomic<bool> primaryFinished{false};
    struct Workers {
        struct Task { std::thread thread;std::atomic<bool> done{false}; };
        std::vector<std::unique_ptr<Task>> tasks;
        Workers() { tasks.reserve(ar::MaxActors); }
        ~Workers() { for(auto& task:tasks) if(task->thread.joinable()) task->thread.join(); }
    } workers;
    // This diagnostic process is VM-owned. A stalled actor is a failed run;
    // the Windows owner reaps QEMU before releasing retained native objects.
    alarm(info.fence_slots==512 ? 240 : 120);
    bool primaryAccepted=false;
    unsigned connections=0;
    while(!primaryFinished || active) {
        for(auto i=workers.tasks.begin();i!=workers.tasks.end();) {
            if((*i)->done) { (*i)->thread.join();i=workers.tasks.erase(i); } else ++i;
        }
        if(primaryFinished || workers.tasks.size()>=ar::MaxActors) { poll(nullptr,0,10);continue; }
        pollfd incoming{listener.get(),POLLIN,0};const auto readyCount=poll(&incoming,1,100);
        if(readyCount<0 && errno==EINTR) continue;
        if(readyCount<0 || (incoming.revents&(POLLERR|POLLNVAL))) throw std::runtime_error("Coordinator listener poll failed");
        if(!readyCount) continue;
        Fd channel(accept4(listener.get(),nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK));
        if(channel.get()<0) { if(errno==EINTR || errno==EAGAIN) continue;throw std::runtime_error("Coordinator accept failed"); }
        if(connections>=64) { receipt("BROKER_COORDINATOR_PEER denied=true lifetimeConnectionLimit=64\n");continue; }
        std::unique_ptr<Peer> peer;
        try { peer=std::make_unique<Peer>(std::move(channel),uid,gid); }
        catch(const std::exception& error) { receipt("BROKER_COORDINATOR_PEER denied=true reason=%s\n",error.what());continue; }
        const auto isPrimary=static_cast<std::uint64_t>(peer->pid())==primary;
        if((!primaryAccepted && !isPrimary) || (isPrimary && primaryAccepted)) {
            receipt("BROKER_COORDINATOR_PEER denied=true primaryPidMismatch=true\n");continue;
        }
        primaryAccepted|=isPrimary;++connections;
        char path[64],exe[4096];snprintf(path,sizeof path,"/proc/%d/exe",peer->pid());
        const auto bytes=readlink(path,exe,sizeof exe);
        std::string hex;
        if(bytes>0) { constexpr char digits[]="0123456789abcdef";for(ssize_t i=0;i<bytes;++i) { const auto c=static_cast<unsigned char>(exe[i]);hex+=digits[c>>4];hex+=digits[c&15]; } }
        receipt("BROKER_COORDINATOR_PEER pid=%d primary=%s exePathHex=%s pidfdAuthenticated=true\n",peer->pid(),isPrimary ? "true" : "false",hex.c_str());
        ++active;
        auto task=std::make_unique<Workers::Task>();auto* completed=task.get();
        task->thread=std::thread([&,peer=std::move(peer),isPrimary,completed]() mutable {
            int result=1;
            try { ActorPort native(controller,isPrimary,peer->pid());ActorObjects store(objects,native);Kernel kernel;
                  result=serveWithObjects(*peer,native,kernel,store,store); }
            catch(const std::exception& error) { std::fprintf(stderr,"BROKER_COORDINATOR_ACTOR failed=true pid=%d reason=%s\n",peer->pid(),error.what()); }
            if(result) ++failures;
            if(isPrimary) primaryFinished=true;
            completed->done=true;
            --active;
        });
        workers.tasks.push_back(std::move(task));
    }
    listener=Fd();
    for(auto& task:workers.tasks) task->thread.join();
    alarm(0);
    std::fprintf(stderr,"BROKER_COORDINATOR_EXIT connections=%u failed=%u primaryPidFromRoot=true gpuProof=false\n",connections,failures.load());
    return failures ? 1 : 0;
}

int main(int argc,char** argv) {
    using namespace guest_broker;
    try {
        const bool group=argc==8 && std::string(argv[6])=="--coordinator";
        if ((argc != 6 && !group) || getuid() || geteuid()) throw std::runtime_error("Broker requires root: SOCKET PORT UID GID READY_FD [--coordinator PRIMARY_PID_FD]");
        const auto number=[](const char* value) {
            if (!value || !*value || std::strspn(value,"0123456789") != std::strlen(value)) throw std::runtime_error("Invalid broker numeric argument");
            const auto n=std::stoul(value); if (n >= std::numeric_limits<unsigned>::max()) throw std::runtime_error("Broker numeric argument overflow"); return static_cast<unsigned>(n);
        };
        const auto uid=number(argv[3]),gid=number(argv[4]),ready=number(argv[5]);
        if (!uid || !gid || ready < 3 || ready > static_cast<unsigned>(std::numeric_limits<int>::max())) throw std::runtime_error("Broker requires an ordinary client and readiness pipe");
        sockaddr_un address{}; address.sun_family=AF_UNIX;
        if (argv[1][0] != '/' || std::strlen(argv[1]) >= sizeof(address.sun_path) || argv[2][0] != '/')
            throw std::runtime_error("Broker paths must be bounded and absolute");
        std::memcpy(address.sun_path,argv[1],std::strlen(argv[1])+1);
        Fd listener(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0));
        const int credentials=1; umask(0077);
        if (listener.get() < 0 || setsockopt(listener.get(),SOL_SOCKET,SO_PASSCRED,&credentials,sizeof(credentials)) ||
            bind(listener.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address)) ||
            chown(argv[1],uid,gid) || chmod(argv[1],0600) || listen(listener.get(),group ? 4 : 1))
            throw std::system_error(errno,std::generic_category(),"Broker listener setup");
        Fd notification(static_cast<int>(ready));
        if(group) {
            const auto descriptor=number(argv[7]);struct stat status{};
            if(descriptor<3 || descriptor==ready || descriptor>INT32_MAX || fstat(static_cast<int>(descriptor),&status) || !S_ISFIFO(status.st_mode) || status.st_uid)
                throw std::runtime_error("Coordinator requires a distinct inherited root pipe for the primary PID");
            return coordinated(listener,argv[2],uid,gid,std::move(notification),Fd(static_cast<int>(descriptor)));
        }
        const char marker='R';
        if (write(notification.get(),&marker,1) != 1) throw std::runtime_error("Broker readiness signal failed");
        notification=Fd();
        Fd accepted;
        for (;;) {
            const auto fd=accept4(listener.get(),nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK);
            if (fd >= 0) { accepted=Fd(fd); break; }
            if (errno != EINTR) throw std::system_error(errno,std::generic_category(),"Broker accept failed");
        }
        listener=Fd(); // One process per native GPU worker until coordinator integration.
        Peer peer(std::move(accepted),uid,gid);
        std::fprintf(stderr,"BROKER_ROUTE_START protocol=%u clientPid=%d clientUid=%u nativeWorkerClients=1 kernelControllersClient=false\n",Version,peer.pid(),uid);
        Port native(argv[2]); Kernel kernel;
        return serve(peer,native,kernel);
    } catch (const std::exception& error) {
        std::fprintf(stderr,"BROKER_ROUTE failed=true startup=true reason=%s\n",error.what()); return 1;
    }
}
