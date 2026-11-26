# Building the node

This page describes how the DeFiChain node is compiled: the `make.sh` entry
point, the dependency system, the autotools and Cargo integration, the
platform requirements and the dependencies themselves.

Part of the [Building & Testing](index.md) section of the
[Developer & Architecture Manual](../index.md).

## The entry point: `make.sh`

`./make.sh` is the recommended way to build the node. It wraps the
autotools build, the `depends` dependency system and the Cargo build of the
Rust workspace, and it keeps every artefact inside the `build/` directory of
the repository, so the source tree stays clean.

```bash
./make.sh            # prints the list of commands
./make.sh build      # dependencies, configure and make
```

The script requires Bash 5 or newer and runs with the `en_US.UTF-8` locale.

### The commands you will use

| Command | What it does |
| --- | --- |
| `build` | The full build: `build-deps`, `build-conf`, `build-make`. |
| `build-deps` | Builds the native dependencies with the `depends` system into `build/depends/<TARGET>`. |
| `build-conf` | Runs `autogen.sh` and `configure` in the build directory. |
| `build-make` | Runs `make` in the build directory. |
| `exec <target>` | Runs `make <target>` in the build directory. |
| `test`, `test-unit`, `test-cpp`, `test-rs`, `test-py` | The test suites, see [Testing and fuzzing](testing-and-fuzzing.md). |
| `fmt`, `fmt-cpp`, `fmt-rs`, `fmt-py` | The formatters, see [Coding and linting](coding-and-linting.md). |
| `check`, `check-lints`, `check-cpp`, `check-rs`, `check-py`, `check-sh` | The linters, see [Coding and linting](coding-and-linting.md). |
| `lib <cargo-target>` | Runs a Cargo target of the Rust workspace (`test`, `check`, `clippy`, `fmt`, `fmt-check`, `doc`). |
| `deploy`, `package`, `release` | Installs, packages and releases the built binaries. |
| `docker-build`, `docker-deploy`, `docker-release` | The same, inside a container image. |
| `clean`, `purge` | Removes the build output; `purge` also removes the dependencies and the Docker images. |
| `pkg-install-deps`, `pkg-install-llvm`, `pkg-user-install-rust`, … | Install the host prerequisites of a platform. |
| `ci-setup-deps`, `ci-setup-user-deps`, `ci-export-vars` | The helpers that the GitHub Actions workflows use. |

Every command of the script is documented by `./make.sh help`.

### Environment variables

| Variable | Default | Meaning |
| --- | --- | --- |
| `TARGET` | auto-detected | The build target, for example `x86_64-pc-linux-gnu`. |
| `BUILD_DIR` | `./build` | The root of the build output. |
| `BUILD_DEPENDS_DIR` | `${BUILD_DIR}/depends` | Where the `depends` system installs the libraries. |
| `PYTHON_VENV_DIR` | `${BUILD_DIR}/pyenv` | The virtual environment for the Python tools. |
| `MAKE_JOBS` | 80 % of the cores | The number of parallel build jobs. |
| `MAKE_DEBUG` | `1` | When `1`, `configure` is called with `--enable-debug`. |
| `MAKE_CONF_ARGS` | target-dependent | Extra arguments for `configure`. |
| `MAKE_CONF_ARGS_OVERRIDE` | empty | Replaces `MAKE_CONF_ARGS` completely. |
| `CLANG_DEFAULT_VERSION` | `15` | The Clang version used for the C++ build and for `clang-format`. |
| `RUST_DEFAULT_VERSION` | `1.79` | The Rust toolchain installed by `pkg-user-install-rust`. |
| `TESTS_FAILFAST` | `0` | Stop the functional tests after the first failure. |
| `TESTS_COMBINED_LOGS` | `0` | Print a combined log of the given length on failure. |

The target is auto-detected from the host: `x86_64-pc-linux-gnu` or
`aarch64-linux-gnu` on Linux, `x86_64-apple-darwin` or
`aarch64-apple-darwin` on macOS and `x86_64-w64-mingw32` under MSYS. The
supported targets are:

| Tier | Targets | Use |
| --- | --- | --- |
| 1 | `x86_64-pc-linux-gnu`, `aarch64-linux-gnu` | Critical infrastructure; fully tested. |
| 2 | `x86_64-apple-darwin`, `aarch64-apple-darwin`, `x86_64-w64-mingw32` | Development and end-user platforms. |
| 3 | `arm-linux-gnueabihf` | Best effort. |

