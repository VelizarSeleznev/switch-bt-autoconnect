#!/usr/bin/env python3
"""Upload dist/sd to a Switch over FTP and read the module log.

Works with Sphaira's FTP server (root shows sdmc:, paths sdmc:/...).

Usage:
  scripts/deploy.py HOST [PORT]                    install (default port 5000)
  scripts/deploy.py HOST [PORT] --log              print the module log
  scripts/deploy.py HOST [PORT] --remove-snapback  also delete SnapBack
                                                   (420000000000000C), which
                                                   does the same job
"""
import hashlib
import io
import os
import sys
from ftplib import FTP, error_perm, error_temp

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist", "sd")
SNAPBACK = "atmosphere/contents/420000000000000C"
LOG = "config/bt-autoconnect/log.txt"
STATE = "config/bt-autoconnect/last_device.txt"
FTP_ERRORS = (error_perm, error_temp)


def sd(path):
    return "sdmc:/" + path


def fetch(ftp, path):
    buf = io.BytesIO()
    try:
        ftp.retrbinary("RETR " + sd(path), buf.write)
    except FTP_ERRORS:
        return None
    return buf.getvalue()


def put(ftp, path, data):
    parts = path.split("/")[:-1]
    for i in range(1, len(parts) + 1):
        try:
            ftp.mkd(sd("/".join(parts[:i])))
        except FTP_ERRORS:
            pass
    ftp.storbinary("STOR " + sd(path), io.BytesIO(data))
    if fetch(ftp, path) != data:
        sys.exit(f"verify failed: {path}")
    print(f"  wrote {path} ({len(data)} B, sha256 {hashlib.sha256(data).hexdigest()[:12]})")


def rmtree(ftp, path):
    try:
        entries = ftp.nlst(sd(path))
    except FTP_ERRORS:
        return
    for e in entries:
        name = e.rstrip("/").split("/")[-1]
        if name in (".", ".."):
            continue
        child = f"{path}/{name}"
        try:
            ftp.delete(sd(child))
        except FTP_ERRORS:
            rmtree(ftp, child)
    try:
        ftp.rmd(sd(path))
        print(f"  removed {path}")
    except FTP_ERRORS as e:
        print(f"  could not remove {path}: {e}")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    host = args[0]
    port = int(args[1]) if len(args) > 1 else 5000
    ftp = FTP()
    ftp.connect(host, port, timeout=20)
    ftp.login()
    ftp.set_pasv(True)

    if "--log" in sys.argv:
        log = fetch(ftp, LOG)
        print(log.decode(errors="replace") if log else "(no log yet)")
        state = fetch(ftp, STATE)
        print("target:", state.decode(errors="replace").strip() if state else "(none yet)")
        return

    if "--remove-snapback" in sys.argv:
        print("removing SnapBack")
        rmtree(ftp, SNAPBACK)
        rmtree(ftp, "switch/snapback")

    print("installing bt-autoconnect")
    for dirpath, _, files in os.walk(DIST):
        for name in files:
            local = os.path.join(dirpath, name)
            rel = os.path.relpath(local, DIST)
            with open(local, "rb") as f:
                put(ftp, rel, f.read())
    ftp.quit()
    print("done; reboot the Switch to start the module")


if __name__ == "__main__":
    main()
