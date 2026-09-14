#!/usr/bin/env python3
"""
AzamiOS — sample package repository generator
File: scripts/generate_pkg_repo.py

Builds a tiny, real package repository under userland/build/repo/ (which
lands at /repo on the disk image, same as every other userland/build path)
and a default /etc/pkg/repos.conf pointing pkg.elf at it — so `pkg install`
works the moment the image boots, with no network required.

Each package is a plain ustar archive (pkg.elf and tar.elf both read this
format) whose first entry is "PKGINFO", a small key=value manifest, followed
by the files to install with paths relative to the root filesystem. This
script writes that format directly with Python's stdlib tarfile module
rather than shelling out to tar.elf, since it runs on the build host, not
under AzamiOS.

Usage: generate_pkg_repo.py <userland-build-dir> <tools-linux-out-bin-dir>
"""
import io
import os
import sys
import tarfile
import time

def add_manifest(tf, name, version, pkgtype, description):
    info = (
        f"name={name}\n"
        f"version={version}\n"
        f"type={pkgtype}\n"
        f"description={description}\n"
    ).encode("utf-8")
    ti = tarfile.TarInfo(name="PKGINFO")
    ti.size = len(info)
    ti.mtime = int(time.time())
    ti.mode = 0o644
    tf.addfile(ti, io.BytesIO(info))

def add_file(tf, arcname, local_path, mode=0o755):
    ti = tarfile.TarInfo(name=arcname)
    st = os.stat(local_path)
    ti.size = st.st_size
    ti.mtime = int(st.st_mtime)
    ti.mode = mode
    with open(local_path, "rb") as f:
        tf.addfile(ti, f)

def build_package(out_path, name, version, pkgtype, description, files):
    """files: list of (arcname, local_path) pairs."""
    with tarfile.open(out_path, "w") as tf:
        add_manifest(tf, name, version, pkgtype, description)
        for arcname, local_path in files:
            add_file(tf, arcname, local_path)

def main():
    if len(sys.argv) != 3:
        print("usage: generate_pkg_repo.py <userland-build-dir> <tools-linux-out-bin-dir>",
              file=sys.stderr)
        return 1
    build_dir, linux_out_bin = sys.argv[1], sys.argv[2]

    repo_dir = os.path.join(build_dir, "repo")
    os.makedirs(repo_dir, exist_ok=True)
    os.makedirs(os.path.join(build_dir, "etc", "pkg"), exist_ok=True)

    index_lines = []

    # -- cowsay: a native AzamiOS-toolchain app -----------------------------
    cowsay_bin = os.path.join(build_dir, "bin", "cowsay.elf")
    if os.path.isfile(cowsay_bin):
        pkg_file = "cowsay-1.0.0.tar"
        build_package(
            os.path.join(repo_dir, pkg_file),
            "cowsay", "1.0.0", "native",
            "Prints a cow saying whatever you tell it",
            [("bin/cowsay.elf", cowsay_bin)],
        )
        index_lines.append(f"cowsay 1.0.0 native {pkg_file} "
                            "Prints a cow saying whatever you tell it")
        print(f"  -> Packaged {pkg_file} (native)")
    else:
        print(f"  (skipping cowsay package: {cowsay_bin} not built)")

    # -- busybox: a stock Linux (musl) binary -------------------------------
    busybox_bin = os.path.join(linux_out_bin, "busybox")
    if os.path.isfile(busybox_bin):
        pkg_file = "busybox-1.36.1.tar"
        build_package(
            os.path.join(repo_dir, pkg_file),
            "busybox", "1.36.1", "linux",
            "Swiss-army-knife of Linux utilities (ash, wget, grep, ...)",
            [("bin/busybox", busybox_bin)],
        )
        index_lines.append(f"busybox 1.36.1 linux {pkg_file} "
                            "Swiss-army-knife of Linux utilities (ash, wget, grep, ...)")
        print(f"  -> Packaged {pkg_file} (linux)")
    else:
        print(f"  (skipping busybox package: {busybox_bin} not built — run 'make linux' first)")

    with open(os.path.join(repo_dir, "index.txt"), "w") as f:
        f.write("# name version type file description\n")
        for line in index_lines:
            f.write(line + "\n")
    print(f"  -> Wrote {os.path.join(repo_dir, 'index.txt')} ({len(index_lines)} package(s))")

    repos_conf = os.path.join(build_dir, "etc", "pkg", "repos.conf")
    if not os.path.exists(repos_conf):
        with open(repos_conf, "w") as f:
            f.write(
                "# One repository URL per line. file:// for a local directory,\n"
                "# http:// for a repository served over the network.\n"
                "file:///repo\n"
            )
        print(f"  -> Wrote {repos_conf}")

    return 0

if __name__ == "__main__":
    sys.exit(main())
