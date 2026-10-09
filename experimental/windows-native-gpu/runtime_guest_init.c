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
    char command[512] = {0};
    FILE* cmdline = fopen("/proc/cmdline", "rb");
    if (cmdline) { const size_t bytes = fread(command, 1, sizeof(command) - 1, cmdline); command[bytes] = 0; fclose(cmdline); }
    const char* store_option = strstr(command, " wddm_cpu_store_test=1");
    const size_t option_bytes = sizeof(" wddm_cpu_store_test=1") - 1;
    const int cpu_store_test = store_option && (store_option[option_bytes] == 0 || store_option[option_bytes] == ' ' || store_option[option_bytes] == '\n');
    const char* eof_option = strstr(command, " wddm_cpu_eof_test=1");
    const size_t eof_bytes = sizeof(" wddm_cpu_eof_test=1") - 1;
    const int cpu_eof_test = eof_option && (eof_option[eof_bytes] == 0 || eof_option[eof_bytes] == ' ' || eof_option[eof_bytes] == '\n');
    const char* span_eof_option = strstr(command, " wddm_cpu_span_eof_test=1");
    const size_t span_eof_bytes = sizeof(" wddm_cpu_span_eof_test=1") - 1;
    const int cpu_span_eof_test = span_eof_option && (span_eof_option[span_eof_bytes] == 0 || span_eof_option[span_eof_bytes] == ' ' || span_eof_option[span_eof_bytes] == '\n');
    const char* sync_eof_option = strstr(command, " wddm_sync_eof_test=1");
    const size_t sync_eof_bytes = sizeof(" wddm_sync_eof_test=1") - 1;
    const int sync_eof_test = sync_eof_option && (sync_eof_option[sync_eof_bytes] == 0 ||
        sync_eof_option[sync_eof_bytes] == ' ' || sync_eof_option[sync_eof_bytes] == '\n');
    const char* sync_no_max_eof_option = strstr(command, " wddm_sync_no_max_eof_test=1");
    const size_t sync_no_max_eof_bytes = sizeof(" wddm_sync_no_max_eof_test=1") - 1;
    const int sync_no_max_eof_test = sync_no_max_eof_option && (sync_no_max_eof_option[sync_no_max_eof_bytes] == 0 ||
        sync_no_max_eof_option[sync_no_max_eof_bytes] == ' ' || sync_no_max_eof_option[sync_no_max_eof_bytes] == '\n');
    const char* reservation_eof_option = strstr(command, " wddm_reservation_eof_test=1");
    const size_t reservation_eof_bytes = sizeof(" wddm_reservation_eof_test=1") - 1;
    const int reservation_eof_test = reservation_eof_option && (reservation_eof_option[reservation_eof_bytes] == 0 ||
        reservation_eof_option[reservation_eof_bytes] == ' ' || reservation_eof_option[reservation_eof_bytes] == '\n');
    const char* gpu_state_eof_option = strstr(command, " wddm_gpu_state_eof_test=1");
    const size_t gpu_state_eof_bytes = sizeof(" wddm_gpu_state_eof_test=1") - 1;
    const int gpu_state_eof_test = gpu_state_eof_option && (gpu_state_eof_option[gpu_state_eof_bytes] == 0 ||
        gpu_state_eof_option[gpu_state_eof_bytes] == ' ' || gpu_state_eof_option[gpu_state_eof_bytes] == '\n');
    const char* hwqueue_eof_option = strstr(command, " wddm_hwqueue_eof_test=1");
    const size_t hwqueue_eof_bytes = sizeof(" wddm_hwqueue_eof_test=1") - 1;
    const int hwqueue_eof_test = hwqueue_eof_option && (hwqueue_eof_option[hwqueue_eof_bytes] == 0 ||
        hwqueue_eof_option[hwqueue_eof_bytes] == ' ' || hwqueue_eof_option[hwqueue_eof_bytes] == '\n');
    const char* hwqueue_no_broadcast_option = strstr(command, " wddm_hwqueue_no_broadcast_eof_test=1");
    const size_t hwqueue_no_broadcast_bytes = sizeof(" wddm_hwqueue_no_broadcast_eof_test=1") - 1;
    const int hwqueue_no_broadcast_eof_test = hwqueue_no_broadcast_option &&
        (hwqueue_no_broadcast_option[hwqueue_no_broadcast_bytes] == 0 || hwqueue_no_broadcast_option[hwqueue_no_broadcast_bytes] == ' ' ||
         hwqueue_no_broadcast_option[hwqueue_no_broadcast_bytes] == '\n');
    const char* port = "/dev/vport0p1";
    for (int i = 0; i < 200 && access(port, F_OK); ++i) usleep(50000);
    char module[130] = {0};
    FILE* name = fopen("/linux-umd-name", "rb");
    int status = 127;
    if (name && fread(module, 1, sizeof(module) - 1, name) && !access(port, F_OK)) {
        module[strcspn(module, "\r\n")] = 0;
        pid_t child = fork();
        if (child == 0) {
            FILE* workload_file = fopen("/runtime-workload", "rb");
            if (workload_file) {
                char workload[8] = {0}; const size_t count = fread(workload, 1, sizeof(workload), workload_file);
                fclose(workload_file);
                if (count == 4 && !memcmp(workload, "copy", 4)) setenv("WDDM_BRIDGE_RUNTIME_WORKLOAD", "copy", 1);
                else if (count == 5 && !memcmp(workload, "clear", 5)) setenv("WDDM_BRIDGE_RUNTIME_WORKLOAD", "clear", 1);
                else if (count != 4 || memcmp(workload, "init", 4)) _exit(126);
            }
            setenv("WDDM_BRIDGE_PORT", port, 1);
            if (cpu_store_test) {
                setenv("WDDM_BRIDGE_CPU_STORE_TEST", "1", 1);
                setenv("WDDM_BRIDGE_CPU_REFERENCE_TEST", "1", 1);
            }
            if (cpu_eof_test) setenv("WDDM_BRIDGE_CPU_EOF_TEST", "1", 1);
            if (cpu_span_eof_test) setenv("WDDM_BRIDGE_CPU_SPAN_EOF_TEST", "1", 1);
            if (sync_eof_test) setenv("WDDM_BRIDGE_SYNC_EOF_TEST", "1", 1);
            if (sync_no_max_eof_test) setenv("WDDM_BRIDGE_SYNC_NO_MAX_EOF_TEST", "1", 1);
            if (reservation_eof_test) setenv("WDDM_BRIDGE_RESERVATION_EOF_TEST", "1", 1);
            if (gpu_state_eof_test) setenv("WDDM_BRIDGE_GPU_STATE_EOF_TEST", "1", 1);
            if (hwqueue_eof_test) setenv("WDDM_BRIDGE_HWQUEUE_EOF_TEST", "1", 1);
            if (hwqueue_no_broadcast_eof_test) setenv("WDDM_BRIDGE_HWQUEUE_EOF_TEST", "2", 1);
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
