// SPDX-License-Identifier: MIT
// Guest-only PID 1: read-only image plus a bounded, temporary RAM overlay.
// This boot prerequisite does not claim graphics or desktop acceptance.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static void fail(const char* operation) {
    const int error=errno;
    dprintf(STDERR_FILENO,"NORMAL_ROOT_BOOT_FAILURE operation=%s errno=%d\n",operation,error);
    sync(); reboot(RB_POWER_OFF);
    for (;;) pause();
}
static void directory(const char* path,mode_t mode) {
    if (mkdir(path,mode) && errno!=EEXIST) fail(path);
}
static void write_new(const char* path,const char* text,mode_t mode) {
    const int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,mode);
    if(fd<0) fail(path);
    for(size_t done=0,bytes=strlen(text);done<bytes;) {
        const ssize_t count=write(fd,text+done,bytes-done);
        if(count<0 && errno==EINTR) continue;
        if(count<=0) fail("overlay-file-write");
        done+=(size_t)count;
    }
    if(close(fd)) fail("overlay-file-close");
}
int main(void) {
    // Never allow this executable to perform mounts on a host or as a child.
    if(getpid()!=1 || getuid()!=0 || geteuid()!=0) return 125;
    setvbuf(stdout,NULL,_IONBF,0); setvbuf(stderr,NULL,_IONBF,0);
    puts("NORMAL_ROOT_BOOT_BEGIN graphics=false sourceWritesAllowed=false");
    directory("/dev",0755); directory("/proc",0555); directory("/sys",0555);
    if(mount("devtmpfs","/dev","devtmpfs",MS_NOSUID,"mode=0755") ||
       mount("proc","/proc","proc",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL) ||
       mount("sysfs","/sys","sysfs",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL)) fail("early-mounts");
    for(unsigned tick=0;tick<400 && access("/dev/vda",F_OK);++tick) usleep(50000);
    const int ro=open("/sys/block/vda/ro",O_RDONLY|O_CLOEXEC);
    char state[3]={0};
    if(ro<0 || read(ro,state,sizeof(state))!=2 || memcmp(state,"1\n",2)) { errno=EROFS; fail("base-block-not-readonly"); }
    if(close(ro)) fail("base-ro-close");
    const int module=open("/overlay.ko",O_RDONLY|O_CLOEXEC);
    if(module<0 || syscall(SYS_finit_module,module,"",0)) fail("overlay-module");
    if(close(module)) fail("overlay-module-close");
    directory("/lower",0700); directory("/overlay",0700); directory("/newroot",0700);
    if(mount("/dev/vda","/lower","ext4",MS_RDONLY|MS_NOATIME,"noload")) fail("readonly-ext4");
    if(mount("tmpfs","/overlay","tmpfs",MS_NOSUID|MS_NODEV,"size=512m,mode=0700")) fail("bounded-overlay-tmpfs");
    directory("/overlay/upper",0700); directory("/overlay/work",0700);
    if(mount("overlay","/newroot","overlay",0,"lowerdir=/lower,upperdir=/overlay/upper,workdir=/overlay/work,index=off")) fail("temporary-root-overlay");
    // The source fstab describes a persistent ext4 root. This explicit boot
    // check uses the already-mounted RAM overlay and no persistent mounts.
    if(unlink("/newroot/etc/fstab") && errno!=ENOENT) fail("overlay-fstab-unlink");
    write_new("/newroot/etc/fstab","# Experimental read-only base with temporary RAM root; no persistent mounts.\n",0644);
    directory("/newroot/usr/local/lib/try-omarchy",0755);
    write_new("/newroot/usr/local/lib/try-omarchy/wddm-normal-boot-check",
        "#!/bin/sh\nset -eu\n"
        "sleep 3\n"
        "test \"$(cat /sys/block/vda/ro)\" = 1\n"
        "test \"$(findmnt -n -o FSTYPE /)\" = overlay\n"
        "test -r /usr/share/omarchy/config/hypr/hyprland.lua\n"
        "printf 'NORMAL_ROOT_OS_VERSION '\ncat /usr/share/omarchy/version\n"
        "printf 'NORMAL_ROOT_SYSTEMD_VERSION '\nsystemctl --version | head -n 1\n"
        "printf 'NORMAL_ROOT_OS_READY sourceBlockReadonly=true rootOverlay=true normalSystemd=true graphics=false desktop=false\\n'\n"
        "systemctl --no-block poweroff\n",0755);
    directory("/newroot/etc/systemd/system",0755);
    directory("/newroot/etc/systemd/system/multi-user.target.wants",0755);
    write_new("/newroot/etc/systemd/system/omarchy-wddm-normal-boot.service",
        "[Unit]\nDescription=Read-only normal Omarchy boot prerequisite\nAfter=local-fs.target systemd-journald.service\n"
        "[Service]\nType=simple\nExecStart=/usr/local/lib/try-omarchy/wddm-normal-boot-check\n"
        "StandardOutput=journal+console\nStandardError=journal+console\nTimeoutStartSec=30\n",0644);
    if(symlink("../omarchy-wddm-normal-boot.service","/newroot/etc/systemd/system/multi-user.target.wants/omarchy-wddm-normal-boot.service")) fail("boot-check-service-link");
    directory("/newroot/dev",0755); directory("/newroot/proc",0555); directory("/newroot/sys",0555);
    if(mount("/dev","/newroot/dev",NULL,MS_MOVE,NULL) || mount("/proc","/newroot/proc",NULL,MS_MOVE,NULL) ||
       mount("/sys","/newroot/sys",NULL,MS_MOVE,NULL)) fail("move-early-mounts");
    puts("NORMAL_ROOT_SWITCH_ROOT sourceBlockReadonly=true overlayBytes=536870912 graphics=false");
    if(chdir("/newroot") || mount(".","/",NULL,MS_MOVE,NULL) || chroot(".") || chdir("/")) fail("switch-root");
    char* const arguments[]={"/sbin/init",NULL};
    char* const environment[]={"PATH=/usr/local/sbin:/usr/local/bin:/usr/bin:/usr/sbin:/bin:/sbin","TERM=linux",NULL};
    execve(arguments[0],arguments,environment); fail("exec-normal-systemd");
}
