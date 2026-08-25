<!--
  Thanks for contributing! Please read CONTRIBUTING.md first — especially the
  note that this mod cannot be compiled in most local setups and that API
  contract changes start in the provider repo.
-->

## What and why

<!-- What does this change and why? Link any related issue (e.g. Closes #12). -->

## Contract impact

<!-- Does this change the HTTP request/response shape at all? -->

- [ ] No API/contract change (pure in-game behaviour, docs, or repo health).
- [ ] Yes — the matching change in
      [`api/openapi.yaml`](https://github.com/daroco/terraform-provider-satisfactory)
      (spec → types → mock → client → provider) is linked here: <!-- link -->

## How this was tested

<!--
  Be specific. This code is verified live, not by a local typecheck.
  - Game + SML version:
  - Environment: single-player / listen / dedicated server
  - What you ran (curl requests, terraform apply/plan) and what you observed
    (e.g. "zero-diff plan afterwards", "clean dismantle, no crash").
  If you could NOT test something, say so and say why.
-->

## Checklist

- [ ] I read [CONTRIBUTING.md](../CONTRIBUTING.md).
- [ ] Commits have imperative summaries and explain *why* in the body.
- [ ] No build output committed (`Binaries/`, `Intermediate/`, `Saved/`, ...).
- [ ] I did **not** add `pull_request` triggers to any workflow that runs on
      the self-hosted runner.
- [ ] Security-sensitive changes (auth, bind address, CSRF gate) are called out
      above and follow [SECURITY.md](../SECURITY.md).
