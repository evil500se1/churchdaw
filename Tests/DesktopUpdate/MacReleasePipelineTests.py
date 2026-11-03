"""Exercise the real macOS packager against controlled OS-tool responses.

No signing identity, network, Keychain, or user app is accessed by these tests.
The mocks deliberately return success with Invalid JSON to reproduce notarytool
submissions that finish without receiving approval.
"""
import os
from pathlib import Path
import plistlib
import platform
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
MOCK = r'''#!/usr/bin/env python3
import json,os,plistlib,shutil,sys
from pathlib import Path
name=Path(sys.argv[0]).name; a=sys.argv[1:]; root=Path(os.environ['MAC_TEST_ROOT']); mode=os.environ['MAC_TEST_MODE']
with (root/'calls.jsonl').open('a') as f: f.write(json.dumps([name]+a)+'\n')
if name=='lipo': print('x86_64 arm64')
elif name=='codesign':
 if '-d' in a and '--entitlements' in a: sys.stdout.buffer.write(plistlib.dumps({key:True for key in ['com.apple.security.cs.allow-jit','com.apple.security.cs.allow-unsigned-executable-memory','com.apple.security.cs.disable-library-validation','com.apple.security.device.audio-input','com.apple.security.device.camera']}))
 elif '-d' in a: print('Authority=Developer ID Application: Fixture (TEAM123456)\nTimestamp=Sep 4 2026\nflags=0x10000(runtime)')
elif name=='ditto':
 if '-c' in a: Path(a[-1]).write_bytes(b'zip fixture')
 elif Path(a[0]).is_dir(): shutil.copytree(a[0],a[1],dirs_exist_ok=True)
 else: shutil.copy2(a[0],a[1])
elif name=='hdiutil':
 if a[0]=='create':
  shutil.copytree(a[a.index('-srcfolder')+1],root/'disk-payload',symlinks=True)
  Path(a[-1]).write_bytes(b'dmg fixture')
 elif a[0]=='attach': shutil.copytree(root/'disk-payload',a[a.index('-mountpoint')+1],dirs_exist_ok=True,symlinks=True)
elif name=='xcrun':
 if a[:2]==['notarytool','submit']:
  label='dmg' if a[2].endswith('.dmg') else 'app'
  print(json.dumps({'id':label+'-submission','status':'Invalid' if mode==label+'-invalid' else 'Accepted'}))
 elif a[:2]==['notarytool','log']:
  Path(a[-1]).write_text(json.dumps({'status':'Accepted','issues':[{'severity':'warning'}] if mode=='notary-warning' else None}))
 elif a[:2]==['stapler','validate']:
  if mode=='ticket-missing': sys.exit(65)
elif name=='spctl':
 path=Path(a[-1]); kind=a[a.index('--type')+1]
 if path.suffix=='.dmg':
  if kind=='open' and ('--context' not in a or 'context:primary-signature' not in a): sys.exit(12)
  if kind not in ('open','install'): sys.exit(12)
  if mode=='legacy-rejected' and kind=='install': sys.exit(3)
  if mode=='dmg-rejected': sys.exit(3)
 elif kind!='execute': sys.exit(12)
 if mode=='app-rejected' and path.suffix=='.app': sys.exit(3)
 if mode=='mounted-rejected' and 'mount' in path.parts: sys.exit(3)
 print('accepted\nsource=Notarized Developer ID')
'''


