// Private, bounded actual-compositor startup. No Windows display acceptance.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>
#include <stdatomic.h>
#if defined(WDDM_BRIDGE_COMPOSITOR_HOST_CAPTURE) && defined(WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT)
#error Select one compositor output diagnostic
#endif
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT
#include "hyprland_queue_probe.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <stddef.h>
#endif
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_CAPTURE
#include "hyprland_capture_probe.h"
#endif

typedef int (*main_function)(int,char**,char**);
static main_function compositor_main;
static _Atomic unsigned compositor_commits;
int wddm_bridge_compositor_commit(int fd,uint32_t width,uint32_t height,uint32_t format,uint32_t stride,uint64_t modifier) {
    int (*info)(int,uint64_t*)=(int (*)(int,uint64_t*))dlsym(RTLD_DEFAULT,"wddm_bridge_native_buffer_info");
    uint64_t bytes=0;
    if(!info || !width || !height || width>4096 || height>4096 || stride!=width*4 ||
       info(fd,&bytes) || bytes!=(uint64_t)stride*height) return -1;
    const unsigned frame=atomic_fetch_add(&compositor_commits,1)+1;
    printf("HYPRLAND_COMPOSITOR_COMMIT index=%u width=%u height=%u format=%u stride=%u modifier=%llu ownedNativeBuffer=true pixelsVerified=false windowsPresentation=false\n",
        frame,width,height,format,stride,(unsigned long long)modifier);
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_CAPTURE
    if(frame==2 || frame==4) return capture_output(fd,width,height,format,stride);
#endif
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT
    if(frame>=2) return queue_output(fd,width,height,format,stride);
#endif
    return 0;
}
static int run_ctl(char* const arguments[]) {
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT
    // Hyprland owns SIGCHLD and may reap a forked hyprctl before waitpid.
    // Use its documented client protocol, without spawning another process.
    const char* instance=getenv("HYPRLAND_INSTANCE_SIGNATURE");
    struct sockaddr_un address={.sun_family=AF_UNIX};
    if (!instance || snprintf(address.sun_path,sizeof(address.sun_path),"/tmp/hypr-runtime/hypr/%s/.socket.sock",instance)>=(int)sizeof(address.sun_path)) return -1;
    char request[1024];
    const int json=arguments[1] && !strcmp(arguments[1],"-j");
    size_t used=(size_t)snprintf(request,sizeof(request),"%s/",json?"j":"");
    for (unsigned index=json?2u:1u;arguments[index];++index) {
        const int count=snprintf(request+used,sizeof(request)-used,"%s%s",used>(json?2u:1u)?" ":"",arguments[index]);
        if (count<0 || (size_t)count>=sizeof(request)-used) return -1;
        used+=(size_t)count;
    }
    const int connection=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if (connection<0) return -1;
    const struct timeval timeout={.tv_sec=5};
    int result=-1;
    if (setsockopt(connection,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) ||
        setsockopt(connection,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)) ||
        connect(connection,(struct sockaddr*)&address,(socklen_t)(offsetof(struct sockaddr_un,sun_path)+strlen(address.sun_path)+1))) goto done;
    for (size_t sent=0;sent<used;) {
        const ssize_t count=send(connection,request+sent,used-sent,MSG_NOSIGNAL);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) goto done;
        sent+=(size_t)count;
    }
    char reply[8192]; size_t received=0;
    for (;;) {
        if (received==sizeof(reply)-1) goto done;
        const ssize_t count=recv(connection,reply+received,sizeof(reply)-1-received,0);
        if (count<0 && errno==EINTR) continue;
        if (count<0) goto done;
        if (!count) break;
        received+=(size_t)count;
    }
    reply[received]='\0';
    printf("HYPRLAND_CONTROL_REPLY command=%s reply=%s\n",request,reply);
    result=(received && (json ? reply[0]=='[' : !strcmp(reply,"ok"))) ? 0 : -1;
done:
    if (close(connection)) result=-1;
    return result;
#else
    char signature[512];
    const char* instance=getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if(!instance || snprintf(signature,sizeof(signature),"HYPRLAND_INSTANCE_SIGNATURE=%s",instance)>=(int)sizeof(signature)) return -1;
    char* const environment[]={signature,"XDG_RUNTIME_DIR=/tmp/hypr-runtime","HOME=/tmp/hypr-home",
        "LD_LIBRARY_PATH=/opt/wddm-mesa/lib:/usr/lib/x86_64-linux-gnu","PATH=/usr/bin",NULL};
    const pid_t child=fork();
    if(child<0) return -1;
    if(!child) { execve("/usr/bin/hyprctl",arguments,environment); _exit(127); }
    int status=0;
    if(waitpid(child,&status,0)!=child) return -1;
    if(!WIFEXITED(status)) {
        printf("HYPRLAND_STARTUP_CONTROL_SIGNAL number=%d\n",WIFSIGNALED(status)?WTERMSIG(status):0);
        return -1;
    }
    return WEXITSTATUS(status);
