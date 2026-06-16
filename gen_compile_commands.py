#!/usr/bin/env python3
"""Generate compile_commands.json for clangd from the sample Makefiles."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parent


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a clangd compile_commands.json with make dry-runs."
    )
    parser.add_argument(
        "--chip",
        choices=("CV180X", "CV181X"),
        default=os.environ.get("CHIP", "CV181X"),
        help="target chip macro set, default: %(default)s",
    )
    parser.add_argument(
        "--arch",
        choices=("riscv64", "arm64"),
        default=infer_arch_from_env(),
        help="target CPU architecture, default: %(default)s",
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        help="use -g -O0 instead of -O3 -DNDEBUG",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=ROOT / "compile_commands.json",
        help="output path, default: %(default)s",
    )
    parser.add_argument(
        "--example",
        action="append",
        default=[],
        help="only scan this example directory; can be passed multiple times",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="print scanned directories and skipped make output",
    )
    parser.add_argument(
        "--no-sanitize",
        action="store_true",
        help="keep linker-only flags that appear in compile recipes",
    )
    parser.add_argument(
        "--no-toolchain-includes",
        action="store_true",
        help="do not append -isystem paths discovered from the selected host-tools compiler",
    )
    return parser.parse_args()


def infer_arch_from_env() -> str:
    cc = os.environ.get("CC", "")
    prefix = os.environ.get("TOOLCHAIN_PREFIX", "")
    if "aarch64" in cc or "aarch64" in prefix:
        return "arm64"
    if "riscv64" in cc or "riscv64" in prefix:
        return "riscv64"
    return "riscv64"


def build_env(chip: str, arch: str, debug: bool) -> dict[str, str]:
    if chip == "CV180X" and arch != "riscv64":
        raise SystemExit("CV180X only supports --arch riscv64 in this project.")

    host_tools = ROOT / "host-tools"
    root_inc = ROOT / "include"
    sys_inc = ROOT / "include" / "system"
    tdl_inc = ROOT / "include" / "tdl"

    sysroot: Path | None = None

    if arch == "riscv64":
        arch_cflags = [
            "-mcpu=c906fdv",
            "-march=rv64imafdcv0p7xthead",
            "-mcmodel=medany",
            "-mabi=lp64d",
        ]
        arch_ldflags = [
            "-D_LARGEFILE_SOURCE",
            "-D_LARGEFILE64_SOURCE",
            "-D_FILE_OFFSET_BITS=64",
        ]
        toolchain_dir = host_tools / "gcc" / "riscv64-linux-x86_64"
        toolchain_prefix = toolchain_dir / "bin" / "riscv64-unknown-linux-gnu-"
        sysroot = toolchain_dir / "sysroot"
    else:
        arch_cflags = ["-march=armv8-a"]
        arch_ldflags = []
        toolchain_dir = (
            host_tools
            / "gcc"
            / "gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu"
        )
        toolchain_prefix = toolchain_dir / "bin" / "aarch64-linux-gnu-"
        candidate_sysroot = toolchain_dir / "aarch64-linux-gnu" / "libc"
        if candidate_sysroot.is_dir():
            sysroot = candidate_sysroot

    if chip == "CV180X":
        sys_lib = ROOT / "libs" / "system" / "glibc_riscv64"
        tdl_lib = ROOT / "libs" / "tdl" / "cv180x_riscv64"
    elif arch == "riscv64":
        sys_lib = ROOT / "libs" / "system" / "glibc_riscv64"
        tdl_lib = ROOT / "libs" / "tdl" / "cv181x_riscv64"
    else:
        sys_lib = ROOT / "libs" / "system" / "glibc_arm64"
        tdl_lib = ROOT / "libs" / "tdl" / "cv181x_arm64"

    debug_cflags = ["-g", "-O0"] if debug else ["-O3", "-DNDEBUG"]
    project_includes = [f"-I{root_inc}", f"-I{sys_inc}", f"-I{tdl_inc}"]
    sysroot_flags = [f"--sysroot={sysroot}"] if sysroot and sysroot.is_dir() else []
    cflags = arch_cflags + debug_cflags + sysroot_flags + project_includes
    ldflags = arch_ldflags + [f"-L{sys_lib}", f"-L{tdl_lib}"]

    env = os.environ.copy()
    env.update(
        {
            "TOOLCHAIN_PREFIX": str(toolchain_prefix),
            "CC": f"{toolchain_prefix}gcc",
            "CXX": f"{toolchain_prefix}g++",
            "CFLAGS": " ".join(cflags),
            "LDFLAGS": " ".join(ldflags),
            "CHIP": chip,
            "COMMON_DIR": str(ROOT / "common"),
        }
    )
    return env


def compiler_include_paths(compiler: str, language: str) -> list[Path]:
    result = subprocess.run(
        [compiler, "-v", "-E", "-x", language, os.devnull],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        return []

    paths: list[Path] = []
    in_include_list = False
    for line in result.stderr.splitlines():
        if line.startswith("#include <...> search starts here:"):
            in_include_list = True
            continue
        if in_include_list and line.startswith("End of search list."):
            break
        if not in_include_list:
            continue

        include_path = Path(line.strip())
        if include_path.is_dir():
            paths.append(include_path.resolve())
    return paths


def make_isystem_flags(paths: list[Path]) -> list[str]:
    flags: list[str] = []
    for include_path in paths:
        flags.extend(["-isystem", str(include_path)])
    return flags


def toolchain_include_flags(env: dict[str, str], disabled: bool) -> dict[str, list[str]]:
    if disabled:
        return {"c": [], "c++": []}

    include_flags: dict[str, list[str]] = {}
    for language, env_key in (("c", "CC"), ("c++", "CXX")):
        compiler = env.get(env_key)
        if not compiler or not Path(compiler).is_file():
            include_flags[language] = []
            continue
        paths: list[Path] = []
        seen: set[Path] = set()
        for include_path in compiler_include_paths(compiler, language):
            if include_path in seen:
                continue
            seen.add(include_path)
            paths.append(include_path)
        include_flags[language] = make_isystem_flags(paths)
    return include_flags


def find_example_dirs(selected: list[str]) -> list[Path]:
    if selected:
        dirs = [(ROOT / item).resolve() for item in selected]
    else:
        dirs = sorted(path.parent for path in ROOT.rglob("Makefile"))

    examples: list[Path] = []
    seen: set[Path] = set()
    for directory in dirs:
        if directory in seen:
            continue
        seen.add(directory)
        if not (directory / "Makefile").is_file():
            raise SystemExit(f"missing Makefile: {directory}")
        if directory == ROOT:
            continue
        if not any(directory.glob(pattern) for pattern in ("*.c", "*.cpp")):
            continue
        examples.append(directory)
    return examples


def run_make_dry_run(directory: Path, env: dict[str, str]) -> str:
    result = subprocess.run(
        ["make", "--no-print-directory", "-B", "-n"],
        cwd=directory,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        output = (result.stdout + result.stderr).strip()
        raise RuntimeError(f"make dry-run failed in {directory}\n{output}")
    return result.stdout


def is_compile_command(argv: list[str]) -> bool:
    if not argv or "-c" not in argv:
        return False
    compiler = Path(argv[0]).name
    return compiler.endswith(("gcc", "g++")) or compiler in {
        "cc",
        "c++",
        "clang",
        "clang++",
    }


def source_after_c(argv: list[str]) -> str | None:
    for index, token in enumerate(argv):
        if token == "-c" and index + 1 < len(argv):
            return argv[index + 1]
        if token.startswith("-c") and len(token) > 2:
            return token[2:]
    return None


def source_language(source: str) -> str:
    suffix = Path(source).suffix
    if suffix in {".cc", ".cpp", ".cxx", ".C"}:
        return "c++"
    return "c"


def output_after_o(argv: list[str]) -> str | None:
    for index, token in enumerate(argv):
        if token == "-o" and index + 1 < len(argv):
            return argv[index + 1]
        if token.startswith("-o") and len(token) > 2:
            return token[2:]
    return None


def sanitize_compile_argv(argv: list[str]) -> list[str]:
    sanitized: list[str] = []
    skip_next = False

    for token in argv:
        if skip_next:
            skip_next = False
            continue
        if token in {"-s", "-Xlinker"}:
            skip_next = token == "-Xlinker"
            continue
        if token.startswith("-l") or token.startswith("-Wl,"):
            continue
        sanitized.append(token)

    return sanitized


def make_entry(directory: Path, argv: list[str]) -> dict[str, object] | None:
    source = source_after_c(argv)
    if source is None:
        return None

    source_path = Path(source)
    if not source_path.is_absolute():
        source_path = directory / source_path

    entry: dict[str, object] = {
        "directory": str(directory),
        "file": str(source_path.resolve()),
        "arguments": argv,
    }

    output = output_after_o(argv)
    if output:
        output_path = Path(output)
        if not output_path.is_absolute():
            output_path = directory / output_path
        entry["output"] = str(output_path.resolve())

    return entry


def append_flags_before_output(argv: list[str], flags: list[str]) -> list[str]:
    if not flags:
        return argv
    try:
        output_index = argv.index("-o")
    except ValueError:
        return argv + flags
    return argv[:output_index] + flags + argv[output_index:]


def collect_entries(
    examples: list[Path],
    env: dict[str, str],
    verbose: bool,
    sanitize: bool,
    extra_flags: dict[str, list[str]],
) -> list[dict[str, object]]:
    entries: list[dict[str, object]] = []
    seen: set[tuple[str, str]] = set()

    for example in examples:
        if verbose:
            print(f"scan: {example.relative_to(ROOT)}", file=sys.stderr)
        output = run_make_dry_run(example, env)

        for line in output.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                argv = shlex.split(line)
            except ValueError:
                if verbose:
                    print(f"skip unparsable: {line}", file=sys.stderr)
                continue
            if not is_compile_command(argv):
                continue
            if sanitize:
                argv = sanitize_compile_argv(argv)

            source = source_after_c(argv)
            if source is None:
                continue
            argv = append_flags_before_output(argv, extra_flags[source_language(source)])

            entry = make_entry(example, argv)
            if entry is None:
                continue

            key = (entry["directory"], entry["file"])  # type: ignore[index]
            if key in seen:
                continue
            seen.add(key)
            entries.append(entry)

    return entries


def main() -> int:
    args = parse_args()
    env = build_env(args.chip, args.arch, args.debug)
    extra_flags = toolchain_include_flags(env, args.no_toolchain_includes)
    examples = find_example_dirs(args.example)
    if not examples:
        print("no example directories found", file=sys.stderr)
        return 1

    try:
        entries = collect_entries(
            examples, env, args.verbose, not args.no_sanitize, extra_flags
        )
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 1

    if not entries:
        print("no compile commands found", file=sys.stderr)
        return 1

    output = args.output
    if not output.is_absolute():
        output = ROOT / output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")

    rel_output = output.relative_to(ROOT) if output.is_relative_to(ROOT) else output
    print(f"wrote {rel_output} with {len(entries)} entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
