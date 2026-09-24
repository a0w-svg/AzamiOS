# Package Management

`pkg.elf` installs, removes, and lists software from a package repository —
one tool for AzamiOS's own native apps, stock Linux binaries, and pure data
packages alike, since both kinds of binary run under this kernel's Linux-ABI
syscall layer (see [LINUX-BINARIES.md](LINUX-BINARIES.md)).

```
/ # pkg list --available
  NAME             VERSION    TYPE    SIZE   DESCRIPTION
  cowsay           1.0.0      native  30K    Prints a cow saying whatever you tell it
  games            1.1.0      native  420K   Terminal games: 2048, snake, minesweeper, breakout, solitaire
  demos-3d         1.0.0      native  110K   Software-rendered 3D demos (3d_test, demo3d)
  x11-apps         1.0.0      native  150K   Classic X clients: xcalc, xclock, xeyes, xgui_demo
  devtools         1.0.0      native  260K   Editor, hex tools and tracers: ide, hexedit, hexdump, strace, ktrace
  netutils         1.0.0      native  240K   Network client tools: curl, nc, nslookup, ip, httpd, ping, netstat
  azami-docs       1.0.0      data    90K    This OS's own documentation under /usr/share/doc/azami
  azami-examples   1.0.0      data    70K    Example C/asm programs under /usr/share/examples
  extra-themes     1.0.0      data    10K    Four more desktop palettes: Gruvbox, Dracula, Tokyo Night, Solarized
  busybox          1.36.1     linux   1.3M   Swiss-army-knife of Linux utilities (ash, wget, grep, ...)
  toybox           0.8.11     linux   940K   Toybox: a second complete command-line toolbox (BSD-licensed)
  lua              5.4.7      linux   640K   Lua interpreter and bytecode compiler (lua, luac)
  micropython      1.23.0     linux   180K   MicroPython: a Python 3 interpreter for small systems
  sqlite           3.46.1     linux   1.6M   SQLite command-line shell with FTS5 and JSON1
  jq               1.7.1      linux   530K   jq: a command-line JSON processor
  gawk             5.3.0      linux   830K   GNU awk, with an awk symlink
  bash             5.2.21     linux   1.1M   GNU Bash (built without readline — scripting shell)
  dash             0.5.12     linux   220K   dash: a small, fast POSIX shell
  tcc              0.9.28rc   linux   700K   TinyCC: a small C compiler that runs on AzamiOS itself
  make             4.4.1      linux   350K   GNU Make — drives a build on the OS itself, with tcc
  bzip2            1.0.8      linux   140K   bzip2 compressor, with bunzip2/bzcat symlinks
  xz               5.4.7      linux   320K   XZ/LZMA compressor, with unxz/xzcat symlinks
  zstd             1.5.6      linux   1.0M   Zstandard compressor, with unzstd/zstdcat symlinks
  curl             8.9.1      linux   930K   curl over plain HTTP (no TLS — this build has no crypto)
  file             5.45       linux   870K   file(1) type identification, with its magic database
  tree             2.1.1      linux   150K   tree: recursive directory listing as an indented tree
  nano             7.2        linux   380K   GNU nano, a full-screen text editor
  less             643        linux   500K   less: the terminal pager
  abi-probe        1.0.0      linux   80K    Linux-ABI conformance probe built by a stock musl toolchain

/ # pkg install cowsay
pkg: fetching cowsay-1.0.0.tar (cowsay 1.0.0, 30K) from file:///repo...
pkg: checksum ok (sha256 28c45684abf6e9de...)
pkg: installing cowsay 1.0.0...
pkg: installed cowsay 1.0.0 — Prints a cow saying whatever you tell it

/ # cowsay.elf AzamiOS package manager works!
 ________________________________
< AzamiOS package manager works! >
 --------------------------------
        \   ^__^
         \  (oo)\_______
            (__)\       )\/\
                ||----w |
                ||     ||

/ # pkg remove cowsay
pkg: removed bin/cowsay.elf
pkg: removed cowsay 1.0.0
```

---

## Commands

