Dependencies
============

These are the dependencies currently used by Bitcoin Core. You can find instructions for installing them in the `build-*.md` file for your platform.

| Dependency | Version used | Minimum required | CVEs | Shared | [Bundled Qt library](https://doc.qt.io/qt-5/configure-options.html#third-party-libraries) |
| --- | --- | --- | --- | --- | --- |
| Berkeley DB | [4.8.30](https://www.oracle.com/technetwork/database/database-technologies/berkeleydb/downloads/index.html) | 4.8.x | No |  |  |
| Boost | [1.86.0](https://www.boost.org/users/download/) | [1.77.0](https://www.boost.org/users/download/) | No |  |  |
| Clang |  | [12+](https://releases.llvm.org/download.html) (C++20 support) |  |  |  |
| Expat | [2.2.7](https://libexpat.github.io/) |  | No | Yes |  |
| fontconfig | [2.12.1](https://www.freedesktop.org/software/fontconfig/release/) |  | No | Yes |  |
| FreeType | [2.7.1](https://download.savannah.gnu.org/releases/freetype) |  | No |  |  |
| GCC |  | [10+](https://gcc.gnu.org/) (C++20 support) |  |  |  |
| HarfBuzz-NG |  |  |  |  |  |
| libpng |  |  |  |  | [Yes](https://github.com/bitcoin/bitcoin/blob/master/depends/packages/qt.mk) |
| librsvg | |  |  |  |  |
| OpenSSL | [1.0.1k](https://www.openssl.org/source) |  | Yes |  |  |
| PCRE |  |  |  |  | [Yes](https://github.com/bitcoin/bitcoin/blob/master/depends/packages/qt.mk) |
| protobuf | [2.6.1](https://github.com/google/protobuf/releases) |  | No |  |  |
| Python (tests) |  | [3.5](https://www.python.org/downloads) |  |  |  |
| qrencode | [3.4.4](https://fukuchi.org/works/qrencode) |  | No |  |  |
| Qt | [5.9.7](https://download.qt.io/official_releases/qt/) | [5.5.1](https://github.com/bitcoin/bitcoin/issues/13478) | No |  |  |
| SQLite | [3.50.4](https://sqlite.org) | 3.7.17 | No |  |  |
| XCB |  |  |  |  | [Yes](https://github.com/bitcoin/bitcoin/blob/master/depends/packages/qt.mk) (Linux only) |
| xkbcommon |  |  |  |  | [Yes](https://github.com/bitcoin/bitcoin/blob/master/depends/packages/qt.mk) (Linux only) |
| ZeroMQ | [4.3.5](https://github.com/zeromq/libzmq/releases) | 4.0.0 | No |  |  |
| zlib | [1.2.11](https://zlib.net/) |  |  |  | No |

C++ standard
------------
The code base is compiled as C++20 (`-std=c++20`, no GNU extensions). The compiler
must therefore support C++20, which means GCC 10 or newer and Clang 12 or newer.
GCC 11+ and Clang 14+ are recommended, as they are the versions used by the CI
builder images. Apple Clang 13 or newer (Xcode 13 or newer) is required on macOS.

Controlling dependencies
------------------------
Some dependencies are not needed in all configurations. The following are some factors that affect the dependency list.

#### Options passed to `./configure`
* Berkeley DB is not needed with `--disable-wallet`.
* SQLite is not needed with `--disable-wallet` or `--without-sqlite`.
* protobuf is not needed with `--disable-bip70`.
* If the qrencode dependency is absent, QR support won't be added. To force an error when that happens, pass `--with-qrencode`.
* ZeroMQ is needed only with the `--with-zmq` option.

#### Other
* librsvg is only needed if you need to run `make deploy` on (cross-compilation to) macOS.
