# DeFiChain Documentation — Authoring Guide

This directory is the canonical documentation suite of this repository. It is
structured, authored and validated according to
[ADR-0002: Canonical Documentation Structure](developer-manual/adr/ADR-0002-documentation-structure.md),
which is the binding contract for everything described below.

## Layout

```text
docs/
├── README.md                          # This guide (contributor entry point)
├── index.md                           # Landing page of the documentation hub
├── assets/                            # Shared images, diagrams and static media
├── user-manual/                       # Manual 1: User & Protocol
│   ├── index.md
│   ├── getting-started/               # Core concepts, wallets, UTXO vs. account model
│   ├── dvm/                           # Native DVM mechanics and transactions
│   ├── evm/                           # MetaChain / EVM layer and cross-layer transfers
│   ├── staking-masternodes/           # Masternodes, staking, minting and rewards
│   ├── dex-liquidity/                 # DEX, liquidity pools and swapping
│   ├── loans-dtokens/                 # Vaults, collateral, loans, dTokens and oracles
│   └── governance/                    # On-chain governance, CFPs and DFIPs
├── node-manual/                       # Manual 2: Node Software
│   ├── index.md
│   ├── installation/                  # Binaries, Docker, system requirements
│   ├── configuration/                 # defi.conf, CLI flags, networking, pruning, IBD
│   ├── operations/                    # Running full nodes, masternodes, backups, monitoring
│   ├── rpc-cli/                       # JSON-RPC and defi-cli command reference
│   └── troubleshooting/                # Common issues, logs, recovery and reindexing
└── developer-manual/                  # Manual 3: Developer & Architecture
    ├── index.md
    ├── architecture/                  # High-level design, C++/DVM/Rust EVM boundaries
    ├── codebase-map/                  # "Where to find what" — directory and module guide
    ├── consensus/                     # Consensus rules, hardforks and DF*Height gating
    ├── subsystems/                    # Mempool, P2P, validation, state storage, RPC layer
    ├── building-testing/              # Build system, C++/Rust/Python tests, fuzzing, linting
    └── adr/                           # Architecture Decision Records
```

`docs/requirements.txt`, `docs/README.md` and this guide are contributor-facing
files and are not part of the published site.

## Which manual do I write in?

| If your reader wants to … | Manual | Typical document type |
| --- | --- | --- |
| use the protocol (wallets, DeFi actions, staking, governance) | `user-manual/` | tutorial, explanation |
| run the software (install, configure, operate, automate) | `node-manual/` | how-to guide, reference |
| change the software (architecture, consensus, subsystems, tests) | `developer-manual/` | explanation, reference |

Every document serves exactly one audience. If a topic is needed by two
audiences, write two documents that link to each other instead of duplicating
or mixing content.

## Authoring conventions

### Document types (Diátaxis)

* **Tutorial** — a guided, linear, first-person walkthrough that ends in a
  working result. The reader follows along.
* **How-to guide** — a task-oriented recipe for a concrete goal, written for a
  reader who already knows the basics.
* **Explanation** — background and reasoning: how and why the system behaves
  the way it does.
* **Reference** — exhaustive, factual and non-narrative: commands, options,
  parameters, error codes.

Do not mix styles inside a document. A tutorial that stops to explain the
protocol internals belongs in two documents.

### Markdown conventions

* One `#` title per file, matching the section intent; section entry points are
  always `index.md`.
* Use relative links between documents (`../node-manual/configuration/index.md`)
  so that they resolve both in the repository and in the rendered site.
* Prefer fenced code blocks with a language tag (```` ```bash ````, ```` ```json ````,
  ```` ```cpp ````) and give every shell block a copy-pasteable command.
* Use `!!! note`, `!!! warning` or `!!! tip` admonitions for side information
  instead of indented paragraphs.
* Diagrams use Mermaid fenced blocks (```` ```mermaid ````).
* Headings are sentence case and do not skip levels.

### Tone and accuracy

* Address the reader as "you"; keep sentences short and active.
* Document the behaviour of the current `master` branch of this repository.
  Never describe a feature or an RPC that is not present in the code.
* When a behaviour is gated by an activation height (for example `DF25Height`),
  state the height explicitly and link to `developer-manual/consensus/index.md`.
* Do not paste credentials, private keys or internal hostnames into documents.

### Assets

Shared images, diagrams and other static media belong in `docs/assets/`. Use
relative paths (`../../assets/example.png`) and keep file names descriptive and
lower-case.

## Architecture Decision Records

