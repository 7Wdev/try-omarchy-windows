"""Corrupt actual broker-rendered shader and native-finalizer captures."""
import copy
import json
from pathlib import Path
import unittest
from verify_broker_routing_capture import validate_broker_routing
from verify_opengl_capture import validate_opengl


class BrokerRoutingMilestone(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence=json.loads((Path(__file__).resolve().parents[2]/'docs/evidence/BROKER-ROUTING-2026-10-13.json').read_text())

    def setUp(self): self.run=copy.deepcopy(self.evidence['runs'][0])

    def check(self,run=None):
        run=self.run if run is None else run; p=run['proof']
        return validate_broker_routing(p['report'],'\n'.join(p['guestLines']),p['records'],finalizer=run['finalizer'])

    def test_all_real_guests_pass_only_their_actual_scopes(self):
        for run in self.evidence['runs']:
            result=self.check(run); self.assertEqual(result,run['audit'])
            self.assertTrue(result['brokerRoutingVerified']); self.assertEqual(result['clientUid'],65534)
            self.assertEqual(result['retiredNativeCommands'],18)
            self.assertEqual(result['brokerNativeFinalizerVerified'],run['finalizer'])
            for flag in ('multiClientGpuRoutingVerified','crossProcessGpuResourceImportVerified','gbmVerified',
                         'desktopVerified','desktopStabilityVerified','nearNativePerformanceVerified','openGl4kVerified'):
                self.assertIs(result[flag],False)

    def test_retained_hardware_failures_stay_failures(self):
        for failure in self.evidence['failures']:
            with self.subTest(label=failure['label']),self.assertRaises(ValueError):
                self.check(dict(proof=failure['proof'],finalizer=False))

    def test_wrong_client_worker_privileges_and_lifetime_rejected(self):
        changes=(('uid=65534 gid=65534','uid=0 gid=0'),('effectiveCapabilities=0','effectiveCapabilities=1'),
                 ('noNewPrivs=true','noNewPrivs=false'),('controllerOpen=denied','controllerOpen=allowed'),
                 ('nativePortOpen=denied','nativePortOpen=allowed'),('clientUid=65534','clientUid=0'),
                 ('nativeWorkerClients=1','nativeWorkerClients=2'),('kernelControllersClient=false','kernelControllersClient=true'),
                 ('processExited=true','processExited=false'),('rejected=0','rejected=1'),('multiClient=false','multiClient=true'))
        for before,after in changes:
            self.setUp(); self.run['proof']['guestLines']=[l.replace(before,after) for l in self.run['proof']['guestLines']]
            with self.subTest(after=after),self.assertRaises(ValueError): self.check()

    def test_mapping_and_cleanup_corruptions_rejected(self):
        for before,after in (('retainedViews=96','retainedViews=95'),('viewsCreated=162','viewsCreated=161'),
                             ('viewsReleased=66','viewsReleased=65'),('nativeCalls=1014','nativeCalls=10001'),
                             ('BROKER_ROUTE_REAP exit=0','BROKER_ROUTE_REAP exit=1'),('BRIDGE_RUNTIME_EXIT=0','BRIDGE_RUNTIME_EXIT=1')):
            self.setUp(); self.run['proof']['guestLines']=[l.replace(before,after) for l in self.run['proof']['guestLines']]
            with self.subTest(after=after),self.assertRaises(ValueError): self.check()
        for prefix in ('BROKER_RENDER_CLIENT ','BROKER_ROUTE_START ','BROKER_ROUTE_EXIT ','BROKER_ROUTE_REAP '):
            for duplicate in (False,True):
                self.setUp(); lines=self.run['proof']['guestLines']; line=next(l for l in lines if l.startswith(prefix))
                self.run['proof']['guestLines']=lines+[line] if duplicate else [l for l in lines if l!=line]
                with self.subTest(prefix=prefix,duplicate=duplicate),self.assertRaises(ValueError): self.check()

    def test_direct_and_broker_transports_cannot_be_substituted_or_mixed(self):
        p=self.run['proof']
        with self.assertRaises(ValueError): validate_opengl(p['report'],'\n'.join(p['guestLines']),p['records'],allocation_slots=256)
        for mixed in (False,True):
            self.setUp(); lines=self.run['proof']['guestLines']; direct='LINUX_BRIDGE transport=virtio-port'
            self.run['proof']['guestLines']=lines+[direct] if mixed else [direct if l.startswith('LINUX_BRIDGE transport=') else l for l in lines]
            with self.subTest(mixed=mixed),self.assertRaises(ValueError): self.check()

    def test_real_finalizer_must_complete_before_broker_retirement(self):
        for corrupt in ('missing','late','wrong-value'):
            self.run=copy.deepcopy(self.evidence['runs'][-1]); self.assertTrue(self.run['finalizer'])
            lines=self.run['proof']['guestLines']; complete='EXIT_LIFETIME_FINALIZER_COMPLETE observed=57 mappingLive=true'
            if corrupt=='missing': lines.remove(complete)
            elif corrupt=='wrong-value': lines[lines.index(complete)]=complete.replace('57','56')
            else:
                end=next(l for l in lines if l.startswith('BROKER_ROUTE_EXIT ')); lines.remove(end); lines.insert(lines.index(complete),end)
            with self.subTest(corrupt=corrupt),self.assertRaises(ValueError): self.check()


if __name__=='__main__': unittest.main()