## The three build steps

```bash
./make.sh build-deps   # 1. native dependencies
./make.sh build-conf   # 2. configure
./make.sh build-make   # 3. compile
```

### 1. `build-deps` — the `depends` system

`depends/` is the dependency build system inherited from Bitcoin Core. It
compiles the native libraries that the node needs and installs them into
`build/depends/<TARGET>`, together with a `share/config.site` file.

```bash
make -C depends DESTDIR=build/depends HOST=<TARGET> -j<jobs>
```

The packages that the `depends` system knows are defined in
`depends/packages/`:

| Package | Version | Purpose |
| --- | --- | --- |
| `bdb` | 4.8.30 | Berkeley DB, the wallet database. |
| `boost` | 1.86.0 | Boost.Test and the utility libraries (chrono, thread). |
| `sqlite` | 3.50.4 | The SQLite descriptor wallet. |
| `zeromq` | 4.3.5 | The ZMQ notification interface. |
| `protobuf` | 27.3 | The code generator (`protoc`) for the gRPC service. |
| `rapidcheck` | `d9482c6` | Property-based testing in the C++ unit tests. |
| `rust` | rustup | The Rust toolchain; the compiler version is pinned by `rust-toolchain.toml`. |
| `solc` | 0.8.28 | The Solidity compiler (`SOLC_PATH`) used by the functional tests. |

If you build the dependencies yourself, use the `depends` system instead of
the distribution packages — the wallet database format of Berkeley DB 4.8 is
not compatible with the 5.x versions that the distributions ship.

### 2. `build-conf` — `autogen.sh` and `configure`

```bash
./autogen.sh
CONFIG_SITE=build/depends/<TARGET>/share/config.site \
  ./configure --prefix=build/depends/<TARGET> <MAKE_CONF_ARGS>
```

`configure` reads the `config.site` file of the `depends` system, so the C++
build links against exactly the library versions that were just built. The
default arguments, chosen by `make.sh`, are:

| Argument | Effect |
| --- | --- |
| `--enable-glibc-back-compat` | Linux only: keeps the binary compatible with older glibc versions. |
| `--enable-static` | Links the node statically where possible. |
| `--enable-reduce-exports` | Hides internal symbols from the dynamic symbol table. |
| `LDFLAGS=-static-libstdc++` | Avoids a runtime dependency on the system libstdc++. |
| `--enable-debug` | Added when `MAKE_DEBUG=1` (the default): debug symbols and no optimisation. |

On `x86_64-pc-linux-gnu` the script also selects `clang-15` and `clang++-15`
unless `CC` and `CXX` are already set in the environment.

### 3. `build-make` — `make`

```bash
make -j<jobs> JOBS=<jobs>
```

The build produces the C++ binaries and drives the Cargo build of the Rust
workspace through `lib/Makefile.am`.

## The Rust part of the build

The Rust workspace lives in `lib/`. Its members are the `ain-*` crates plus
`cli` and `parser` (`lib/Cargo.toml`); the crates are described in the
[codebase map](../codebase-map/index.md).

The toolchain is pinned by `rust-toolchain.toml`:

```toml
[toolchain]
channel = "1.79"
```

`rustup` therefore selects the right compiler automatically once the
workspace is checked out. Install it with:

```bash
./make.sh pkg-user-install-rust
```

`lib/Makefile.am` wraps Cargo and exposes it to the autotools build:

| Target | Command |
| --- | --- |
| `build` | `cargo build` and copies `libain_rs_exports.a` into the Cargo target directory. |
| `check` | `cargo check`. |
| `test` | `cargo test`. |
| `clippy` | `cargo clippy -- -Dwarnings`. |
| `fmt` / `fmt-check` | `cargo fmt` / `cargo fmt --check`. |
| `doc` | `cargo doc`. |

Call them through the script, which enters the build directory and passes the
right environment:

```bash
./make.sh lib build
./make.sh lib test
./make.sh lib clippy
```

!!! note

    The Cargo build produces the bridge header and the static library
    `libain_rs_exports.a` that the C++ side links against. If the C++ build
    fails with a missing `ain_rs_exports.h`, build the Rust workspace first
    with `./make.sh lib build`.