ADRs live in `developer-manual/adr/` and follow the MADR format. Every ADR must
contain the sections `## Status`, `## Context`, `## Decision` and
`## Consequences`. Number them sequentially (`ADR-0003-…`), keep them immutable
once accepted, and record a decision that replaces an earlier one in a new ADR
that references the old one. See
[ADR-0002](developer-manual/adr/ADR-0002-documentation-structure.md) for the
rules that govern the documentation structure itself.

## Local preview

The site is built with [Material for MkDocs](https://squidfunk.github.io/mkdocs-material/):

```bash
python3 -m venv .venv-docs
. .venv-docs/bin/activate
pip install -r docs/requirements.txt   # pinned by the static-site setup
mkdocs serve          # live preview on http://127.0.0.1:8000/
mkdocs build --strict # strict build; broken links fail the build
```

## Generated reference pages

The RPC and CLI reference below `node-manual/rpc-cli/categories/` is generated
from the binary itself. `contrib/devtools/gen-rpc-docs.py` starts a temporary
regtest node, reads the help text of every command from it and renders the
Markdown pages:

```bash
# Build the node first; the script uses build/src/defid and build/src/defi-cli.
./make.sh build

# Regenerate the pages.
python3 contrib/devtools/gen-rpc-docs.py

# Verify that the committed pages still match the binary (exit code 1 on drift).
python3 contrib/devtools/gen-rpc-docs.py --check
```

Never edit a generated page by hand; the next run of the generator overwrites
it. The details are described in
[RPC & CLI Reference](node-manual/rpc-cli/index.md).

## Legacy `doc/` directory

The legacy `doc/` directory still exists and is still referenced by the build
system (`Makefile.am` → `DIST_DOCS`) and by the Doxygen configuration
(`configure.ac` → `doc/Doxyfile.in`). It is not removed by the restructuring.
When a legacy page is consolidated into `docs/`, move the content, keep the Git
history (a rename, not a copy-and-delete) and remove the legacy file in the
same change so that exactly one authoritative version remains.

A page that is already consolidated but still has to be shipped by the build
system stays in `doc/` with a pointer notice at its top. The notice names the
canonical page below `docs/`, so a reader of the legacy file is sent to the
authoritative version. The pointer notices are in place for

| Legacy page | Canonical page |
| --- | --- |
| `doc/setup-nodes.md` | `node-manual/installation/binaries.md` |
| `doc/setup-nodes-docker.md` | `node-manual/installation/docker.md` |
| `doc/files.md` | `node-manual/installation/data-directory.md` |
| `doc/defi-conf.md` | `node-manual/configuration/index.md` |
| `doc/init.md` | `node-manual/operations/index.md` |
| `doc/tor.md` | `node-manual/configuration/networking-privacy.md` |
| `doc/i2p.md` | `node-manual/configuration/networking-privacy.md` |
| `doc/dnsseed-policy.md` | `node-manual/configuration/networking-privacy.md` |
| `doc/reduce-memory.md` | `node-manual/configuration/performance-ibd.md` |
| `doc/reduce-traffic.md` | `node-manual/configuration/performance-ibd.md` |
| `doc/setup-masternodes.md` | `node-manual/operations/masternodes.md` |
| `doc/JSON-RPC-interface.md` | `node-manual/operations/rpc-rest-interfaces.md` |
| `doc/REST-interface.md` | `node-manual/operations/rpc-rest-interfaces.md` |
| `doc/zmq.md` | `node-manual/operations/zmq-notifications.md` |
| `doc/external-signer.md` | `node-manual/operations/backups-signers.md` |

`doc/README.md` carries the same table for readers who enter the directory
through its index.

## Navigation

The navigation of the site lives in `mkdocs.yml` at the repository root. Every
page below `docs/` has to be listed there; the canonical-layout check
(`test/lint/check-docs-structure.py`) reports pages that are missing from the
navigation and navigation entries that point to pages that do not exist. The
generated pages below `node-manual/rpc-cli/categories/` are part of the
navigation as well, so a new RPC category is visible in the site as soon as
the generator has produced it.

## Change checklist

Before opening a pull request that touches `docs/`:

1. The document is in the manual of its audience and has the right type.
2. Links are relative and resolve; `mkdocs build --strict` succeeds.
3. The canonical layout is respected: no new top-level directory under `docs/`.
4. ADR files contain all four MADR sections.
5. Legacy content that was consolidated has been removed from `doc/`, or
   carries a pointer notice to its canonical page below `docs/`.
6. Every new page is wired into the navigation of `mkdocs.yml`.
