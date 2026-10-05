#!/usr/bin/env python3
"""Compile the actual WHP baseline setup against a deterministic WHP fixture."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
args = parser.parse_args()
source = (args.source / 'target/i386/whpx/whpx-all.c').read_text()
start = source.index('static int whpx_setup_model(')
body = source[start:source.index('\nstatic int whpx_setup_host(', start)]
fields = re.findall(r'MODEL_FEATURE\((\w+),', body)[1:]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
typedef int HRESULT;
#define FAILED(hr) ((hr) < 0)
#define WHvCapabilityCodeProcessorFeaturesBanks 1
#define WHvPartitionPropertyCodeProcessorFeaturesBanks 2
#define WHvPartitionPropertyCodeProcessorXsaveFeatures 3
typedef struct { unsigned cpuid_level; } CPUX86State;
typedef struct { uint64_t AsUINT64; } WHV_PROCESSOR_XSAVE_FEATURES;
typedef struct { unsigned BanksCount; struct {
''' + '\n'.join('unsigned '+field+';' for field in fields) + r'''
} Bank0; struct { uint64_t AsUINT64; } Bank1; } WHV_PROCESSOR_FEATURES_BANKS;
struct whpx_state { int partition; } whpx_global;
static WHV_PROCESSOR_XSAVE_FEATURES whpx_xsave_cap;
static bool whpx_partition_ready;
static int calls, failing, initialized;
static uint32_t extra;
static WHV_PROCESSOR_FEATURES_BANKS result;
static void cpu_x86_cpuid(CPUX86State *env, uint32_t leaf, uint32_t sub,
                         uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    *a = *b = *c = *d = 0;
    if (leaf == 1) *c = extra | (1U<<0) | (1U<<9) | (1U<<19) |
        (1U<<20) | (1U<<23) | (1U<<25);
    if (leaf == 0x80000001) *c = 1;
}
static HRESULT get_cap(int code, void *value, unsigned size, void *actual) {
    assert(code == 1);
    WHV_PROCESSOR_FEATURES_BANKS *banks = value;
    assert(banks->BanksCount == 2);
''' + '\n'.join('    banks->Bank0.'+field+' = 1;' for field in fields) + r'''
    banks->Bank1.AsUINT64 = UINT64_MAX;
    return 0;
}
static HRESULT set_prop(int part, int code, void *value, unsigned size) {
    if (failing) return -1;
    if (code == 2) {
        assert(calls++ == 0);
        result = *(WHV_PROCESSOR_FEATURES_BANKS *)value;
    } else {
        assert(code == 3 && calls++ == 1);
        assert(((WHV_PROCESSOR_XSAVE_FEATURES *)value)->AsUINT64 == 0);
    }
    return 0;
}
static HRESULT setup(int part) { assert(calls++ == 2); return 0; }
static struct { HRESULT (*WHvGetCapability)(int,void*,unsigned,void*);
    HRESULT (*WHvSetPartitionProperty)(int,int,void*,unsigned);
    HRESULT (*WHvSetupPartition)(int); } whp_dispatch = {get_cap,set_prop,setup};
static void whpx_memory_init(void) { assert(whpx_partition_ready); initialized++; }
static void whpx_init_emu(void) { initialized++; }
#define error_report(...) ((void)0)
''' + body + r'''
int main(void) {
    CPUX86State env = { .cpuid_level = 4 };
    assert(whpx_setup_model(&env) == 0);
    assert(calls == 3 && initialized == 2 && whpx_xsave_cap.AsUINT64 == 0);
    assert(result.Bank0.AesSupport && result.Bank0.Sse4_2Support);
    assert(!result.Bank0.XopSupport && !result.Bank0.Fma4Support);
    assert(!result.Bank0.F16CSupport && !result.Bank0.Bmi2Support);
    assert(!result.Bank1.AsUINT64);
    for (int bit = 0; bit < 4; bit++) {
        unsigned bits[] = {26,28,12,29};
        calls = initialized = 0; whpx_partition_ready = false;
        extra = 1U << bits[bit];
        assert(whpx_setup_model(&env) == -EINVAL);
        assert(!whpx_partition_ready && initialized == 0 && calls == 1);
    }
    extra = 0; failing = 1;
    assert(whpx_setup_model(&env) == -EINVAL);
    assert(!whpx_partition_ready && initialized == 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='whpx-model-') as directory:
    root = Path(directory)
    (root / 'test.c').write_text(fixture)
    subprocess.run(['gcc', '-std=gnu11', '-O2', str(root / 'test.c'), '-o', str(root / 'test.exe')], check=True)
    subprocess.run([str(root / 'test.exe')], check=True)
print('ok - WHP qemu64 feature masks, zero XSAVE, setup ordering and fail-closed errors')
