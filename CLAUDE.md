# Fulskrin — notes for Claude

## Tests

Always run the tests under a virtual X display, in this repo and in every session:

```sh
sh test/make_media.sh
cmake --build build --target fulskrin_tests
xvfb-run -a ctest --test-dir build
```

Without it the 16 areas fail at startup with `Unable to create an OpenGL 3.3 context` (no display, no GPU in the sandbox). That is an environment problem, not a code one.

## Workflow

- Every change goes through a branch and a pull request, never directly to `main`.
- Commit messages and PR bodies end with the session attribution lines.
