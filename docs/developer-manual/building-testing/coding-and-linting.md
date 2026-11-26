# Coding and linting

The conventions of the code base and the checks that run before a change is
merged.

Part of the [Building & Testing](index.md) section of the
[Developer & Architecture Manual](../index.md).

## Coding style

### C++

The code base is compiled as C++20 (`-std=c++20`, no GNU extensions) and
formatted with `clang-format` and the configuration in `src/.clang-format`,
which is based on the Chromium style with DeFiChain specific adjustments.

Layout and formatting:

* four spaces per indentation level, no tabs;
* braces on a new line for classes, functions and methods, on the same line
  for everything else;
* no indentation for `public`, `protected`, `private` and `namespace`;
* no space after the function name, one space after `if`, `for` and `while`;
* a single-statement `if` may stay on one line, every other branch needs
  braces.

Naming:

| Element | Convention | Example |
| --- | --- | --- |
| Classes and structs | `C` prefix, `UpperCamelCase` | `CAccountsView` (`src/dfi/accounts.h`) |
| Methods and free functions | `UpperCamelCase` | `GetBalance()` (`src/dfi/accounts.h`) |
| Local variables and parameters | `snake_case` in the Bitcoin-derived code; the DVM modules also use camelCase parameters | `tokenID` (`src/dfi/accounts.h`) |
| Member variables (Bitcoin-derived code) | `m_` prefix | `m_banlist_dat` (`src/addrdb.h`) |
| Global variables | `g_` prefix | `g_connman` (`src/dfi/masternodes.cpp`) |
| Constants | `UPPER_CASE` | `DEFAULT_LIQUIDITY_CALC_SAMPLING_PERIOD` (`src/dfi/validation.h`) |

The member naming is not uniform across the tree: the Bitcoin-derived headers
use the `m_` prefix, while the DeFiChain modules often use a plain camelCase
member (`enforceAccountMaxMoney` in `src/dfi/accounts.h`). The DVM result
types are the other exception to the `C` prefix (`Res` and `ResVal` in
`src/dfi/res.h`). Follow the convention of the file you edit.

Further C++ rules:

* prefer `++i` over `i++`, `nullptr` over `NULL`, `static_assert` over
  `assert` and `enum class` over plain enums;
* do not submit patches that only change the style of existing code;
* document the public API with Doxygen comments (`/** … */`), because the
  Doxygen configuration in `doc/Doxyfile.in` still builds the API
  documentation.

### Rust

The Rust crates follow the standard Rust style, enforced by `rustfmt` and
`clippy`:

```bash
./make.sh fmt-rs          # cargo fmt
./make.sh check-rs        # cargo fmt --check
./make.sh lib clippy      # cargo clippy -- -Dwarnings
```

`clippy` warnings are errors: the `Rust Lint` job of the `Lint` workflow runs
`cargo clippy` with `-Dwarnings`, so a new warning fails CI. The `fmt`
targets filter the `group_imports`, `unstable_features` and
`imports_granularity` warnings of the nightly-only options, so that the
stable toolchain can format the workspace.

The crates use the Rust edition 2021 (`edition = "2021"` in the
`Cargo.toml` of every crate) and the pinned toolchain of
`rust-toolchain.toml` (channel `1.79`).

### Python

The Python code — the functional tests and the tooling in `test/` and
`contrib/` — follows PEP 8. The detailed rules for the functional tests are
in `test/functional/README.md`:

* the file name describes the area (`feature_<area>.py`, `rpc_<command>.py`,
  `interface_<interface>.py`, `mempool_<behaviour>.py`,
  `wallet_<topic>.py`), with underscores between words and without a
  redundant `test` suffix;
* every test file has a module-level docstring that describes what is
  tested;
* avoid wildcard imports and use `'{}'.format(x)` instead of `'%s' % x`;
* put the overrides of `set_test_params()`, `add_options()` and the
  `setup_*()` methods at the top of the test class.

`black` formats the Python code and `flake8` and the lint scripts in
`test/lint/` check the style rules that `black` does not cover.

### Documentation

The documentation below `docs/` follows the authoring rules of
[ADR-0002](../adr/ADR-0002-documentation-structure.md) and the guide in
`docs/README.md`: one audience per document, sentence-case headings,
relative links, fenced code blocks with a language tag, and Mermaid for
diagrams.

## Formatting

