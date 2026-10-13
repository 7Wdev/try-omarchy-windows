"""Private Mesa packaging bounds using inert ELF fixtures, never vendor code."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import make_runtime_initramfs


@unittest.skipUnless(sys.platform == 'linux', 'The private initramfs packer runs on Linux')
class OpenGlPackaging(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='opengl-packaging-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.elf = bytearray(64); self.elf[:6] = b'\x7fELF\x02\x01'; self.elf[18:20] = b'\x3e\x00'
        self.input = self.base/'inert-fixture.so'; self.input.write_bytes(self.elf)
        self.output = self.base/'private.cpio.gz'
        self.mesa = self.base/'mesa'; (self.mesa/'lib').mkdir(parents=True)
        (self.mesa/'lib/libEGL.so.1').write_bytes(self.elf)

    def run_packer(self, *extra):
        argv = ['make_runtime_initramfs.py']
        for key in ('init','probe','shim','linux-umd','d3d12','d3d12core','dxcore'):
            argv += ['--'+key,str(self.input)]
        argv += ['--output',str(self.output)] + list(extra)
        with patch.object(sys,'argv',argv), patch.object(subprocess,'run',return_value=subprocess.CompletedProcess([],0,stdout='')):
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                make_runtime_initramfs.main()

    def test_requires_explicit_paired_workload_and_mesa(self):
        for extra in (('--runtime-workload','opengl'), ('--mesa-runtime-directory',str(self.mesa))):
            with self.subTest(extra=extra), self.assertRaises(SystemExit): self.run_packer(*extra)
        self.assertFalse(self.output.exists())

    def test_exit_probe_requires_opengl_and_valid_elf(self):
        with self.assertRaises(SystemExit): self.run_packer('--exit-lifetime-probe', str(self.input))
        bad = self.base/'bad-exit-probe.so'; bad.write_bytes(b'not an ELF')
        with self.assertRaises(SystemExit):
            self.run_packer('--runtime-workload', 'opengl', '--mesa-runtime-directory', str(self.mesa),
                            '--exit-lifetime-probe', str(bad))
        self.assertFalse(self.output.exists())

    def test_exit_probe_is_explicitly_packaged_and_hashed(self):
        self.run_packer('--runtime-workload', 'opengl', '--mesa-runtime-directory', str(self.mesa),
                        '--exit-lifetime-probe', str(self.input))
        manifest = json.loads(self.output.with_suffix('.manifest.json').read_text())
        probe = next(f for f in manifest['files'] if f['path'] == '/runtime-exit-probe.so')
        self.assertEqual(probe['bytes'], len(self.elf))
        self.assertEqual(len(probe['sha256']), 64)

    def test_bounded_relocation_manifest(self):
        self.run_packer('--runtime-workload','opengl','--mesa-runtime-directory',str(self.mesa))
        manifest = json.loads(self.output.with_suffix('.manifest.json').read_text())
        self.assertTrue(manifest['mesaFrontendIncluded'])
        self.assertFalse(manifest['redistributionApproved'])
        self.assertFalse(manifest['guestBrokerContractIncluded'])
        self.assertFalse(manifest['guestGraphicsBrokerIncluded'])
        self.assertEqual([f['path'] for f in manifest['files'] if f['path'].startswith('/opt/')], ['/opt/wddm-mesa/lib/libEGL.so.1'])

    def test_broker_contract_requires_paired_bounded_elf_inputs(self):
        for key in ('--guest-broker-contract', '--guest-broker-finalizer'):
            with self.subTest(key=key), self.assertRaises(SystemExit): self.run_packer(key,str(self.input))
        invalid=self.base/'invalid-broker.so'; invalid.write_bytes(b'not an ELF')
        with self.assertRaises(SystemExit):
            self.run_packer('--guest-broker-contract',str(self.input),'--guest-broker-finalizer',str(invalid))
        self.assertFalse(self.output.exists())

    def test_broker_contract_is_explicitly_packaged_and_hashed(self):
        self.run_packer('--guest-broker-contract',str(self.input),'--guest-broker-finalizer',str(self.input))
        manifest=json.loads(self.output.with_suffix('.manifest.json').read_text())
        self.assertTrue(manifest['guestBrokerContractIncluded'])
        files={f['path']:f for f in manifest['files']}
        for name in ('/broker-peer-contract','/broker-finalizer-probe.so'):
            self.assertEqual(files[name]['bytes'],len(self.elf))
            self.assertEqual(len(files[name]['sha256']),64)

    def test_external_library_symlink_rejected(self):
        (self.mesa/'lib/libOutside.so').symlink_to(self.input)
        with self.assertRaises(SystemExit): self.run_packer('--runtime-workload','opengl','--mesa-runtime-directory',str(self.mesa))
        self.assertFalse(self.output.exists())

    def broker_kernel_arguments(self):
        module=self.base/'broker-wddm-aperture.ko'
        data=bytearray(self.elf); data[16:18]=b'\x01\x00'; data.extend(b'name=wddm_aperture\0'); module.write_bytes(data)
        return ['--guest-aperture-module',str(module),'--guest-aperture-contract',str(self.input)]

    def test_graphics_broker_requires_kernel_and_supported_workload(self):
        with self.assertRaises(SystemExit): self.run_packer('--guest-graphics-broker',str(self.input))
        with self.assertRaises(SystemExit):
            self.run_packer(*self.broker_kernel_arguments(),'--guest-graphics-broker',str(self.input),'--runtime-workload','shared')
        self.assertFalse(self.output.exists())

    def test_graphics_broker_elf_is_validated(self):
        bad=self.base/'invalid-graphics-broker'; bad.write_bytes(b'not an ELF')
        with self.assertRaises(SystemExit):
            self.run_packer(*self.broker_kernel_arguments(),'--guest-graphics-broker',str(bad))
        self.assertFalse(self.output.exists())

    def test_graphics_broker_is_explicitly_packaged_and_hashed(self):
        self.run_packer(*self.broker_kernel_arguments(),'--guest-graphics-broker',str(self.input))
        manifest=json.loads(self.output.with_suffix('.manifest.json').read_text())
        self.assertTrue(manifest['guestGraphicsBrokerIncluded'])
        broker=next(f for f in manifest['files'] if f['path']=='/guest-graphics-broker')
        self.assertEqual(broker['bytes'],len(self.elf)); self.assertEqual(len(broker['sha256']),64)

    def test_native_texture_broker_requires_and_records_its_explicit_workload(self):
        self.run_packer(*self.broker_kernel_arguments(),'--guest-graphics-broker',str(self.input),'--runtime-workload','native')
        manifest=json.loads(self.output.with_suffix('.manifest.json').read_text())
        self.assertEqual(manifest['runtimeWorkload'],'native')
        self.assertTrue(manifest['guestGraphicsBrokerIncluded'])
        self.assertTrue(manifest['kernelApertureIncluded'])

    def test_library_count_bound_rejected(self):
        for i in range(32): (self.mesa/f'lib/libFixture{i}.so').write_bytes(self.elf)
        with self.assertRaises(SystemExit): self.run_packer('--runtime-workload','opengl','--mesa-runtime-directory',str(self.mesa))
        self.assertFalse(self.output.exists())

    def test_missing_egl_or_wrong_elf_rejected(self):
        (self.mesa/'lib/libEGL.so.1').write_bytes(b'not an ELF')
        with self.assertRaises(SystemExit): self.run_packer('--runtime-workload','opengl','--mesa-runtime-directory',str(self.mesa))
        (self.mesa/'lib/libEGL.so.1').unlink()
        with self.assertRaises(SystemExit): self.run_packer('--runtime-workload','opengl','--mesa-runtime-directory',str(self.mesa))

    def test_existing_manifest_is_never_overwritten(self):
        manifest = self.output.with_suffix('.manifest.json'); manifest.write_text('retained')
        with self.assertRaises(SystemExit): self.run_packer('--runtime-workload','opengl','--mesa-runtime-directory',str(self.mesa))
        self.assertEqual(manifest.read_text(),'retained')
        self.assertFalse(self.output.exists())

    def test_kernel_inputs_must_be_paired(self):
        for key in ('guest-aperture-module','guest-aperture-contract'):
            with self.subTest(key=key), self.assertRaises(SystemExit):
                self.run_packer('--'+key,str(self.input))
        self.assertFalse(self.output.exists())

    def test_kernel_module_identity_and_elf_type(self):
        module=self.base/'wddm_aperture.ko'
        valid=bytearray(self.elf); valid[16:18]=b'\x01\x00'; valid.extend(b'name=wddm_aperture\0')
        for bad in (b'not a module', self.elf, valid[:64],
                    valid.replace(b'wddm_aperture',b'other_module_'),
                    valid[:18]+b'\x28\x00'+valid[20:]):
            module.write_bytes(bad)
            with self.subTest(data=bad[:20]), self.assertRaises(SystemExit):
                self.run_packer('--guest-aperture-module',str(module),'--guest-aperture-contract',str(self.input))
        self.assertFalse(self.output.exists())

    def test_kernel_module_size_bound(self):
        module=self.base/'wddm_aperture.ko'
        module.write_bytes(b'\0'*(16*1024*1024+1))
        with self.assertRaises(SystemExit):
            self.run_packer('--guest-aperture-module',str(module),'--guest-aperture-contract',str(self.input))
        self.assertFalse(self.output.exists())

    def test_kernel_module_explicit_manifest(self):
        module=self.base/'wddm_aperture.ko'
        valid=bytearray(self.elf); valid[16:18]=b'\x01\x00'; valid.extend(b'name=wddm_aperture\0')
        module.write_bytes(valid)
        self.run_packer('--guest-aperture-module',str(module),'--guest-aperture-contract',str(self.input))
        manifest=json.loads(self.output.with_suffix('.manifest.json').read_text())
        self.assertTrue(manifest['kernelApertureIncluded'])
        self.assertIn('/wddm-aperture.ko',[f['path'] for f in manifest['files']])
        self.assertIn('/aperture-contract',[f['path'] for f in manifest['files']])
        self.assertFalse(manifest['redistributionApproved'])


if __name__ == '__main__':
    unittest.main()
