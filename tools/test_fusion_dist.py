"""Ensure desktop release archives retain the runtime fusion catalog."""
import tempfile
from pathlib import Path
import unittest

from dist import files


class FusionDistributionTest(unittest.TestCase):
    def test_required_sidecars_are_packaged(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("mhgurecomp", "mhgugame.so", "mhgurecomp-updater", "pipelines.list",
                         "armor-fusion.afus", "armor-fusion.names"):
                (root / name).write_bytes(b"fixture")
            (root / "licenses").mkdir()
            names = {entry[0] for entry in files(directory, "linux")}
            self.assertIn("armor-fusion.afus", names)
            self.assertIn("armor-fusion.names", names)
            (root / "armor-fusion.names").unlink()
            with self.assertRaises(SystemExit):
                files(directory, "linux")


if __name__ == "__main__":
    unittest.main()
