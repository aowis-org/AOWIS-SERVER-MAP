#!/usr/bin/env python3
"""Optional offline builder regression (requires NumPy and Rasterio)."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

import numpy as np

SPEC = importlib.util.spec_from_file_location('overview_builder',
    Path(__file__).resolve().parents[1] / 'tools/build_global_terrain_overview.py')
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


class OverviewBuilderTest(unittest.TestCase):
    def test_pack_and_shared_edges(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'test.aowpack'
            cells = (np.arange(256 * 256, dtype=np.float32).reshape(256, 256) % 5000) - 300
            builder.write_pack(cells, output, 1855, max_zoom=2)
            data = output.read_bytes()
            self.assertEqual(len(data), 16 + 21 * builder.RECORD_BYTES)
            self.assertEqual(struct.unpack_from('<8sII', data),
                             (b'AOWCOAR1', 2, builder.RECORD_BYTES))

            def grid(z, x, y):
                start = builder.tile_offset(z, x, y)
                record = data[start:start + builder.RECORD_BYTES]
                self.assertEqual(record[:8], b'AOWTRN\0\0')
                self.assertEqual(record[24], z)
                self.assertEqual(struct.unpack_from('<II', record, 28), (x, y))
                self.assertEqual(zlib.crc32(record[80:]), struct.unpack_from('<I', record, 60)[0])
                self.assertEqual(record[80:164], bytes(84))
                return np.frombuffer(record[164:], dtype='<u2').reshape(65, 65)

            for z in range(3):
                side = 1 << z
                for y in range(side):
                    for x in range(side):
                        tile = grid(z, x, y)
                        np.testing.assert_array_equal(tile[:, -1], grid(z, (x + 1) % side, y)[:, 0])
                        if y + 1 < side:
                            np.testing.assert_array_equal(tile[-1, :], grid(z, x, y + 1)[0, :])

    def test_invalid_input_does_not_replace_pack(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'test.aowpack'
            output.write_bytes(b'previous-pack')
            cells = np.zeros((64, 64), dtype=np.float32)
            cells[0, 0] = np.nan
            with self.assertRaises(ValueError):
                builder.write_pack(cells, output, 1855, max_zoom=0)
            self.assertEqual(output.read_bytes(), b'previous-pack')


if __name__ == '__main__':
    unittest.main()
