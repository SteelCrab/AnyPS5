from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOL = Path(sys.argv.pop(1)).resolve()
FIXTURES = Path(sys.argv.pop(1)).resolve()


class ShaderToolTests(unittest.TestCase):
    def test_native_compilation_accepts_spirv_and_rdna_capture(self):
        with tempfile.TemporaryDirectory(prefix="metal ' $(literal) ") as directory:
            fixture = subprocess.run([str(FIXTURES), directory], capture_output=True, text=True, timeout=30)
            self.assertEqual(fixture.returncode, 0, fixture.stderr)
            for name in ("compute.spv", "fragment.req"):
                with self.subTest(name=name):
                    source = Path(directory) / name
                    original = source.read_bytes()
                    output = source.with_suffix(".metal")
                    result = subprocess.run([str(TOOL), str(source), str(output)],
                                            capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("validated Metal shader", result.stdout)
                    self.assertIn("Apple", result.stdout)
                    self.assertIn("metal", output.read_text())
                    self.assertEqual(source.read_bytes(), original)

    def test_failure_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "invalid.spv"
            source.write_bytes(b"invalid")
            output = Path(directory) / "output.metal"
            output.write_text("existing source")
            result = subprocess.run([str(TOOL), str(source), str(output)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(output.read_text(), "existing source")


if __name__ == "__main__":
    unittest.main()
