# SatisfactoTerraform (UE mod)

The in-game half of the project: an SML mod that hosts the HTTP API described
in [`../api/openapi.yaml`](../api/openapi.yaml). The Terraform provider is its
only intended client.

## Status (M0-M3 done)

Functionally verified live end-to-end: `terraform apply` on
`examples/iron-plate-line` creates all 8 resources (4 foundations, 2
buildings, a belt, a power line) against a running game session, and the
next `plan` shows zero drift.

- [x] Plugin/module scaffolding, SML root game-world module
- [x] HTTP listener (UE `HTTPServer` module), bearer-token auth, JSON helpers
- [x] Registry subsystem persisting `tf_id -> actor`/lightweight-ref/
      connection-endpoints in the save game (see "Lightweight buildables"
      and "Connections" below)
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
- [x] Belts & power lines (M3) — see "Connections" below

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

**Session-boundary self-heal.** `FLightweightBuildableInstanceRef` caches a
weak pointer (`OwnerSubsystem`) to the `AFGLightweightBuildableSubsystem`
that existed when it was created, but that subsystem actor is recreated
fresh every session — a ref deserialized from a save has a stale
`OwnerSubsystem` even though the underlying lightweight data (which the game
itself saves) is fine. `ASTFRegistrySubsystem::RevalidateLightweightRef`
re-points a stale ref at the current session's subsystem using its own
recoverable class (`GetBuildableClass()`, public) and id
(`LightweightBuildableID`, a protected `UPROPERTY` reached via reflection,
the same pattern used elsewhere in this file) and re-`Initialize()`s it.
`FindLightweight` and `GetAll` call this before trusting a stored ref.
Grounded in the real API (every member/method name confirmed against the
FactoryGame source) but hasn't had its own dedicated quit/relaunch re-test
yet — worth doing before M4.

### Connections (belts & power lines, M3)

`SpawnConnection` handles both connection classes in `POST
/api/v1/connections`:

- **Belts** (any `Build_ConveyorBelt*_C`) spawn via
  `UFGBuildableSpawnStrategy_Spline::RouteSpline`, driven manually through
  the `PreSpawnBuildable`/`BeginSpawnBuildable`/`FinishSpawning`/
  `PostSpawnBuildable` lifecycle (the same pattern the game's own
  blueprint-driven placement uses), then hooked up at both ends with
  `UFGFactoryConnectionComponent::SetConnection`.
- **Power lines** (`Build_PowerLine_C`) connect directly via
  `AFGBuildableWire::Connect` between two `UFGPowerConnectionComponent`s.

Both ends are resolved from the registry by `from_id`/`to_id` (which may be
a full actor or a lightweight ref — buildings are never lightweight, but
this keeps the connector-lookup path uniform) and connector index
(`GetFactoryConnector`/`GetPowerConnector`, sorted for factory connectors to
match the provider's documented indexing). The spawned connection gets its
own `tf_id` and is registered like any other buildable, plus a
`FSTFConnectionEndpoints` entry (see `STFRegistrySubsystem.h`) recording
which two buildables/connectors it joins, since that's not recoverable from
the connection actor alone. `HandleConnections`/`HandleConnectionByID` use
`GetAll()` + `FindConnectionEndpoints()` to distinguish connections from
plain buildables sharing the same registry and produce the
`{tf_id, class, from, to}` shape from `api/openapi.yaml`.

Verified live: `terraform apply` on `examples/iron-plate-line` creates a
belt between a smelter and constructor and a power line between the same
two buildings, alongside 4 foundations, with a clean zero-diff `plan`
afterward.

Reference implementations this was cribbed from:
[FactorySpawner](https://github.com/uniqueSimon/FactorySpawner) (spawning,
belt/wire hookup) and
[FicsitRemoteMonitoring](https://github.com/porisius/FicsitRemoteMonitoring)
(in-game web server patterns).

### Placement offset

A `satisfactory_building`/`satisfactory_foundation`'s `z` is where its own
actor origin lands, not a "sits on top of" height — there's no snapping to
whatever's underneath, since the API places at an exact transform. Building
a floor + machines with Terraform therefore means picking the right
constant offset between a foundation's `z` and the `z` a machine on top of
it needs. Calibrated live (spawn a row of constructors at several
z-deltas on one foundation, eyeball which one sits flush - see
`examples/factory-hub`'s "ruler" approach): **+200**, not the +100
originally guessed in `examples/iron-plate-line`. All three `examples/*`
foundations-and-machines layouts use `base_z + 200`. This is a fixed
constant for these specific classes (`Build_Foundation_8x4_01_C` on top of
`Build_SmelterMk1_C`/`Build_ConstructorMk1_C`/the conveyor attachments) -
different foundation/machine classes would need their own calibration, and
the API has no way to ask a class for its own footprint/pivot (see the
`grid-2d` module's README for the same caveat on spacing).

### Route rebinding across a save switch (open issue)

`BindRoutes`/`EndPlay` (see `ASTFApiServerSubsystem.h`/`.cpp`) bind the six
routes in `BeginPlay` and unbind them in `EndPlay`, which should make a
clean level transition rebind fine. Observed live, though: the *first* save
loaded after launching the game routes fine, but switching to a *second*
save without fully quitting the process leaves the two `:tf_id` templated
routes (`/api/v1/buildables/:tf_id`, `/api/v1/connections/:tf_id`)
returning `route_handler_not_found` for every request, while the
non-templated routes (`/health`, `/world`, the bare `/buildables` and
`/connections` list/create routes) keep working fine on the new save.
`EndPlay`'s `UnbindRoute` calls look correct, so this looks like an
`IHttpRouter` quirk specific to re-registering a path *template* rather
than a mod bug in the unbind logic - not root-caused yet. Breaks
`GetBuildable`/`PatchBuildable`/`DeleteBuildable` (so `terraform plan`
shows every resource as needing recreation) until the whole game process is
restarted. Workaround: fully quit and relaunch Satisfactory (not just
load a different save) after switching saves before running Terraform
against it again.

### Conveyor attachment dismantle crash (fixed)

Deleting a `satisfactory_building` that's a splitter/merger/lift
(`AFGBuildableConveyorAttachment` and subclasses) through
`DELETE /api/v1/buildables/:tf_id` crashed the whole game - confirmed live,
twice, both times mid-`terraform apply` while replacing a merger/splitter
for the placement-offset fix above. The game's own crash report:
`Assertion failed: O != 0` in `FGDismantleInterface.gen.cpp:48`, called
from `AFGBuildableConveyorAttachment::Dismantle_Implementation()`
(`FGBuildableConveyorAttachment.cpp:179` - real compiled game code, not
ours; the local SML source for both files is stub-only, so the actual
logic isn't inspectable). Something inside that function's own dismantle
handling calls `IFGDismantleInterface::Execute_CanDismantle()` on a null
object in this context - not something a mod can fix, only avoid.
`DismantleBuildable` (`STFApiServerSubsystem.cpp`) now special-cases
`AFGBuildableConveyorAttachment` to `Destroy()` directly instead of going
through `IFGDismantleInterface::Execute_Dismantle`, same as the existing
fallback for classes that don't implement the interface at all. Cost: no
build-cost refund on dismantle for this class family specifically - cheap
for a splitter/merger, and far better than a crash.

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
