#!/usr/bin/env python3
"""pckhub - package and system-update hub for ROS Command.

Serves GitHub Release assets to the guest over plain HTTP on 10.0.2.2:8000
(QEMU slirp maps that address to the host). The kernel only speaks plain
HTTP, while GitHub requires HTTPS, so this small host-side helper bridges
the two.

Endpoints:
    GET /index.txt    "NAME.PCK  SIZE" lines for every release asset
    GET /NAME.PCK     download (and cache) that asset from the release
    GET /os/meta.txt  latest system image: "VERSION x.y.z" + "SIZE bytes"
    GET /os/image     raw 32 MB system image (newest release asset)
"""
import gzip
import json
import os
import shutil
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

OWNER = os.environ.get("PCK_OWNER", "AaronXieA")
REPO = os.environ.get("PCK_REPO", "ROS")
TAG = os.environ.get("PCK_TAG", "pck-packages")
PORT = int(os.environ.get("PCKHUB_PORT", "8000"))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "build", "pckcache")

API = "https://api.github.com/repos/%s/%s/releases/tags/%s" % (OWNER, REPO, TAG)
RELEASES = "https://api.github.com/repos/%s/%s/releases" % (OWNER, REPO)
DL = "https://github.com/%s/%s/releases/download/%s/" % (OWNER, REPO, TAG)

# testing override: serve a local raw image instead of a GitHub release
OS_IMAGE = os.environ.get("ROS_IMAGE")
OS_VERSION = os.environ.get("ROS_VERSION")

# last successfully fetched image, used when GitHub is temporarily unreachable
OS_META = os.path.join(CACHE, "os-meta.txt")

_os_cache = {"time": 0.0, "value": None}
_os_lock = threading.Lock()


def cached_assets():
    out = {}
    if os.path.isdir(CACHE):
        for name in os.listdir(CACHE):
            if name.upper().endswith(".PCK"):
                out[name.upper()] = os.path.getsize(os.path.join(CACHE, name))
    return out


