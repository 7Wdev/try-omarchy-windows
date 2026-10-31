"""Add named graphics-device discovery to the journal-only bounded guest probe.

This remains a diagnostic session. Normal user/account bootstrap is not supplied.
"""
from pathlib import Path
import hashlib
import importlib.util
import json


def generate(gpu, output):
    gpu, output = Path(gpu), Path(output)
    logging = gpu/'launcher-guest-logging/generate.py'
    spec = importlib.util.spec_from_file_location('launcher_guest_logging', logging)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    parent = module.generate(gpu, output)
    target = output/'normal_root_graphics_service.c'
    original = target.read_text(encoding='utf-8')
    before = '    const char* port="/dev/vport0p1";'
    after = '    char port[128]={0};'
    wait = ('    for(unsigned tick=0;tick<200 && access(port,F_OK);++tick) usleep(50000);\n'
            '    if(chmod(port,0600)) { perror("native port permissions");return 125; }')
    discover = '''    int port_ready=0;
    for(unsigned tick=0;tick<200;++tick) {
        if(!wddm_graphics_port_resolve("/sys/class/virtio-ports","/dev",port,sizeof port)) { port_ready=1;break; }
        if(errno!=ENOENT && errno!=ENODEV && errno!=EAGAIN) break;
        usleep(50000);
    }
    if(!port_ready) { perror("named native graphics port");return 125; }
    if(chmod(port,0600)) { perror("native port permissions");return 125; }
    printf("NAMED_NATIVE_GRAPHICS_PORT path=%s name=org.7wdev.wddm characterIdentityVerified=true\\n",port);'''
    include = '#include "guest_session_process.h"'
    expanded = include+'\n#include "guest_graphics_port.h"'
    cleanup = 'cleanup:\n    if(uwsm && compositor_pid>0) {'
    outcome = '''cleanup:
    if(uwsm) {
        puts("NAMED_GRAPHICS_SESSION_OUTCOME_BEGIN normalOmarchySession=false");
        uwsm_inspect(uid,gid,0);
        const pid_t journal=fork();
        if(!journal) {
            execl("/usr/bin/journalctl","journalctl","-b","--no-pager","-n","160","-o","short-monotonic",
                "_SYSTEMD_USER_UNIT=wayland-wm@wddmprobe.desktop.service",(char*)NULL);_exit(127);
        }
        if(journal>0) {
            alarm(5);int status=0;
            if(waitpid(journal,&status,0)!=journal) { kill(journal,SIGKILL);waitpid(journal,NULL,0); }
            alarm(0);
        }
        puts("NAMED_GRAPHICS_SESSION_OUTCOME_END exitStatusAloneIsNotGraphicsAcceptance=true");
    }
    if(uwsm && compositor_pid>0) {'''
    for old in (before, wait, include, cleanup):
        if original.count(old) != 1:
            raise ValueError('Guest graphics discovery boundary changed')
    text = original.replace(before, after, 1).replace(wait, discover, 1).replace(include, expanded, 1).replace(cleanup, outcome, 1)
    if text.replace(after, before, 1).replace(discover, wait, 1).replace(expanded, include, 1).replace(outcome, cleanup, 1) != original:
        raise ValueError('Unexpected guest graphics discovery change')
    target.write_text(text, encoding='utf-8', newline='\n')
    header = output/'guest_graphics_port.h'
    header.write_bytes((gpu/header.name).read_bytes())
    receipt = dict(schema=1, parentLogging=parent,
        generatorSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        generatedSourceSha256=hashlib.sha256(target.read_bytes()).hexdigest(),
        graphicsPortHeaderSha256=hashlib.sha256(header.read_bytes()).hexdigest(),
        discovery='unique sysfs name and matching character-device identity',
        terminalCompositorJournalLines=160, terminalStatusCapturedBeforeUserStop=True,
        fixedPortOrdinal=False, opensOtherDevicePorts=False,
        inputImplementationChanged=False, gpuProtocolChanged=False,
        probeSessionStillBounded=True, normalAccountBootstrapImplemented=False,
        dailyUseAccepted=False)
    (output/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8', newline='\n')
    return receipt


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    print(json.dumps(generate(Path(__file__).resolve().parent.parent, args.output), indent=2))
