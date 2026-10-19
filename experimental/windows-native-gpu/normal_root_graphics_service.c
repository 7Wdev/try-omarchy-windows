// SPDX-License-Identifier: MIT
// Supervised guest-only graphics service for the read-only normal-root test.
// Reuse the tested module/broker teardown; never call the legacy PID 1 main.
#define main wddm_unused_legacy_diagnostic_main
#include "runtime_guest_init.c"
#undef main
#include <pwd.h>
#include <stdint.h>

int main(void) {
    if(getpid()==1 || getuid() || geteuid() || access("/opt/wddm-root/normal-root-graphics",R_OK)) return 125;
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    struct rlimit core={0,0};
    if(setrlimit(RLIMIT_CORE,&core)) { perror("disable experimental renderer core dumps");return 125; }
    struct passwd* user=getpwnam("omarchy");
    if(!user || user->pw_uid!=1000 || !user->pw_gid || strcmp(user->pw_dir,"/home/omarchy")) return 125;
    const unsigned uid=user->pw_uid,gid=user->pw_gid;
    const int desktop=access("/opt/wddm-root/normal-desktop-probe",R_OK)==0;
    const char* port="/dev/vport0p1";
    puts("NORMAL_ROOT_GRAPHICS_BEGIN normalSystemdService=true normalUid=1000 diagnosticCompositor=true normalOmarchySession=false");
    if(desktop) {
        // Start the normal user manager and session bus in this disposable OS.
        const pid_t manager=fork();
        if(!manager) { execl("/usr/bin/systemctl","systemctl","start","user@1000.service",(char*)NULL);_exit(127); }
        int status=0;
        if(manager<0 || waitpid(manager,&status,0)!=manager || !WIFEXITED(status) || WEXITSTATUS(status)) return 125;
        puts("OMARCHY_DESKTOP_PROBE_BEGIN normalConfig=true normalUserBus=true uwsmSession=false");
    }
    if(desktop && !access("/opt/wddm-root/desktop-content-probe",R_OK)) {
        // This install writes only the owned RAM overlay. Decode the shipped
        // 6K wallpaper at the actual 4K output size before its GPU upload.
        const pid_t wallpaper=fork();
        if(!wallpaper) {
            execl("/usr/bin/install","install","-m","0644","/opt/wddm-root/data/Background.qml",
                "/usr/share/omarchy/shell/plugins/background/Background.qml",(char*)NULL);_exit(127);
        }
        int installed=0;
        if(wallpaper<0 || waitpid(wallpaper,&installed,0)!=wallpaper || !WIFEXITED(installed) || WEXITSTATUS(installed)) return 125;
        puts("OMARCHY_WALLPAPER_DECODE width=3840 height=2160 sourceImageUnchanged=true ownedRamOverlayOnly=true gpuProof=false");
        const pid_t setup=fork();
        if(!setup) {
            if(setgroups(0,NULL) || setgid(gid) || setuid(uid) || setenv("HOME","/home/omarchy",1)) _exit(126);
            execl("/bin/bash","bash","-eu","-c",
                "umask 077; current=$HOME/.local/state/omarchy/current; mkdir -p \"$current\"; "
                "test -f /usr/share/omarchy/themes/tokyo-night/backgrounds/0-winding-road.jpg; "
                "ln -sfn /usr/share/omarchy/themes/tokyo-night \"$current/theme\"; "
                "printf 'tokyo-night\\n' > \"$current/theme.name\"; "
                "ln -sfn /usr/share/omarchy/themes/tokyo-night/backgrounds/0-winding-road.jpg \"$current/background\"",(char*)NULL);
            _exit(127);
        }
        int status=0;
        if(setup<0 || waitpid(setup,&status,0)!=setup || !WIFEXITED(status) || WEXITSTATUS(status)) return 125;
        puts("OMARCHY_DESKTOP_CONTENT_SETUP ordinaryUid=1000 shippedTheme=tokyo-night shippedWallpaper=0-winding-road.jpg ownedRamOverlayOnly=true");
    }
    for(unsigned tick=0;tick<200 && access(port,F_OK);++tick) usleep(50000);
    if(chmod(port,0600)) { perror("native port permissions");return 125; }
    int result=125;
    pid_t shell_trace=-1;
    const int aperture=broker_peer_start() ? -1 : kernel_aperture_start();
    const int render=aperture>0 ? native_render_start() : -1;
    const pid_t broker=aperture>0 && render>0 ? graphics_broker_start_as(port,uid,gid) : -1;
    const char* runtime="/run/user/1000";
    if((mkdir(runtime,0700) && errno!=EEXIST) || chown(runtime,uid,gid) || chmod(runtime,0700)) {
        perror("ordinary user runtime");goto cleanup;
    }
    if(broker>0) {
        const pid_t child=fork();
        if(!child) {
            const char* variables[][2]={{"WDDM_BRIDGE_RUNTIME_WORKLOAD","opengl"},
                {"EGL_PLATFORM","surfaceless"},{"GALLIUM_DRIVER","d3d12"},{"MESA_D3D12_DEFAULT_ADAPTER_NAME","NVIDIA"},
                {"LIBGL_DRIVERS_PATH","/opt/wddm-mesa/lib/dri"},{"MESA_SHADER_CACHE_DISABLE","true"},{"EGL_LOG_LEVEL","debug"},
                {"WDDM_BRIDGE_KERNEL_APERTURE","1"},{"WDDM_BRIDGE_RENDER_NODE",native_render_path},
                {"LD_PRELOAD","/linux-ioctl-bridge.so:/hyprland-startup-hook.so"},
                {"LD_LIBRARY_PATH","/opt/wddm-mesa/lib:/usr/lib/wsl/lib:/usr/lib/x86_64-linux-gnu"},
                {"WDDM_BRIDGE_HYPRLAND_RUNTIME",runtime},{"WDDM_BRIDGE_HYPRLAND_HOME","/home/omarchy"},
                {"FONTCONFIG_FILE","/opt/wddm-root/data/compositor-fonts.conf"},{"LC_ALL","C.UTF-8"},
                {"HOME","/home/omarchy"},{"USER","omarchy"},{"LOGNAME","omarchy"},{"SHELL","/bin/bash"},
                {"PATH","/usr/bin:/usr/local/bin"}};
            FILE* name=fopen("/linux-umd-name","rb");char module[256]={0};
            if(!name || !fread(module,1,sizeof(module)-1,name)) _exit(125);
            fclose(name);module[strcspn(module,"\r\n")]=0;
            if(setenv("WDDM_BRIDGE_LINUX_UMD_NAME",module,1)) _exit(125);
            if(desktop && (setenv("WDDM_BRIDGE_NORMAL_DESKTOP_PROBE","1",1) ||
                setenv("DBUS_SESSION_BUS_ADDRESS","unix:path=/run/user/1000/bus",1) ||
                setenv("OMARCHY_PATH","/usr/share/omarchy",1))) _exit(125);
            if(desktop && !access("/opt/wddm-root/normal-desktop-shell-software",R_OK)) {
                if(setenv("QT_QUICK_BACKEND","software",1)) _exit(125);
                puts("OMARCHY_DESKTOP_SHELL_RENDERER qtQuickSoftware=true compositorNvidia=true productAcceptance=false");
            }
            if(desktop && !access("/opt/wddm-root/desktop-4k-probe",R_OK) &&
               setenv("WDDM_BRIDGE_DESKTOP_4K_PROBE","1",1)) _exit(125);
            if(desktop && !access("/opt/wddm-root/desktop-4k-cpu-probe",R_OK) &&
               setenv("WDDM_BRIDGE_DESKTOP_4K_CPU_PROBE","1",1)) _exit(125);
            if(desktop && !access("/opt/wddm-root/desktop-sparse-probe",R_OK) &&
               setenv("WDDM_BRIDGE_DESKTOP_SPARSE_PROBE","1",1)) _exit(125);
            if(desktop && !access("/opt/wddm-root/desktop-presentation-probe",R_OK) &&
               setenv("WDDM_BRIDGE_DESKTOP_PRESENTATION_PROBE","1",1)) _exit(125);
            if(desktop && !access("/opt/wddm-root/desktop-coordinator-probe",R_OK)) {
                if(setenv("WDDM_BRIDGE_ACTOR_PROFILE",!access("/opt/wddm-root/desktop-reference-probe",R_OK) ? "13" : !access("/opt/wddm-root/desktop-slot-probe",R_OK) ? "12" : !access("/opt/wddm-root/desktop-fence-probe",R_OK) ? "11" : "10",1) || setenv("QSG_RHI_BACKEND","opengl",1) ||
                   setenv("QSG_INFO","1",1)) _exit(125);
            }
            if(desktop && !access("/opt/wddm-root/desktop-content-probe",R_OK) && setenv("WDDM_BRIDGE_DESKTOP_CONTENT","1",1)) _exit(125);
            if(desktop && !access("/opt/wddm-root/desktop-protocol-trace",R_OK) && setenv("WAYLAND_DEBUG","1",1)) _exit(125);
            if(desktop && !access("/opt/wddm-root/desktop-large-map-probe",R_OK) &&
               setenv("WDDM_BRIDGE_DESKTOP_LARGE_MAP_PROBE","1",1)) _exit(125);
            for(unsigned index=0;index<sizeof(variables)/sizeof(variables[0]);++index)
                if(setenv(variables[index][0],variables[index][1],1)) _exit(125);
            if(!access("/opt/wddm-root/desktop-coordinator-probe",R_OK) && unsetenv("EGL_PLATFORM")) _exit(125);
            if(graphics_client_drop_as(port,uid,gid)) { perror("normal graphics privilege drop");_exit(126); }
            execl("/guest-probe","/guest-probe",(char*)NULL);perror("normal graphics compositor exec");_exit(127);
        }
        if(child>0) {
            if(graphics_primary_writer>=0) {
                const uint64_t primary=(uint64_t)child;
                const ssize_t written=write(graphics_primary_writer,&primary,sizeof primary);
                close(graphics_primary_writer);graphics_primary_writer=-1;
                if(written!=sizeof primary) { kill(child,SIGKILL);waitpid(child,NULL,0);goto cleanup; }
                shell_trace=fork();
                if(!shell_trace) {
                    execl("/usr/bin/journalctl","journalctl","-b","-t","omarchy-shell","-t","omarchy-gpu-app","--no-pager","-f","-n","100",(char*)NULL);_exit(127);
                }
            }
            // Full-resolution initialization + explicit cleanup must fit a
            // separately bounded diagnostic window. Older probes keep 45 s.
            const unsigned deadline=access("/opt/wddm-root/desktop-fence-probe",R_OK)==0 ? 240u : access("/opt/wddm-root/desktop-coordinator-probe",R_OK)==0 ? 120u : access("/opt/wddm-root/desktop-presentation-probe",R_OK)==0 ? 90u : 45u;
            printf("NORMAL_ROOT_GRAPHICS_SUPERVISION deadlineSeconds=%u\n",deadline);
            struct sigaction action={0};action.sa_handler=timeout_handler;sigaction(SIGALRM,&action,NULL);alarm(deadline);
            int status=0;
            if(waitpid(child,&status,0)==child) {
                result=WIFEXITED(status) ? WEXITSTATUS(status) : 128;
                if(WIFSIGNALED(status)) {
                    // A tty hangup can invalidate inherited stdout. Reopen
                    // the console for the supervisor's diagnostic receipt.
                    const int trace=open("/dev/console",O_WRONLY|O_NOCTTY|O_CLOEXEC);
                    if(trace>=0) {
                        dprintf(trace,"NORMAL_ROOT_GRAPHICS_CHILD_SIGNAL signal=%d coreDumped=%s\n",WTERMSIG(status),WCOREDUMP(status) ? "true" : "false");
                        close(trace);
                    }
                }
            }
            else { kill(child,SIGKILL);waitpid(child,NULL,0);result=124; }
            alarm(0);
        }
    }
cleanup:
    if(shell_trace>0) { kill(shell_trace,SIGTERM);waitpid(shell_trace,NULL,0); }
    if(desktop && !access("/opt/wddm-root/desktop-coordinator-probe",R_OK)) {
        const pid_t stop=fork();
        if(!stop) { execl("/usr/bin/systemctl","systemctl","stop","user@1000.service",(char*)NULL);_exit(127); }
        int status=0;
        const int stopped=stop>0 && waitpid(stop,&status,0)==stop && WIFEXITED(status) && !WEXITSTATUS(status);
        if(!stopped) result=125;
        printf("NORMAL_ROOT_COORDINATOR_USER_STOP ordinaryUserManagerStopped=%s ownedGuestOnly=true\n",stopped ? "true" : "false");
    }
    if(graphics_broker_wait(broker)) result=125;
    if(desktop) {
        // Replay only the owned applications' renderer identities once their
        // trace writers have stopped. Keep journal metadata, including PID,
        // so verification can bind each renderer to its authenticated peer.
        if(write(STDOUT_FILENO,"\n",1)!=1) result=125;
        const pid_t journal=fork();
        if(!journal) {
            execl("/usr/bin/journalctl","journalctl","-b","-t","omarchy-shell","-t","omarchy-gpu-app",
                "--no-pager","-n","64","--grep",
                "OpenGL VENDOR|Created QRhi|Running on D3D12|Using OpenGL 3.3 renderer",(char*)NULL);
            _exit(127);
        }
        if(journal>0) waitpid(journal,NULL,0);
    }
    if(native_render_loaded && native_render_stop()) result=125;
    if(aperture>0 && kernel_aperture_stop()) result=125;
    printf("NORMAL_ROOT_GRAPHICS_EXIT result=%d normalOmarchySession=false\n",result);
    sync();return result;
}
