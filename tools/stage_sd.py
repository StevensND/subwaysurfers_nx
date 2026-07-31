#!/usr/bin/env python3
"""Stage Subway Surfers 3.66.1 for the Switch port."""
import argparse, glob, os, shutil, sys, zipfile

LIBS = ["libmain.so", "libunity.so", "libil2cpp.so"]
LIB_PREFIX = "lib/arm64-v8a/"

def find_apks(apk_dir):
    base = os.path.join(apk_dir, "base.apk")
    arm64 = None
    for cand in glob.glob(os.path.join(apk_dir, "split_config.arm64*.apk")):
        arm64 = cand; break
    return base, arm64

def has_arm64_libs(apk):
    with zipfile.ZipFile(apk) as z:
        names = set(z.namelist())
    return all(LIB_PREFIX + lib in names for lib in LIBS)

def extract_libs(apk, out):
    lib_out = os.path.join(out, "lib", "arm64-v8a")
    os.makedirs(lib_out, exist_ok=True)
    with zipfile.ZipFile(apk) as z:
        names = set(z.namelist())
        for lib in LIBS:
            entry = LIB_PREFIX + lib
            if entry not in names:
                sys.exit(f"missing {entry} in {apk}")
            data = z.read(entry)
            with open(os.path.join(lib_out, lib), "wb") as f:
                f.write(data)
            print(f"  lib   {lib}  ({len(data):,} B)")

def extract_assets(base_apk, out):
    n = big = 0
    total = 0
    with zipfile.ZipFile(base_apk) as z:
        for info in z.infolist():
            if not info.filename.startswith("assets/"):
                continue
            dst = os.path.join(out, info.filename.replace("/", os.sep))
            if info.is_dir():
                os.makedirs(dst, exist_ok=True); continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with z.open(info) as fsrc, open(dst, "wb") as fdst:
                while True:
                    chunk = fsrc.read(1 << 20)
                    if not chunk: break
                    fdst.write(chunk); total += len(chunk)
            n += 1
            if info.file_size >= (1 << 20): big += 1
    print(f"  assets/ extracted: {n} files ({total/1024/1024:.1f} MB, {big} >=1MB)")
    meta = os.path.join(out, "assets", "bin", "Data", "Managed", "Metadata", "global-metadata.dat")
    ggm  = os.path.join(out, "assets", "bin", "Data", "globalgamemanagers")
    cat  = os.path.join(out, "assets", "aa", "catalog.json")
    print(f"    global-metadata.dat present={os.path.isfile(meta)}")
    print(f"    globalgamemanagers  present={os.path.isfile(ggm)}")
    print(f"    aa/catalog.json     present={os.path.isfile(cat)}")

def validate_base_apk(base_apk):
    with zipfile.ZipFile(base_apk) as z:
        names = set(z.namelist())
        manifest = "assets/tower/client/manifest.json" in names
        gamedata_count = sum(
            name.startswith("assets/tower/gamedata/") and name.endswith(".json")
            for name in names
        )
        if "assets/bin/Data/globalgamemanagers" not in names:
            sys.exit("the APK has no Unity globalgamemanagers file")
        managers = z.read("assets/bin/Data/globalgamemanagers")
    if not manifest or not gamedata_count:
        sys.exit("the APK has no bundled Tower manifest/gamedata")
    if b"3.66.1" not in managers or b"2022.3.62f2" not in managers:
        sys.exit("the APK is not Subway Surfers 3.66.1 / Unity 2022.3.62f2")
    return gamedata_count

def remove_legacy_layout(out):
    for name in ["base.apk", *LIBS]:
        path = os.path.join(out, name)
        if os.path.isfile(path):
            os.remove(path)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apk-dir", help="APK bundle extraction directory")
    ap.add_argument("--base", help="base or universal APK")
    ap.add_argument("--arm64", help="optional arm64 split APK")
    destination = ap.add_mutually_exclusive_group(required=True)
    destination.add_argument("--out", help="staging output directory")
    destination.add_argument("--sd-root", help="mounted SD-card root, for example E:\\")
    ap.add_argument("--nro", help="NRO to install with --sd-root")
    a = ap.parse_args()

    base, arm64 = a.base, a.arm64
    if a.apk_dir:
        base, arm64 = find_apks(a.apk_dir)
    if not base or not os.path.isfile(base):
        sys.exit("need --base base.apk (or --apk-dir with base.apk)")
    native_apk = arm64 if arm64 and os.path.isfile(arm64) else base
    if not has_arm64_libs(native_apk):
        sys.exit("arm64 libraries are missing; pass the arm64 split with --arm64")
    validate_base_apk(base)

    sd_root = os.path.abspath(a.sd_root) if a.sd_root else None
    if sd_root and not os.path.isdir(sd_root):
        sys.exit(f"SD-card root does not exist: {sd_root}")
    nro = None
    if sd_root:
        project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        nro = os.path.abspath(a.nro or os.path.join(project_root, "subwaysurfers_nx.nro"))
        if not os.path.isfile(nro):
            sys.exit(f"NRO not found: {nro} (pass it with --nro)")
    out = os.path.join(sd_root, "switch", "subwaysurfers_nx") if sd_root else a.out

    os.makedirs(out, exist_ok=True)
    print(f"base  : {base}\nnative: {native_apk}\nout   : {out}\n")
    extract_libs(native_apk, out)
    extract_assets(base, out)
    remove_legacy_layout(out)

    if sd_root:
        nro_dst = os.path.join(out, "subwaysurfers_nx.nro")
        shutil.copy2(nro, nro_dst)
        print(f"  nro   {nro_dst} ({os.path.getsize(nro_dst):,} B)")

    if sd_root:
        print("\nInstallation complete. Existing saves and settings were preserved.")
    else:
        print(f"\nDone. Copy the CONTENTS of:\n  {out}\nto your SD card at:\n  sdmc:/switch/subwaysurfers_nx/\n")
        print("Then place subwaysurfers_nx.nro in that same directory.")
    print("Launch via TITLE OVERRIDE (hold R over a game) so the process gets a large heap + JIT syscalls.")

if __name__ == "__main__":
    main()
