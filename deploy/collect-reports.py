#!/usr/bin/env python3
# Packs the desync reports polarity-reports has received (and with --crashes the
# crash reports) into one zip to download, then deletes them from the server.
# The zip holds them as the mod repository's docs/reviewing-reports.md lays
# them out, so it unpacks straight into the game's Binaries\Win64\reports:
#
#   desyncs/<match id>/<file names as the game saved them>, received.txt
#   crashes/<date>/<date>_<time>_crash_<id>.log (and .dmp), received.txt
#
# The folders are moved aside first (polarity-reports goes on taking reports, a
# new file for a moved match starts a new folder for the next zip), and
# deleted only once the zip is complete and reads back; anything that fails
# puts them back. A match that got a file in the last --min-age minutes (10)
# stays for the next run, so both players' halves usually go in one zip.
#
# Run as root (installed as /opt/sfvr-server/bin/collect-reports.py):
#   sudo python3 /opt/sfvr-server/bin/collect-reports.py            # into the current folder
#   sudo python3 /opt/sfvr-server/bin/collect-reports.py --crashes --out /root
#   sudo python3 /opt/sfvr-server/bin/collect-reports.py --dry-run  # only list them
# Standard library only.
import argparse
import os
import pwd
import shutil
import sys
import time
import zipfile

STATE = "/var/lib/sfvr-server"


def size_of(path):
    total = 0
    for base, _, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(base, f))
            except OSError:
                pass
    return total


def newest_change(path):
    # A folder's own time moves with each upload (polarity-reports touches it);
    # its files' times too, in case something else wrote one.
    newest = os.path.getmtime(path)
    for base, _, files in os.walk(path):
        for f in files:
            try:
                newest = max(newest, os.path.getmtime(os.path.join(base, f)))
            except OSError:
                pass
    return newest


def ready_folders(root, min_age):
    """The folders in root to pack: (name, path), oldest first."""
    if not os.path.isdir(root):
        return []
    now = time.time()
    found = []
    for d in os.listdir(root):
        path = os.path.join(root, d)
        if not os.path.isdir(path) or d.startswith("."):
            continue
        changed = newest_change(path)
        if now - changed < min_age:
            print(f"  {d}: a file came {int(now - changed)} s ago, left for the next run")
            continue
        found.append((changed, d, path))
    return [(d, path) for _, d, path in sorted(found)]


def give_to_caller(path):
    # Under sudo, the zip belongs to the login that ran it (scp as that login).
    user = os.environ.get("SUDO_USER")
    if os.geteuid() != 0 or not user or user == "root":
        return
    try:
        p = pwd.getpwnam(user)
        os.chown(path, p.pw_uid, p.pw_gid)
    except (KeyError, OSError):
        pass


def main():
    p = argparse.ArgumentParser(description="Pack polarity-reports' desync (and crash) reports into a zip and delete them.")
    p.add_argument("--reports-dir", default=os.path.join(STATE, "reports"))
    p.add_argument("--crashes-dir", default=os.path.join(STATE, "crashes"))
    p.add_argument("--crashes", action="store_true", help="the crash reports too")
    p.add_argument("--out", default=".", help="the folder the zip goes to (the current one)")
    p.add_argument("--min-age", type=int, default=10, help="minutes since a match's last file (10)")
    p.add_argument("--keep", action="store_true", help="pack them but leave them on the server")
    p.add_argument("--dry-run", action="store_true", help="only list what would be packed")
    a = p.parse_args()

    kinds = [("desyncs", a.reports_dir)]
    if a.crashes:
        kinds.append(("crashes", a.crashes_dir))
    picked = []  # (kind, name, path)
    for kind, root in kinds:
        print(f"{kind} in {root}:")
        for name, path in ready_folders(root, a.min_age * 60):
            picked.append((kind, name, path))
    if not picked:
        print("nothing to pack")
        return 0
    total = sum(size_of(path) for _, _, path in picked)
    for kind in dict(kinds):
        n = sum(1 for k, _, _ in picked if k == kind)
        if n:
            print(f"{n} {kind} folders")
    print(f"{total // 1024} KB in all")
    if a.dry_run:
        for kind, name, _ in picked:
            print(f"  {kind}/{name}")
        return 0

    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    stamp = time.strftime("%Y-%m-%d_%H%M%S")
    zip_path = os.path.join(out, f"polarity-reports-{stamp}.zip")
    if shutil.disk_usage(out).free < total + 64 * 1024 * 1024:
        print(f"not enough free space in {out} for the zip ({total // (1024 * 1024)} MB)", file=sys.stderr)
        return 1

    # Moved aside within its own folder (same disk: a rename), so polarity-reports
    # files a late upload in a new folder instead of one being packed.
    moved = []  # (kind, name, original path, aside path)
    for kind, name, path in picked:
        aside_root = os.path.join(os.path.dirname(path), f".collecting-{stamp}")
        os.makedirs(aside_root, mode=0o700, exist_ok=True)
        aside = os.path.join(aside_root, name)
        try:
            os.rename(path, aside)
        except OSError as e:
            print(f"  {kind}/{name}: cannot move it aside ({e}), left for the next run")
            continue
        moved.append((kind, name, path, aside))

    def put_back():
        for kind, name, path, aside in moved:
            if not os.path.exists(aside):
                continue
            if os.path.exists(path):
                # A new upload made the folder again: merge, the new files win.
                for f in os.listdir(aside):
                    if not os.path.exists(os.path.join(path, f)):
                        os.rename(os.path.join(aside, f), os.path.join(path, f))
                shutil.rmtree(aside, ignore_errors=True)
            else:
                os.rename(aside, path)
        remove_aside_roots()

    def remove_aside_roots():
        for root in {os.path.dirname(aside) for _, _, _, aside in moved}:
            shutil.rmtree(root, ignore_errors=True)

    part = zip_path + ".part"
    try:
        count = 0
        with zipfile.ZipFile(part, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for kind, name, _, aside in moved:
                for base, _, files in os.walk(aside):
                    for f in sorted(files):
                        if f.endswith(".part"):
                            continue  # an upload cut off mid-write
                        full = os.path.join(base, f)
                        z.write(full, f"{kind}/{name}/{os.path.relpath(full, aside)}".replace(os.sep, "/"))
                        count += 1
        with zipfile.ZipFile(part) as z:
            bad = z.testzip()
            if bad:
                raise RuntimeError(f"{bad} does not read back")
        os.replace(part, zip_path)
    except BaseException as e:
        try:
            os.remove(part)
        except OSError:
            pass
        put_back()
        print(f"packing failed ({e}): nothing deleted", file=sys.stderr)
        return 1
    give_to_caller(zip_path)

    if a.keep:
        put_back()
        kept = "left on the server (--keep)"
    else:
        remove_aside_roots()
        kept = "deleted from the server"
    print(f"{zip_path}: {count} files from {len(moved)} folders, {os.path.getsize(zip_path) // 1024} KB; {kept}")
    print(f"download: scp <login>@<server>:{zip_path} .")
    return 0


if __name__ == "__main__":
    sys.exit(main())
