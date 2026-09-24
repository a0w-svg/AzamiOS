#!/usr/bin/env python3
"""
AzamiOS — sample package repository generator
File: scripts/generate_pkg_repo.py

Builds a real package repository under userland/build/repo/ (which lands at
/repo on the disk image, same as every other userland/build path) and a
default /etc/pkg/repos.conf pointing pkg.elf at it — so `pkg install` works
the moment the image boots, with no network required. The same directory is
what scripts/serve_pkg_repo.py publishes over HTTP (`make pkgserve`) when
you want to exercise the network path instead; nothing about the packages
changes between the two, only the URL in repos.conf.

Each package is a plain ustar archive (pkg.elf and tar.elf both read this
format) whose first entry is "PKGINFO", a small key=value manifest, followed
by the files to install with paths relative to the root filesystem. This
script writes that format directly with Python's stdlib tarfile module
rather than shelling out to tar.elf, since it runs on the build host, not
under AzamiOS.

The catalogue below is declarative: one CATALOG entry per package, naming
its contents by where they come from (a built binary, a source tree, or a
string generated right here). Anything whose inputs are missing — the Linux
binaries before `make linux`, an app that failed to build — is skipped with
a note instead of failing the build, so this always produces a valid, if
smaller, repository.

index.txt carries a size and a SHA-256 digest per package, which pkg.elf
checks before unpacking anything it fetched. See docs/PACKAGES.md.

Usage: generate_pkg_repo.py <userland-build-dir> <tools-linux-out-bin-dir>
"""
import hashlib
import io
import os
import re
import subprocess
import sys
import tarfile
import time

# Repo root, so packages can ship things that live outside userland/build
# (docs/, userland/examples/) rather than only built binaries.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ── Content sources ─────────────────────────────────────────────────────
#
# A package's `files` is a list of entries, each of which resolves to a list
# of Members at build time:
#
#   FromFile    — one file on disk at a fixed path
#   FromGlob    — every file in a directory matching a suffix
#   FromTree    — a directory copied whole, recursively
#   FromString  — content generated here, with no file behind it
#   FromApplets — the applet symlinks a multi-call binary answers to,
#                 enumerated by running the binary itself
#
# resolve() returns None when a required input is missing, which is how a
# package with an unbuilt input gets skipped as a whole. Returning an empty
# list means "nothing to contribute", which skips the package too.


class Member:
    """One tar entry: a regular file (data set) or a symlink (linkname set)."""
    __slots__ = ("arcname", "mode", "data", "linkname")

    def __init__(self, arcname, mode=0o755, data=None, linkname=None):
        self.arcname, self.mode, self.data, self.linkname = arcname, mode, data, linkname

class FromFile:
    def __init__(self, arcname, path, mode=0o755):
        self.arcname, self.path, self.mode = arcname, path, mode

    def resolve(self):
        if not os.path.isfile(self.path):
            return None
        with open(self.path, "rb") as f:
            return [Member(self.arcname, self.mode, f.read())]


class FromGlob:
    def __init__(self, arcdir, srcdir, suffixes, mode=0o644):
        self.arcdir, self.srcdir = arcdir, srcdir
        self.suffixes, self.mode = suffixes, mode

    def resolve(self):
        if not os.path.isdir(self.srcdir):
            return []
        out = []
        for entry in sorted(os.listdir(self.srcdir)):
            if not entry.endswith(tuple(self.suffixes)):
                continue
            path = os.path.join(self.srcdir, entry)
            if not os.path.isfile(path):
                continue
            with open(path, "rb") as f:
                out.append(Member(f"{self.arcdir}/{entry}", self.mode, f.read()))
        return out


class FromTree:
    """A whole directory, recursively — tcc's runtime headers, file's magic db."""

    def __init__(self, arcdir, srcdir, mode=0o644):
        self.arcdir, self.srcdir, self.mode = arcdir, srcdir, mode

    def resolve(self):
        if not os.path.isdir(self.srcdir):
            return None
        out = []
        for root, _dirs, files in os.walk(self.srcdir):
            for name in sorted(files):
                path = os.path.join(root, name)
                if os.path.islink(path) or not os.path.isfile(path):
                    continue
                rel = os.path.relpath(path, self.srcdir)
                with open(path, "rb") as f:
                    out.append(Member(f"{self.arcdir}/{rel}", self.mode, f.read()))
        return out


