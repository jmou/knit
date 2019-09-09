# Open questions

## Logical \<-> physical data storage

Local FS, S3, relational DB, Google Sheets, Google Drive.
Non-storage mediums: e-mail, message queue, RPC, Slack, Arrow.
Immutability (tamper proofing) vs tamper evidence.
Storage system abstraction.

## Side effects

Actions: publish, Slack.

## Asynchronicity

Manual: QA, closed data analysis, Excel, Mechanical Turk.
Long-running operations.
Convergent operations: model training.

## Datasets

Defined by flow?
vs directories.

## Resource abstractions (unit, job result, direct inputs)

Interplay between data and code change. Backtesting. Regression tracking.

## Non-materialized transforms

DB views.
Streaming pipelines.

## Streaming data

In some use cases, maybe data partitioning.
vs Nifi.

## Reusability

Flow parameterization.
Instantiate parameterized flow with overridden units.
Relate to parameterized type systems, traits.
Open-closed principle. Composition (trait-like) vs overrides (OOP-like).

## Modularity

1:1 w/ flows?

## Dependency graph evaluation

Runtime vs top-down dynamic vs static.

## Inter-module dependencies

## Failure handling

Testing.

## Metadata

Data format. Taints (e.g., PII). Access control. Documentation.
Run time/duration, commit?
Propagation.
Access revocation. Mutable?
Readable by commands?
vs parameterization.

## Determinism

Resolve jobs to resources. Execution nonce in job. Execution epoch (single job type, inconsistencies possible). Triple (job, epoch, resource).

## Environment separation

Don't transform within prod databases.
Control w/ metadata?

# TODO

## Demo features

- Pipeline history by target unit
- Reddit voting

## Value props

- Collaboration and commenting
- Data vetting
- Frictionless branching
- Templatized presentation

## Logical units

- csv
- sql
- jinja
- html
- command
