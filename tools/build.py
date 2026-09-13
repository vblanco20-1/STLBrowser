"""Run a build tool with a case-normalized Windows environment.

Some launchers supply both Path and PATH, which breaks MSBuild's environment
dictionary. Only the child environment is normalized; no user settings change.
Usage: python tools/build.py cmake --preset windows
"""

import os
import subprocess
import sys

environment = (
    {key.upper(): value for key, value in os.environ.items()}
    if os.name == "nt"
    else dict(os.environ)
)

raise SystemExit(subprocess.call(sys.argv[1:], env=environment))