class FromString:
    def __init__(self, arcname, text, mode=0o644):
        self.arcname, self.text, self.mode = arcname, text, mode

    def resolve(self):
        return [Member(self.arcname, self.mode, self.text.encode("utf-8"))]


class Symlink:
    """One symlink entry, target relative to the link's own directory."""

    def __init__(self, arcname, target):
        self.arcname, self.target = arcname, target

    def resolve(self):
        return [Member(self.arcname, 0o777, None, self.target)]


class FromApplets:
    """Every name a multi-call binary answers to, as symlinks pointing at it.

    The applet list comes from the binary itself (`busybox --list-full`,
    `toybox --long`) rather than a list kept here, because that list changes
    with the build's configuration and a stale copy would install dead
    links. These are x86_64 Linux binaries and this script runs on an
    x86_64 Linux build host, so running one to ask is legitimate — and if
    it cannot be run, the applets are simply skipped and the package still
    installs its binary.
    """

    def __init__(self, binary_path, arcname, list_args, default_dir="usr/bin"):
        self.binary_path, self.arcname = binary_path, arcname
        self.list_args, self.default_dir = list_args, default_dir

    def resolve(self):
        if not os.path.isfile(self.binary_path):
            return None
        try:
            out = subprocess.run([self.binary_path] + self.list_args,
                                 capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.SubprocessError):
            return []
        if out.returncode != 0:
            return []

        members = []
        seen = set()
        for line in out.stdout.split():
            applet = line.strip().lstrip("/")
            if not applet:
                continue
            name = applet.rsplit("/", 1)[-1]
            # Respect the directory upstream lists the applet under when it
            # gives one (busybox --list-full does; toybox --long does too),
            # falling back to usr/bin for a bare name.
            if "/" in applet:
                arcdir = applet.rsplit("/", 1)[0]
                # Never shadow the OS's own /bin and /sbin: AzamiOS's native
                # tools live there and the shell finds them first by design
                # (see tools/linux/Makefile's install target).
                if arcdir in ("bin", "sbin"):
                    arcdir = "usr/" + arcdir
            else:
                arcdir = self.default_dir
            arcpath = f"{arcdir}/{name}"
            if arcpath in seen or arcpath == self.arcname:
                continue
            seen.add(arcpath)
            target = os.path.relpath(self.arcname, arcdir)
            members.append(Member(arcpath, 0o777, None, target))
        return members


# ── Upstream versions ───────────────────────────────────────────────────
#
# Read out of the makefiles that actually build these, rather than written
# down a second time here: a version bump in tools/linux/ports.mk then moves
# the package version with it instead of silently mislabelling an archive.

def _parse_versions():
    out = {}
    for mk in (os.path.join(ROOT, "tools", "linux", "ports.mk"),
               os.path.join(ROOT, "tools", "linux", "Makefile")):
        try:
            with open(mk) as f:
                for line in f:
                    m = re.match(r"^([A-Z0-9_]+)_VER\s*:?=\s*(\S+)", line)
                    if m:
                        out.setdefault(m.group(1).lower(), m.group(2))
        except OSError:
            pass
    return out


VERSIONS = _parse_versions()


def ver(key, fallback="0"):
    return VERSIONS.get(key, fallback)


def app(name, build_dir):
    """A /bin app built by AzamiOS's own cross-toolchain."""
    return FromFile(f"bin/{name}.elf", os.path.join(build_dir, "bin", f"{name}.elf"))


def sbin_app(name, build_dir):
    """A /sbin app built by AzamiOS's own cross-toolchain."""
    return FromFile(f"sbin/{name}.elf", os.path.join(build_dir, "sbin", f"{name}.elf"))


# ── Theme files shipped by the extra-themes package ─────────────────────
#
# Palettes only — the same key=value format userland/Makefile's `skeleton`
# target writes for the five built-in themes, dropped into the same
# directory, discovered by the same az_theme_scan_dirs() scan. Installing
# this package adds them to the Settings theme list on the next launch; no
# code anywhere knows these four exist. The 05- .. 08- prefixes continue
# the built-ins' 00- .. 04- numbering.

