---
name: steward
description: Repo-specific guidance for driving PRs and CI on satisfacto-form - which checks gate what, how to babysit the self-hosted mod build, and the hard rules that protect the owner's PC.
---

# Stewarding satisfacto-form CI and PRs

## Which checks matter

- **provider-ci** (hosted) is the merge gate. It must be green: build, vet,
  unit tests, acceptance tests, and an apply/plan/destroy of
  `examples/iron-plate-line` against the mock. A red provider-ci is always
  this repo's problem — reproduce locally (`mock-stack` skill; acceptance
  tests need a terraform binary, `TF_ACC_TERRAFORM_PATH` accepted), fix, push.
- **mod-build** (self-hosted Windows runner on the owner's gaming PC) builds
  the UE mod. It is allowed to be red without blocking provider work, but
  never leave it red silently: diagnose from job logs and either fix the
  workflow or report exactly what the owner must do (secrets, runner state).

## mod-build failure triage

Read the failing step first; the workflow fails fast with explicit messages.

- **Preflight: "ENGINE_GH_TOKEN is not set"** — owner action: classic PAT
  with `repo` scope from an account holding satisfactorymodding/UnrealEngine
  access (Epic-link + https://linker.ficsit.app/link), saved as repo secret.
  Not a workflow bug; do not "fix" the workflow.
- **Preflight: 7z not found** — 7-Zip missing on the runner; workflow already
  searches PATH + Program Files. Owner installs 7-Zip.
- **gh release download failures** — token lacks engine-repo access, or the
  release asset naming changed. Check with the owner's access before assuming
  layout drift; canonical asset pattern lives in FIN/SML CI (see below).
- **Wwise step** — missing WWISE_EMAIL/WWISE_PASSWORD secrets, or wwise-cli
  SDK-version drift. Version pins live in the workflow env.
- **Build/UAT step** — real compile errors in `mod/Source` are ours: fix the
  C++ (remember it cannot be compiled in the cloud environment — reason
  carefully from the error text and SML/FactoryGame headers). UAT
  flag/target-name errors: compare against
  SatisfactoryModLoader/.github/workflows/build.yml and
  Panakotta00/FicsIt-Networks/.github/workflows/build.yml — those are the
  canonical references this workflow was modeled on.
- Engine download/extract takes ~1h on first run and is cached at `C:\CI\UE`
  afterwards; a long-running job is not a hung job.

## Hard rules (protect the owner's PC)

- **Never add `pull_request` / `pull_request_target` triggers to
  mod-build.yml.** The self-hosted runner executes checked-out code on the
  owner's personal machine. Push-to-trusted-branches + workflow_dispatch only.
- Never echo or log secrets; never widen secret usage beyond the steps that
  need them.
- Don't "fix" a red mod-build by deleting its checks or making steps
  non-fatal; the fail-fast preflight exists so failures are diagnosable.

## Conventions recap

- Contract changes follow the `add-resource` skill ordering (spec → types →
  mock → client → provider → tests → mod).
- Commits: imperative summary, body explains why; never include model names
  in committed content.
- provider-ci and the `mock-stack` skill must describe the same loop; if you
  change one, change the other.
