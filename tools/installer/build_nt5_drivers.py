"""Build the NT5 WDM, XDDM and PnP helper. Never load them on the host."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess

import pefile

ROOT = Path(__file__).resolve().parents[2]
PIN = "25ee72d97cc6129b79329b249cea337ae88a4e25"
BASE = [
    "--target=i686-w64-windows-gnu", "-O2", "-march=i386",
    "-D_X86_=1", "-D_WIN32_WINNT=0x0500", "-DNTDDI_VERSION=0x05000000",
    "-DNTDDI_WINVISTA=0x06000000", "-ffreestanding", "-fno-stack-protector",
    "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
    "-mno-stack-arg-probe", "-fno-builtin", "-mno-sse", "-mno-mmx",
]
LINK = ["/nologo", "/machine:x86", "/nodefaultlib", "/timestamp:0",
        "/dynamicbase:no", "/nxcompat:no", "/safeseh:no"]
GROUPS = [
    "WGPU_NT_ENABLE_MATERIAL", "WGPU_NT_ENABLE_GROUPS", "WGPU_NT_GROUP_32",
    "WGPU_NT_PIPELINE", "WGPU_NT_WIDE_GROUPS",
]


def build(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    p = ROOT / "drivers/nt"
    winvbe = args.winvbe.resolve()
    velocity = args.velocity.resolve()
    headers = args.headers.resolve()
    llvm = args.llvm.resolve()

    def run(tool, *options):
        result = subprocess.run([str(llvm / f"{tool}.exe"), *map(str, options)],
                                cwd=ROOT, check=True, stdout=subprocess.PIPE, text=True)
        return result.stdout

    if (winvbe / ".git").exists():
        version = subprocess.check_output(["git", "-C", str(winvbe), "rev-parse", "HEAD"],
                                          text=True).strip()
        changes = subprocess.check_output(["git", "-C", str(winvbe), "status",
                                           "--porcelain", "--", "SRC"], text=True)
        if version != PIN or changes:
            raise ValueError(f"Use the unmodified WINVBE source at {PIN}")

    include = ["-isystem", headers, "-isystem", headers / "ddk"]
    source = winvbe / "SRC"
    compat = p / "display/compat"

    def compile_unit(name, filename, flags):
        obj = out / (name + ".obj")
        run("clang", *flags, "-c", filename, "-o", obj)
        return obj

    def undefined(*objects):
        return re.findall(r" U (\S+)", run("llvm-nm", "--undefined-only", *objects))

    def alias_import(symbol, aliases):
        raw = symbol.removeprefix("__imp_")
        if raw.startswith("@"):
            decorated = raw[1:]
            aliases.append("/alternatename:" + symbol + "=__imp__" + decorated)
            return decorated
        return raw.removeprefix("_")

    def imports_library(dll, exports):
        stem = Path(dll).stem if dll in ("hal.dll", "ntoskrnl.exe", "kernel32.dll") else dll
        definition = out / (stem + ".def")
        definition.write_text("LIBRARY " + dll + "\nEXPORTS\n" +
                              "".join("  " + s + "\n" for s in sorted(exports)),
                              encoding="ascii")
        lib = out / (stem + ".lib")
        run("llvm-dlltool", "-m", "i386", "-k", "-d", definition, "-l", lib)
        return lib

    wdm_flags = [
        *BASE, *include, "-Wall", "-Wextra", "-Werror", "-std=c99",
        "-ffunction-sections", "-fdata-sections", "-I" + str(velocity / "include"),
        *["-D" + flag for flag in [*GROUPS, "WGPU_NT_SHORT_WAIT", "WGPU_NT_BULK_COPY",
          "WGPU_NT_BATCH_TEMPLATES", "WGPU_NT_QUEUED_GROUPS", "WGPU_NT_SHADER_CACHE",
          "WGPU_NT_TIMING", "WGPU_NT_PROFILE_TSC"]],
    ]
    wdm_sources = [p / (name + ".c") for name in ("wgpu_nt", "wgpu_batch", "wgpu_triangle")]
    wdm_sources += [ROOT / "efi/platform/gen8_asm.c", p / "wgpu_material.c"]
    wdm_sources += [velocity / "src/chipsets/intel" / (name + ".c")
                    for name in ("gen8_3d", "hsw_3d", "gen8_eu", "hsw_eu")]
    wdm = [compile_unit(path.stem, path, wdm_flags) for path in wdm_sources]
    defined = set(re.findall(r" [Tt] (\S+)", run("llvm-nm", "--defined-only", *wdm)))
    unresolved = set(undefined(*wdm)) - defined - {"_wgpu_nt_build_smoke", "_wgpu_nt_build_triangle"}
    aliases = []
    exports = {alias_import(symbol, aliases) for symbol in sorted(unresolved)}
    hal = {name for name in exports if name.startswith(
        ("KeQueryPerformanceCounter@", "KeStallExecutionProcessor@"))}
    kernel = imports_library("ntoskrnl.exe", exports - hal)
    hal_libs = [imports_library("hal.dll", hal)] if hal else []
    path = out / "wgpunt.sys"
    run("lld-link", *LINK, "/driver:wdm", "/subsystem:native,5.0", "/osversion:5.0",
        "/entry:DriverEntry@8", "/base:0x10000", "/filealign:512", "/fixed:no",
        "/opt:ref", "/out:" + str(path), *wdm, kernel, *hal_libs, *aliases)
    fix_pe(path, display=False)

    display_flags = [
        *BASE, *include, *["-I" + str(directory) for directory in
          (compat, source / "nt/common", source / "nt/disp", source / "d3d")],
        *["-D" + flag for flag in [*GROUPS, "WGPU_D3D_HAL", "WGPU_NT_OPENGL_ICD",
          "WGPU_NT_ENABLE_MIPS", "WGPU_D3D_DIAGNOSTICS", "WGPU_NT_SCENE_GROUPS",
          "WGPU_NT_SCENE_PIPELINE", "WGPU_NT_FAST_STREAM", "WGPU_NT_GDI_PIPELINE",
          "WGPU_NT_ADAPTIVE_GROUPS", "WGPU_NT_LAZY_GROUP_DAMAGE",
          "WGPU_NT_CLEAN_TEXTURES", "WGPU_NT_ENABLE_DEPTH"]],
    ]
    mini = compile_unit("mini", p / "display/miniport.c",
                        ["-I" + str(source / "nt/mini"), *display_flags])
    transport = compile_unit("transport", p / "display/transport.c", display_flags)
    i915 = compile_unit("i915core", source / "patch/i915core.c", display_flags)
    disp = compile_unit("disp", p / "display/display.c",
                        ["-I" + str(p / "display"), *display_flags])
    divide = compile_unit("udivdi3", compat / "udivdi3.c", display_flags)
    groups = {dll: set() for dll in ("videoprt.sys", "ntoskrnl.exe", "win32k.sys", "hal.dll")}
    aliases = []
    for obj in (mini, transport, disp, divide):
        for symbol in undefined(obj):
            if not symbol.startswith("__imp_") and not re.match(
                    r"^_(?:Eng|PALOBJ_|PATHOBJ_|WNDOBJ_)\w*@\d+$", symbol):
                continue
            name = alias_import(symbol, aliases)
            if name.startswith("VideoPort"):
                dll = "videoprt.sys"
            elif name.startswith(("Eng", "PALOBJ_", "PATHOBJ_", "WNDOBJ_",
                                   "HeapVidMemAllocAligned", "VidMemFree")):
                dll = "win32k.sys"
            elif name.startswith("KeGetCurrentIrql@"):
                dll = "hal.dll"
            else:
                dll = "ntoskrnl.exe"
            groups[dll].add(name)
    libs = {dll: imports_library(dll, exports) for dll, exports in groups.items()}
    flags = [*LINK, "/subsystem:native,4.0", "/osversion:4.0", "/base:0x10000",
             "/filealign:512", "/dll"]
    run("lld-link", *flags, "/entry:DriverEntry@8", "/out:" + str(out / "wgpuvbe.sys"),
        mini, transport, i915, libs["videoprt.sys"], libs["ntoskrnl.exe"],
        libs["hal.dll"], *aliases)
    run("lld-link", *flags, "/entry:DrvEnableDriver@12", "/out:" + str(out / "wgpuvbe.dll"),
        disp, divide, libs["win32k.sys"])
    for name in ("wgpuvbe.sys", "wgpuvbe.dll"):
        fix_pe(out / name, display=True)

    helper = compile_unit("ntinstall", p / "ntinstall.c", [
        *BASE, "-std=c89", "-Wall", "-Wextra", "-Werror", "-isystem", headers,
    ])
    kernel32 = imports_library("kernel32.dll", {
        s.removeprefix("__imp_").removeprefix("_") for s in undefined(helper)
    })
    run("lld-link", *LINK, "/subsystem:console,5.0", "/osversion:5.0",
        "/entry:mainCRTStartup", "/base:0x400000", "/out:" + str(out / "ntinst.exe"),
        helper, kernel32)

    for name, location in (("wgpudsp.inf", p / "display"), ("wgpunt.inf", p)):
        shutil.copyfile(location / name, out / name)
    for name in ("wgpunt.sys", "wgpuvbe.sys", "wgpuvbe.dll", "ntinst.exe"):
        print(name)
        if args.w2k_system32:
            verify_imports(run, out / name, args.w2k_system32.resolve())


def fix_pe(path, display):
    with pefile.PE(str(path)) as image:
        if display:
            image.OPTIONAL_HEADER.DllCharacteristics = 0
            image.OPTIONAL_HEADER.MajorLinkerVersion = 3
            image.OPTIONAL_HEADER.MinorLinkerVersion = 10
            if path.suffix == ".sys":
                image.FILE_HEADER.Characteristics &= ~0x2000
        for section in image.sections:
            if not display or section.Name.rstrip(b"\0") in (
                    b".text", b".data", b".rdata", b".idata", b".bss"):
                section.Characteristics |= 0x08000000
        image.OPTIONAL_HEADER.CheckSum = image.generate_checksum()
        image.write(str(path))


def verify_imports(run, path, system32):
    with pefile.PE(str(path)) as image:
        for entry in image.DIRECTORY_ENTRY_IMPORT:
            dll = entry.dll.decode("ascii")
            target = system32 / dll
            if not target.is_file():
                target = system32 / "drivers" / dll
            available = set(re.findall(r"\n  Name: (\S+)",
                                       run("llvm-readobj", "--coff-exports", target)))
            missing = {symbol.name.decode("ascii") for symbol in entry.imports} - available
            if missing:
                raise ValueError(f"{path.name}: {dll} is missing {sorted(missing)}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--winvbe", required=True, type=Path, help=f"WINVBE source at {PIN}")
    parser.add_argument("--velocity", required=True, type=Path, help="Extracted Velocity9x source")
    parser.add_argument("--headers", required=True, type=Path, help="MinGW-w64 include directory")
    parser.add_argument("--llvm", type=Path, default=Path(os.environ.get("LLVM", "C:/Program Files/LLVM/bin")))
    parser.add_argument("--w2k-system32", type=Path, help="Optional local Windows 2000 import reference")
    parser.add_argument("--output", required=True, type=Path, help="New output directory")
    build(parser.parse_args())