def theme(filename, name, colors):
    body = f"# AzamiOS Theme: {name}\nname={name}\n" + "".join(
        f"{k}={v}\n" for k, v in colors.items()
    )
    return FromString(f"usr/share/themes/{filename}", body)


EXTRA_THEMES = [
    theme("05-gruvbox.theme", "Gruvbox Dark", {
        "crust": "0xFF1D2021", "mantle": "0xFF282828", "base": "0xFF32302F",
        "surface0": "0xFF3C3836", "surface1": "0xFF504945", "surface2": "0xFF665C54",
        "overlay0": "0xFF7C6F64", "overlay1": "0xFF928374", "text": "0xFFEBDBB2",
        "accent": "0xFFD79921", "accent_sec": "0xFFD65D0E",
        "red": "0xFFCC241D", "green": "0xFF98971A", "yellow": "0xFFFABD2F",
        "blue": "0xFF458588",
    }),
    theme("06-dracula.theme", "Dracula", {
        "crust": "0xFF191A21", "mantle": "0xFF21222C", "base": "0xFF282A36",
        "surface0": "0xFF343746", "surface1": "0xFF44475A", "surface2": "0xFF565A70",
        "overlay0": "0xFF6272A4", "overlay1": "0xFF7F8CBF", "text": "0xFFF8F8F2",
        "accent": "0xFFBD93F9", "accent_sec": "0xFFFF79C6",
        "red": "0xFFFF5555", "green": "0xFF50FA7B", "yellow": "0xFFF1FA8C",
        "blue": "0xFF8BE9FD",
    }),
    theme("07-tokyonight.theme", "Tokyo Night", {
        "crust": "0xFF16161E", "mantle": "0xFF1A1B26", "base": "0xFF24283B",
        "surface0": "0xFF2F334D", "surface1": "0xFF3B4261", "surface2": "0xFF545C7E",
        "overlay0": "0xFF6B7089", "overlay1": "0xFF8A90A8", "text": "0xFFC0CAF5",
        "accent": "0xFF7AA2F7", "accent_sec": "0xFFBB9AF7",
        "red": "0xFFF7768E", "green": "0xFF9ECE6A", "yellow": "0xFFE0AF68",
        "blue": "0xFF2AC3DE",
    }),
    theme("08-solarized.theme", "Solarized Dark", {
        "crust": "0xFF002027", "mantle": "0xFF002B36", "base": "0xFF073642",
        "surface0": "0xFF0E4453", "surface1": "0xFF17505F", "surface2": "0xFF2A5C69",
        "overlay0": "0xFF586E75", "overlay1": "0xFF657B83", "text": "0xFFEEE8D5",
        "accent": "0xFF268BD2", "accent_sec": "0xFF2AA198",
        "red": "0xFFDC322F", "green": "0xFF859900", "yellow": "0xFFB58900",
        "blue": "0xFF6C71C4",
    }),
]


# ── The catalogue ───────────────────────────────────────────────────────
#
# `type` is what `pkg list` shows and nothing else branches on:
#   native — built by this OS's cross-toolchain
#   linux  — a stock Linux (musl) binary, run via the Linux-ABI syscall
#            layer (docs/LINUX-BINARIES.md)
#   data   — no executables at all, just files

def sqlite_version():
    """3460100 -> 3.46.1, the way SQLite itself writes it."""
    raw = ver("sqlite")
    if len(raw) == 7 and raw.isdigit():
        return f"{int(raw[0:1])}.{int(raw[1:3])}.{int(raw[3:5])}"
    return raw


