# Contributing to SatisfactoryTerraform

Thanks for your interest. This is the **in-game half** of a two-repo project;
before opening a PR, it helps to know how the pieces fit and — importantly —
what can and cannot be verified from a clone.

## The two repos

| Repo | What it is |
|---|---|
| [terraform-provider-satisfactory](https://github.com/daroco/terraform-provider-satisfactory) | The Terraform provider (Go), the **API contract** (`api/openapi.yaml`), an in-memory mock of that contract, and the examples. |
| **This repo** | The SML/UE C++ mod that implements the same contract inside a running game. |

`api/openapi.yaml` in the provider repo is the **single source of truth**. Any
change to the request/response shape is made there first, in this order:

```
spec → wire types → mock → provider client → provider → mod (this repo)
```

So a contract change is never a mod-only PR. If your idea needs a new endpoint
or field, start in the provider repo (its `add-resource` skill documents the
flow); this repo implements the final step.

## The hard constraint: you cannot compile this locally in most setups

This is a UE C++ plugin built against Satisfactory's **custom engine**. There
is no lightweight local build:

- A full build needs the custom engine, Visual Studio 2022, Wwise, and the SML
  starter project — see [Building locally](README.md#building-locally).
- CI builds it on a **self-hosted Windows runner** (`mod-build` workflow);
  GitHub-hosted runners cannot (the engine is tens of GB). See
  [`docs/mod-ci.md`](docs/mod-ci.md).
- The local FactoryGame source ships **stub `.cpp` bodies** — real logic lives
  only in the compiled game. Header-inline functions are safe to rely on;
  anything else must be verified empirically against a live session.

Because of this, treat all of `Source/**` and the `.uplugin` as
**write-carefully code**. Reason from SML / FactoryGame headers, cite them, and
say plainly in the PR what you verified live versus what you inferred. Several
of the trickiest behaviours (lightweight buildables, the conveyor-attachment
dismantle crash, route rebinding across save switches) are documented at length
in the README precisely because they were only discovered live — read those
war stories before touching the related code.

## Workflow

1. Open an issue first for anything non-trivial (see the issue templates), so a
   contract or architectural conflict is caught before you write engine code.
2. Branch from `main`.
3. Keep commits small and logically grouped, with an imperative summary and a
   body that explains **why**. Match the existing history.
4. Do not commit build output — [`.gitignore`](.gitignore) already covers
   `Binaries/`, `Intermediate/`, `Saved/`, etc.
5. Open a PR against `main` and fill in the template. State exactly what you
   tested and how (which game version, single-player vs dedicated server, what
   `terraform apply`/`plan` showed).

## CI and the self-hosted runner

`mod-build` compiles and packages the mod on a Windows runner attached to the
maintainer's hardware. **It never runs on pull requests**, by design: a
self-hosted runner executes whatever a workflow checks out, and fork PRs must
not run arbitrary code on someone's PC. A maintainer builds your branch after
review. Please don't propose adding `pull_request` triggers to any workflow
that touches the self-hosted runner.

## Security-sensitive changes

The API is a control plane. If your change touches `CheckRequest` /
`CheckTransport`, the listener bind, or auth, call that out prominently and read
[`SECURITY.md`](SECURITY.md). Do not report a vulnerability through a public PR
or issue — use the process in `SECURITY.md`.

## License

By contributing you agree that your contributions are licensed under the
project's [MPL-2.0](LICENSE) license.
