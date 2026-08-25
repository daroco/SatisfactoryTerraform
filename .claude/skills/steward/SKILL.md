---
name: steward
description: Repo-specific guidance for driving PRs and CI on SatisfactoryTerraform - which checks gate what, how to babysit the self-hosted mod build, and the hard rules that keep the runner safe.
---

# Stewarding SatisfactoryTerraform CI and PRs

## Which checks matter

- **mod-build** (self-hosted Windows runner) builds
  and packages the UE mod - it is this repo's only CI and its merge gate.
  Never leave it red silently: diagnose from job logs and either fix the
  workflow or report exactly what the owner must do (secrets, runner state).
- The Terraform provider and its hosted `provider-ci` live in
  [terraform-provider-satisfactory](https://github.com/daroco/terraform-provider-satisfactory);
  contract changes land there first (spec/types/mock/client), then here.

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
- **Build/UAT step** — real compile errors in `Source` are ours: fix the
  C++ (remember it cannot be compiled in the cloud environment — reason
  carefully from the error text and SML/FactoryGame headers). UAT
  flag/target-name errors: compare against
  SatisfactoryModLoader/.github/workflows/build.yml and
  Panakotta00/FicsIt-Networks/.github/workflows/build.yml — those are the
  canonical references this workflow was modeled on.
- Engine download/extract takes ~1h on first run and is cached at `C:\CI\UE`
  afterwards; a long-running job is not a hung job.

## Hard rules (runner safety)

- **Never add `pull_request` / `pull_request_target` triggers to
  mod-build.yml.** A self-hosted runner executes whatever the workflow
  checks out. Push-to-trusted-branches + workflow_dispatch only.
- Never echo or log secrets; never widen secret usage beyond the steps that
  need them.
- Don't "fix" a red mod-build by deleting its checks or making steps
  non-fatal; the fail-fast preflight exists so failures are diagnosable.

## Conventions recap

- Contract changes follow the provider repo's `add-resource` skill ordering
  (spec → types → mock → client → provider → tests → mod); this repo
  implements the final step.
- Commits: imperative summary, body explains why; never include model names
  in committed content.