## Platform notes

### Linux (Debian and Ubuntu)

```bash
# Build requirements and the native libraries.
sudo ./make.sh pkg-install-deps

# The Rust toolchain.
./make.sh pkg-user-install-rust

# The build itself.
./make.sh build
```

`pkg-install-deps` installs `build-essential`, `git`, `libtool`,
`autotools-dev`, `automake`, `pkg-config`, `bsdmainutils`, `python3`,
`python3-pip`, `python3-venv`, `libssl-dev`, `libevent-dev`,
`libboost-chrono-dev`, `libboost-test-dev`, `libboost-thread-dev`,
`libminiupnpc-dev`, `libzmq3-dev`, `libqrencode-dev`, `libdb-dev`,
`libdb++-dev`, `libdb5.3-dev`, `libdb5.3++-dev`, `libsqlite3-dev`,
`curl`, `cmake`, `zip`, `unzip`, `wget`, `ccache`, `libc6-dev`,
`gcc-multilib`, `locales` and `python3-dev`.

To use the distribution libraries instead of the `depends` build, pass
`--with-incompatible-bdb` to `configure`: the Berkeley DB 5.x packages of the
distributions are not binary compatible with the 4.8 wallets of the released
binaries.

### macOS

```bash
xcode-select --install
brew install automake berkeley-db@4 libtool boost openssl pkg-config protobuf python qrencode
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
./make.sh build
```

Apple Clang 13 or newer (Xcode 13 or newer) is required, because the code
base is compiled as C++20. On Apple Silicon the target is
`aarch64-apple-darwin`; add the Rust target with
`rustup target add aarch64-apple-darwin`.

The macOS build of the dependencies needs an SDK that `make.sh` downloads
automatically (`pkg-local-ensure-osx-sysroot`).

### Windows

The supported option is a cross build with the MinGW-w64 tool chain, either
on Linux or in the Windows Subsystem for Linux on Ubuntu 22.04:

```bash
sudo TARGET=x86_64-w64-mingw32 ./make.sh ci-setup-deps
TARGET=x86_64-w64-mingw32 ./make.sh ci-setup-user-deps
rustup target add x86_64-pc-windows-gnu
TARGET=x86_64-w64-mingw32 ./make.sh build
```

The source directory must live inside the default WSL file system (for
example `/usr/src/ain`), not under `/mnt/d/`, because the autotools scripts
of the dependencies fail on the mounted Windows paths.

## What the build produces

| Artefact | Location | Produced by |
| --- | --- | --- |
| `defid` | `build/src/defid` | The autotools build. |
| `defi-cli` | `build/src/defi-cli` | The autotools build. |
| `test_defi` | `build/src/test/test_defi` | The unit test binary (`--enable-tests`). |
| `libain_rs_exports.a` | `build/lib/target/<triplet>/<profile>/` | The Cargo build. |
| The Rust services | `build/lib/target/<triplet>/<profile>/` | The Cargo build. |

`./make.sh deploy` installs the binaries into `build/<image>/`, and
`./make.sh package` creates the release archive (`tar.gz`, or `zip` for the
Windows target) together with its SHA-256 file.

## Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| `configure: error: C++ compiler cannot create executables` | The compiler is too old. The code base needs GCC 10+ or Clang 12+ for C++20. |
| `error: 'ain_rs_exports.h' file not found` | The Rust workspace has not been built. Run `./make.sh lib build`. |
| `Berkeley DB 5.3` is detected | The distribution packages were used. Build with `depends`, or pass `--with-incompatible-bdb`. |
| The link step fails with a `libstdc++` version error | The release binaries are built with `LDFLAGS=-static-libstdc++`; keep the default `MAKE_CONF_ARGS`. |
| The build is very slow or the machine runs out of memory | Lower the parallelism: `MAKE_JOBS=2 ./make.sh build`. |
| `make.sh` fails with a `readlink` error | The system lacks GNU coreutils (`readlink -m`). Install `coreutils`. |

## Legacy documentation

This page consolidates the platform notes of the legacy documentation:
`doc/build-quick.md`, `doc/build-unix.md`, `doc/build-osx.md`,
`doc/build-windows.md` and `doc/dependencies.md`. The legacy files remain in
the repository because the build system ships them (`Makefile.am` →
`DIST_DOCS`); they are replaced by a pointer to this page.
