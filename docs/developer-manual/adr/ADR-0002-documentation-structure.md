# ADR-0002: Canonical Documentation Structure

## Status

Accepted

## Context

DeFiChain's technical documentation is currently fragmented across the legacy
`doc/` directory of this repository, scattered Markdown files, external wikis and
website pages. The content mixes end-user protocol concepts, node operator
concerns and internal implementation details in the same flat directory, which
makes it hard for a reader to tell which document answers which question and
hard for contributors to decide where new documentation belongs.

The repository also lacks a written contract for documentation structure:

* There is no authoritative list of allowed top-level documentation directories,
  so ad-hoc directories appear over time.
* New documents are added without a documented audience, leading to duplicated
  and contradictory content.
* The legacy `doc/` tree is referenced by the build system (`Makefile.am`
  variable `DIST_DOCS`) and by the Doxygen configuration (`configure.ac`,
  `doc/Doxyfile.in`). Any restructuring must therefore not break those
  references while legacy content is progressively consolidated.
* Architecture Decision Records (ADRs) live in a top-level `docs/adr/`
  directory that is unrelated to the manual structure and is therefore easy to
  overlook.

A permanent, statically hosted documentation suite is planned. To make that
work, the directory layout, the authoring conventions and the ownership
boundaries must be fixed in a binding decision before content is migrated.

## Decision

### 1. Canonical layout

All new documentation lives under `docs/` in this repository and follows exactly
this top-level structure:

```text
docs/
├── README.md          # Authoring guide (contributor entry point, not part of the site)
├── index.md           # Landing page / hub of the three manuals
├── assets/            # Shared images, diagrams and static media
├── user-manual/       # Manual 1: User & Protocol
├── node-manual/       # Manual 2: Node Software
└── developer-manual/  # Manual 3: Developer & Architecture
```

No other top-level directory may be created under `docs/`. Each of the three
manuals contains an `index.md` and one directory per section, each with its own
`index.md`:

* `user-manual/`: `getting-started/`, `dvm/`, `evm/`, `staking-masternodes/`,
  `dex-liquidity/`, `loans-dtokens/`, `governance/`
* `node-manual/`: `installation/`, `configuration/`, `operations/`, `rpc-cli/`,
  `troubleshooting/`
* `developer-manual/`: `architecture/`, `codebase-map/`, `consensus/`,
  `subsystems/`, `building-testing/`, `adr/`

### 2. Audience-driven authoring (Diátaxis)

Each manual has a primary audience, and every document is written for exactly
one of them:

* `user-manual/` — readers who want to *use* the protocol (wallets, DeFi
  actions, staking, governance). Explanation and tutorial material.
* `node-manual/` — readers who *run* the software (installation, configuration,
  operations, RPC/CLI reference, troubleshooting). How-to and reference
  material.
* `developer-manual/` — readers who *develop* the software (architecture,
  codebase map, consensus rules, subsystems, build & test workflow, ADRs).
  Explanation and reference material.

Within a manual, a document is classified as *tutorial*, *how-to guide*,
*explanation* or *reference* and is written in the corresponding style: tutorials
are task-oriented and linear, how-to guides are problem-oriented, explanations
are conceptual and background-oriented, references are exhaustive and
non-narrative. Material that serves two audiences is split, not duplicated.

### 3. Ownership boundaries

* Incident analyses, temporary lab artifacts, release notes and research notes
  do **not** belong under `docs/`. They are maintained outside this repository
  or in clearly separated, non-canonical locations, and are never mixed into the
  three manuals.
* The three manuals are the only published documentation surface of this
  repository. Only the section directories listed above are allowed.
* Every manual section directory contains exactly one entry point, `index.md`,
  which states the section's scope and links to its documents.

### 4. Architecture Decision Records

ADRs are stored in `docs/developer-manual/adr/`. They follow the MADR format
and must contain the sections `## Status`, `## Context`, `## Decision` and
`## Consequences`. ADRs are immutable except for their status and for
superseding amendments; a decision that is replaced gets a new ADR that
references the old one. The previous ADR `ADR-0001-pos-chainwork-header-tip-selection.md`
was migrated from the former top-level `docs/adr/` directory to this location.

### 5. Coexistence with the legacy `doc/` tree

The legacy `doc/` directory remains in place and untouched while its content is
consolidated into `docs/`:

* `Makefile.am` (`DIST_DOCS`) and `configure.ac` (`doc/Doxyfile.in`) keep
  referencing `doc/`. Their behaviour and the distributed tarball content are
  not changed by this decision.
* Content is migrated file by file. A migration moves the content and
  preserves the Git history (rename detection), it never leaves a copy behind.
* Once a legacy file has been migrated, the legacy copy is deleted in the same
  change so that there is exactly one authoritative version.

### 6. Static site generation

The documentation is rendered with [Material for MkDocs](https://squidfunk.github.io/mkdocs-material/),
which is configured at the repository root (`mkdocs.yml`) with `docs/` as the
documentation directory. The site build is strict: broken internal links and
references to missing files fail the build. The structure described here is
validated automatically by a canonical-layout check, so violations of this ADR
are caught in CI rather than during review.

## Consequences

### Positive

* The location of a document, and the audience it addresses, become derivable
  from the path alone.
* New documentation has a defined home, which removes the ambiguity that
  previously led to duplicated or contradictory pages.
* The legacy `doc/` tree can be migrated incrementally without breaking the
  build system or the Doxygen output.
* The static site and the canonical-layout check enforce the structure
  mechanically, so the decision cannot silently drift.

### Negative

* Existing links to `doc/…` or `docs/adr/…` paths break once a file is
  migrated; redirects and link updates have to be handled per migration.
* Contributors have to choose an audience and a document type up front, which
  costs a little more effort per document than dropping a file into a flat
  directory.
* Two documentation trees (`doc/` and `docs/`) coexist during the migration
  period and must be kept distinguishable by the audience rules above.

### Alternatives considered

* **Keep a single flat `doc/` directory.** Rejected: it cannot express the
  audience split and gives no guidance for new documents.
* **Move everything to the workspace repository.** Rejected: the manuals must
  live next to the code they describe so that they can be updated and reviewed
  with it.
* **Move the legacy `doc/` tree into `docs/` in one step.** Rejected: it would
  break `DIST_DOCS` and the Doxygen configuration and would make review of
  individual migrations impossible.
* **Use Doxygen as the site generator.** Rejected: Doxygen is appropriate for
  API reference but not for tutorials, how-to guides and protocol explanations,
  and it cannot provide the tabbed three-manual navigation.
