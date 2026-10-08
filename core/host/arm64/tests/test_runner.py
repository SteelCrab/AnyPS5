from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


RUNNER = Path(sys.argv.pop(1)).resolve()
FIXTURES = Path(sys.argv.pop(1)).resolve()


class RunnerTests(unittest.TestCase):
    def test_static_elf_executes_as_arm64_without_changing_input(self):
        with tempfile.TemporaryDirectory(prefix="arm64 ' $(literal) ") as directory:
            generated = subprocess.run([str(FIXTURES), directory], capture_output=True, text=True, timeout=30)
            self.assertEqual(generated.returncode, 0, generated.stderr)
            source = Path(directory) / "homebrew.elf"
            original = source.read_bytes()
            result = subprocess.run([str(RUNNER), str(source)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Native ARM64 execution: guest returned 0x2a", result.stdout)
            self.assertEqual(source.read_bytes(), original)
            invalid_budget = subprocess.run([str(RUNNER), str(source), "0"], capture_output=True, text=True, timeout=30)
            self.assertEqual(invalid_budget.returncode, 2)
            self.assertIn("positive integer", invalid_budget.stderr)
            dynamic = bytearray(original)
            struct.pack_into("<I", dynamic, 64, 2)
            source.write_bytes(dynamic)
            unsupported = subprocess.run([str(RUNNER), str(source)], capture_output=True, text=True, timeout=30)
            self.assertEqual(unsupported.returncode, 2)
            self.assertIn("Dynamic imports", unsupported.stderr)


if __name__ == "__main__":
    unittest.main()