```
pkg install [--force] <name>...   # fetch, verify, install; --force reinstalls/upgrades
pkg remove <name>...              # delete every file the package installed
pkg list                          # what's installed
pkg list --available              # what every configured repository offers
pkg search <term>                 # match a substring of any name or description
pkg info <name>                   # installed files, and what the repositories offer
pkg repo list                     # configured repositories
pkg repo add <url>                # append a repository to /etc/pkg/repos.conf
pkg repo remove <url>             # drop one
```

In `pkg list --available` and `pkg search`, an `i` in the first column marks
a package that is already installed.

There is no dependency resolution: a package is a flat list of files.
`pkg install` on an already-installed package is a no-op unless you pass
`--force`, which reinstalls it and deletes any file the previous version
owned that the new one does not.

## Packages

A package is a plain [ustar](https://en.wikipedia.org/wiki/Tar_(computing))
archive — the same format `tar.elf` reads and writes — whose first entry is
named `PKGINFO`, a small `key=value` manifest:

```
name=cowsay
version=1.0.0
type=native
description=Prints a cow saying whatever you tell it
```

Every other entry is a file to install, with its path relative to the root
filesystem (`bin/cowsay.elf`, not `/bin/cowsay.elf`). Directories in that
path are created as needed, so a package can install into a tree the base
image has never had.

Symlink entries are installed as symlinks. That is what makes a toolbox
package usable: `busybox` and `toybox` each ship one multi-call binary plus
a few hundred `usr/bin/<applet>` links pointing at it, so `pkg install
toybox` puts real command names on the `PATH` rather than a single binary
you have to prefix everything with. The links go under `/usr/bin` and
`/usr/sbin`, never `/bin` or `/sbin` — AzamiOS's own tools live there and
the shell finds them first by design. A link whose name already exists
and cannot be replaced is reported and skipped; the rest of the package
still installs.

`type` is display-only — `pkg.elf` installs all three the same way:

| type     | meaning                                                         |
|----------|-----------------------------------------------------------------|
| `native` | built by AzamiOS's own cross-toolchain                           |
| `linux`  | a stock Linux (musl-linked) binary, run via the Linux-ABI layer  |
| `data`   | no executables — documentation, examples, themes                 |

Unknown `PKGINFO` keys are ignored, so the manifest can grow fields without
breaking an older `pkg.elf`.

## Repositories

A repository is a directory — local (`file:///path`) or served over plain
HTTP (`http://host[:port]/path`) — holding an `index.txt`:

```
# name version type file size sha256 description
cowsay 1.0.0 native cowsay-1.0.0.tar 30720 28c45684abf6e9de...899ad8 Prints a cow saying whatever you tell it
```

and the package archives it names. The description runs to the end of the
line.

`size` and `sha256` describe the archive as published. `pkg install` checks
both **before unpacking anything**: a wrong length or digest means the
archive is truncated, corrupted, or not the file the index describes, and in
none of those cases should its contents reach the filesystem. The bad copy
is dropped from the cache so the next attempt re-fetches instead of
re-reading it.

```
/ # pkg install tampered
pkg: fetching tampered-1.0.0.tar (tampered 1.0.0, 30K) from file:///repo...
pkg: checksum mismatch for tampered-1.0.0.tar — refusing to install
pkg:   expected 28c45684abf6e9deef48f0dc775806fe4985f53d3ba94bf9f8dbb58e7a899ad8
pkg:   got      0e410910527829055d2e216ffdf967c469f6ddb2d3459fd9012a6ab9651b5abf
```

Both columns are optional. An index whose fifth field is not a byte count
followed by a 64-hex-digit digest is read as the original four-field format
(`name version type file description...`), which installs exactly as before,
just without the integrity check.

### Configured repositories

`/etc/pkg/repos.conf` lists one repository URL per line, `#` comments
allowed; `pkg install`, `pkg list --available` and `pkg search` check each in
order, and the first repository offering a name is the one an install uses.
`pkg repo add`/`pkg repo remove` edit that file in place, leaving every other
line (comments included) as they found it.

The disk image ships with one local repository already configured:

```
file:///repo
```

built by [`scripts/generate_pkg_repo.py`](../scripts/generate_pkg_repo.py)
from whatever this OS's own toolchain and `tools/linux/out/bin` (`make
linux`) have produced at image-build time — see the `pkgrepo` target in
[userland/Makefile](../userland/Makefile). The catalogue is declarative:
adding a package means adding one entry to `CATALOG` naming its contents.
Packages whose inputs are missing (the Linux binaries before `make linux`)
are skipped with a note rather than failing the build, so the repository is
always valid, if sometimes smaller.

## Ported software

Everything with type `linux` in the catalogue is an unmodified upstream
release, downloaded from its own project, built against musl and linked
static by [tools/linux/ports.mk](../tools/linux/ports.mk):

```
make -C tools/linux ports          # build them all
make -C tools/linux ports-fetch    # just download the tarballs
```

Nothing is patched. Each one runs on AzamiOS because the kernel implements
the Linux x86_64 syscall ABI — the same reason BusyBox has always run here
(see [LINUX-BINARIES.md](LINUX-BINARIES.md)). A port whose build fails
leaves a log in `tools/linux/log/<name>.log` and is skipped by the
repository generator rather than breaking the build, so a partial ports tree
still produces a valid repository.

Ports are deliberately *not* staged into the image (the one exception is the
compiler, below). They exist to be installed from the repository, which is
what the package manager is for.

| package | what it is |
|---|---|
| `toybox`, `busybox` | two complete command-line toolboxes, applet symlinks included |
| `bash`, `dash` | a full shell and a small POSIX one |
| `tcc`, `make` | a C compiler that runs here, and GNU Make to drive it |
| `lua`, `micropython` | Lua 5.4 and a Python 3 interpreter |
| `sqlite`, `jq`, `gawk` | a SQL database shell, a JSON processor, GNU awk |
| `bzip2`, `xz`, `zstd` | compressors, each with its un*/**cat symlinks |
| `curl`, `file`, `tree` | HTTP client, file-type identification, directory tree |
| `nano`, `less` | a full-screen editor and a pager, on a compiled-in terminfo |

`tcc` is the compiler the image itself now carries instead of the cross-GCC
it used to pack — `pkg install tcc` installs the identical files. See
[TOOLCHAIN.md](TOOLCHAIN.md) for building C programs on the OS.

## Downloading over the network

Nothing about a package changes between a local and an HTTP repository —
only the URL. To serve the same repository from the build host:

```
host$  make pkgserve                     # or: make pkgserve PKG_PORT=9000
  Serving 11 package(s) from .../userland/build/repo
  Listening on http://0.0.0.0:8080
```

Inside QEMU's user-mode network the host is `10.0.2.2`, so from the guest:

```
/ # pkg repo add http://10.0.2.2:8080
pkg: added http://10.0.2.2:8080
/ # pkg install games
pkg: fetching games-1.1.0.tar (games 1.1.0, 420K) from http://10.0.2.2:8080...
pkg: checksum ok (sha256 40017c77010d11d9...)
pkg: installing games 1.1.0...
pkg: installed games 1.1.0 — Terminal games: 2048, snake, minesweeper, breakout, solitaire
```

[`scripts/serve_pkg_repo.py`](../scripts/serve_pkg_repo.py) is what `make
pkgserve` runs: GET and HEAD for regular files under one directory, no
directory listings, no writes, no path escapes. It is a development tool for
a VM on your own machine, not a package mirror. The default `repos.conf`
carries the `http://10.0.2.2:8080` line commented out, so the image never
waits on a server that is not running.

An install over HTTP is two fetches — the index, then the archive. For a
while only the *first* TCP connection of a boot completed and a second one
hung the machine; that was a locking bug in `tcp_connect()`/`tcp_input()`
(see [kernel/net/tcp.c](../kernel/net/tcp.c)), not anything specific to
`pkg.elf`, and is fixed: index fetch, package fetch and further installs all
complete within one boot session. `file://` repositories were never affected
— they involve no sockets.

`pkg.elf` puts a 15-second alarm around each connect/recv, so an unreachable
repository costs one timeout and a warning, not a hung package manager.

## Installed-package state

`/var/pkg/db/<name>` records what `pkg install` put where — the manifest,
then a `FILES:` line, then one installed path per line. `pkg remove` deletes
exactly those files and nothing else; `pkg list` (no `--available`) reads
this directory to show what's actually installed, independent of whatever a
repository currently offers.

Fetched archives are cached under `/var/pkg/cache/`.
