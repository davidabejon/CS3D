"""Build the app with devkitPro (devkitARM, libctru, citro2d and 3dsxtool).

    python build.py --fetch-deps        # first build: downloads mbedTLS, minimp3
    python build.py                     # later builds, offline
    python build.py --debug             # -O0, readable TLS errors
    python build.py --cia               # also an installable .cia

Output: dist/<APP_TITLE>/<APP_TITLE>.3dsx (and .cia). Copy the folder to
SD:/3ds/ and start it from the Homebrew Launcher, or send it with 3dslink.

Every .c under source/ is compiled; each subfolder is also an include path.
Every .v.pica under source/ is assembled with picasso and linked in as
<name>_shbin / <name>_shbin_size (e.g. globe.v.pica -> globe_shbin).
libpng and zlib come from devkitPro's portlibs (3ds-libpng, 3ds-zlib).
Python 3.12+ is required. devkitPro is found through $DEVKITPRO, or at
C:\\devkitPro (Windows) or /opt/devkitpro.
"""
import argparse
import hashlib
import io
import os
import subprocess
import tarfile
import urllib.request
from pathlib import Path

# ----------------------------------------------------------------- your app
APP_TITLE = "CS3D"
APP_DESCRIPTION = "3DS radio player"
APP_PUBLISHER = "David Abejón"
# Sent as the User-Agent of every HTTP request (http3ds.h).
APP_USER_AGENT = "cs3d/0.1 (+https://github.com/davidabejon/CS3D)"
# --------------------------------------------------------------------------

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "source"
DEPS = ROOT / "deps"
ASSETS = ROOT / "assets"
ROMFS = ROOT / "romfs"
ARCH = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft"]
MBEDTLS_VERSION = "3.6.7"
MBEDTLS_SHA256 = "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6"
# MP3 decoder for source/audio (CC0, single header), pinned to a commit.
MINIMP3_COMMIT = "afb604c06bc8beb145fecd42c0ceb5bda8795144"
MINIMP3_SHA256 = "5fb296a790734772b65a5514438cc06d14f24e539c13475ff2a7f737681c82c0"


def find_devkitpro(explicit: Path | None) -> Path:
    candidates = [explicit] if explicit is not None else [
        Path(os.environ["DEVKITPRO"]) if os.environ.get("DEVKITPRO") else None,
        Path("C:\\devkitPro") if os.name == "nt" else Path("/opt/devkitpro"),
    ]
    for root in candidates:
        if root is not None and (root / "devkitARM").is_dir():
            return root.resolve()
    # Under MSYS2 on Windows $DEVKITPRO is /opt/devkitpro, a path native
    # Python cannot see; the default install location usually is it.
    fallback = Path("C:\\devkitPro")
    if os.name == "nt" and (fallback / "devkitARM").is_dir():
        return fallback
    raise SystemExit("devkitPro not found; use --devkitpro PATH or set DEVKITPRO")


def require_tool(directory: Path, name: str) -> Path:
    path = directory / (name + (".exe" if os.name == "nt" else ""))
    if not path.is_file():
        raise SystemExit(f"Missing tool: {path}")
    return path


def mbedtls_tree(fetch: bool) -> Path:
    name = f"mbedtls-{MBEDTLS_VERSION}"
    tree = DEPS / name
    if (tree / "include" / "mbedtls" / "ssl.h").is_file():
        return tree
    if not fetch:
        raise SystemExit("mbedTLS is missing; rerun with --fetch-deps")
    if tree.exists():
        raise SystemExit(f"Incomplete mbedTLS tree: {tree}; delete it and retry")
    url = (f"https://github.com/Mbed-TLS/mbedtls/releases/download/{name}/"
           f"{name}.tar.bz2")
    print(f"Downloading {name}...", flush=True)
    with urllib.request.urlopen(url, timeout=90) as response:
        archive = response.read()
    if hashlib.sha256(archive).hexdigest() != MBEDTLS_SHA256:
        raise SystemExit("mbedTLS archive checksum mismatch")
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:bz2") as tar:
        if any(not member.name.startswith(name + "/") for member in tar.getmembers()):
            raise SystemExit("Unexpected paths in mbedTLS archive")
        tar.extractall(DEPS, filter="data")
    return tree


