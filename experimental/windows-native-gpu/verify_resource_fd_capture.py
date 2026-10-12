"""Audit the bounded native-resource FD diagnostic, never dma-buf or desktop acceptance."""
import argparse
from functools import lru_cache
import json
from pathlib import Path
import re
from verify_4k_capture import require
from verify_kernel_aperture_capture import validate_kernel_aperture
from verify_opengl_capture import validate_opengl_cleanup
from test_owned_runtime_qemu import async_submissions_complete


@lru_cache(maxsize=2)
def uniform_digest(rgba):
    # Independent reference for exactly 3840x2160 tightly packed RGBA8 pixels.
    value = 14695981039346656037
    pixel = bytes.fromhex(rgba)
    for _ in range(3840 * 2160):
        for byte in pixel:
            value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f'{value:016x}'


def validate_resource_fds(report, guest, records):
    result = validate_kernel_aperture(report, guest, records)
    lines = guest.splitlines()
    def exactly(line):
        require(lines.count(line) == 1, 'Missing or duplicated resource-FD receipt: ' + line)
        return lines.index(line)
    start = exactly('MESA_RESOURCE_FD_BEGIN width=3840 height=2160 format=28 native=true dmaBuf=false desktop=false')
    created = exactly('MESA_RESOURCE_FD_TEXTURE_CREATED width=3840 height=2160 shared=true')
    exported = exactly('MESA_RESOURCE_FD_EXPORTED native=true rawHostHandle=false')
    imported = exactly('MESA_RESOURCE_FD_IMPORTED fdClosed=true descriptionMatches=true')
    complete = exactly('MESA_RESOURCE_FD_COMPLETE rounds=2 pixelsPerRound=8294400 native=true dmaBuf=false desktop=false')
    exactly('RESOURCE_FD_CONTRACT_PASS checks=15 held=0 active=0 typeIsolation=true ownerIsolation=true dupLifetime=true dmaBuf=false')
    require(not any(token in guest for token in ('MESA_RESOURCE_FD_FAILURE', 'MESA_RESOURCE_FD_NEGATIVE_CONTROL', 'RESOURCE_FD_CONTRACT_FAIL', 'D3D12: Removing Device.')),
            'Resource-FD or device failure')
    pattern = (r'^MESA_RESOURCE_FD_SHARED_STORAGE round=(\d+) writer=(source|imported) importedRead=true '
               r'sourceReleased=(true|false) fdClosed=true width=3840 height=2160 pixels=8294400 '
               r'rgba=([0-9a-f]{8}) bands=5 readbackBytes=7864320 fnv1a=([0-9a-f]{16})$')
    receipts = [line for line in lines if line.startswith('MESA_RESOURCE_FD_SHARED_STORAGE')]
    require(len(receipts) == 2, 'Missing or duplicated shared-storage round')
    rounds = []
    for line, expected in zip(receipts, ((1, 'source', 'false', 'ff0000ff'), (2, 'imported', 'true', '00ff00ff'))):
        match = re.fullmatch(pattern, line)
        require(match is not None, 'Malformed shared-storage receipt')
        number, writer, released, rgba, digest = match.groups()
        require((int(number), writer, released, rgba) == expected, 'Wrong storage ownership phase or pixels')
        require(digest == uniform_digest(rgba), 'Shared-storage digest differs from the independent full-image reference')
        rounds.append(dict(round=int(number), writer=writer, sourceReleased=released == 'true',
                           verifiedPixels=3840 * 2160, rgba=rgba, fnv1a=digest))
    require(start < created < exported < imported < lines.index(receipts[0]) < lines.index(receipts[1]) < complete,
            'Invalid export/import/storage ordering')
    block = '\n'.join(lines[imported:complete])
    targets = re.findall(r'^LINUX_BRIDGE nativeHwQueueSignalQueued=true flags=4 queues=1 target=(\d+) observedAtReturn=\d+ noGpuAccess=true$', block, re.M)
    require([int(n) for n in targets] == list(range(1, 11)), 'Missing or reordered band retirement signals')
    tickets = re.findall(r'^LINUX_BRIDGE kernelObjectExported=true kind=resource ticket=(\d+) cookie=(\d+)$', guest, re.M)
    require(len(tickets) == 1 and all(int(n) > 0 for n in tickets[0]), 'Missing typed resource FD export')
    ticket, _ = tickets[0]
    reaped = exactly('LINUX_BRIDGE kernelObjectReaped=true ticket=' + ticket)
    require(imported < reaped < lines.index(receipts[0]), 'FD reference was not reaped after import and before storage proof')
    def one(key):
        events = [event for event in records if event.get(key) is True]
        require(len(events) == 1, 'Missing or duplicated native event: ' + key)
        return events[0]
    negotiated = one('nativeResourceSharingNegotiated')
    native_export = one('nativeResourceHandleExported')
    require(native_export.get('rawHandleExposed') is False, 'Native resource HANDLE exposed')
    open_begin = one('nativeResourceFdOpenBegin')
    native_open = one('nativeResourceFdOpenReturned')
    require(native_open.get('ntstatus') == 0 and native_open.get('hasResource') is True and native_open.get('hasAllocation') is True and
            native_open.get('hasKeyedMutex') is False and native_open.get('hasSync') is False, 'Invalid native resource-open result')
    native_import = one('nativeResourceFdImported')
    require(native_import.get('allocations') == 1 and native_import.get('rawHandleExposed') is False, 'Invalid native resource import')
    queries = [event for event in records if event.get('nativeResourceInfoQueried') is True]
    require(len(queries) == 2 and queries[0] == queries[1], 'Inconsistent native metadata queries')
    info = queries[0]
    require(0 < info.get('guestRuntimeBytes', 0) <= 1024 and 0 < info.get('hostRuntimeBytes', 0) <= 4000 and
            0 < info.get('driverBytes', 0) <= 4000 and info.get('resourceDriverBytes') == 0 and info.get('allocations') == 1,
            'Unbounded or unsupported native resource metadata')
    require(native_open.get('runtimeBytes') == info['hostRuntimeBytes'] and native_open.get('driverBytes') == info['driverBytes'] and
            native_open.get('allocationDriverBytes') == info['driverBytes'] and native_open.get('resourceDriverBytes') == 0 and
            native_import.get('guestRuntimeBytes') == info['guestRuntimeBytes'] and native_import.get('driverBytes') == info['driverBytes'],
            'Native/Linux resource metadata substitution')
    native_closed = one('nativeResourceHandleClosed')
    require(records.index(negotiated) < records.index(native_export) < records.index(queries[0]) <
            records.index(open_begin) < records.index(native_open) < records.index(native_import) < records.index(native_closed),
            'Invalid native NT-reference lifetime or import ordering')
    cleanup = report['disconnectCleanup']
    require(cleanup.get('exportedResourceHandles') == 1 and cleanup.get('closedResourceHandles') == 1 and
            cleanup.get('liveSharedResourceHandles') == 0 and cleanup.get('vendorCpuSlotQuotaRejections') == 0,
            'Resource handle leak or CPU aperture exhaustion')
    result.update(nativeResourceFdVerified=True, sharedStorageVerified=True, width=3840, height=2160,
                  rounds=rounds, readbackBandsPerRound=5, diagnosticReadbackBytes=7864320,
                  sourceAndFdReleaseVerified=True, resourceFdContractChecks=15,
                  drmDeviceVerified=False, dmaBufVerified=False, desktopVerified=False,
                  openGl4kVerified=False, nearNativePerformanceVerified=False)
    return result


