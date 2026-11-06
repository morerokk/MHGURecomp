"""Ensure desktop release archives retain the runtime fusion catalog."""
import tempfile
from pathlib import Path
import unittest

from dist import files


class FusionDistributionTest(unittest.TestCase):
    def test_catalog_is_packaged_without_names(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("mhgurecomp", "mhgugame.so", "mhgurecomp-updater", "pipelines.list",
                         "armor-fusion.afus"):
                (root / name).write_bytes(b"fixture")
            (root / "licenses").mkdir()
            names = {entry[0] for entry in files(directory, "linux")}
            self.assertIn("armor-fusion.afus", names)
            self.assertNotIn("armor-fusion.names", names)
            # Older build folders can still contain the retired names file.
            (root / "armor-fusion.names").write_bytes(b"old labels")
            self.assertNotIn("armor-fusion.names", {entry[0] for entry in files(directory, "linux")})
            (root / "armor-fusion.afus").unlink()
            with self.assertRaises(SystemExit):
                files(directory, "linux")


if __name__ == "__main__":
    unittest.main()
