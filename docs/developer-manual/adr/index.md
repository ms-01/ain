# Architecture Decision Records

An Architecture Decision Record (ADR) captures a decision that shapes the
node: why it was taken, what alternatives were rejected and what the
consequences are. ADRs are part of the
[Developer & Architecture Manual](../index.md) and follow the
[MADR](https://adr.github.io/madr/) format.

The structure of the documentation suite itself is decided by
[ADR-0002](ADR-0002-documentation-structure.md); the rules for writing ADRs
are defined there as well.

## Records

| ADR | Status | Title |
| --- | --- | --- |
| [ADR-0001](ADR-0001-pos-chainwork-header-tip-selection.md) | Accepted | PoS Chainwork Soundness and Anchored Header-Tip Selection |
| [ADR-0002](ADR-0002-documentation-structure.md) | Accepted | Canonical Documentation Structure |

## When to write an ADR

Write an ADR when a change is hard to reverse, when it constrains other parts
of the code base, or when the reasoning behind a decision would otherwise be
lost. Typical cases are:

* a change to the consensus rules or to an activation-height gating pattern;
* a change to the boundary between the native (C++/DVM) and the MetaChain
  (Rust/EVM) layers;
* a change to the on-disk format, the RPC surface or the build layout;
* a change to the documentation structure itself.

A change that is local, obvious and easy to revert does not need an ADR: the
pull request description is the record.

## Numbering and file names

* ADRs live in `docs/developer-manual/adr/`.
* Numbers are assigned sequentially and are never reused, not even when an ADR
  is rejected or withdrawn. The next number is always the highest existing
  number plus one.
* File names follow `ADR-<number>-<kebab-case-title>.md`, for example
  `ADR-0003-header-tip-selection-revision.md`.
* The title of the record is `# ADR-<number>: <Title>`.

## Status lifecycle

| Status | Meaning |
| --- | --- |
| `Proposed` | The decision is drafted and under review; no code depends on it yet. |
| `Accepted` | The decision is in force. The implementation may change the surrounding code, but not the decision itself. |
| `Deprecated` | The decision no longer applies, but it is not replaced by another decision. |
| `Superseded` | The decision is replaced by a later ADR, which is referenced explicitly. |

The status is a single word in the `## Status` section of the record. A
record that is accepted stays in place; it is never deleted, because the
reasoning behind the current behaviour has to remain traceable.

## Template

Copy this skeleton into a new file in `docs/developer-manual/adr/` and fill in
every section:

```markdown
# ADR-<number>: <Title>

## Status

Proposed

## Context

<What is the problem? What forces are at play? What constraints apply
(consensus safety, compatibility, performance, release process)?>

## Decision

<What is the decision, stated in the present tense? Describe the concrete
mechanism, the files that implement it and the interfaces it changes.>

## Consequences

<What becomes easier, what becomes harder, what is now possible and what is
no longer possible. Name the accepted residual risks explicitly.>
```

The four headings `Status`, `Context`, `Decision` and `Consequences` are
required. The layout check (`test/lint/check-docs-structure.py`) rejects a
record that is missing one of them, and it runs in the documentation workflow.

## Writing a good record

* **Context before decision.** A reader must understand the problem before
  reading the solution. Link the relevant files, functions and consensus
  parameters with relative paths.
* **State the mechanism.** "We use a chainwork-based scheme" is not a
  decision; "the header tip is selected by `IsBetterHeaderTip()` in
  `src/validation.cpp`" is.
* **Name the residual risks.** Every hardening has an accepted weakness; write
  it down and link the follow-up issue instead of hiding it.
* **Mark the consensus impact.** If the decision changes consensus rules, say
  so explicitly and point at the activation height in
  `src/consensus/params.h`.
* **Keep it English and reviewable.** ADRs are written in English, use short
  sentences and contain no personal data.

## Reviewing a proposal

1. The author opens a pull request that adds the ADR file and wires it into
   the navigation of `mkdocs.yml`.
2. Reviewers check that the context is complete, that the decision is
   implementable and that the consequences are honest about the trade-offs.
3. The status changes from `Proposed` to `Accepted` once the decision is
   agreed; the change is a separate, explicit commit.
4. The implementation follows in the same or in a later pull request and
   references the ADR.

## Amending an accepted ADR

Accepted ADRs are immutable with two exceptions: the `## Status` section and
additive amendments that clarify the decision without changing it. If a
decision is replaced, write a **new** ADR that references the old one by
number and mark the old record as `Superseded`. The history of the node has
to stay readable.

## Related pages

* [Developer & Architecture Manual](../index.md) — the manual this section
  belongs to.
* [Consensus](../consensus/index.md) — consensus rules, hardfork timeline and
  activation-height gating.
* [Architecture](../architecture/index.md) — the design the decisions above
  are about.
