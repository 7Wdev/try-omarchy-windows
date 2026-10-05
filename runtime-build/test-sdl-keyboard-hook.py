#!/usr/bin/env python3
"""Exercise the actual SDL keyboard opt-out with mocked SDL hint calls."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
args = parser.parse_args()
source = (args.source / 'ui/sdl2.c').read_text()
start = source.index('#ifdef _WIN32\n    /* The launcher routes reserved keys')
body = source[start:source.index('#endif', start) + len('#endif')]
fixture = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define _WIN32 1
#define SDL_HINT_GRAB_KEYBOARD 1
#define SDL_HINT_OVERRIDE 2
static const char *selected, *setting;
static const char *test_getenv(const char *name) { return setting; }
#define getenv test_getenv
static void SDL_SetHintWithPriority(int hint, const char *value, int priority) {
    assert(hint == 1 && priority == 2); selected = value;
}
static void configure(void) {
''' + body + r'''
}
int main(void) {
    setting = NULL;
    configure(); assert(!strcmp(selected,"1"));
    setting = "1";
    configure(); assert(!strcmp(selected,"0"));
    setting = "0";
    configure(); assert(!strcmp(selected,"1"));
    setting = "true";
    configure(); assert(!strcmp(selected,"1"));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sdl-hook-') as directory:
    root = Path(directory)
    (root / 'test.c').write_text(fixture)
    subprocess.run(['gcc', '-std=gnu11', '-O2', str(root / 'test.c'), '-o', str(root / 'test.exe')], check=True)
    subprocess.run([str(root / 'test.exe')], check=True)
print('ok - SDL hook defaults, exact opt-out and SDL override priority')
