#!/usr/bin/env python3
"""Validate the canonical documentation structure of this repository.

The layout that is checked here is defined by ADR-0002
(`docs/developer-manual/adr/ADR-0002-documentation-structure.md`):

* exactly three manuals below `docs/` (`user-manual/`, `node-manual/`,
  `developer-manual/`) plus the shared `assets/` directory,
* every manual and every manual section has an `index.md` entry point,
* no other top-level directory exists below `docs/`,
* every ADR in `docs/developer-manual/adr/` follows the MADR format and
  contains the headings `Status`, `Context`, `Decision` and `Consequences`.

The script has no third-party dependencies and can be run directly:

    python3 test/lint/check-docs-structure.py

Use `--docs-dir` to check a different documentation root (useful for
regression tests of the checker itself).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Top-level directories that are allowed below `docs/`.
ALLOWED_TOP_LEVEL_DIRS = ("assets", "developer-manual", "node-manual", "user-manual")

# Top-level files that are allowed below `docs/`.
ALLOWED_TOP_LEVEL_FILES = ("README.md", "index.md", "requirements.txt")

# Manual sections and their entry point, per manual.
REQUIRED_SECTIONS = {
    "user-manual": (
        "dex-liquidity",
        "dvm",
        "evm",
        "getting-started",
        "governance",
        "loans-dtokens",
        "staking-masternodes",
    ),
    "node-manual": (
        "configuration",
        "installation",
        "operations",
        "rpc-cli",
        "troubleshooting",
    ),
    "developer-manual": (
        "adr",
        "architecture",
        "building-testing",
        "codebase-map",
        "consensus",
        "subsystems",
    ),
}

# Files that live next to the manual sections but are not manuals themselves.
REQUIRED_FILES = ("README.md", "index.md")

# MADR headings every ADR has to provide.
REQUIRED_ADR_HEADINGS = ("Status", "Context", "Decision", "Consequences")


class Report:
    """Collects errors and warnings of a single check run."""

    def __init__(self) -> None:
        self.errors: list[str] = []
        self.warnings: list[str] = []

    def error(self, message: str) -> None:
        self.errors.append(message)

    def warn(self, message: str) -> None:
        self.warnings.append(message)


def check_required_files(docs_dir: Path, report: Report) -> None:
    """Check the files that must exist directly below `docs/`."""
    for name in REQUIRED_FILES:
        path = docs_dir / name
        if not path.is_file():
            report.error(f"missing required file: {rel(path, docs_dir)}")


def check_top_level(docs_dir: Path, report: Report) -> None:
    """Reject unexpected top-level directories and warn about unknown files."""
    for entry in sorted(docs_dir.iterdir(), key=lambda p: p.name):
        if entry.is_dir():
            if entry.name not in ALLOWED_TOP_LEVEL_DIRS:
                report.error(
                    f"unexpected top-level directory: {rel(entry, docs_dir)} "
                    f"(allowed: {', '.join(ALLOWED_TOP_LEVEL_DIRS)})"
                )
        elif entry.is_file() and entry.name not in ALLOWED_TOP_LEVEL_FILES:
            report.warn(f"unexpected top-level file: {rel(entry, docs_dir)}")


def check_manuals(docs_dir: Path, report: Report) -> None:
    """Check that every manual and every manual section has an index.md."""
    for manual, sections in REQUIRED_SECTIONS.items():
        manual_dir = docs_dir / manual
        if not manual_dir.is_dir():
            report.error(f"missing required directory: {rel(manual_dir, docs_dir)}")
            continue

        index = manual_dir / "index.md"
        if not index.is_file():
            report.error(f"missing entry point: {rel(index, docs_dir)}")

        for section in sections:
            section_dir = manual_dir / section
            if not section_dir.is_dir():
                report.error(f"missing required directory: {rel(section_dir, docs_dir)}")
                continue
            section_index = section_dir / "index.md"
            if not section_index.is_file():
                report.error(f"missing entry point: {rel(section_index, docs_dir)}")


def check_assets(docs_dir: Path, report: Report) -> None:
    """Check that the shared assets directory exists."""
    assets = docs_dir / "assets"
    if not assets.is_dir():
        report.error(f"missing required directory: {rel(assets, docs_dir)}")


def check_adrs(docs_dir: Path, report: Report) -> None:
    """Check that every ADR uses the MADR format with the required headings."""
    adr_dir = docs_dir / "developer-manual" / "adr"
    if not adr_dir.is_dir():
        # The missing directory is already reported by check_manuals().
        return

    for adr in sorted(adr_dir.glob("*.md"), key=lambda p: p.name):
        if adr.name == "index.md":
            continue
        headings = {
            line.lstrip("#").strip()
            for line in adr.read_text(encoding="utf-8").splitlines()
            if line.startswith("## ")
        }
        missing = [h for h in REQUIRED_ADR_HEADINGS if h not in headings]
        if missing:
            report.error(
                f"{rel(adr, docs_dir)}: missing required MADR heading(s): "
                f"{', '.join('## ' + h for h in missing)}"
            )


def rel(path: Path, root: Path) -> str:
    """Return `path` relative to `root` (or the absolute path if outside)."""
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--docs-dir",
        default="docs",
        help="documentation root to check (default: docs)",
    )
    args = parser.parse_args()

    docs_dir = Path(args.docs_dir).resolve()
    if not docs_dir.is_dir():
        print(f"error: documentation directory not found: {docs_dir}", file=sys.stderr)
        return 1

    report = Report()
    check_required_files(docs_dir, report)
    check_top_level(docs_dir, report)
    check_assets(docs_dir, report)
    check_manuals(docs_dir, report)
    check_adrs(docs_dir, report)

    for warning in report.warnings:
        print(f"warning: {warning}", file=sys.stderr)
    for error in report.errors:
        print(f"error: {error}", file=sys.stderr)

    if report.errors:
        print(
            f"docs structure check failed: {len(report.errors)} error(s), "
            f"{len(report.warnings)} warning(s)",
            file=sys.stderr,
        )
        return 1

    print(
        f"docs structure check passed: {len(report.warnings)} warning(s)",
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