def catalog(build_dir, linux_out_bin):
    # Ports that ship more than one file (tcc's runtime tree, file's magic
    # database) were installed next to out/bin, not into it.
    linux_out = os.path.dirname(os.path.abspath(linux_out_bin))
    return [
        # -- native single app: the smallest possible package, and the one
        #    docs/PACKAGES.md walks through -------------------------------
        dict(name="cowsay", version="1.0.0", type="native",
             description="Prints a cow saying whatever you tell it",
             files=[app("cowsay", build_dir)]),

        # -- native bundles: several binaries installed as one unit -------
        dict(name="games", version="1.1.0", type="native",
             description="Terminal games: 2048, snake, minesweeper, breakout, solitaire",
             files=[app(n, build_dir) for n in
                    ("2048", "snake", "minesweeper", "breakout", "pasjans")]),

        dict(name="demos-3d", version="1.0.0", type="native",
             description="Software-rendered 3D demos (3d_test, demo3d)",
             files=[app(n, build_dir) for n in ("3d_test", "demo3d")]),

        dict(name="x11-apps", version="1.0.0", type="native",
             description="Classic X clients: xcalc, xclock, xeyes, xgui_demo",
             files=[app(n, build_dir) for n in
                    ("xcalc", "xclock", "xeyes", "xgui_demo")]),

        dict(name="devtools", version="1.0.0", type="native",
             description="Editor, hex tools and tracers: ide, hexedit, hexdump, strace, ktrace",
             files=[app(n, build_dir) for n in
                    ("ide", "hexedit", "hexdump", "strace", "ktrace")]),

        dict(name="netutils", version="1.0.0", type="native",
             description="Network client tools: curl, nc, nslookup, ip, httpd, ping, netstat",
             files=[app(n, build_dir) for n in ("curl", "nc", "nslookup", "ip", "httpd")] +
                   [sbin_app(n, build_dir) for n in ("ping", "netstat")]),

        # -- data: files the base image does not carry at all -------------
        dict(name="azami-docs", version="1.0.0", type="data",
             description="This OS's own documentation under /usr/share/doc/azami",
             files=[FromGlob("usr/share/doc/azami", os.path.join(ROOT, "docs"), [".md"]),
                    FromFile("usr/share/doc/azami/README.md",
                             os.path.join(ROOT, "README.md"), mode=0o644)]),

        dict(name="azami-examples", version="1.0.0", type="data",
             description="Example C/asm programs under /usr/share/examples",
             files=[FromGlob("usr/share/examples",
                             os.path.join(ROOT, "userland", "examples"), [".c", ".asm"])]),

        dict(name="extra-themes", version="1.0.0", type="data",
             description="Four more desktop palettes: Gruvbox, Dracula, Tokyo Night, Solarized",
             files=EXTRA_THEMES),

        # -- stock Linux binaries (present only after `make linux`) -------
        #
        # Unmodified upstream releases, built static against musl by
        # tools/linux/{Makefile,ports.mk} and running here purely on the
        # kernel's Linux syscall ABI. A toolbox package also ships the
        # applet symlinks its binary dispatches on, so installing it puts
        # real command names on the PATH and not just one multi-call
        # binary.
        dict(name="busybox", version=ver("busybox", "1.36.1"), type="linux",
             description="Swiss-army-knife of Linux utilities (ash, wget, grep, ...)",
             files=[FromFile("bin/busybox", os.path.join(linux_out_bin, "busybox")),
                    FromApplets(os.path.join(linux_out_bin, "busybox"),
                                "bin/busybox", ["--list-full"])]),

        dict(name="toybox", version=ver("toybox"), type="linux",
             description="Toybox: a second complete command-line toolbox (BSD-licensed)",
             files=[FromFile("bin/toybox", os.path.join(linux_out_bin, "toybox")),
                    FromApplets(os.path.join(linux_out_bin, "toybox"),
                                "bin/toybox", ["--long"])]),

        dict(name="abi-probe", version="1.0.0", type="linux",
             description="Linux-ABI conformance probe built by a stock musl toolchain",
             files=[FromFile("bin/azami-abi-probe",
                             os.path.join(linux_out_bin, "azami-abi-probe"))]),

        # -- languages and data tools -------------------------------------
        dict(name="lua", version=ver("lua"), type="linux",
             description="Lua interpreter and bytecode compiler (lua, luac)",
             files=[FromFile("bin/lua", os.path.join(linux_out_bin, "lua")),
                    FromFile("bin/luac", os.path.join(linux_out_bin, "luac"))]),

        dict(name="micropython", version=ver("micropy"), type="linux",
             description="MicroPython: a Python 3 interpreter for small systems",
             files=[FromFile("bin/micropython",
                             os.path.join(linux_out_bin, "micropython"))]),

        dict(name="sqlite", version=sqlite_version(), type="linux",
             description="SQLite command-line shell with FTS5 and JSON1",
             files=[FromFile("bin/sqlite3", os.path.join(linux_out_bin, "sqlite3"))]),

        dict(name="jq", version=ver("jq"), type="linux",
             description="jq: a command-line JSON processor",
             files=[FromFile("bin/jq", os.path.join(linux_out_bin, "jq"))]),

        dict(name="gawk", version=ver("gawk"), type="linux",
             description="GNU awk, with an awk symlink",
             files=[FromFile("bin/gawk", os.path.join(linux_out_bin, "gawk")),
                    Symlink("bin/awk", "gawk")]),

        # -- shells --------------------------------------------------------
        dict(name="bash", version=ver("bash"), type="linux",
             description="GNU Bash (built without readline — scripting shell)",
             files=[FromFile("bin/bash", os.path.join(linux_out_bin, "bash"))]),

        dict(name="dash", version=ver("dash"), type="linux",
             description="dash: a small, fast POSIX shell",
             files=[FromFile("bin/dash", os.path.join(linux_out_bin, "dash"))]),

        # -- development ---------------------------------------------------
        #
        # tcc needs its runtime library and headers next to it, not just the
        # binary, or every compile fails at link time; the package ships the
        # whole /usr/lib/tcc tree its `make install` lays out.
        dict(name="tcc", version=ver("tcc"), type="linux",
             description="TinyCC: a small C compiler that runs on AzamiOS itself",
             files=[FromFile("bin/tcc", os.path.join(linux_out_bin, "tcc")),
                    FromTree("usr/lib/tcc",
                             os.path.join(linux_out, "tcc", "usr", "lib", "tcc"))]),

        dict(name="make", version=ver("make"), type="linux",
             description="GNU Make — drives a build on the OS itself, with tcc",
             files=[FromFile("bin/make", os.path.join(linux_out_bin, "make"))]),

        # -- compression ---------------------------------------------------
        dict(name="bzip2", version=ver("bzip2"), type="linux",
             description="bzip2 compressor, with bunzip2/bzcat symlinks",
             files=[FromFile("bin/bzip2", os.path.join(linux_out_bin, "bzip2")),
                    Symlink("bin/bunzip2", "bzip2"),
                    Symlink("bin/bzcat", "bzip2")]),

        dict(name="xz", version=ver("xz"), type="linux",
             description="XZ/LZMA compressor, with unxz/xzcat symlinks",
             files=[FromFile("bin/xz", os.path.join(linux_out_bin, "xz")),
                    Symlink("bin/unxz", "xz"),
                    Symlink("bin/xzcat", "xz")]),

        dict(name="zstd", version=ver("zstd"), type="linux",
             description="Zstandard compressor, with unzstd/zstdcat symlinks",
             files=[FromFile("bin/zstd", os.path.join(linux_out_bin, "zstd")),
                    Symlink("bin/unzstd", "zstd"),
                    Symlink("bin/zstdcat", "zstd")]),

        # -- network and files ---------------------------------------------
        dict(name="curl", version=ver("curl"), type="linux",
             description="curl over plain HTTP (no TLS — this build has no crypto)",
             files=[FromFile("bin/curl", os.path.join(linux_out_bin, "curl"))]),

        dict(name="file", version=ver("file"), type="linux",
             description="file(1) type identification, with its magic database",
             files=[FromFile("bin/file", os.path.join(linux_out_bin, "file")),
                    FromFile("usr/share/misc/magic.mgc",
                             os.path.join(linux_out, "file", "usr", "share",
                                          "misc", "magic.mgc"), mode=0o644)]),

        dict(name="tree", version=ver("tree"), type="linux",
             description="tree: recursive directory listing as an indented tree",
             files=[FromFile("bin/tree", os.path.join(linux_out_bin, "tree"))]),

        # -- full-screen terminal programs ---------------------------------
        #
        # Both are linked against an ncurses built with compiled-in terminal
        # descriptions, so they need no terminfo database on the image.
        dict(name="nano", version=ver("nano"), type="linux",
             description="GNU nano, a full-screen text editor",
             files=[FromFile("bin/nano", os.path.join(linux_out_bin, "nano"))]),

        dict(name="less", version=ver("less"), type="linux",
             description="less: the terminal pager",
             files=[FromFile("bin/less", os.path.join(linux_out_bin, "less"))]),
    ]


