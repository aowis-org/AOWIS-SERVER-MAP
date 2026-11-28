#!/usr/bin/env python3
"""No network: exercise persistent source reuse and independent data packaging."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('prepare_data',
    Path(__file__).resolve().parents[1] / 'tools/prepare_global_terrain_data.py')
prepare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prepare)


class PrepareDataTest(unittest.TestCase):
    def test_existing_original_and_pack_never_use_network(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            source = directory / prepare.SOURCE
            source.write_bytes(b'original fixture')
            original_mtime = source.stat().st_mtime_ns
            pack = directory / prepare.PACK
            def builder(command, **kwargs):
                self.assertNotEqual(command[0], 'curl')
                pack.write_bytes(b'derived fixture')
            with patch.object(prepare, 'validate_source'), patch.object(prepare, 'validate_pack'), \
                    patch.object(prepare.subprocess, 'run', side_effect=builder) as run:
                first = prepare.prepare(directory)
                self.assertEqual(run.call_count, 1)
                second = prepare.prepare(directory)
                self.assertEqual(first, second)
                self.assertEqual(run.call_count, 1)
                pack.unlink()
                self.assertEqual(prepare.prepare(directory, offline=True), first)
                self.assertEqual(run.call_count, 2)
                self.assertEqual(source.stat().st_mtime_ns, original_mtime)
                source.write_bytes(b'changed source')
                with self.assertRaisesRegex(ValueError, 'refusing automatic replacement'):
                    prepare.prepare(directory)
                self.assertEqual(run.call_count, 2)

    def test_missing_offline_source_never_downloads(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(prepare.subprocess, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'offline'):
                prepare.prepare(Path(tmp), offline=True)
            run.assert_not_called()

    @unittest.skipUnless(shutil.which('dpkg-deb'), 'Debian packaging tool unavailable')
    def test_independently_versioned_data_deb_is_reused(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            for name in (prepare.SOURCE, prepare.PACK, prepare.MANIFEST):
                (directory / name).write_bytes(name.encode())
            provenance = {'fixture': True}
            deb = prepare.build_deb(directory, provenance)
            digest, mtime = prepare.sha256(deb), deb.stat().st_mtime_ns
            self.assertEqual(prepare.build_deb(directory, provenance), deb)
            self.assertEqual(prepare.sha256(deb), digest)
            self.assertEqual(deb.stat().st_mtime_ns, mtime)
            self.assertEqual(subprocess.check_output(['dpkg-deb','--field',str(deb),'Version'], text=True).strip(), prepare.VERSION)
            extracted = directory / 'extracted'
            subprocess.run(['dpkg-deb','--extract',str(deb),str(extracted)], check=True)
            for name in (prepare.SOURCE, prepare.PACK, prepare.MANIFEST):
                self.assertEqual((extracted / 'usr/share/aowis-server-map/terrain' / name).read_bytes(), name.encode())


if __name__ == '__main__':
    unittest.main()
