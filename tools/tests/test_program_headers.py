from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


RELINKER = Path(sys.argv.pop(1)).resolve()


def synthetic_guest():
    image = bytearray(0x8000)
    image[:16] = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<HHIQQQIHHHHHH", image, 16,
                     3, 62, 1, 0, 64, 0, 0, 64, 56, 5, 64, 0, 0)
    code = bytes.fromhex("b83c000000bf2a0000000f05")
    image[0x4000:0x4000 + len(code)] = code
    struct.pack_into("<IIQQQQQQ", image, 64, 1, 5, 0x4000, 0, 0, 0x1000, 0x1000, 0x4000)
    tags = [(5, 0x600), (10, 1), (6, 0x620), (11, 24), (7, 0x700), (8, 0), (9, 24), (0, 0)]
    struct.pack_into("<IIQQQQQQ", image, 120, 2, 6, 0x4600, 0x600, 0x600,
                     len(tags) * 16, len(tags) * 16, 8)
    for index in range(2, 5):
        struct.pack_into("<IIQQQQQQ", image, 64 + index * 56, 0x6FFFFF01, 0, 0, 0, 0, 0, 0, 1)
    for index, tag in enumerate(tags):
        struct.pack_into("<qQ", image, 0x4600 + index * 16, *tag)
    return image


class ProgramHeaderTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform == "darwin", "macOS-only execution guard")
    def test_macos_autorun_rejects_guest_execution_before_writing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "game.elf"
            result = subprocess.run([str(RELINKER), "--autorun", str(Path(directory) / "missing.elf"), str(output)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 2)
            self.assertIn("Native macOS game execution is not implemented", result.stderr)
            self.assertFalse(output.exists())

    def test_relocated_table_preserves_original_bytes_and_is_fully_mapped(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source"
            source.mkdir()
            (source / "sce_module").mkdir()
            original = synthetic_guest()
            old_table_end = 64 + 5 * 56
            marker = b"preserve bytes after the original program headers"
            original[old_table_end:old_table_end + len(marker)] = marker
            executable = source / "eboot.elf"
            executable.write_bytes(original)
            output = Path(directory) / "game.elf"
            result = subprocess.run([str(RELINKER), "--skip-syscall-check", str(executable), str(output)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(executable.read_bytes(), original)
            converted = output.read_bytes()
            offset, = struct.unpack_from("<Q", converted, 32)
            stride, count = struct.unpack_from("<HH", converted, 54)
            headers = [struct.unpack_from("<IIQQQQQQ", converted, offset + index * stride)
                       for index in range(count)]
            mapped_headers = next(header for header in headers if header[0] == 1 and header[2] == 0)
            self.assertGreaterEqual(mapped_headers[5], offset + stride * count)
            phdr = next(header for header in headers if header[0] == 6)
            self.assertEqual(phdr[2], offset)
            self.assertEqual(phdr[5], stride * count)
            self.assertEqual(phdr[6], stride * count)
            self.assertGreaterEqual(offset, len(original))
            self.assertEqual(converted[old_table_end:old_table_end + len(marker)], marker)


if __name__ == "__main__":
    unittest.main()
