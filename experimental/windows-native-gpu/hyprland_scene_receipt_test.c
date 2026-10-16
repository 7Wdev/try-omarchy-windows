// SPDX-License-Identifier: MIT
// Exercise receipt boundaries with another process sharing the same console.
#define WDDM_BRIDGE_COMPOSITOR_HOST_PRESENT 1
#define WDDM_BRIDGE_COMPOSITOR_HOST_SCENE 1
#define __libc_start_main unused_compositor_interposer_entry
#include "hyprland_startup_probe.c"
#undef __libc_start_main

int main(void) {
    const char* receipt="HYPRLAND_SCENE_CLOSED clientUnmapped=true childExitMarker=true\n";
    const char* other="LINUX_BRIDGE competing-process-log\n";
    int channel[2];
    if(pipe(channel)) return 1;
    pid_t children[2];
    for(unsigned writer=0;writer<2;++writer) {
        children[writer]=fork();
        if(children[writer]<0) return 1;
        if(!children[writer]) {
            close(channel[0]);
            if(dup2(channel[1],STDOUT_FILENO)<0) _exit(1);
            close(channel[1]);
            for(unsigned line=0;line<1000;++line) {
                if(writer ? write(STDOUT_FILENO,other,strlen(other))!=(ssize_t)strlen(other) : scene_receipt(receipt)) _exit(2);
            }
            // A failed console write must invalidate acceptance.
            if(!writer && (close(STDOUT_FILENO) || !scene_receipt(receipt) || !atomic_load(&queue_failed))) _exit(3);
            _exit(0);
        }
    }
    close(channel[1]);
    FILE* stream=fdopen(channel[0],"r");
    if(!stream) return 1;
    char line[128];unsigned receipts=0,competing=0;
    while(fgets(line,sizeof line,stream)) {
        if(!strcmp(line,receipt)) ++receipts;
        else if(!strcmp(line,other)) ++competing;
        else return 1;
    }
    if(ferror(stream) || fclose(stream) || receipts!=1000 || competing!=1000) return 1;
    for(unsigned writer=0;writer<2;++writer) {
        int status=0;
        if(waitpid(children[writer],&status,0)!=children[writer] || !WIFEXITED(status) || WEXITSTATUS(status)) return 1;
    }
    puts("PASS: 2000 concurrent console lines retain boundaries; failed receipt rejects acceptance; gpu=false");
    return 0;
}