def validate_resource_fd_control(report, guest, records):
    """An explicit actual-GPU wrong-color run must fail, not become a milestone."""
    validate_opengl_cleanup(report, records)
    lines = guest.splitlines()
    require(report.get('runtimeWorkload') == 'opengl' and report.get('hostBridgeExit') == 0, 'Wrong control workload or native exit')
    require(re.findall(r'^BRIDGE_RUNTIME_EXIT=(\d+)$', guest, re.M) == ['1'] and 'BRIDGE_RUNTIME_SIGNAL' not in guest,
            'Wrong negative-control guest exit')
    require(lines.count('MESA_RESOURCE_FD_NEGATIVE_CONTROL kind=sourceColor expectedFailure=SharedPixels') == 1 and
            len(re.findall(r'^MESA_RESOURCE_FD_FAILURE stage=SharedPixels status=80004005 errno=\d+$', guest, re.M)) == 1 and
            lines.count('OPENGL_FAILURE stage=SharedResourceFd eglError=3000') == 1,
            'Missing actual GPU color-mismatch failure')
    require('MESA_RESOURCE_FD_COMPLETE' not in guest and 'MESA_RESOURCE_FD_SHARED_STORAGE' not in guest and 'Removing Device' not in guest,
            'Control reported a success or unrelated device failure')
    require(lines.count('MESA_RESOURCE_FD_IMPORTED fdClosed=true descriptionMatches=true') == 1 and
            lines.count('KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true') == 1,
            'Missing real import or kernel cleanup in control')
    imported = [r for r in records if r.get('nativeResourceFdImported') is True]
    require(len(imported) == 1 and imported[0].get('rawHandleExposed') is False, 'Missing native resource import')
    for key, marker in (('nativeAsyncSubmissionAcceptances', 'nativeAsyncSubmissionAccepted'),
                        ('nativeAsyncSubmissionRetirements', 'nativeAsyncSubmissionRetired')):
        actual = [r for r in records if marker in r]
        require(report[key] == actual, 'Control/native command receipt mismatch')
    require(async_submissions_complete(report['guestCommandEnqueues'], report['nativeAsyncSubmissionAcceptances'],
            report['nativeAsyncSubmissionRetirements'], report['disconnectCleanup'], maximum=64), 'Unretired control GPU commands')
    cleanup = report['disconnectCleanup']
    require(cleanup.get('exportedResourceHandles') == cleanup.get('closedResourceHandles') == 1 and
            cleanup.get('liveSharedResourceHandles') == cleanup.get('vendorCpuSlotQuotaRejections') == 0,
            'Control resource leak or aperture exhaustion')
    return dict(schema=1, intentionalWrongColorRejected=True, guestExit=1, nativeCleanupVerified=True,
                nativeResourceFdImported=True, sharedStorageVerified=False, desktopVerified=False,
                dmaBufVerified=False, nearNativePerformanceVerified=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--wrong-color-control', action='store_true')
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    guest = args.report.with_suffix('.log').read_text()
    records = [json.loads(line) for line in args.report.with_suffix('.host.log').read_text().splitlines() if line.startswith('{')]
    validator = validate_resource_fd_control if args.wrong_color_control else validate_resource_fds
    result = validator(report, guest, records)
    with args.output.open('x', encoding='utf-8') as destination:
        json.dump(result, destination, indent=2); destination.write('\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
