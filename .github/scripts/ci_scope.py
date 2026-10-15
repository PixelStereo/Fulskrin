"""Conservative change routing: skips are explicit, nightly safety checks remain complete."""
import json
import os
import subprocess


def classify(paths, event="pull_request", ref_type="branch", full=False):
    scheduled = event == "schedule"
    release = ref_type == "tag"
    force = full or release or scheduled
    infrastructure = any(p.startswith((".github/", "cmake/")) or p == "CMakeLists.txt" for p in paths)
    code = infrastructure or any(p.startswith(("src/", "test/", "third_party/", "isf/", "assets/", "scripts/", "examples/")) or p == ".gitmodules" for p in paths)
    engine = infrastructure or any(p.startswith(("src/engine/", "test/engine", "test/video", "test/cue_", "test/emergency", "test/recall_")) for p in paths)
    ui = infrastructure or engine or any(p.startswith(("src/ui/", "src/main", "test/")) for p in paths)
    fuzz = infrastructure or any(p.startswith(("src/engine/Hap", "src/engine/Snappy", "src/engine/OscCodec", "test/fuzz_")) for p in paths)
    result = {
        "build": (code or force) and not scheduled,
        "ui": ui or force,
        "memory": ui or engine or force,
        "tsan": engine or force,
        "fuzz": fuzz or force,
        # Allocation lifetime complements ASan, but does not need a second full workspace build on every PR.
        "valgrind": force or any(p == "test/valgrind.supp" for p in paths),
    }
    result["media"] = any(result[k] for k in ("build", "ui", "tsan", "fuzz"))
    return result


def main():
    event = os.environ.get("GITHUB_EVENT_NAME", "pull_request")
    with open(os.environ["GITHUB_EVENT_PATH"]) as stream:
        payload = json.load(stream)
    base = payload.get("pull_request", {}).get("base", {}).get("sha") or payload.get("before")
    # Unknown base (first push, dispatch): validate conservatively instead of guessing that nothing changed.
    unknown = not base or not base.strip("0")
    paths = [] if unknown else subprocess.check_output(["git", "diff", "--name-only", base, "HEAD"], text=True).splitlines()
    full = unknown or os.environ.get("FULL_VALIDATION", "false").lower() == "true"
    scope = classify(paths, event, os.environ.get("GITHUB_REF_TYPE", "branch"), full)
    with open(os.environ["GITHUB_OUTPUT"], "a") as out:
        for key, value in scope.items():
            out.write(f"{key}={str(value).lower()}\n")
    with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as out:
        out.write("### Validation scope\n\n| Check | Selected |\n|---|---|\n")
        for key, value in scope.items():
            out.write(f"| {key} | {'yes' if value else 'no'} |\n")


if __name__ == "__main__":
    main()
