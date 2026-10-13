// SPDX-License-Identifier: MIT
#include "guest_broker_server.h"
#include <sys/stat.h>
#include <sys/un.h>
#include <limits>

int main(int argc,char** argv) {
    using namespace guest_broker;
    try {
        if (argc != 6 || getuid() || geteuid()) throw std::runtime_error("Broker requires root: SOCKET PORT UID GID READY_FD");
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
            chown(argv[1],uid,gid) || chmod(argv[1],0600) || listen(listener.get(),1))
            throw std::system_error(errno,std::generic_category(),"Broker listener setup");
        Fd notification(static_cast<int>(ready)); const char marker='R';
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
