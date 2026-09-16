# Proportionate Complexity and Maintainable Integrations

Status: Accepted. Date: 2026-09-12.

## Context and Scope

SpecForge aims to be lightweight, fast and fluid. Implementation and ongoing
maintenance cost must remain proportionate to demonstrated user benefit. This
decision applies project-wide to enhancements, optimizations and compatibility
work, including infrastructure introduced to support or test them.

It does not relax correctness, data integrity, security, or existing product/API
contracts. Unresolved correctness defects must remain explicitly identified.

## Decision

### Prefer supported, maintainable implementations

Within required product and correctness contracts, reliability, predictable
maintenance and low complexity take priority over optimal visual or performance
results. Prefer officially documented platform APIs, supported dependency interfaces and
existing ownership boundaries. SpecForge remains responsible for its integration's
lifetime, failure and upgrade behavior. Changing this priority requires an explicit
revision of this decision; performance alone does not justify an exception.

Only attempt bounded optimizations within these supported integration contracts
and maintenance constraints. A better effect does not justify exceeding them;
limited improvement is acceptable, and perfect results are not guaranteed.

### Bound scope and accept limited outcomes

Define the observable benefit and proportionate regression checks before expanding
implementation. Assess cost by ongoing responsibilities, lifetime interactions,
upgrade burden and regression surface, rather than line count. Further experiments
must answer an unresolved question with a new hypothesis.

If the maintainable approach is unsafe, ineffective or disproportionate, stop that
path, retain current behavior or defer the discretionary improvement, and record
the limitations. Partial improvement is acceptable; unresolved symptoms must not
be reported as fixed.

A local improvement does not authorize broader infrastructure. Expanding the agreed
scope requires an explicit project decision covering benefit, supported alternatives,
ownership, failure/upgrade testing and removal or replacement. Record material
architecture changes in an ADR. Routine choices within accepted scope need no
repeated permission or additional architecture paperwork.

### Separate experiments from production commitments

Experimental success alone does not establish net user benefit or authorize
production adoption. Promote only code needed for a validated solution; remove or
isolate unnecessary scaffolding. Existing experimental code is subject to the same
review and gains no production commitment merely by having been implemented.

## Consequences

Discretionary improvements may be narrower, delayed or declined to keep maintenance
bounded. Change descriptions should state the demonstrated benefit, added maintenance
responsibilities and remaining limitations. This does not require a separate
checklist or approval ceremony for every change.

## Relationship to Existing Decisions

This complements [ADR 0006](0006-distribution-capability-and-linkage-policy.md)'s
dependency policy and [ADR 0009](0009-source-loading-dependency-boundaries.md)'s
justified dependency boundaries. It does not supersede specific architecture or
product contracts established by other ADRs.