@unittest.skipUnless(platform.system() == 'Darwin', 'macOS packaging test')
class MacReleasePipelineTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='churchdaw-release-test-')
        self.root = Path(self.temp.name)
        self.build = self.root / 'build'
        self.app = self.build / 'Release/ChurchDAW.app'
        (self.app / 'Contents/MacOS').mkdir(parents=True)
        self.exe = self.app / 'Contents/MacOS/ChurchDAW'
        self.exe.write_text('CHURCHDAW_RELEASE_LICENSE_GATE=DESKTOP_ENABLED\n')
        self.exe.chmod(0o755)
        (self.app / 'Contents/Info.plist').write_bytes(plistlib.dumps({
            'CFBundleShortVersionString': '1.6.31',
            'CFBundleIdentifier': 'com.churchdaw.app'}))
        (self.build / 'CMakeCache.txt').write_text(
            'CHURCHDAW_ENABLE_DESKTOP_LICENSING:BOOL=ON\n'
            'CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE:BOOL=ON\n')
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        for name in ['lipo', 'codesign', 'ditto', 'hdiutil', 'xcrun', 'spctl']:
            p = self.bin / name
            p.write_text(MOCK)
            p.chmod(0o755)
        self.out = self.root / 'output'
        self.final = self.out / 'ChurchDAW-1.6.31.dmg'
        self.env = dict(os.environ, PATH=str(self.bin)+os.pathsep+os.environ['PATH'],
                        MAC_TEST_ROOT=str(self.root), MAC_TEST_MODE='accepted')

    def tearDown(self):
        self.temp.cleanup()

    def run_packager(self, mode='accepted', auth=None):
        self.env['MAC_TEST_MODE'] = mode
        args = ['bash', str(REPO/'Packaging/BuildMacRelease.sh'),
                '-b', str(self.build), '-o', str(self.out)]
        args += ['-i', 'fixture', '-p', 'fixture'] if auth is None else auth
        return subprocess.run(args, env=self.env, capture_output=True, text=True, timeout=30)

    def test_no_identity_cannot_produce_release(self):
        r = self.run_packager(auth=[])
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse(self.final.exists())

    def test_signing_without_notarization_is_rejected_early(self):
        r = self.run_packager(auth=['-i', 'fixture'])
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse((self.root/'calls.jsonl').exists())

    def test_rejections_never_promote_or_print_policy(self):
        for mode in ['app-invalid', 'dmg-invalid', 'notary-warning', 'ticket-missing',
                     'app-rejected', 'dmg-rejected', 'legacy-rejected', 'mounted-rejected']:
            with self.subTest(mode=mode):
                shutil.rmtree(self.root/'disk-payload', ignore_errors=True)
                r = self.run_packager(mode)
                self.assertNotEqual(r.returncode, 0, r.stdout+r.stderr)
                self.assertFalse(self.final.exists())
                self.assertNotIn('Entitlements da assinatura divergem', r.stdout+r.stderr)
                self.assertNotIn('mac_installer_url', r.stdout)

    def test_success_verifies_mounted_app_before_promoting(self):
        before = self.exe.read_bytes()
        r = self.run_packager()
        self.assertEqual(r.returncode, 0, r.stdout+r.stderr)
        self.assertTrue(self.final.is_file())
        self.assertTrue(Path(str(self.final)+'.verification.json').is_file())
        self.assertIn('MAC_RELEASE_VERIFIED', r.stdout)
        self.assertEqual(self.exe.read_bytes(), before)
        calls = (self.root/'calls.jsonl').read_text()
        self.assertIn('context:primary-signature', calls)
        self.assertIn('mount/ChurchDAW.app', calls)

    def test_existing_output_is_never_overwritten(self):
        self.out.mkdir()
        self.final.write_bytes(b'prior release')
        r = self.run_packager()
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(self.final.read_bytes(), b'prior release')

    def test_unsigned_is_explicit_and_never_advertised_as_update(self):
        r = self.run_packager(auth=['-u'])
        self.assertEqual(r.returncode, 0, r.stdout+r.stderr)
        self.assertFalse(self.final.exists())
        self.assertTrue((self.out/'ChurchDAW-1.6.31-NAO-PARA-DISTRIBUICAO.dmg').exists())
        self.assertNotIn('mac_installer_url', r.stdout)

if __name__ == '__main__':
    unittest.main(verbosity=2)
