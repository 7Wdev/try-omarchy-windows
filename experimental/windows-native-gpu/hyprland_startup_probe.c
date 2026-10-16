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
#include <time.h>
#if defined(WDDM_BRIDGE_COMPOSITOR_HOST_CAPTURE) && defined(WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT)
#error Select one compositor output diagnostic
#endif
#if defined(WDDM_BRIDGE_COMPOSITOR_HOST_SCENE) && !defined(WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT)
#error Scene diagnostics require queued presentation
#endif
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_SCENE
static _Atomic int scene_ready;
static char last_control_reply[8192];
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
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_SCENE
    if(!atomic_load(&scene_ready)) return 0;
#endif
    if(frame>=2) return queue_output(fd,width,height,format,stride);
#endif
    return 0;
}
static int control_socket_path(char* target,size_t size) {
    const char* instance=getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char* runtime=getenv("XDG_RUNTIME_DIR");
    if(!instance || !instance[0] || !runtime || runtime[0]!='/') return -1;
    const int bytes=snprintf(target,size,"%s/hypr/%s/.socket.sock",runtime,instance);
    return bytes<0 || (size_t)bytes>=size ? -1 : 0;
}
static int run_ctl(char* const arguments[]) {
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT
    // Hyprland owns SIGCHLD and may reap a forked hyprctl before waitpid.
    // Use its documented client protocol, without spawning another process.
    struct sockaddr_un address={.sun_family=AF_UNIX};
    if(control_socket_path(address.sun_path,sizeof address.sun_path)) return -1;
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
    // Full desktop startup can still be busy in shader/import initialization.
    // Keep native GPU retirement bounds unchanged; bound only this IPC client.
    const struct timeval timeout={.tv_sec=getenv("WDDM_BRIDGE_DESKTOP_PRESENTATION_PROBE") ? 15 : 5};
    int result=-1;const char* stage="connect";size_t received=0;
    struct timespec started={0},finished={0};clock_gettime(CLOCK_MONOTONIC,&started);
    if (setsockopt(connection,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) ||
        setsockopt(connection,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)) ||
        connect(connection,(struct sockaddr*)&address,(socklen_t)(offsetof(struct sockaddr_un,sun_path)+strlen(address.sun_path)+1))) goto done;
    stage="send";
    for (size_t sent=0;sent<used;) {
        const ssize_t count=send(connection,request+sent,used-sent,MSG_NOSIGNAL);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) goto done;
        sent+=(size_t)count;
    }
    stage="receive";
    char reply[8192];
    for (;;) {
        if (received==sizeof(reply)-1) goto done;
        const ssize_t count=recv(connection,reply+received,sizeof(reply)-1-received,0);
        if (count<0 && errno==EINTR) continue;
        if (count<0) goto done;
        if (!count) break;
        received+=(size_t)count;
    }
    reply[received]='\0';
    stage="reply";
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_SCENE
    memcpy(last_control_reply,reply,received+1);
    if(strcmp(request,"j/clients"))
#endif
    printf("HYPRLAND_CONTROL_REPLY command=%s reply=%s\n",request,reply);
    clock_gettime(CLOCK_MONOTONIC,&finished);
    printf("HYPRLAND_CONTROL_TIMING timeoutSeconds=%ld elapsedMs=%lld\n",(long)timeout.tv_sec,
        (long long)(finished.tv_sec-started.tv_sec)*1000+(finished.tv_nsec-started.tv_nsec)/1000000);
    result=(received && (json ? (reply[0]=='[' || reply[0]=='{') : !strcmp(reply,"ok"))) ? 0 : -1;
    if(result) errno=EPROTO;
done:
    if(result) {
        const int failure=errno;clock_gettime(CLOCK_MONOTONIC,&finished);
        printf("HYPRLAND_CONTROL_FAILURE command=%s stage=%s errno=%d receivedBytes=%zu elapsedMs=%lld\n",request,stage,failure,received,
            (long long)(finished.tv_sec-started.tv_sec)*1000+(finished.tv_nsec-started.tv_nsec)/1000000);
    }
    if (close(connection)) result=-1;
    return result;