| Formatter | Configuration | Applies to | Commands |
| --- | --- | --- | --- |
| `clang-format` | `src/.clang-format` | `src/dfi/` and `src/ffi/`, plus `src/miner.{cpp,h}`, `src/txmempool.{cpp,h}` and `src/validation.{cpp,h}` | `./make.sh fmt-cpp`, `./make.sh check-cpp` |
| `rustfmt` | the defaults of the pinned toolchain | the whole Cargo workspace | `./make.sh fmt-rs`, `./make.sh check-rs` |
| `black` | the defaults of `black` 24.8.0 | the repository, except `src/crc32c/`, `src/secp256k1/` and `docs/` | `./make.sh fmt-py`, `./make.sh check-py` |

The C++ formatting scope is intentionally narrow: the Bitcoin-derived tree
keeps its historical formatting, while the DeFiChain modules and the bridge
are formatted automatically. The version of the formatter is the
`clang-format-15` of `CLANG_DEFAULT_VERSION`.

## The lint scripts

The lint scripts live in `test/lint/`. `test/lint/lint-all.sh` runs every
`lint-*.sh` script in that directory.

| Script | What it checks |
| --- | --- |
| `lint-circular-dependencies.sh` | The include graph of the C++ tree. |
| `lint-includes.sh`, `lint-include-guards.sh` | The include style and the include guards. |
| `lint-locale-dependence.sh`, `lint-shell-locale.sh` | Locale-dependent code. |
| `lint-logs.sh` | The format of the log messages. |
| `lint-assertions.sh` | The use of assertions. |
| `lint-filenames.sh` | The file names. |
| `lint-python.sh`, `lint-python-dead-code.sh` | The Python style and the unused Python code. |
| `lint-rust-test-optout.sh` | The Rust test modules that are silently disabled with a crate level attribute. |
| `lint-shell.sh` | The shell scripts with `shellcheck`. |
| `lint-shebang.sh` | The shebang lines. |
| `lint-tests.sh` | The naming conventions of the test suites: the suite in `foo_tests.cpp` must be named `foo_tests`, and the suite names must be unique. |
| `lint-format-strings.sh`, `lint-format-strings.py` | The format strings. |
| `check-doc.py` | That every command line argument is documented. |
| `check-rpc-mappings.py` | The consistency of the RPC arguments. |
| `check-docs-structure.py` | The canonical structure of `docs/` and the navigation of `mkdocs.yml`. |
| `extended-lint-cppcheck.sh` | `cppcheck` findings, for example constructors with a single argument that are not `explicit`. |

`extended-lint-cppcheck.sh` needs `cppcheck`; the CI helper
`ci/extended_lint/deps.sh` builds it from source (version 2.10).

## Running the checks locally

```bash
# Everything that the pre-push hook runs.
./make.sh check

# The doc lints and the lint-all suite.
./make.sh check-lints

# A single check.
./make.sh check-cpp
./make.sh check-py
./make.sh check-rs
./make.sh check-sh
```

`./make.sh check` refuses to run on a dirty working tree and then executes
the shell, Python, Rust and C++ checks. The `check-lints` command runs
`test/lint/check-doc.py` and `test/lint/lint-all.sh`; it is a separate
command because it needs the Python virtual environment.

`./make.sh git-add-hooks` installs a `pre-push` hook that runs
`./make.sh check` before every push.

## The documentation checks

```bash
# The canonical structure of docs/ and the navigation of mkdocs.yml.
python3 test/lint/check-docs-structure.py

# Every command line argument is documented.
python3 test/lint/check-doc.py

# The site builds without warnings and without broken links.
pip install -r docs/requirements.txt
mkdocs build --strict
```

The `Docs` workflow runs the first and the third check on every change below
`docs/` or `mkdocs.yml`, and publishes the site from `master`.

## CI

The `Lint` workflow (`.github/workflows/lint.yaml`) has two jobs:

| Job | Steps |
| --- | --- |
| `generic` | `./ci/lint/main.sh` (which runs `check-lints`, `check_sh`, `check_py` and `check_cpp`) and `./ci/extended_lint/main.sh` (which installs `cppcheck` and runs `test/lint/extended-lint-all.sh`). |
| `rust` | `cargo fmt --check`, `cargo clippy` with `-Dwarnings` and `cargo audit` for the dependency advisories. |

The `Docs` workflow validates the documentation structure and builds the site
with `mkdocs build --strict`.

## Legacy documentation

This page consolidates the coding conventions of `doc/developer-notes.md`.
The legacy file remains in the repository because the build system ships it
(`Makefile.am` → `DIST_DOCS`); it is replaced by a pointer to this page.