# ── Archive writing ─────────────────────────────────────────────────────

def add_member(tf, m, mtime):
    ti = tarfile.TarInfo(name=m.arcname)
    ti.mtime = mtime
    ti.mode = m.mode
    if m.linkname is not None:
        ti.type = tarfile.SYMTYPE
        ti.linkname = m.linkname
        ti.size = 0
        tf.addfile(ti)
    else:
        ti.size = len(m.data)
        tf.addfile(ti, io.BytesIO(m.data))


def add_bytes(tf, arcname, payload, mode, mtime):
    add_member(tf, Member(arcname, mode, payload), mtime)


def build_package(out_path, pkg, members):
    """members: list of Member."""
    mtime = int(time.time())
    manifest = (
        f"name={pkg['name']}\n"
        f"version={pkg['version']}\n"
        f"type={pkg['type']}\n"
        f"description={pkg['description']}\n"
    ).encode("utf-8")
    with tarfile.open(out_path, "w") as tf:
        # PKGINFO first: pkg.elf reads the manifest as it walks the archive,
        # so anything after it is already covered by a known package name.
        add_bytes(tf, "PKGINFO", manifest, 0o644, mtime)
        for m in members:
            add_member(tf, m, mtime)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


# ── /etc/pkg/repos.conf ─────────────────────────────────────────────────
#
# Written only when it is missing or still byte-for-byte a default this
# script itself wrote — a config edited by hand (or by `pkg repo add` on a
# running system, if you copied it back out) is never clobbered by a
# rebuild.