def minimp3_dir(fetch: bool) -> Path:
    folder = DEPS / "minimp3"
    header = folder / "minimp3.h"
    if header.is_file():
        if hashlib.sha256(header.read_bytes()).hexdigest() != MINIMP3_SHA256:
            raise SystemExit(f"{header} does not match the pinned version; "
                             "delete it and rerun with --fetch-deps")
        return folder
    if not fetch:
        raise SystemExit("minimp3 is missing; rerun with --fetch-deps")
    url = (f"https://raw.githubusercontent.com/lieff/minimp3/"
           f"{MINIMP3_COMMIT}/minimp3.h")
    print("Downloading minimp3...", flush=True)
    with urllib.request.urlopen(url, timeout=90) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != MINIMP3_SHA256:
        raise SystemExit("minimp3 checksum mismatch")
    folder.mkdir(parents=True, exist_ok=True)
    header.write_bytes(data)
    return folder


def build_mbedtls(cc: Path, ar: Path, tree: Path, out: Path,
                  flags: list[str], includes: list[str]) -> Path:
    """Builds libmbedtls.a with the app's flags and config. Reused while the
    flags and mbedtls_config_3ds.h stay the same (stamp file)."""
    dep_out = out / "mbedtls"
    dep_out.mkdir(parents=True, exist_ok=True)
    lib = dep_out / "libmbedtls.a"
    config = SOURCE / "net" / "mbedtls_config_3ds.h"
    stamp = hashlib.sha256("\0".join([*ARCH, *flags, *includes]).encode() +
                           config.read_bytes()).hexdigest()
    stamp_file = dep_out / "stamp"
    if lib.is_file() and stamp_file.is_file() and \
            stamp_file.read_text() == stamp:
        print("mbedTLS up to date", flush=True)
        return lib
    print(f"Building mbedTLS {MBEDTLS_VERSION}...", flush=True)
    objects = []
    for src in sorted((tree / "library").glob("*.c")):
        obj = dep_out / (src.stem + ".o")
        subprocess.run([str(cc), *ARCH, *flags, "-w", *includes,
                        "-c", str(src), "-o", str(obj)], check=True)
        objects.append(obj)
    if not objects:
        raise SystemExit(f"No mbedTLS sources in {tree}")
    lib.unlink(missing_ok=True)
    # Keep commands below Windows' command-line length limit.
    for start in range(0, len(objects), 32):
        subprocess.run([str(ar), "rcs", str(lib),
                        *map(str, objects[start:start + 32])], check=True)
    stamp_file.write_text(stamp)
    return lib


def build_shaders(picasso: Path, cc: Path, out: Path,
                  flags: list[str]) -> list[Path]:
    """Assembles each source/**/*.v.pica and wraps the binary in a .c file
    exporting `<name>_shbin` and `<name>_shbin_size`."""
    objects = []
    for src in sorted(SOURCE.rglob("*.v.pica")):
        name = src.name[:-len(".v.pica")]
        shbin = out / f"{name}.shbin"
        print(f"Assembling {src.relative_to(ROOT)}", flush=True)
        subprocess.run([str(picasso), "-o", str(shbin), str(src)], check=True)
        data = shbin.read_bytes()
        rows = ",\n".join(", ".join(str(b) for b in data[i:i + 16])
                           for i in range(0, len(data), 16))
        wrapper = out / f"{name}_shbin.c"
        wrapper.write_text(
            f"const unsigned char {name}_shbin[] __attribute__((aligned(4))) = {{\n"
            f"{rows}\n}};\n"
            f"const unsigned int {name}_shbin_size = {len(data)};\n")
        obj = out / f"{name}_shbin.o"
        subprocess.run([str(cc), *ARCH, *flags, "-c", str(wrapper),
                        "-o", str(obj)], check=True)
        objects.append(obj)
    return objects


def make_smdh(bannertool: Path, out: Path) -> Path:
    smdh = out / "icon.smdh"
    subprocess.run([str(bannertool), "makesmdh",
                    "-s", APP_TITLE, "-l", APP_DESCRIPTION,
                    "-p", APP_PUBLISHER, "-i", str(ASSETS / "icon.png"),
                    "-o", str(smdh)], check=True, stdout=subprocess.DEVNULL)
    return smdh