def release_assets():
    assets = cached_assets()
    try:
        req = urllib.request.Request(API, headers={"User-Agent": "ros-pckhub"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.load(resp)
        for a in data.get("assets", []):
            name = a.get("name", "")
            if name.upper().endswith(".PCK"):
                assets[name.upper()] = int(a.get("size", 0))
        print("index: %d asset(s) from %s/%s@%s" % (len(assets), OWNER, REPO, TAG))
    except Exception as exc:                 # offline: fall back to the cache
        print("index: GitHub unavailable (%s), using %d cached asset(s)"
              % (exc, len(assets)), file=sys.stderr)
    return assets


def fetch(name):
    dest = os.path.join(CACHE, name)
    if os.path.isfile(dest):
        return dest
    os.makedirs(CACHE, exist_ok=True)
    tmp = dest + ".part"
    print("download: %s" % name)
    with urllib.request.urlopen(DL + name, timeout=60) as src, open(tmp, "wb") as out:
        shutil.copyfileobj(src, out)
    os.replace(tmp, dest)
    return dest


def safe_name(s):
    return "".join(c if c.isalnum() or c in "-._" else "_" for c in s)


def fetch_os_asset(url, dest, compressed):
    os.makedirs(CACHE, exist_ok=True)
    part = dest + ".part"
    with urllib.request.urlopen(url, timeout=120) as src, open(part, "wb") as out:
        shutil.copyfileobj(src, out)
    if compressed:
        tmp = dest + ".tmp"
        with gzip.open(part, "rb") as src, open(tmp, "wb") as out:
            shutil.copyfileobj(src, out)
        os.remove(part)
        os.replace(tmp, dest)
    else:
        os.replace(part, dest)


def write_os_meta(ver, fn, asset_size=0):
    os.makedirs(CACHE, exist_ok=True)
    tmp = OS_META + ".part"
    with open(tmp, "w") as out:
        out.write("VERSION %s\nSIZE %d\nPATH %s\nASSET_SIZE %d\n"
                  % (ver, os.path.getsize(fn), os.path.basename(fn), asset_size))
    os.replace(tmp, OS_META)


def meta_asset_size(fn):
    """Compressed asset size recorded for fn, or -1 if unknown."""
    try:
        fields = {}
        with open(OS_META) as f:
            for line in f:
                key, _, val = line.partition(" ")
                fields[key] = val.strip()
        if fields.get("PATH") == os.path.basename(fn):
            return int(fields.get("ASSET_SIZE", "-1"))
    except Exception:
        pass
    return -1


def cached_os():
    """(version, image path) from the last successful fetch, or None."""
    try:
        fields = {}
        with open(OS_META) as f:
            for line in f:
                key, _, val = line.partition(" ")
                fields[key] = val.strip()
        fn = os.path.join(CACHE, fields["PATH"])
        if os.path.isfile(fn) and os.path.getsize(fn) == int(fields["SIZE"]):
            return (fields["VERSION"], fn)
    except Exception:
        pass

    # no metadata file yet: use the newest cached image
    best = None
    try:
        for name in os.listdir(CACHE):
            if not (name.startswith("os-") and name.endswith(".img")):
                continue
            fn = os.path.join(CACHE, name)
            if os.path.isfile(fn) and (best is None or
                                       os.path.getmtime(fn) > os.path.getmtime(best)):
                best = fn
    except Exception:
        return None
    if best:
        return (os.path.basename(best)[3:-4], best)
    return None


def latest_os():
    with _os_lock:
        return _latest_os()


def _latest_os():
    """(version, raw image path) for the newest release that has an image."""
    if OS_IMAGE:
        if not os.path.isfile(OS_IMAGE):
            raise RuntimeError("ROS_IMAGE not found: %s" % OS_IMAGE)
        return (OS_VERSION or "local"), os.path.abspath(OS_IMAGE)

    now = time.time()
    if _os_cache["value"] and now - _os_cache["time"] < 60:
        return _os_cache["value"]

    try:
        req = urllib.request.Request(RELEASES, headers={"User-Agent": "ros-pckhub"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            releases = json.load(resp)

        for rel in releases:
            for a in rel.get("assets", []):
                name = a.get("name", "")
                if not (name.endswith(".img.gz") or name.endswith(".img")):
                    continue
                tag = rel.get("tag_name", "unknown").lstrip("v")
                dest = os.path.join(CACHE, "os-" + safe_name(tag) + ".img")
                asset_size = int(a.get("size", 0))
                if (not os.path.isfile(dest) or
                        meta_asset_size(dest) != asset_size):
                    print("os: fetching %s" % name)
                    fetch_os_asset(a["browser_download_url"], dest,
                                   name.endswith(".gz"))
                    print("os: cached %s (%d bytes)" % (dest, os.path.getsize(dest)))
                value = (tag, dest)
                write_os_meta(*value, asset_size)
                _os_cache["time"] = now
                _os_cache["value"] = value
                return value

        raise RuntimeError("no .img/.img.gz asset found in releases")
    except Exception as exc:                 # offline: reuse the last image
        value = cached_os()
        if not value:
            raise
        print("os: GitHub unavailable (%s), using cached %s"
              % (exc, value[1]), file=sys.stderr)
        _os_cache["time"] = now
        _os_cache["value"] = value
        return value


class Handler(BaseHTTPRequestHandler):
    server_version = "pckhub/1.0"

    def do_GET(self):
        path = self.path.split("?", 1)[0]

        if path == "/os/meta.txt":
            try:
                ver, fn = latest_os()
                body = ("VERSION %s\nSIZE %d\n"
                        % (ver, os.path.getsize(fn))).encode()
            except Exception as exc:
                print("os meta failed: %s" % exc, file=sys.stderr)
                self.send_error(503, "no system image")
                return
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if path == "/os/image":
            try:
                _, fn = latest_os()
                size = os.path.getsize(fn)
                f = open(fn, "rb")
            except Exception as exc:
                print("os image failed: %s" % exc, file=sys.stderr)
                self.send_error(503, "no system image")
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.end_headers()
            with f:
                while True:
                    chunk = f.read(65536)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
            return

        if path in ("/", "/index.txt"):
            lines = ["%s %d" % (n, s) for n, s in sorted(release_assets().items())]
            body = ("\n".join(lines) + ("\n" if lines else "")).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        name = os.path.basename(path).upper()
        if not name.endswith(".PCK") or "/" in name or name != name.strip():
            self.send_error(404, "not found")
            return
        try:
            fn = fetch(name)
        except Exception as exc:
            print("download failed: %s (%s)" % (name, exc), file=sys.stderr)
            self.send_error(502, "download failed")
            return

        size = os.path.getsize(fn)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.end_headers()
        with open(fn, "rb") as f:
            while True:
                chunk = f.read(16384)
                if not chunk:
                    break
                self.wfile.write(chunk)

    def log_message(self, fmt, *args):
        sys.stderr.write("pckhub: " + fmt % args + "\n")


def warm_os():
    try:
        ver, fn = latest_os()
        print("os: ready %s (%d bytes)" % (ver, os.path.getsize(fn)))
    except Exception as exc:
        print("os: warmup failed: %s" % exc, file=sys.stderr)


def main():
    os.makedirs(CACHE, exist_ok=True)
    threading.Thread(target=warm_os, daemon=True).start()
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print("pck hub: http://127.0.0.1:%d (guest: http://10.0.2.2:%d)" % (PORT, PORT))
    print("release: %s" % DL)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
