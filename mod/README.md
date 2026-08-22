# SatisfactoTerraform (UE mod)

The in-game half of the project: an SML mod that hosts the HTTP API described
in [`../api/openapi.yaml`](../api/openapi.yaml). The Terraform provider is its
only intended client.

## Status (M2 in progress)

M1 compiles and packages via CI (client + Windows dedicated server). M2 items
below are implemented in source but **their exact FactoryGame API calls are
unverified against real headers** (this repo doesn't have them locally) — the
mod-build CI run against the real engine is the actual compile check:

- [x] Plugin/module scaffolding, SML root game-world module
- [x] HTTP listener (UE `HTTPServer` module), bearer-token auth, JSON helpers
- [x] Registry subsystem persisting `tf_id -> actor` in the save game
- [x] `GET /health`, `GET /world`, buildable spawn/read/list/delete
- [x] `PATCH` recipe/clock (M2) — `AFGBuildableFactory::SetRecipe` /
      `SetPendingPotential`, applied at spawn too; read back on GET
- [x] Robust class resolution via an asset-registry index (M2) —
      `ResolveClassByName` in `STFApiServerSubsystem.cpp`, shared by
      buildable and recipe lookups
- [x] Proper dismantle (M2) — routes through `IFGDismantleInterface` when a
      buildable implements it, falls back to `Destroy()` otherwise
- [ ] Belts & power lines (M3) — see TODO(M3) in `SpawnConnection`

Reference implementations to crib from while filling in the TODOs:
[FactorySpawner](https://github.com/uniqueSimon/FactorySpawner) (spawning,
belt/wire hookup) and
[FicsitRemoteMonitoring](https://github.com/porisius/FicsitRemoteMonitoring)
(in-game web server patterns).

## Building locally

1. Follow the [Satisfactory modding docs](https://docs.ficsit.app/) beginner
   guide: custom engine (`satisfactorymodding/UnrealEngine`), Visual Studio
   2022, Wwise, and the SML starter project.
2. Copy (or junction) this `mod/` directory to
   `<starter-project>/Mods/SatisfactoTerraform/`.
3. Open the project, let it compile, then package with Alpakit.

CI does the same thing on a self-hosted runner — see
[`../.github/workflows/mod-build.yml`](../.github/workflows/mod-build.yml) and
[`../docs/mod-ci.md`](../docs/mod-ci.md).

## Smoke test

With the mod loaded and a session running:

```sh
curl http://localhost:8090/api/v1/health
curl -X POST http://localhost:8090/api/v1/buildables \
  -H 'Content-Type: application/json' \
  -d '{"tf_id":"smoke-1","class":"Build_ConstructorMk1_C","transform":{"x":0,"y":0,"z":30000}}'
```
