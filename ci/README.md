# ci scripts

This directory contains scripts for each build step in each build stage.

Currently defined types:

- `lint`
- `extended_lint`
- `test`
- `sync`

Each stage has its own lifecycle. Every script in here is named
and numbered according to which stage and lifecycle step it belongs to.

In addition, this directory contains helper scripts that are invoked directly
by a workflow instead of by a build stage:

- `fuzz` — startup regression check for sanitizer-instrumented fuzz binaries
  (invoked by `.github/workflows/fuzz.yaml`).
