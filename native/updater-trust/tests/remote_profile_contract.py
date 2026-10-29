"""Remote-only tracer of the developer compiled-profile probe.

The probe has no input/adoption operation and never touches an installation or
network. Separate default/configured Cargo target directories are mandatory;
ephemeral test trust must never enter a production-library/package target.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import unittest


class CompiledProfileContract(unittest.TestCase):
    default_probe: Path
    configured_probe: Path

    def probe(self, path: Path, *, environment=None, arguments=()):
        result = subprocess.run([str(path), *arguments], capture_output=True,
                                timeout=15, env=environment or os.environ.copy())
        self.assertLessEqual(len(result.stdout), 8192)
        response = json.loads(result.stdout)
        self.assertEqual(response["schema"], "gsw.profile_probe/1")
        self.assertEqual(result.returncode, 0 if response["ok"] else 2,
                         result.stderr.decode(errors="replace"))
        return response

    def test_default_stays_not_configured_despite_runtime_environment(self):
        expected = {"schema": "gsw.profile_probe/1", "ok": True, "code": "NOT_CONFIGURED"}
        self.assertEqual(self.probe(self.default_probe), expected)
        environment = os.environ.copy()
        for name in ("GSW_UPDATER_PRODUCTION_PROFILE_PATH", "GSW_UPDATER_FIXTURE_PROFILE_PATH",
                     "GSW_UPDATE_ROOT", "GSW_UPDATE_FEED", "GSW_UPDATE_CA", "GSW_UPDATE_TIME"):
            environment[name] = "runtime-input-must-be-ignored"
        self.assertEqual(self.probe(self.default_probe, environment=environment), expected)

    def test_explicit_compile_time_production_profile_is_configured(self):
        response = self.probe(self.configured_probe)
        self.assertTrue(response["ok"], response)
        self.assertEqual(response["code"], "CONFIGURED")
        self.assertEqual(response["product"], "Fubukisama.GaussianSceneWorkbench.Native")
        self.assertEqual(response["platform"], "windows-x64")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--default-probe", required=True, type=Path)
    parser.add_argument("--configured-probe", required=True, type=Path)
    arguments = parser.parse_args()
    CompiledProfileContract.default_probe = arguments.default_probe.absolute()
    CompiledProfileContract.configured_probe = arguments.configured_probe.absolute()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(CompiledProfileContract)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 2


if __name__ == "__main__":
    raise SystemExit(main())