def make_cia(sdk: Path, bannertool: Path, elf: Path, smdh: Path,
             out: Path, dest: Path) -> Path:
    makerom = require_tool(sdk / "tools" / "bin", "makerom")
    banner = out / "banner.bnr"
    subprocess.run([str(bannertool), "makebanner",
                    "-i", str(ASSETS / "banner.png"),
                    "-a", str(ASSETS / "banner.wav"),
                    "-o", str(banner)], check=True, stdout=subprocess.DEVNULL)
    # The RSF reads the romfs root from $(APP_ROMFS): an absolute path with
    # forward slashes, so makerom's YAML parser never sees a backslash.
    subprocess.run([str(makerom), "-f", "cia", "-o", str(dest),
                    "-target", "t", "-exefslogo",
                    "-elf", str(elf), "-rsf", str(ASSETS / "template.rsf"),
                    "-icon", str(smdh), "-banner", str(banner),
                    f"-DAPP_ROMFS={ROMFS.resolve().as_posix()}"], check=True)
    return dest


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--devkitpro", type=Path, help="devkitPro root")
    ap.add_argument("--fetch-deps", action="store_true",
                    help="download and verify the pinned mbedTLS and minimp3")
    ap.add_argument("--debug", action="store_true",
                    help="-O0, and APP_DEBUG (readable mbedTLS errors)")
    ap.add_argument("--cia", action="store_true",
                    help="also build an installable .cia")
    args = ap.parse_args()

    sdk = find_devkitpro(args.devkitpro)
    bindir = sdk / "devkitARM" / "bin"
    cc = require_tool(bindir, "arm-none-eabi-gcc")
    ar = require_tool(bindir, "arm-none-eabi-ar")
    tools = sdk / "tools" / "bin"
    packer = require_tool(tools, "3dsxtool")
    picasso = require_tool(tools, "picasso")
    portlibs = sdk / "portlibs" / "3ds"
    if not (portlibs / "include" / "png.h").is_file():
        raise SystemExit("libpng is missing; install 3ds-libpng and 3ds-zlib "
                         "with devkitPro's pacman")
    bannertool = require_tool(tools, "bannertool")
    if not (sdk / "libctru" / "include" / "citro2d.h").is_file():
        raise SystemExit(f"libctru/citro2d are missing under {sdk}; install "
                         "the 3ds-dev group with devkitPro's pacman")
    if not ROMFS.is_dir():
        raise SystemExit(f"The romfs directory is missing: {ROMFS}")

    sources = sorted(SOURCE.rglob("*.c"))
    if not sources:
        raise SystemExit(f"No sources under {SOURCE}")
    source_dirs = sorted({src.parent for src in sources})

    out = ROOT / "build"
    out.mkdir(parents=True, exist_ok=True)
    flags = ["-std=gnu11", "-D__3DS__",
             "-O0" if args.debug else "-O2", "-g", "-mword-relocations",
             "-ffunction-sections", "-fdata-sections",
             f'-DHTTP_USER_AGENT="{APP_USER_AGENT}"',
             '-DMBEDTLS_CONFIG_FILE="mbedtls_config_3ds.h"',
             *(["-DAPP_DEBUG"] if args.debug else [])]
    tree = mbedtls_tree(args.fetch_deps)
    includes = ["-I" + str(tree / "include"),
                "-I" + str(sdk / "libctru" / "include"),
                *("-I" + str(d) for d in source_dirs)]
    mbedtls_lib = build_mbedtls(cc, ar, tree, out, flags, includes)
    # portlibs goes last: it also ships mbedTLS headers, and ours must win.
    app_includes = [*includes, "-I" + str(minimp3_dir(args.fetch_deps)),
                    "-I" + str(portlibs / "include")]

    objects = build_shaders(picasso, cc, out, flags)
    for src in sources:
        rel = src.relative_to(SOURCE).with_suffix("")
        obj = out / ("_".join(rel.parts) + ".o")
        print(f"Compiling {src.relative_to(ROOT)}", flush=True)
        subprocess.run([str(cc), *ARCH, *flags, "-Wall", "-Wextra",
                        *app_includes, "-c", str(src), "-o", str(obj)],
                       check=True)
        objects.append(obj)

    elf = out / f"{APP_TITLE}.elf"
    subprocess.run([str(cc), *ARCH, "-specs=3dsx.specs", "-g",
                    "-Wl,--gc-sections",
                    "-Wl,-Map," + str(out / f"{APP_TITLE}.map"),
                    "-o", str(elf), *map(str, objects), str(mbedtls_lib),
                    "-L" + str(sdk / "libctru" / "lib"),
                    "-L" + str(portlibs / "lib"),
                    "-Wl,--start-group", "-lcitro2d", "-lcitro3d", "-lctru",
                    "-lpng16", "-lz", "-lm", "-Wl,--end-group"], check=True)

    dest = ROOT / "dist" / APP_TITLE / f"{APP_TITLE}.3dsx"
    dest.parent.mkdir(parents=True, exist_ok=True)
    smdh = make_smdh(bannertool, out)
    subprocess.run([str(packer), str(elf), str(dest), f"--smdh={smdh}",
                    f"--romfs={ROMFS.resolve()}"], check=True)
    print(f"\nBuilt {dest}  ({dest.stat().st_size:,} bytes)")
    if args.cia:
        cia = make_cia(sdk, bannertool, elf, smdh, out,
                       dest.with_suffix(".cia"))
        print(f"Built {cia}  ({cia.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
