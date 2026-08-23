# SatisfactoTerraform (UE mod)

The in-game half of the project: an SML mod that hosts the HTTP API described
in [`../api/openapi.yaml`](../api/openapi.yaml). The Terraform provider is its
only intended client.

## Status (M2 done, M3 in progress)

M1 and M2 are functionally verified live: `terraform apply` against a
running game session, including dismantle-and-recreate of tainted resources
and fresh foundation placement, all working end-to-end.

- [x] Plugin/module scaffolding, SML root game-world module
- [x] HTTP listener (UE `HTTPServer` module), bearer-token auth, JSON helpers
- [x] Registry subsystem persisting `tf_id -> actor`/lightweight-ref in the
      save game (see "Lightweight buildables" below)
- [x] `GET /health`, `GET /world`, buildable spawn/read/list/delete
- [x] Path-parameter routing (`/api/v1/buildables/:tf_id`) — `IHttpRouter`
      does NOT prefix-match a bare trailing-slash route; GET/PATCH/DELETE-by-id
      were unreachable until this was fixed
- [x] `PATCH` recipe/clock (M2) — `AFGBuildableManufacturer::SetRecipe` /
      `SetPendingPotential` (inherited from `AFGBuildableFactory`), applied
      after `FinishSpawning` (not before — the buildable's factory
      connectors aren't initialized yet mid-deferred-construction, and the
      recipe silently doesn't stick), read back on GET
- [x] Robust class resolution via an asset-registry index (M2) —
      `ResolveClassByName` in `STFApiServerSubsystem.cpp`, shared by
      buildable and recipe lookups
- [x] Proper dismantle (M2) — routes through `IFGDismantleInterface` when a
      buildable implements it, falls back to `Destroy()` otherwise
- [ ] Belts & power lines (M3) — see TODO(M3) in `SpawnConnection`

### Lightweight buildables

Simple structural buildables (foundations, walls, ramps, pipes, ...) are
eligible for Satisfactory's Lightweight Buildable system, which destroys the
spawned actor and migrates it to a memory-efficient non-actor representation
shortly after placement (`AFGBuildable::ManagedByLightweightBuildableSubsystem()`).
Manufacturers are never eligible, which is why this only ever affected
`satisfactory_foundation`, not `satisfactory_building`.

`SpawnBuildable` detects eligible classes and converts them deterministically
right after spawning — `AFGLightweightBuildableSubsystem::AddFromBuildable()`
returns a runtime index, wrapped in an `FLightweightBuildableInstanceRef`
(a real UE `USTRUCT`, explicitly documented as safe to store indefinitely) —
instead of letting the game's own async, build-effect-triggered conversion
run on its own timing and orphaning the registry's actor pointer. The
registry tracks both representations per `tf_id`; the API layer (GET/DELETE)
checks both. See `STFRegistrySubsystem.h` and the `SpawnBuildable` comments
in `STFApiServerSubsystem.cpp` for the full reasoning — every API name here
was confirmed against the real FactoryGame source, not guessed.

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