#endif
}
static void* bounded_control(void* unused) {
    (void)unused;
    for(unsigned n=0;n<60;++n) {
        sleep(1);
        const char* instance=getenv("HYPRLAND_INSTANCE_SIGNATURE");
        if(!instance || !instance[0]) continue;
        char socket[768];
        if(snprintf(socket,sizeof(socket),"/tmp/hypr-runtime/hypr/%s/.socket.sock",instance)>=(int)sizeof(socket) || access(socket,F_OK)) continue;
        char* const create[]={"/usr/bin/hyprctl","output","create","headless","WDDM-1",NULL};
        const int created=run_ctl(create);
        printf("HYPRLAND_STARTUP_OUTPUT createExit=%d windowsPresentation=false\n",created);
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT
        for(unsigned tick=0;tick<100 && atomic_load(&queued_frames)<HYPRLAND_QUEUE_FRAMES && !atomic_load(&queue_failed);++tick) {
            char* const repaint[]={"/usr/bin/hyprctl","keyword","decoration:rounding","0",NULL};
            if (run_ctl(repaint)) { atomic_store(&queue_failed,1); break; }
            usleep(50000);
        }
        if (atomic_load(&queued_frames)!=HYPRLAND_QUEUE_FRAMES) atomic_store(&queue_failed,1);
        printf("HYPRLAND_HOST_QUEUE_END frames=%u target=%u failed=%d\n",atomic_load(&queued_frames),HYPRLAND_QUEUE_FRAMES,atomic_load(&queue_failed));
#else
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_CAPTURE
        const unsigned minimum_commits=4;
#else
        const unsigned minimum_commits=2;
#endif
        for(unsigned tick=0;tick<100 && atomic_load(&compositor_commits)<minimum_commits;++tick) usleep(50000);
#endif
        char* const monitors[]={"/usr/bin/hyprctl","-j","monitors","all",NULL};
        printf("HYPRLAND_STARTUP_MONITORS exit=%d\n",run_ctl(monitors));
        char* const stop[]={"/usr/bin/hyprctl","dispatch","exit",NULL};
        puts("HYPRLAND_STARTUP_EXIT_REQUEST method=hyprctl-dispatch-exit");
        run_ctl(stop);
        break;
    }
    sleep(15);
    puts("HYPRLAND_STARTUP_STOP reason=bounded-diagnostic signal=TERM windowsPresentation=false");
    kill(getpid(),SIGTERM);
    return NULL;
}
static int startup_main(int argc,char** argv,char** environment) {
    if(argc<1 || strcmp(argv[0],"/guest-probe")) return compositor_main(argc,argv,environment);
    setvbuf(stdout,NULL,_IONBF,0);
    if(setenv("D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE","true",1) ||
       setenv("D3D12_EXPERIMENTAL_NATIVE_BUFFERS","true",1) ||
       setenv("AQ_EXPERIMENTAL_NATIVE_RENDER","1",1) ||
       setenv("GBM_BACKENDS_PATH","/opt/wddm-mesa/lib/gbm",1) ||
       setenv("XDG_RUNTIME_DIR","/tmp/hypr-runtime",1) ||
       setenv("HOME","/tmp/hypr-home",1) ||
       setenv("HYPRLAND_LOG_WLR","1",1) ||
       setenv("HYPRLAND_TRACE","1",1)) return 121;
    if((mkdir("/tmp/hypr-runtime",0700) && errno!=EEXIST) ||
       (mkdir("/tmp/hypr-home",0700) && errno!=EEXIST)) return 122;
    int (*begin)(void)=(int (*)(void))dlsym(RTLD_DEFAULT,"wddm_bridge_begin_opengl_compositor_render");
    if(!begin || begin()) { perror("Hyprland profile negotiation"); return 123; }
    puts("HYPRLAND_STARTUP_BEGIN version=0.56.2 boundedProfile=4 actualCompositor=true windowsPresentation=false");
    pthread_t thread;
    if(pthread_create(&thread,NULL,bounded_control,NULL) || pthread_detach(thread)) return 124;
    char* arguments[]={"/guest-probe","--config","/hyprland.conf",NULL};
    const int result=compositor_main(3,arguments,environment);
    printf("HYPRLAND_STARTUP_MAIN_RETURN exit=%d windowsPresentation=false\n",result);
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT
    if (atomic_load(&queue_failed) || atomic_load(&queued_frames)!=HYPRLAND_QUEUE_FRAMES) return 125;
#endif
    return result;
}
int __libc_start_main(main_function main,int argc,char** argv,void (*init)(void),
    void (*fini)(void),void (*rtld_fini)(void),void* stack_end) {
    typedef int (*start_function)(main_function,int,char**,void (*)(void),void (*)(void),void (*)(void),void*);
    start_function original=(start_function)dlsym(RTLD_NEXT,"__libc_start_main");
    if(!original) _exit(120);
    compositor_main=main;
    return original(startup_main,argc,argv,init,fini,rtld_fini,stack_end);
}
