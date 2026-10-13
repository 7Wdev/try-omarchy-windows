// SPDX-License-Identifier: MIT
// Runs only in the owned QEMU guest. Does not read an unbacked PCI slice.
#define _GNU_SOURCE
#include "wddm_aperture.h"
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

_Static_assert(sizeof(struct wddm_aperture_info) == 72, "fixed info ABI");
_Static_assert(sizeof(struct wddm_aperture_create) == 64, "fixed lease ABI");
_Static_assert(sizeof(struct wddm_aperture_revoke) == 16, "fixed revoke ABI");
static unsigned checks;
#define REQUIRE(test) do { if (!(test)) { \
  fprintf(stderr,"APERTURE_CONTRACT_FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); \
} ++checks; } while (0)
static struct wddm_aperture_info info(int control)
{
    struct wddm_aperture_info result = {.version=WDDM_APERTURE_VERSION};
    REQUIRE(!ioctl(control,WDDM_APERTURE_INFO,&result));
    return result;
}
static struct wddm_aperture_create fence(void)
{
    struct wddm_aperture_create result = {.version=WDDM_APERTURE_VERSION,
        .kind=WDDM_APERTURE_FENCE,.bytes=4096};
    return result;
}
static void revoke_lease(int control, uint64_t cookie)
{
    struct wddm_aperture_revoke arg = {.version=WDDM_APERTURE_VERSION,.cookie=cookie};
    REQUIRE(!ioctl(control,WDDM_APERTURE_REVOKE,&arg));
}
static void bus_signal(int signal_number)
{
    _exit(signal_number == SIGBUS ? 74 : 75);
}
#include "object_contract.h"
#include "native_sync_contract.h"
static void transfer_and_revoke_lease(int control)
{
    int sockets[2], status;
    struct wddm_aperture_create arg = fence();
    REQUIRE(!ioctl(control,WDDM_APERTURE_CREATE,&arg));
    REQUIRE(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sockets));
    pid_t child = fork(); REQUIRE(child >= 0);
    if (!child) {
        close(sockets[0]); close(arg.fd);
        REQUIRE(!setgroups(0,NULL) && !setgid(65534) && !setuid(65534));
        char byte;
        struct iovec vec = {.iov_base=&byte,.iov_len=1};
        union { struct cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(int))]; } buffer;
        memset(&buffer,0,sizeof(buffer));
        struct msghdr message = {.msg_iov=&vec,.msg_iovlen=1,
            .msg_control=buffer.bytes,.msg_controllen=sizeof(buffer)};
        REQUIRE(recvmsg(sockets[1],&message,0)==1);
        struct cmsghdr* header=CMSG_FIRSTHDR(&message);
        REQUIRE(header && header->cmsg_level==SOL_SOCKET && header->cmsg_type==SCM_RIGHTS &&
            header->cmsg_len==CMSG_LEN(sizeof(int)) && !(message.msg_flags&MSG_CTRUNC));
        int fd; memcpy(&fd,CMSG_DATA(header),sizeof(fd));
        struct wddm_aperture_create denied=fence();
        errno=0; REQUIRE(ioctl(control,WDDM_APERTURE_CREATE,&denied)==-1 && errno==EPERM);
        struct wddm_object_create denied_object={.version=1,.kind=WDDM_OBJECT_SYNC,.ticket=5000};
        errno=0; REQUIRE(ioctl(control,WDDM_OBJECT_CREATE,&denied_object)==-1 && errno==EPERM);
        puts("OBJECT_CONTRACT_UNPRIVILEGED_CREATE denied=true");
        void* view=mmap(NULL,4096,PROT_READ,MAP_SHARED,fd,0);
        REQUIRE(view!=MAP_FAILED);
        errno=0; REQUIRE(mprotect(view,4096,PROT_READ|PROT_WRITE)==-1 && errno==EACCES);
        errno=0; REQUIRE(mprotect(view,4096,PROT_READ|PROT_EXEC)==-1 && errno==EACCES);
        REQUIRE(write(sockets[1],"R",1)==1);
        REQUIRE(read(sockets[1],&byte,1)==1 && byte=='X');
        errno=0; REQUIRE(mmap(NULL,4096,PROT_READ,MAP_SHARED,fd,0)==MAP_FAILED && errno==ENODEV);
        struct sigaction action={0}; action.sa_handler=bus_signal;
        REQUIRE(!sigaction(SIGBUS,&action,NULL));
        /* Revoke must remove PTEs in the recipient's separate address space.
         * If it fails, this would incorrectly reach the unbacked PCI page. */
        volatile uint64_t observed=*(volatile uint64_t*)view; (void)observed;
        _exit(76);
    }
    close(sockets[1]);
    char byte='F'; struct iovec vec={.iov_base=&byte,.iov_len=1};
    union { struct cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(int))]; } buffer;
    memset(&buffer,0,sizeof(buffer));
    struct msghdr message={.msg_iov=&vec,.msg_iovlen=1,.msg_control=buffer.bytes,.msg_controllen=sizeof(buffer)};
    struct cmsghdr* header=CMSG_FIRSTHDR(&message);
    header->cmsg_level=SOL_SOCKET; header->cmsg_type=SCM_RIGHTS; header->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header),&arg.fd,sizeof(arg.fd));
    REQUIRE(sendmsg(sockets[0],&message,0)==1);
    REQUIRE(read(sockets[0],&byte,1)==1 && byte=='R');
    revoke_lease(control,arg.cookie);
    REQUIRE(write(sockets[0],"X",1)==1);
    REQUIRE(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==74);
    close(arg.fd); close(sockets[0]);
    puts("APERTURE_CONTRACT fdTransfer=true unprivilegedMap=true readOnly=true revokeSigbus=true");
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    alarm(20);
    int control=open("/dev/wddm-aperture",O_RDWR|O_CLOEXEC);
    REQUIRE(control>=0);
    struct wddm_aperture_info layout=info(control);
    REQUIRE(layout.version==1 && (layout.fence_slots==64 || layout.fence_slots==128) &&
        layout.fence_bytes==(uint64_t)layout.fence_slots*4096 &&
        layout.allocation_slots>=16 && layout.allocation_slots<=256 &&
        !(layout.allocation_slots&(layout.allocation_slots-1)) &&
        layout.cpu_slot_bytes==1048576 &&
        layout.allocation_bytes==(uint64_t)layout.allocation_slots*layout.cpu_slot_bytes &&
        !layout.active_leases && !layout.active_cpu_bytes);
    for (unsigned n=0;n<19;++n) {
        struct wddm_aperture_create bad=fence();
        switch (n) {
        case 0: bad.version=0; break;
        case 1: bad.version=2; break;
        case 2: bad.kind=0; break;
        case 3: bad.kind=3; break;
        case 4: bad.offset=1; break;
        case 5: bad.bytes=0; break;
        case 6: bad.bytes=4095; break;
        case 7: bad.bytes=8192; break;
        case 8: bad.offset=layout.fence_bytes; break;
        case 9: bad.offset=UINT64_MAX-4095; break;
        case 10: bad.flags=1; break;
        case 11: bad.flags=0x80000000; break;
        case 12: bad.host_generation=1; break;
        case 13: bad.cookie=1; break;
        case 14: bad.fd=-1; break;
        case 15: bad.reserved[0]=1; break;
        case 16: bad.reserved[1]=1; break;
        case 17: bad.kind=2; bad.flags=1; break;
        case 18: bad.kind=2; bad.flags=1; bad.host_generation=UINT64_MAX; break;
        }
        errno=0; REQUIRE(ioctl(control,WDDM_APERTURE_CREATE,&bad)==-1 && errno==EINVAL);
    }
    struct wddm_aperture_create arg=fence();
    REQUIRE(!ioctl(control,WDDM_APERTURE_CREATE,&arg) && arg.cookie && arg.fd>=0);
    REQUIRE(fcntl(arg.fd,F_GETFD)&FD_CLOEXEC);
    errno=0; REQUIRE(mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,arg.fd,0)==MAP_FAILED && errno==EACCES);
    /* The anon-inode noexec mount can reject before the driver's mmap hook. */
    errno=0; REQUIRE(mmap(NULL,4096,PROT_READ|PROT_EXEC,MAP_SHARED,arg.fd,0)==MAP_FAILED &&
        (errno==EACCES || errno==EPERM));
    errno=0; REQUIRE(mmap(NULL,4096,PROT_READ,MAP_PRIVATE,arg.fd,0)==MAP_FAILED && errno==EINVAL);
    errno=0; REQUIRE(mmap(NULL,8192,PROT_READ,MAP_SHARED,arg.fd,0)==MAP_FAILED && errno==EINVAL);
    errno=0; REQUIRE(mmap(NULL,4096,PROT_READ,MAP_SHARED,arg.fd,4096)==MAP_FAILED && errno==EINVAL);
    void* view=mmap(NULL,4096,PROT_READ,MAP_SHARED,arg.fd,0); REQUIRE(view!=MAP_FAILED);
    errno=0; REQUIRE(mremap(view,4096,8192,MREMAP_MAYMOVE)==MAP_FAILED);
    int foreign=open("/dev/wddm-aperture",O_RDWR|O_CLOEXEC); REQUIRE(foreign>=0);
    struct wddm_aperture_revoke wrong={.version=1,.cookie=arg.cookie};
    errno=0; REQUIRE(ioctl(foreign,WDDM_APERTURE_REVOKE,&wrong)==-1 && errno==ENOENT);
    close(foreign); revoke_lease(control,arg.cookie);
    errno=0; REQUIRE(ioctl(control,WDDM_APERTURE_REVOKE,&wrong)==-1 && errno==ENOENT);
    REQUIRE(!munmap(view,4096)); close(arg.fd);
    /* Copyout failure must undo accounting and never install an inaccessible FD. */
    void* readonly=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    REQUIRE(readonly!=MAP_FAILED); *(struct wddm_aperture_create*)readonly=fence();
    REQUIRE(!mprotect(readonly,4096,PROT_READ));
    errno=0; REQUIRE(ioctl(control,WDDM_APERTURE_CREATE,readonly)==-1 && errno==EFAULT);
    REQUIRE(!munmap(readonly,4096));
    REQUIRE(!info(control).active_leases);
    /* An allocation lease spans contiguous slots only within the advertised BAR. */
    struct wddm_aperture_create cpu={.version=1,.kind=2,.bytes=4096,.host_generation=1,.flags=1};
    REQUIRE(!ioctl(control,WDDM_APERTURE_CREATE,&cpu));
    struct wddm_aperture_create overlap={.version=1,.kind=2,.bytes=4096,.host_generation=2,.flags=1};
    errno=0; REQUIRE(ioctl(control,WDDM_APERTURE_CREATE,&overlap)==-1 && errno==EBUSY);
    view=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,cpu.fd,0); REQUIRE(view!=MAP_FAILED);
    REQUIRE(!munmap(view,4096)); revoke_lease(control,cpu.cookie); close(cpu.fd);
    /* Exhaustion rejects without allocating an FD or changing accounting. */
    struct wddm_aperture_create leases[WDDM_APERTURE_MAX_LEASES];
    for (unsigned n=0;n<WDDM_APERTURE_MAX_LEASES;++n) {
        leases[n]=fence(); REQUIRE(!ioctl(control,WDDM_APERTURE_CREATE,&leases[n]));
    }
    arg=fence(); errno=0;
    REQUIRE(ioctl(control,WDDM_APERTURE_CREATE,&arg)==-1 && errno==ENOSPC);
    REQUIRE(info(control).active_leases==WDDM_APERTURE_MAX_LEASES);
    for (unsigned n=0;n<WDDM_APERTURE_MAX_LEASES;++n) { revoke_lease(control,leases[n].cookie); close(leases[n].fd); }
    if (layout.allocation_bytes > WDDM_APERTURE_CPU_LIMIT) {
        cpu=(struct wddm_aperture_create){.version=1,.kind=2,.bytes=WDDM_APERTURE_CPU_LIMIT,.host_generation=3,.flags=1};
        REQUIRE(!ioctl(control,WDDM_APERTURE_CREATE,&cpu));
        overlap=(struct wddm_aperture_create){.version=1,.kind=2,.offset=WDDM_APERTURE_CPU_LIMIT,
            .bytes=4096,.host_generation=4,.flags=1};
        errno=0; REQUIRE(ioctl(control,WDDM_APERTURE_CREATE,&overlap)==-1 && errno==ENOSPC);
        REQUIRE(info(control).active_cpu_bytes==WDDM_APERTURE_CPU_LIMIT);
        revoke_lease(control,cpu.cookie); close(cpu.fd);
    }
    object_contract(control);
    resource_object_contract(control);
    native_sync_contract(control);
    transfer_and_revoke_lease(control);
    /* Controller close revokes a recipient FD even when that FD stays open. */
    arg=fence(); REQUIRE(!ioctl(control,WDDM_APERTURE_CREATE,&arg));
    REQUIRE(!close(control));
    errno=0; REQUIRE(mmap(NULL,4096,PROT_READ,MAP_SHARED,arg.fd,0)==MAP_FAILED && errno==ENODEV);
    close(arg.fd);
    control=open("/dev/wddm-aperture",O_RDWR|O_CLOEXEC); REQUIRE(control>=0);
    layout=info(control); REQUIRE(!layout.active_leases && !layout.active_cpu_bytes);
    close(control);
    alarm(0);
    printf("APERTURE_CONTRACT_PASS checks=%u activeLeases=0 activeCpuBytes=0\n",checks);
    return 0;
}
