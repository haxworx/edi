# EDI - The Enlightened IDE

EDI is an integrated development environment built with the Enlightenment
Foundation Libraries (EFL). It is designed to make EFL and Enlightenment
development approachable while remaining useful for a wide range of C/C++ and
general software projects.

![Edi Logo](data/desktop/edi.png?raw=true "EDI icon")

## 📚 Table of Contents
- [🔥 Features](#-features)
- [🗂️ Supported Languages and File Types](#%EF%B8%8F-supported-languages-and-file-types)
- [📌 Requirements](#-requirements)
- [⚙️ Build Instructions](#%EF%B8%8F-build-instructions)
- [🚀 Installation](#-installation)
- [🎯 Usage Examples](#-usage-examples)
- [🧰 Helper Programs](#-helper-programs)
- [🔧 Meson Options](#-meson-options)
- [🛠️ Troubleshooting](#%EF%B8%8F-troubleshooting)
- [🤝 Contributions](#-contributions)

## 🔥 Features
- Full IDE experience for EFL-focused development.
- Project-aware editing with syntax highlighting and code intelligence.
- Optional `libclang` integration for inline diagnostics and completion.
- Integrated build/test/run workflows inside the IDE.
- AI assistant panel with project-scoped provider configuration and inline responses.
- Built-in source control workflows via Git.
- SCM log browser with selectable commits and dedicated commit-change viewer.
- Multiple build-system backends detected automatically:
  - `meson`
  - `make` / autotools-style trees
  - `cmake`
  - `cargo`
  - `python` (`setup.py`)
  - `go`
- Additional standalone utilities:
  - `edi_build` for CLI project build orchestration
  - `edi_scm` for source-control operations and diagnostics

## 🗂️ Supported Languages and File Types
Language-aware editing and tooling currently includes:
- C / C headers (`text/x-csrc`, `text/x-chdr`)
- Python (`text/x-python`, `text/x-python3`)
- Rust (`text/rust`)
- Go (`text/x-go`)
- C# (`text/x-csharp`)
- Shell scripts (`application/x-shellscript`)

Content/file handlers include:
- Generic text (`text/plain`, and other `text/*` via text fallback)
- Diff/Patch (`text/x-diff`, `text/x-patch`)
- Images (`image/*`)
- Additional MIME mappings can be configured in EDI settings.

## 📌 Requirements
Minimum core dependencies:
- EFL development libraries (**>= 1.24.0**)
- `meson` (**>= 0.50.0**)
- `ninja`
- C compiler toolchain (`gcc` or `clang`)
- `pkg-config`

Optional but recommended:
- `libclang` development package for C/C++ autocomplete and inline errors
- `bear` for compile-command capture with make-based builds
- `git` for source control features in EDI and `edi_scm`
- AI provider credentials (API key/token) for AI assistant features

Example package installs:

### Debian / Ubuntu
```sh
sudo apt update
sudo apt install efl-all-dev meson ninja-build pkg-config git
sudo apt install libclang-dev bear
```

### Fedora
```sh
sudo dnf install efl-devel meson ninja-build pkgconf-pkg-config git
sudo dnf install clang-devel bear
```

If EFL is in a custom prefix (for example `/opt`), ensure `PKG_CONFIG_PATH` is
set correctly:

```sh
export PKG_CONFIG_PATH="$PKG_CONFIG_PATH:/opt/libdata/pkgconfig"
```

## ⚙️ Build Instructions
Configure and build:

```sh
meson setup build
ninja -C build
```

Build without `libclang` support:

```sh
meson setup build -Dlibclang=false
ninja -C build
```

Run test suite:

```sh
meson test -C build
```

## 🚀 Installation
Install system-wide after building:

```sh
sudo ninja -C build install
```

Package/staged install:

```sh
DESTDIR=/path/to/stage ninja -C build install
```

## 🎯 Usage Examples
Open project picker:

```sh
edi
```

Open a project directly:

```sh
edi ~/Code/myproject
```

Main `edi` options:

```sh
edi -c            # create a new project
edi -h            # help
edi -V            # version
edi -L            # license
edi -C            # copyright
```

AI assistant setup:

1. Open `Settings -> Project -> AI Agents`.
2. Enable AI agent support.
3. Select a provider and configure `Model`, `Endpoint`, and `API Key / Token`.
4. Use the bottom-panel `AI` tab to send prompts and copy responses.

## 🧰 Helper Programs
### `edi_build`
Command-line build helper for the current project directory.

```sh
edi_build [options] [build|clean|test|create|example]
```

Examples:

```sh
edi_build                 # build (default)
edi_build test            # run tests for detected provider
edi_build clean           # clean artifacts
edi_build create TEMPLATE PARENT_PATH PROJECT_NAME PROJECT_URL AUTHOR_NAME AUTHOR_EMAIL
edi_build example EXAMPLE_NAME PARENT_PATH PROJECT_NAME
```

### `edi_scm`
Source-control utility UI for Git-backed workflows.

```sh
edi_scm [options] [directory]
```

Useful options:

```sh
edi_scm -c                    # open commit screen
edi_scm -d                    # show working-tree diff view
edi_scm -l                    # show log view
edi_scm -s <commit-hash>      # show commit changes view
edi_scm -a user@example.com   # print gravatar URL for email
edi_scm -A user@example.com   # run avatar download/debug checks
edi_scm -h                    # help
```

In log view (`edi_scm -l`), selecting a commit opens the commit changes view in
a new `edi_scm` window (`--show` mode) with a scrollable diff panel.

## 🔧 Meson Options
Project-specific options from `meson_options.txt`:

```sh
-Dlibclang=true|false
-Dbear=true|false
-Dlibclang-libdir=/custom/lib/path
-Dlibclang-headerdir=/custom/include/path
```

Examples:

```sh
meson setup build -Dlibclang=false
meson setup build -Dlibclang-headerdir=/usr/lib/llvm-18/include -Dlibclang-libdir=/usr/lib/llvm-18/lib
```

## 🛠️ Troubleshooting
- `Clang header not found!`: install `libclang` development headers or build with `-Dlibclang=false`.
- EFL dependency not found: verify EFL dev packages are installed and `PKG_CONFIG_PATH` is exported correctly.
- SCM actions fail: ensure `git` is installed and the project is inside a valid repository.
- AI panel shows `Agent is not configured`: configure provider settings in `Settings -> Project -> AI Agents`.
- Provider not detected by `edi_build`: run inside a supported project tree containing one of:
  `meson.build`, `Makefile`, `CMakeLists.txt`, `Cargo.toml`, `setup.py`, or `.go` files.

## 🤝 Contributions
Patches, bug fixes, and portability improvements are welcome.

If you are adding a substantial feature, please include:
- clear rationale
- build/run impact notes
- testing notes (platform and workflow coverage)