#else
    char signature[512],runtime_env[1024],home_env[1024];
    const char* instance=getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char* runtime=getenv("XDG_RUNTIME_DIR");
    const char* home=getenv("HOME");
    if(!instance || snprintf(signature,sizeof(signature),"HYPRLAND_INSTANCE_SIGNATURE=%s",instance)>=(int)sizeof(signature)) return -1;
    if(!runtime || !home || runtime[0]!='/' || home[0]!='/' ||
       snprintf(runtime_env,sizeof runtime_env,"XDG_RUNTIME_DIR=%s",runtime)>=(int)sizeof runtime_env ||
       snprintf(home_env,sizeof home_env,"HOME=%s",home)>=(int)sizeof home_env) return -1;
    char* const environment[]={signature,runtime_env,home_env,
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
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_SCENE
static int scene_receipt(const char* line) {
    // Keep the message and newline in one syscall. Other guest processes share
    // the serial console; unbuffered puts can split these into separate writes.
    const size_t bytes=strlen(line);
    ssize_t written;
    do { written=write(STDOUT_FILENO,line,bytes); } while(written<0 && errno==EINTR);
    if(written!=(ssize_t)bytes) { atomic_store(&queue_failed,1);return -1; }
    return 0;
}
static void scene_hex_receipt(const char* label,const char* data,size_t bytes) {
    static const char hex[]="0123456789abcdef";
    char line[17000];
    const int prefix=snprintf(line,sizeof(line),"%s ",label);
    if(prefix<=0 || (size_t)prefix+bytes*2+1>=sizeof(line)) return;
    for(size_t index=0;index<bytes;++index) {
        const unsigned char value=(unsigned char)data[index];
        line[prefix+index*2]=hex[value>>4]; line[prefix+index*2+1]=hex[value&15];
    }
    line[prefix+bytes*2]='\n';
    const size_t count=(size_t)prefix+bytes*2+1;
    if(write(STDOUT_FILENO,line,count)!=(ssize_t)count) atomic_store(&queue_failed,1);
}
static void scene_diagnostics(void) {
    const char* paths[]={"/tmp/foot-scene.log","/tmp/scene-font-sans","/tmp/scene-font-mono","/tmp/desktop-scene-ready"};
    for(unsigned index=0;index<sizeof(paths)/sizeof(paths[0]);++index) {
        FILE* file=fopen(paths[index],"rb");
        if(!file) { printf("HYPRLAND_SCENE_MISSING file=%s errno=%d\n",paths[index],errno); continue; }
        char data[2048]; const size_t bytes=fread(data,1,sizeof(data),file);
        fclose(file);
        printf("HYPRLAND_SCENE_DIAGNOSTIC file=%s bytes=%zu\n",paths[index],bytes);
        scene_hex_receipt("HYPRLAND_SCENE_DIAGNOSTIC_HEX",data,bytes);
    }
}
static int prepare_scene(void) {
    if(getenv("WDDM_BRIDGE_NORMAL_DESKTOP_PROBE")) {
        // Inspect the shipped desktop without pretending the controlled Foot
        // fixture proves the shell or a normal UWSM session.
        sleep(8);
        char* const layers[]={"/usr/bin/hyprctl","-j","layers",NULL};
        run_ctl(layers);
        scene_hex_receipt("OMARCHY_DESKTOP_LAYERS_HEX",last_control_reply,strlen(last_control_reply));
        if(scene_receipt("OMARCHY_DESKTOP_PROBE_CAPTURE normalConfig=true shellVerified=false applicationGpuVerified=false\n")) return -1;
        atomic_store(&scene_ready,1);
        return 0;
    }
    char* const launch[]={"/usr/bin/hyprctl","dispatch","exec",
        "[float; size 480 200; move 80 120] LC_ALL=C.UTF-8 XCOMPOSEFILE=/usr/share/X11/locale/en_US.UTF-8/Compose LD_PRELOAD= /usr/bin/foot --config=/foot.ini --app-id=wddm-scene --title='Wayland desktop scene' /bin/sh /desktop-scene.sh > /tmp/foot-scene.log 2>&1",NULL};
    if(run_ctl(launch)) return -1;
    for(unsigned tick=0;tick<100;++tick) {
        char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
        if(!run_ctl(clients) && strstr(last_control_reply,"\"class\": \"wddm-scene\"") && !access("/tmp/desktop-scene-ready",F_OK)) {
            scene_hex_receipt("HYPRLAND_SCENE_CLIENTS_HEX",last_control_reply,strlen(last_control_reply));
            for(unsigned index=0;index<2;++index) {
                const char* path=index?"/tmp/scene-font-mono":"/tmp/scene-font-sans";
                FILE* file=fopen(path,"rb"); char font[256]={0};
                if(!file) return -1;
                const size_t bytes=fread(font,1,sizeof(font)-1,file);
                const int error=ferror(file),closed=fclose(file);
                if(error || closed || bytes==0 || bytes==sizeof(font)-1 ||
                    strcmp(font,index?"/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf\n":"/usr/share/fonts/noto/NotoSans-Regular.ttf\n")) return -1;
                printf("HYPRLAND_SCENE_FONT index=%u file=%s",index+1,font);
            }
            // Allow the terminal's PTY update to reach its first surface commit,
            // then force actual compositor repaints through the existing loop.
            usleep(200000);
            if(scene_receipt("HYPRLAND_SCENE_READY realWaylandClient=true applicationGpuAccelerated=false compositorGpu=true\n")) return -1;
            atomic_store(&scene_ready,1);
            return 0;
        }
        usleep(100000);
    }
    puts("HYPRLAND_SCENE_FAILURE reason=client-or-font-readiness");
    scene_diagnostics();
    return -1;
}
static int close_scene(void) {
    if(getenv("WDDM_BRIDGE_NORMAL_DESKTOP_PROBE")) return 0;
    char* const close_window[]={"/usr/bin/hyprctl","dispatch","closewindow","class:wddm-scene",NULL};
    if(run_ctl(close_window)) return -1;
    for(unsigned tick=0;tick<50;++tick) {
        char* const clients[]={"/usr/bin/hyprctl","-j","clients",NULL};
        if(!run_ctl(clients) && !strstr(last_control_reply,"\"class\": \"wddm-scene\"") && !access("/tmp/desktop-scene-exit",F_OK)) {
            return scene_receipt("HYPRLAND_SCENE_CLOSED clientUnmapped=true childExitMarker=true\n");
        }
        usleep(100000);
    }
    puts("HYPRLAND_SCENE_FAILURE reason=client-shutdown");
    return -1;
}
#endif
static void* bounded_control(void* unused) {
    (void)unused;
    for(unsigned n=0;n<60;++n) {
        sleep(1);
        char socket[768];
        if(control_socket_path(socket,sizeof socket) || access(socket,F_OK)) continue;
        char* const create[]={"/usr/bin/hyprctl","output","create","headless","WDDM-1",NULL};
        const int created=run_ctl(create);
        printf("HYPRLAND_STARTUP_OUTPUT createExit=%d windowsPresentation=false\n",created);
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_SCENE
        if(created || prepare_scene()) atomic_store(&queue_failed,1);
#endif
        for(unsigned tick=0;tick<HYPRLAND_QUEUE_FRAMES*2+100 && atomic_load(&queued_frames)<HYPRLAND_QUEUE_FRAMES && !atomic_load(&queue_failed);++tick) {
            char* const legacy_repaint[]={"/usr/bin/hyprctl","keyword","decoration:rounding","0",NULL};
            // A Lua config assignment to an unchanged value does not damage a
            // quiet desktop. A short-lived real notification supplies bounded
            // changing content without recreating outputs or hiding warnings.
            char notification[128];
            snprintf(notification,sizeof notification,"GPU composition probe %u; Quickshell GPU integration pending",tick);
            char* const lua_repaint[]={"/usr/bin/hyprctl","notify","-1","100","rgb(4477aa)",notification,NULL};
            char* const* repaint=getenv("WDDM_BRIDGE_NORMAL_DESKTOP_PROBE") ? lua_repaint : legacy_repaint;
            if (run_ctl(repaint)) { atomic_store(&queue_failed,1); break; }
            usleep(50000);
        }
        if (atomic_load(&queued_frames)!=HYPRLAND_QUEUE_FRAMES) atomic_store(&queue_failed,1);
        printf("HYPRLAND_HOST_QUEUE_END frames=%u target=%u failed=%d\n",atomic_load(&queued_frames),HYPRLAND_QUEUE_FRAMES,atomic_load(&queue_failed));
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_SCENE
        if(close_scene()) atomic_store(&queue_failed,1);
#endif
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
        char* const legacy_stop[]={"/usr/bin/hyprctl","dispatch","exit",NULL};
        char* const lua_stop[]={"/usr/bin/hyprctl","eval","hl.dispatch(hl.dsp.exit())",NULL};
        char* const* stop=getenv("WDDM_BRIDGE_NORMAL_DESKTOP_PROBE") ? lua_stop : legacy_stop;
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
    const char* runtime=getenv("WDDM_BRIDGE_HYPRLAND_RUNTIME");
    const char* home=getenv("WDDM_BRIDGE_HYPRLAND_HOME");
    if((runtime && !home) || (home && !runtime) || (runtime && (runtime[0]!='/' || home[0]!='/'))) return 121;
    if(!runtime) { runtime="/tmp/hypr-runtime";home="/tmp/hypr-home"; }
    if(setenv("D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE","true",1) ||
       setenv("D3D12_EXPERIMENTAL_NATIVE_BUFFERS","true",1) ||
       setenv("AQ_EXPERIMENTAL_NATIVE_RENDER","1",1) ||
       setenv("GBM_BACKENDS_PATH","/opt/wddm-mesa/lib/gbm",1) ||
       setenv("XDG_RUNTIME_DIR",runtime,1) ||
       setenv("HOME",home,1) ||
       setenv("HYPRLAND_LOG_WLR","1",1) ||
       setenv("HYPRLAND_TRACE","1",1)) return 121;
    if((mkdir(runtime,0700) && errno!=EEXIST) ||
       (mkdir(home,0700) && errno!=EEXIST)) return 122;
#ifdef WDDM_BRIDGE_COMPOSITOR_HOST_SCENE
    const unsigned profile=getenv("WDDM_BRIDGE_DESKTOP_PRESENTATION_PROBE") ? 10 : getenv("WDDM_BRIDGE_DESKTOP_LARGE_MAP_PROBE") ? 9 : getenv("WDDM_BRIDGE_DESKTOP_SPARSE_PROBE") ? 8 : getenv("WDDM_BRIDGE_DESKTOP_4K_CPU_PROBE") ? 7 : getenv("WDDM_BRIDGE_DESKTOP_4K_PROBE") ? 6 : 5;
    const char* begin_name=profile==10 ? "wddm_bridge_begin_opengl_desktop_presentation_render" : profile==9 ? "wddm_bridge_begin_opengl_desktop_large_map_render" : profile==8 ? "wddm_bridge_begin_opengl_desktop_sparse_render" : profile==7 ? "wddm_bridge_begin_opengl_desktop_4k_cpu_render" : profile==6 ? "wddm_bridge_begin_opengl_desktop_4k_render" : "wddm_bridge_begin_opengl_desktop_render";
#else
    const unsigned profile=4;
    const char* begin_name="wddm_bridge_begin_opengl_compositor_render";
#endif
    int (*begin)(void)=(int (*)(void))dlsym(RTLD_DEFAULT,begin_name);
    if(!begin || begin()) { perror("Hyprland profile negotiation"); return 123; }
    printf("HYPRLAND_STARTUP_BEGIN version=0.56.2 boundedProfile=%u actualCompositor=true windowsPresentation=false\n",profile);
    pthread_t thread;
    if(pthread_create(&thread,NULL,bounded_control,NULL) || pthread_detach(thread)) return 124;
    const char* config=getenv("WDDM_BRIDGE_NORMAL_DESKTOP_PROBE") ?
        "/opt/wddm-root/data/normal-desktop.lua" : "/hyprland.conf";
    char* arguments[]={"/guest-probe","--config",(char*)config,NULL};
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