REPOS_CONF_DEFAULT = """\
# AzamiOS package repositories — one URL per line, checked in order.
#
#   file:///path              a directory on this machine
#   http://host[:port]/path   a repository served over the network
#
# The bundled sample repository, built into the image at /repo:
file:///repo
#
# The same repository served from your build host over HTTP. Start it with
# `make pkgserve` (scripts/serve_pkg_repo.py) and uncomment the line below,
# or just run: pkg repo add http://10.0.2.2:8080
# 10.0.2.2 is the host as seen from QEMU's user-mode network.
#http://10.0.2.2:8080
"""

# Every default this script has ever written. A repos.conf matching one of
# these is untouched boilerplate and gets upgraded in place.
KNOWN_DEFAULTS = {
    REPOS_CONF_DEFAULT,
    "# One repository URL per line. file:// for a local directory,\n"
    "# http:// for a repository served over the network.\n"
    "file:///repo\n",
}


def write_repos_conf(build_dir):
    path = os.path.join(build_dir, "etc", "pkg", "repos.conf")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        with open(path) as f:
            if f.read() not in KNOWN_DEFAULTS:
                print(f"  (keeping customized {path})")
                return
    with open(path, "w") as f:
        f.write(REPOS_CONF_DEFAULT)
    print(f"  -> Wrote {path}")


def main():
    if len(sys.argv) != 3:
        print("usage: generate_pkg_repo.py <userland-build-dir> <tools-linux-out-bin-dir>",
              file=sys.stderr)
        return 1
    build_dir, linux_out_bin = sys.argv[1], sys.argv[2]

    repo_dir = os.path.join(build_dir, "repo")
    os.makedirs(repo_dir, exist_ok=True)

    index_lines = []
    skipped = []

    for pkg in catalog(build_dir, linux_out_bin):
        members = []
        missing = False
        for src in pkg["files"]:
            resolved = src.resolve()
            if resolved is None:
                missing = True
                break
            members.extend(resolved)
        if missing or not members:
            skipped.append(pkg["name"])
            continue

        pkg_file = f"{pkg['name']}-{pkg['version']}.tar"
        out_path = os.path.join(repo_dir, pkg_file)
        build_package(out_path, pkg, members)

        size = os.path.getsize(out_path)
        digest = sha256_of(out_path)
        index_lines.append(
            f"{pkg['name']} {pkg['version']} {pkg['type']} {pkg_file} "
            f"{size} {digest} {pkg['description']}"
        )
        print(f"  -> Packaged {pkg_file} ({pkg['type']}, {len(members)} file(s), {size} bytes)")

    with open(os.path.join(repo_dir, "index.txt"), "w") as f:
        f.write("# name version type file size sha256 description\n")
        for line in index_lines:
            f.write(line + "\n")
    print(f"  -> Wrote {os.path.join(repo_dir, 'index.txt')} ({len(index_lines)} package(s))")
    if skipped:
        print(f"  (skipped, inputs not built: {', '.join(skipped)})")

    write_repos_conf(build_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
