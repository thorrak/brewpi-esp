"""The source identity must cover every shared controller implementation file."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("controller_identity", ROOT / "scripts/controller_identity.py")
IDENTITY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(IDENTITY)


class ControllerIdentityTest(unittest.TestCase):
    def test_byte_changes_and_missing_dependencies_are_detected(self):
        expected = IDENTITY.controller_identity(ROOT)
        self.assertRegex(expected, r"^sha256:[0-9a-f]{64}$")
        with tempfile.TemporaryDirectory() as directory:
            copied = Path(directory)
            for name in IDENTITY.CONTROLLER_SOURCES:
                target = copied / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((ROOT / name).read_bytes())
            self.assertEqual(expected, IDENTITY.controller_identity(copied))
            for name in IDENTITY.CONTROLLER_SOURCES:
                target = copied / name
                original = target.read_bytes()
                target.write_bytes(original + b"\n// changed\n")
                self.assertNotEqual(expected, IDENTITY.controller_identity(copied), name)
                target.unlink()
                with self.assertRaises(FileNotFoundError):
                    IDENTITY.controller_identity(copied)
                target.write_bytes(original)
            self.assertEqual(expected, IDENTITY.controller_identity(copied))


if __name__ == "__main__":
    unittest.main()
