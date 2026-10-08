// SPDX-License-Identifier: MIT
// Private, disk-free Linux runtime diagnostic. No guest disk is attached.
#define _DEFAULT_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static void timeout_handler(int signal_number) { (void)signal_number; }
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    struct rlimit core = {0, 0}; setrlimit(RLIMIT_CORE, &core);
    mkdir("/dev", 0755); mkdir("/proc", 0755); mkdir("/sys", 0755); mkdir("/tmp", 01777);
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) && errno != EBUSY) perror("devtmpfs");
    mount("proc", "/proc", "proc", 0, NULL); mount("sysfs", "/sys", "sysfs", 0, NULL);
    mkdir("/dev/shm", 01777); mount("tmpfs", "/dev/shm", "tmpfs", 0, "mode=1777");
    const char* port = "/dev/vport0p1";
    for (int i = 0; i < 200 && access(port, F_OK); ++i) usleep(50000);
    char module[130] = {0};
    FILE* name = fopen("/linux-umd-name", "rb");
    int status = 127;
    if (name && fread(module, 1, sizeof(module) - 1, name) && !access(port, F_OK)) {
        module[strcspn(module, "\r\n")] = 0;
        pid_t child = fork();
        if (child == 0) {
            setenv("WDDM_BRIDGE_PORT", port, 1);
            setenv("WDDM_BRIDGE_LINUX_UMD_NAME", module, 1);
            setenv("LD_PRELOAD", "/linux-ioctl-bridge.so", 1);
            setenv("LD_LIBRARY_PATH", "/usr/lib/wsl/lib:/usr/lib/x86_64-linux-gnu", 1);
            if (!access("/runtime-tracer", X_OK)) {
                unsetenv("LD_PRELOAD");
                execl("/runtime-tracer", "/runtime-tracer", "-f", "-e", "status=failed", "-e",
                      "trace=memory,file,eventfd2,memfd_create,prlimit64", "-o", "/dev/console",
                      "-E", "LD_PRELOAD=/linux-ioctl-bridge.so", "/guest-probe", (char*)NULL);
                perror("exec tracer"); _exit(127);
            }
            execl("/guest-probe", "/guest-probe", (char*)NULL); perror("exec runtime probe"); _exit(127);
        }
        if (child > 0) {
            struct sigaction action = {0}; action.sa_handler = timeout_handler;
            sigaction(SIGALRM, &action, NULL); alarm(40);
            if (waitpid(child, &status, 0) == child) status = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
            else { kill(child, SIGKILL); waitpid(child, NULL, 0); status = 124; }
            alarm(0);
        }
    } else puts("FAIL: runtime name or virtio port missing");
    if (name) fclose(name);
    printf("BRIDGE_RUNTIME_EXIT=%d\n", status);
    sync(); reboot(RB_POWER_OFF);
    for (;;) pause();
}
