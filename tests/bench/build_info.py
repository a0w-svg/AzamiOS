#!/usr/bin/env python3
"""Record the actual build configuration and hashes next to each payload."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    mode, output, *args = sys.argv[1:]
    destination = Path(output)
    if mode == "config":
        compiler, cflags, ldflags = args
        version = subprocess.check_output([compiler, "--version"], text=True)
        data = {"compiler": str(Path(compiler).resolve()), "compiler_version": version,
                "cflags": cflags, "ldflags": ldflags}
    else:
        source, common, config = args
        data = json.loads(Path(config).read_text())
        data.update(binary_sha256=digest(output),
                    sources={source: digest(source), common: digest(common)})
        destination = Path(output + ".json")
    content = json.dumps(data, sort_keys=True, indent=2) + "\n"
    if not destination.exists() or destination.read_text() != content:
        destination.write_text(content)


if __name__ == "__main__":
    main()
