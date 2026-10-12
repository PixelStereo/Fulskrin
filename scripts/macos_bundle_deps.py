#!/usr/bin/env python3
"""Bundle non-system Mach-O dependencies into a macOS .app.

Qt's macdeployqt handles Qt frameworks/plugins. This script handles the remaining
third-party dylibs (notably FFmpeg's Homebrew dependencies), including unresolved
@rpath references. It intentionally fails closed if a dependency cannot be found.
"""
from __future__ import annotations

import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

APP = Path(sys.argv[1]).resolve()
APP_CONTENTS = APP / "Contents"
APP_EXE = APP_CONTENTS / "MacOS" / "Fulskrin"
FRAMEWORKS = APP_CONTENTS / "Frameworks"
DEPS_DIR = FRAMEWORKS / "FulskrinDeps"

SYSTEM_PREFIXES = ("/System/Library/", "/usr/lib/", "/System/iOSSupport/")
BREW_ROOTS = [Path("/opt/homebrew"), Path("/usr/local")]


def run(*args: str, check: bool = True) -> str:
    p = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and p.returncode:
        raise RuntimeError(f"Command failed ({p.returncode}): {' '.join(args)}\n{p.stderr}")
    return p.stdout


def is_macho(path: Path) -> bool:
    if not path.is_file():
        return False
    p = subprocess.run(["file", str(path)], text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return p.returncode == 0 and "Mach-O" in p.stdout


def dylib_dependencies(binary: Path) -> list[str]:
    output = run("otool", "-L", str(binary))
    deps = []
    for line in output.splitlines()[1:]:
        m = re.match(r"^\s+(.+?) \(compatibility version ", line)
        if m:
            deps.append(m.group(1))
    return deps


def rpaths(binary: Path) -> list[str]:
    output = run("otool", "-l", str(binary))
    found = []
    in_rpath = False
    for line in output.splitlines():
        stripped = line.strip()
        if stripped == "cmd LC_RPATH":
            in_rpath = True
        elif stripped.startswith("cmd "):
            in_rpath = False
        elif in_rpath:
            m = re.match(r"path (.+?) \(offset \d+\)$", stripped)
            if m:
                found.append(m.group(1))
                in_rpath = False
    return found


def expand_path(value: str, binary: Path) -> Path | None:
    executable_dir = APP_CONTENTS / "MacOS"
    if value.startswith("@loader_path/"):
        return (binary.parent / value[len("@loader_path/"):]).resolve()
    if value == "@loader_path":
        return binary.parent.resolve()
    if value.startswith("@executable_path/"):
        return (executable_dir / value[len("@executable_path/"):]).resolve()
    if value == "@executable_path":
        return executable_dir.resolve()
    if value.startswith("/"):
        return Path(value)
    return None


def within_app(path: Path) -> bool:
    try:
        path.resolve().relative_to(APP.resolve())
        return True
    except ValueError:
        return False


def candidate_brew_files(name: str) -> list[Path]:
    """Find matching dylibs/framework binaries in Homebrew, resolving opt symlinks."""
    candidates: dict[str, Path] = {}
    for root in BREW_ROOTS:
        if not root.exists():
            continue
        # Search lib and Frameworks trees. Restrict traversal to these known library locations.
        for pattern in ("Cellar/*/*/lib/**/" + name, "opt/*/lib/**/" + name,
                        "Cellar/*/*/Frameworks/**/" + name, "opt/*/Frameworks/**/" + name):
            for path in root.glob(pattern):
                if path.is_file():
                    real = path.resolve()
                    candidates[str(real)] = real
    return sorted(candidates.values(), key=lambda p: str(p))


def resolve_brew_dependency(dep: str, binary: Path) -> Path | None:
    # First try the exact absolute path (common for Homebrew's Cellar/opt references).
    if dep.startswith("/"):
        p = Path(dep)
        if p.is_file():
            return p.resolve()
    # For @rpath references, first evaluate all rpaths in the Mach-O image.
    if dep.startswith("@rpath/"):
        suffix = dep[len("@rpath/"):]
        for rp in rpaths(binary):
            base = expand_path(rp, binary)
            if base is None:
                continue
            candidate = (base / suffix).resolve()
            if candidate.is_file() and not within_app(candidate):
                return candidate
        name = Path(suffix).name
    else:
        name = Path(dep).name
    matches = candidate_brew_files(name)
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        # Prefer the exact file in an opt path if the dependency named one.
        if dep.startswith("/opt/homebrew/opt/") or dep.startswith("/usr/local/opt/"):
            wanted = Path(dep).resolve()
            if wanted in matches:
                return wanted
        # Prefer the latest installed Cellar version within the same formula if exact resolution failed.
        exact = [p for p in matches if p.name == name]
        if len(exact) == 1:
            return exact[0]
        raise RuntimeError(f"Ambiguous Homebrew dependency {dep} referenced by {binary}: " +
                           ", ".join(str(x) for x in matches))
    return None


def add_rpath(binary: Path, desired: str) -> None:
    if desired not in rpaths(binary):
        run("install_name_tool", "-add_rpath", desired, str(binary))


def normalize_install_id(binary: Path, relative_name: str) -> None:
    old_id = run("otool", "-D", str(binary), check=False).splitlines()
    if len(old_id) < 2:
        return
    old_id = old_id[1].strip()
    new_id = "@rpath/" + relative_name
    if old_id and old_id != new_id and not old_id.startswith("/usr/lib/"):
        run("install_name_tool", "-id", new_id, str(binary))


def copy_dependency(source: Path, requested_name: str) -> tuple[Path, str]:
    """Copy a dependency once; reject collisions rather than silently bundle the wrong ABI."""
    source = source.resolve()
    # Keep the requested load-command basename so versioned symlink names remain loadable.
    safe_name = Path(requested_name).name
    dest = DEPS_DIR / safe_name
    DEPS_DIR.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        if hashlib.sha256(dest.read_bytes()).digest() != hashlib.sha256(source.read_bytes()).digest():
            raise RuntimeError(f"Library filename collision for {safe_name}: {source} conflicts with {dest}")
    else:
        shutil.copy2(source, dest)
        dest.chmod(dest.stat().st_mode | 0o200)
    return dest, "FulskrinDeps/" + safe_name


def main() -> None:
    if not APP_EXE.is_file():
        raise RuntimeError(f"App executable not found: {APP_EXE}")
    if not FRAMEWORKS.is_dir():
        raise RuntimeError(f"Frameworks directory not found: {FRAMEWORKS}")

    # Queue-based recursive traversal: each copied dependency is inspected as well.
    # Use @loader_path-relative references for copied libraries instead of injecting LC_RPATH
    # into every third-party Mach-O. This avoids failures on binaries without load-command padding.
    binaries = [p for p in APP_CONTENTS.rglob("*") if is_macho(p)]
    queue = list(binaries)
    processed: set[str] = set()
    while queue:
        binary = queue.pop(0)
        key = str(binary.resolve())
        if key in processed or not is_macho(binary):
            continue
        processed.add(key)

        for dep in dylib_dependencies(binary):
            if dep.startswith(SYSTEM_PREFIXES):
                continue

            # A reference that resolves to a file inside the app bundle is already portable.
            resolved_inside: Path | None = None
            if dep.startswith("@rpath/"):
                suffix = dep[len("@rpath/"):]
                for rp in rpaths(binary):
                    base = expand_path(rp, binary)
                    if base is None:
                        continue
                    candidate = (base / suffix).resolve()
                    if candidate.is_file() and within_app(candidate):
                        resolved_inside = candidate
                        break
            else:
                candidate = expand_path(dep, binary)
                if candidate is not None and candidate.is_file() and within_app(candidate):
                    resolved_inside = candidate
                elif dep.startswith("/") and Path(dep).is_file() and within_app(Path(dep)):
                    resolved_inside = Path(dep).resolve()

            if resolved_inside is not None:
                continue

            # macdeployqt usually leaves @rpath framework references intact. Resolve those
            # directly against Contents/Frameworks even if the original LC_RPATH chain is incomplete.
            if dep.startswith("@rpath/"):
                suffix = dep[len("@rpath/"):]
                bundled_candidate = (FRAMEWORKS / suffix).resolve()
                if bundled_candidate.is_file() and within_app(bundled_candidate):
                    rel = os.path.relpath(bundled_candidate, binary.parent).replace(os.sep, "/")
                    run("install_name_tool", "-change", dep, "@loader_path/" + rel, str(binary))
                    continue

            source = resolve_brew_dependency(dep, binary)
            if source is None:
                # Do not silently accept unresolved @rpath or external paths.
                raise RuntimeError(f"Unresolved non-system dependency: {dep}\n  referenced by: {binary}")

            requested_name = Path(dep[len("@rpath/"):] if dep.startswith("@rpath/") else dep).name
            dest, relative_name = copy_dependency(source, requested_name)
            normalize_install_id(dest, relative_name)
            relative_from_loader = os.path.relpath(dest, binary.parent).replace(os.sep, "/")
            replacement = "@loader_path/" + relative_from_loader
            if dep != replacement:
                run("install_name_tool", "-change", dep, replacement, str(binary))
            if str(dest) not in processed:
                queue.append(dest)

    # Final strict verification: every non-system dependency must resolve to a file inside this bundle.
    binaries = [p for p in APP_CONTENTS.rglob("*") if is_macho(p)]
    errors: list[str] = []
    for binary in binaries:
        for dep in dylib_dependencies(binary):
            if dep.startswith(SYSTEM_PREFIXES):
                continue
            if dep.startswith("@rpath/"):
                suffix = dep[len("@rpath/"):]
                resolved = False
                for rp in rpaths(binary):
                    base = expand_path(rp, binary)
                    if base is None:
                        continue
                    candidate = (base / suffix).resolve()
                    if candidate.is_file() and within_app(candidate):
                        resolved = True
                        break
                if not resolved:
                    errors.append(f"{binary}: {dep}")
            elif dep.startswith("@loader_path/") or dep.startswith("@executable_path/"):
                candidate = expand_path(dep, binary)
                if candidate is None or not candidate.is_file() or not within_app(candidate):
                    errors.append(f"{binary}: {dep}")
            elif dep.startswith("/") and not dep.startswith(SYSTEM_PREFIXES):
                errors.append(f"{binary}: {dep}")
            else:
                errors.append(f"{binary}: unsupported dependency form {dep}")

    if errors:
        raise RuntimeError("Dependency verification failed:\n  " + "\n  ".join(errors))

    print(f"Bundled and verified {len(processed)} Mach-O binaries in {APP}")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"macOS dependency bundling failed: {exc}", file=sys.stderr)
        sys.exit(1)
