#!/usr/bin/env python3
"""
AzamiOS — package repository HTTP server
File: scripts/serve_pkg_repo.py

Publishes a package repository directory (by default the generated
userland/build/repo, i.e. exactly what ships at /repo on the image) over
plain HTTP, so a booted AzamiOS can install from a URL instead of from the
local copy baked into its own filesystem. `make pkgserve` runs this.

From inside QEMU's user-mode network the build host is 10.0.2.2, so:

    host$  make pkgserve
    guest# pkg repo add http://10.0.2.2:8080
    guest# pkg install games

Deliberately small. It serves GET and HEAD for regular files under one
directory and nothing else: no directory listings (a repository is fetched
by name — index.txt, then an archive it names — so a listing would only be
a way to leak whatever else ended up in the directory), no uploads, no
symlink following outside the root, no caching. It is a development tool
for a VM on your own machine, not a package mirror.

Usage: serve_pkg_repo.py [--port N] [--bind ADDR] [--dir PATH]
"""
import argparse
import os
import posixpath
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DIR = os.path.join(ROOT, "userland", "build", "repo")

# What pkg.elf actually asks for. Everything else gets a 404 rather than a
# guess at a content type.
CONTENT_TYPES = {
    ".tar": "application/x-tar",
    ".txt": "text/plain; charset=utf-8",
}


class RepoHandler(BaseHTTPRequestHandler):
    # HTTP/1.1 so Content-Length is meaningful, but every response closes:
    # pkg.elf sends "Connection: close" and reads until EOF, and a server
    # that tried to keep the connection alive would just make it wait for
    # the socket timeout on every fetch.
    protocol_version = "HTTP/1.1"
    server_version = "AzamiOS-pkgrepo/1.0"

    root = DEFAULT_DIR

    def _resolve(self, urlpath):
        """Maps a URL path to a file inside `root`, or None if it escapes.

        Everything is resolved to an absolute real path and checked to be
        under the root afterwards, so "../" segments and symlinks pointing
        out of the repository both fail the same way instead of each
        needing their own special case.
        """
        path = urlpath.split("?", 1)[0].split("#", 1)[0]
        path = posixpath.normpath(path)
        parts = [p for p in path.split("/") if p not in ("", ".", "..")]
        candidate = os.path.realpath(os.path.join(self.root, *parts))
        root_real = os.path.realpath(self.root)
        if candidate != root_real and not candidate.startswith(root_real + os.sep):
            return None
        if not os.path.isfile(candidate):
            return None
        return candidate

    def _send(self, code, body=b"", ctype="text/plain; charset=utf-8", head_only=False):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if body and not head_only:
            self.wfile.write(body)

    def _serve(self, head_only):
        path = self._resolve(self.path)
        if path is None:
            self._send(404, b"not found\n", head_only=head_only)
            return
        ctype = CONTENT_TYPES.get(os.path.splitext(path)[1], "application/octet-stream")
        try:
            with open(path, "rb") as f:
                body = f.read()
        except OSError as exc:
            self._send(500, f"{exc}\n".encode(), head_only=head_only)
            return
        self._send(200, body, ctype=ctype, head_only=head_only)

    def do_GET(self):
        self._serve(head_only=False)

    def do_HEAD(self):
        self._serve(head_only=True)

    def log_message(self, fmt, *args):
        sys.stdout.write("  %s  %s  %s\n" % (time.strftime("%H:%M:%S"),
                                             self.client_address[0], fmt % args))
        sys.stdout.flush()


class Server(ThreadingHTTPServer):
    # Restarting the server right after a guest connection closed should not
    # hit "Address already in use" for the length of a TIME_WAIT.
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description="Serve an AzamiOS package repository over HTTP.")
    ap.add_argument("--port", type=int, default=8080, help="port to listen on (default 8080)")
    ap.add_argument("--bind", default="0.0.0.0",
                    help="address to bind (default 0.0.0.0, reachable from QEMU as 10.0.2.2)")
    ap.add_argument("--dir", default=DEFAULT_DIR,
                    help=f"repository directory to serve (default {DEFAULT_DIR})")
    args = ap.parse_args()

    repo_dir = os.path.abspath(args.dir)
    index = os.path.join(repo_dir, "index.txt")
    if not os.path.isfile(index):
        print(f"serve_pkg_repo: no index.txt in {repo_dir}", file=sys.stderr)
        print("serve_pkg_repo: build the repository first (make, or "
              "scripts/generate_pkg_repo.py)", file=sys.stderr)
        return 1

    with open(index) as f:
        npkgs = sum(1 for line in f if line.strip() and not line.startswith("#"))

    RepoHandler.root = repo_dir
    try:
        httpd = Server((args.bind, args.port), RepoHandler)
    except OSError as exc:
        print(f"serve_pkg_repo: cannot listen on {args.bind}:{args.port}: {exc}", file=sys.stderr)
        return 1

    print(f"  Serving {npkgs} package(s) from {repo_dir}")
    print(f"  Listening on http://{args.bind}:{args.port}")
    print(f"  From a booted AzamiOS guest:  pkg repo add http://10.0.2.2:{args.port}")
    print("  Ctrl-C to stop.")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n  Stopped.")
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
