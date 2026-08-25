# SatisfactoryTerraform

A Satisfactory mod (SML 3.x, UE C++) exposing a localhost REST API so
Terraform can manage buildings, belts and power lines as code. The only
intended client is
[terraform-provider-satisfactory](https://github.com/daroco/terraform-provider-satisfactory);
the API contract (`api/openapi.yaml`) lives there and is the single source of
truth — change it there first (spec → wire types → mock → client → provider),
then implement here.

## Layout

- `SatisfactoryTerraform.uplugin` — plugin descriptor (repo root IS the
  plugin; CI copies the checkout into the SML starter project's `Mods/` dir)
- `Source/SatisfactoryTerraform/` — the module:
  - `STFApiServerSubsystem` — HTTP listener (UE `HTTPServer`), route
    handlers, spawn/patch/dismantle logic
  - `STFRegistrySubsystem` — `tf_id → actor`/lightweight-record/connection
    maps, persisted in the save game
  - `STFGameWorldModule` — SML root game-world module registering both
- `docs/mod-ci.md` — self-hosted runner setup
- `README.md` — implementation notes: lightweight buildables, placement
  offsets, connections, and the war stories behind every non-obvious line

## Constraints that shape everything

- **This code cannot be compiled in the dev environment.** It builds on a
  self-hosted Windows runner (mod-build workflow) or locally in the modding
  starter project. Treat it as write-carefully code; reason from SML /
  FactoryGame headers (the runner keeps a full checkout under `D:\ci`).
- **Stub source:** the local FactoryGame source ships empty-bodied `.cpp`
  implementations; real logic lives only in the compiled game. Header-inline
  functions are safe to rely on; anything else must be verified empirically
  against a live game session.
- Resource identity is `tf_id` (provider-generated UUID), persisted by the
  registry in the save. Never derive identity from position/class — except
  lightweight recovery, which by design re-finds instances by saved
  class+location (see `FSTFLightweightRecord`).
- Error contract: 404 unknown tf_id, 409 duplicate, 422 validation.
- Routes bind once per process and never unbind (`BindRoutesOnce` /
  `ActiveInstance`) — re-registering `:param` templates on `IHttpRouter`
  breaks silently. Don't "clean this up."

## CI

- `mod-build` (self-hosted Windows runner): compiles and
  packages the mod (Windows client + Windows dedicated server; Linux server
  needs a cross-compile toolchain, descoped). Engine cached at `C:\CI\UE`,
  Wwise at `C:\CI\Wwise`. Secrets: `ENGINE_GH_TOKEN`, `WWISE_EMAIL`,
  `WWISE_PASSWORD`.
  **Never add `pull_request` triggers** — a self-hosted runner executes
  whatever the workflow checks out. Babysitting guidance: `.claude/skills/steward/SKILL.md` and
  `docs/mod-ci.md`.
