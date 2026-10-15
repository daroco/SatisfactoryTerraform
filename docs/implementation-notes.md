# Implementation notes

The war stories behind the non-obvious code in this mod. Most of these
behaviours were only discovered by running against a live game session — the
local FactoryGame source ships stub `.cpp` bodies, so anything not
header-inline had to be verified empirically. If you are about to touch
lightweight buildables, connections, placement, routing, or dismantling, read
the relevant section first; each rule here fixed a real, sometimes
world-corrupting bug.

See also [`architecture.md`](architecture.md) for the request lifecycle and the
contract, and the [Security section of the README](../README.md#security) for
the transport/auth model.

## Lightweight buildables

Simple structural buildables (foundations, walls, ramps, pipes, ...) are
eligible for Satisfactory's Lightweight Buildable system, which destroys the
spawned actor and migrates it to a memory-efficient non-actor representation
shortly after placement (`AFGBuildable::ManagedByLightweightBuildableSubsystem()`).
Manufacturers are never eligible, which is why this only ever affected
`satisfactory_foundation`, not `satisfactory_building`.

The conversion is NOT async: `AFGBuildable::BeginPlay` early-outs into
`HandleLightweightAddition()` when `ShouldConvertToLightweight()`, adding the
lightweight instance and destroying the actor synchronously — i.e. it has
already happened by the time `FinishSpawning` returns inside
`SpawnBuildable`. The mod must therefore never convert eligible buildables
itself: an earlier version called `AddFromBuildable()` after spawning
("deterministic conversion"), which silently **doubled** every foundation —
two pixel-perfectly overlapping instances, ours tracked, the game's
orphaned. That was the root cause of the entire "phantom tile" family of
bugs (API deletes removed our copy while the orphan stayed standing;
manual dismantling showed two instances per tile). `SpawnBuildable` now
detects that the game converted (the actor is pending destruction after
`FinishSpawning`) and registers the game's own instance, re-found by
class + location (`RegisterLightweightByIdentity`). The registry tracks
both representations per `tf_id`; the API layer (GET/DELETE) checks both.
See `STFRegistrySubsystem.h` and the `SpawnBuildable` comments in
`STFApiServerSubsystem.cpp` — grounded in the real
`FGBuildable.cpp` source, not guessed.

**Session-boundary self-heal.** The registry must not persist
`FLightweightBuildableInstanceRef` itself: none of that struct's members
(`BuildableClass`/`Transform`/`LightweightBuildableID`) carry the
`SaveGame` flag, so a SaveGame-marked map of refs round-trips as *empty
structs* — the root cause behind two prior failed versions of this fix
(id-based recovery via reflection, then class+location recovery reading
the ref's own fields: both were healing refs that had loaded back blank,
confirmed live when 64 foundations vanished from the API after every
relaunch while still standing in the world - issue #2). Two other real
pitfalls discovered along the way, still relevant: the ref's
`OwnerSubsystem` weak pointer dies every session (the subsystem actor is
recreated), and `LightweightBuildableID` is just an index into a per-class
array the game rebuilds on load, so a saved id can point at nothing — or
at a *different* tile. The registry therefore stores its own
`FSTFLightweightRecord`: SaveGame-flagged class + transform (the identity
that actually survives), plus a Transient engine ref re-resolved through
`GetAllLightweightBuildableInstances()` (a public, header-inline accessor,
so no stub-source caveat) by matching class + location within 1cm.

Two rules that fixed real, world-corrupting bugs and must not be
"optimized" away:

**Never trust a cached ref.** `FLightweightBuildableInstanceRef::IsValid()`
only checks that the instance's array slot resolves to a non-null pointer,
and the subsystem never shrinks those arrays: removal calls
`FRuntimeBuildableInstanceData::Clear()` and recycles the slot later. So a
ref to a *dismantled* instance keeps reporting `IsValid()` forever - which
made the API insist a hand-dismantled foundation still existed and silently
broke drift detection, the whole point of the project. `Clear()` nulls
`BuiltWithRecipe`, so `IsValidOnLoad()` is the honest liveness check and
every resolve goes through it. The stored index is kept only as a hint that
must re-verify before it is used.

**Prune records that fail to resolve.** A record whose buildable is gone
must be dropped, not kept around: records resolve by class + location, so
a dead record will happily re-bind to the *next* buildable placed at those
coordinates — and Terraform, having already dropped that `tf_id` from
state on the 404, recreates exactly there. Two records then claim one
instance, and deleting either destroys the other's buildable. Observed
live: a stale record resurrected onto a freshly applied tile within
minutes. `FindLightweight` and `GetAll` therefore remove records that fail
revalidation.

**Fail closed when ambiguous.** The scan binds only when *exactly one* live
instance matches. Zero means genuinely dismantled; more than one means two
buildables share a position and we cannot tell them apart. Both return
false → 404 → Terraform plans a recreate, which is safe. Picking the first
match instead is not: two records then resolve to the same instance, and
deleting one destroys the other's buildable. That cost 27 real foundations
once (issue #3).

## Connections (belts & power lines, M3)

`SpawnConnection` handles both connection classes in `POST
/api/v1/connections`:

- **Belts** (any `Build_ConveyorBelt*_C`) spawn via
  `UFGBuildableSpawnStrategy_Spline::RouteSpline`, driven manually through
  the `PreSpawnBuildable`/`BeginSpawnBuildable`/`FinishSpawning`/
  `PostSpawnBuildable` lifecycle (the same pattern the game's own
  blueprint-driven placement uses), then wired **into** the chain with
  `UFGFactoryConnectionComponent::SetConnection`: source output ->
  `GetConnection0()` (belt input), `GetConnection1()` (belt output) ->
  destination input. Wiring the two machine connectors straight to each
  other instead — which an earlier version did, leaving the belt's own
  connectors dangling — still moves items, but produces a world state
  vanilla can't: a machine connector attached to another machine
  connector. `AFGBuildableConveyorAttachment::Dismantle_Implementation`
  assumes anything on its connectors is a conveyor and feeds the failed
  cast into `Execute_CanDismantle`, whose `check(O != NULL)` then takes
  the whole game down (issue #2) — including from the player's own build
  gun, which no mod-side dismantle guard can protect.
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

## Pipelines and hypertubes (water flow verified live 2026-10-14)

Placed by `SpawnConnection` alongside belts and wires. A pipe is spawned
with the game's own helper,
`UFGTestBlueprintFunctionLibrary::SpawnSplineBuildable(class, fromConnection,
toConnection)`, which paths the spline between the two connectors and
positions the pipe's two connection components the way the pipe hologram
does. Whatever the helper leaves unjoined is then joined with
`MakeConnectionBetweenComponents`, machine connector against the pipe's own
end (never the two machine connectors directly - see the conveyor-attachment
dismantle crash above), and the result is read back through `GetConnection`
from both ends.

Why not the belt path (`BeginSpawnBuildable` + `UFGBuildableSpawnStrategy_Spline`
+ `SetConnection`)? It was tried first, and live every typed machine
connector - producer (water extractor) or consumer (coal generator) - refused
the pipe's connection0 through `CanConnectTo` and `CheckCompatibility`, in
both directions, while a junction's untyped connector accepted it and the
pipe's connection1 was accepted by everything. The hand-rolled spawn leaves
connection0 facing along the pipe instead of back at the machine; belts do
not care, pipes do. The helper exists for exactly this job, so the mod uses
it instead of guessing at connector transforms.

Two things differ from belts regardless.

**Connector indices are per-kind.** Fluid connectors are
`UFGPipeConnectionComponent`, hypertube connectors
`UFGPipeConnectionComponentHyper`; both derive from
`UFGPipeConnectionComponentBase` but are indexed as separate lists, chosen by
the class being placed. Connector 0 of a machine's fluid connectors has nothing
to do with connector 0 of its power connectors. `GetPipeConnector` (spawn) and
`EndpointJson` (export) index the same way, deliberately - if they ever
diverge, exported pipes re-apply against the wrong ports.

**The game is asked whether the pairing is legal.** `CanConnectTo` and
`CheckCompatibility` know the rules this code would otherwise be guessing at:
producer/consumer direction, occupied connectors, fluid vs hypertube. A
refusal is a 422 that carries both connectors' type, occupancy, location and
normal, because a pipe that exists and carries nothing looks exactly like
success.

### What has not been checked

- **Fluid moving: verified.** Water extractor -> pipe -> cross junction ->
  pipe -> coal generator, every piece placed through the API with the
  spline helper, and the generator ran on hand-fed coal (2026-10-14). The
  earlier hand-rolled spawn, whose pipe end faced along the pipe, was the
  reason typed sockets refused the join; the helper fixed it.
- **Hypertubes** still go through the belt-style path and have never been
  placed live.

## Exporting the world (`GET /world/buildables`, `GET /players`)

These are the read-only endpoints an exporter uses to turn a hand-built
factory back into configuration. Three things about them are not obvious.

**Actor enumeration alone misses every floor.**
`AFGBuildableSubsystem::GetAllBuildablesRef()` is the obvious accessor and it
returns actors only. Foundations, walls and ramps are lightweight instances,
which are *not* actors — the same split behind most of the bugs in this file.
The handler therefore unions two sources: the buildable subsystem's actor list
and `AFGLightweightBuildableSubsystem::GetAllLightweightBuildableInstances()`.
An export written against the obvious accessor would have looked correct and
silently omitted the entire floor of every factory.

**The spatial filter is required, not optional.** A mature save holds tens of
thousands of buildables. `x`, `y`, `z` and `radius` are all mandatory (422
otherwise) and the radius is capped at 1 km. Making it optional would only
move the discovery of that into a live game.

**Connections force two passes.** A belt is defined by the two connectors it
joins, not by where it sits. Each end is reported as an index *into the same
response*, because an untracked buildable has no `tf_id` and no other stable
handle. The far end of a belt is usually enumerated after the belt itself, so
nothing can be serialised until everything in range has been collected — hence
the `FWorldItem` gather pass followed by a separate serialise pass.

Connector indices are produced through the same ordering `SpawnConnection`
consumes them by (`UFGFactoryConnectionComponent::SortComponentList` for
factory connectors, plain component order for power). If those two ever
diverge, exports will re-apply against the wrong ports.

Endpoint resolution fails closed. An end outside the radius, an owner that is
not an `AFGBuildable`, or a connector that cannot be found in the owner's list
all drop the whole `connects` object. The dangerous outcome is not a missing
belt but a guessed one: a wrong connector index yields configuration that
applies cleanly and wires the wrong port, which is only discovered by watching
a factory quietly not run.

**Players use the plain engine API.** `GetPlayerControllerIterator` and the
pawn's transform, not FactoryGame's `GetLocalPlayerController` /
`GetPawnLocation`. Those ship as stubs in the local headers — empty bodies —
so their return values cannot be trusted. Same rule that picked `GetProducedIn`
over `IsProducedIn` for recipe validation: prefer the function with a real
body, and prefer engine code over game code when both would work.

## Placement offset

A `satisfactory_building`/`satisfactory_foundation`'s `z` is where its own
actor origin lands, not a "sits on top of" height — there's no snapping to
whatever's underneath, since the API places at an exact transform. Building
a floor + machines with Terraform therefore means picking the right
constant offset between a foundation's `z` and the `z` a machine on top of
it needs. Calibrated live (spawn a row of constructors at several
z-deltas on one foundation, eyeball which one sits flush - see
the provider repo's `examples/factory-hub` "ruler" approach): **+200**, not the +100
originally guessed in `examples/iron-plate-line`. All three `examples/*`
foundations-and-machines layouts use `base_z + 200`. This is a fixed
constant for these specific classes (`Build_Foundation_8x4_01_C` on top of
`Build_SmelterMk1_C`/`Build_ConstructorMk1_C`/the conveyor attachments) -
different foundation/machine classes would need their own calibration, and
the API has no way to ask a class for its own footprint/pivot (see the
`grid-2d` module's README for the same caveat on spacing).

## Route rebinding across a save switch (fixed)

Observed live: the *first* save loaded after launching the game routed
fine, but switching to a *second* save without quitting the process left
the two `:tf_id` templated routes (`/api/v1/buildables/:tf_id`,
`/api/v1/connections/:tf_id`) returning `route_handler_not_found` for
every request, while the non-templated routes kept working. The original
code bound routes in `BeginPlay` and unbound them in `EndPlay` -
correct-looking, but re-registering a path *template* on the same
`IHttpRouter` after an unbind is where the engine quirk lives (issue #3;
never root-caused inside the engine). The fix sidesteps it: routes are now
bound exactly once per process (`BindRoutesOnce`) and never unbound;
handlers dispatch through a static `ActiveInstance` weak pointer to
whichever subsystem instance belongs to the currently loaded session, and
return 503 when none does (main menu, mid-load). See
`STFApiServerSubsystem.h`'s comment on `ActiveInstance`.

## Conveyor attachment dismantle crash (fixed)

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

## Extractors and resource nodes (live-verified 2026-10-14)

Miners, water extractors, oil extractors and fracking buildings
(`AFGBuildableResourceExtractorBase`) are not placed at a coordinate - they
are placed *on* a world feature and refuse to produce otherwise. Two things
make that work through the API:

- `GET /api/v1/world/resource-nodes` enumerates the features with
  `TActorIterator<AFGResourceNodeBase>` (deposits skipped - those are the
  pickable rocks) and `TActorIterator<AFGWaterVolume>` (only volumes whose
  `CanPlaceResourceExtractor()` is true). Plain iteration, because
  `AFGResourceNodeManager` keeps its node arrays private and its accessor is
  stub-only. Each feature's `id` is its actor path name (`GetPathName()`),
  which is the one stable handle the engine gives a level actor. Position
  and yaw are what the feature itself answers to
  `GetPlacementLocation/Rotation` - the same question the hologram asks -
  falling back to the actor transform when it answers zero. Water volumes
  are areas, so they are reported at the centre of their component bounding
  box at surface height (`Max.Z`), with `bounds_min/max`.
- `POST /api/v1/buildables` with `node_id`: `ResolvePlacement` resolves the
  path (`FindObject`, falling back to iterating the two actor kinds),
  refuses an occupied feature with 409, asks the class default object
  `IsAllowedOnResource` (the public predicate the hologram also uses: form,
  allowed-resource list, node-type restriction - real compiled code, stub
  locally), and derives the transform from the feature. The spawn mirrors
  the hologram's order: `BeginSpawnBuildable` →
  `SetExtractableResource(node)` → `FinishSpawning`, because
  `AFGResourceExtractorHologram::ConfigureActor` binds the resource on the
  deferred actor before BeginPlay runs. After spawning, the node is marked
  occupied if the game did not already do it; on DELETE it is marked free
  before dismantling. Both are belt-and-braces: the real `BeginPlay` /
  `Destroyed` bodies almost certainly do this, but they are not inspectable
  here.

Assumed, to be checked in a live session, in this order:

1. The whole-map node count and the Grasslands nodes nearest a known point
   come back with the right resource/purity. If distant nodes are missing
   the listing is "loaded features only" and the docs should say so.
2. A miner spawned on a free iron node reads `occupied=true` on the node,
   sits on it visually, and puts ore on a belt. A second POST on the same
   node is a 409 with nothing left behind. DELETE frees the node.
3. A water extractor via `node_id` + `placement.x/y` sits at the surface,
   and a pipeline from it carries water into a coal generator - this is also
   the first live proof that the pipeline spawn path works at all.
4. `GetPlacementLocation/Rotation` really return the node transform (if
   they return zero for every node, the fallback silently makes this the
   actor transform, which may be a different pivot).

`clock_speed` is now reported and patchable on any `AFGBuildableFactory`
whose `GetCanChangePotential()` is true (manufacturers, extractors,
generators) instead of manufacturers only; `recipe` stays
manufacturer-only. A generator placed as a `satisfactory_building`
therefore now echoes `clock_speed: 1` where it previously omitted the field.

## Preview holograms (live-verified 2026-10-14; verdict fix pending a build)

`POST /api/v1/preview` shows a would-be buildable as the game's own hologram
and reports the hologram's placement verdict, so `terraform plan` can put the
whole pending change in the world and turn clearance/overlap/node-snapping
failures into plan warnings. Nothing is constructed and nothing is saved -
holograms are transient actors, destroyed on expiry (`ttl_seconds`, default
120, max 600), on `DELETE /preview[/{tf_id}]`, when a real spawn with the
same `tf_id` replaces them, and when the session ends.

How one is driven, mirroring one build-gun aim:

- The build recipe comes from `AFGRecipeManager::FindBuildingDescriptorByClass`
  + `FindRecipesByProduct(descriptor, onlyAvailable=false)`, with
  `Recipe_<Name>_C` as the naming-convention fallback.
- `AFGHologram::SpawnHologramFromRecipe(recipe, owner, location, instigator)`
  with the local player's build gun as owner and their character as
  instigator when there is a local player, the subsystem actor otherwise
  (dedicated server). Some hologram classes assume their owner is a build
  gun; which ones tolerate another owner is unknown until tried, so a null
  hologram yields `valid: null` + `preview_unavailable` rather than an error.
- A synthetic `FHitResult` at the requested point (hit actor = the node, for
  extractors, so the extractor hologram's snap logic finds it), then
  `SetScrollRotateValue(yaw)` → `UpdateHologramPlacement(hit)`, the build
  gun's one entry point per frame. Calling its parts (pre-placement, snap or
  place, post-placement) separately left the hologram reporting
  `FGCDInitializing` forever - seen live. The hit is kept with the preview
  and the hologram is driven again on every read (`GET /preview/:id`), since
  the game's answer to the first update is "initializing" and only a later
  update settles it. Coordinate-placed classes are then forced to the exact
  requested transform (the API does not snap, so the ghost should not
  either). `CheckValidPlacement` is protected, reached through a
  derived-class access shim rather than `ValidatePlacementAndCost`, which
  would also run the build-cost check against an inventory and report "cannot
  afford" as "cannot place". `CanConstruct()` and
  `GetConstructDisqualifiers()` give the verdict; `SetPlacementMaterialState`
  colours the ghost.

Unknowns, in the order they will be hit live:

1. Whether holograms spawned outside a build-gun state tick happily (they
   are normally owned and ticked by `AFGBuildGunStateBuild`). If one crashes
   in its Tick, the fix is probably `SetActorTickEnabled(false)` after the
   check.
2. Whether `SetScrollRotateValue` is in degrees (it is applied through the
   hologram's own placement; the forced transform afterwards makes this
   moot for coordinate-placed classes).
3. Whether `CheckValidPlacement` on a hologram with no build-gun owner
   produces the same disqualifiers the build gun shows.
4. Whether a client in a multiplayer session sees the holograms
   (replication is the hologram's own business; irrelevant for the
   single-player target).

### Visible to everyone

Holograms are spawned on the host with authority and nothing here opts out
of replication, so in a multiplayer session every connected player should
see the planned factory appear as ghosts, the same way vanilla shows another
pioneer's build hologram. That is intended: a plan the whole server can
walk through before it is applied. `AFGHologram::IsNetRelevantFor` is
overridden in the game and stubbed locally, so whether it lets a hologram
owned by the host's build gun (or by the subsystem) reach other clients is
a live check in a two-player session, not a certainty. If it does not,
the fallback is to spawn the preview with no owner and
`bOnlyRelevantToOwner` off.

### Live results (2026-10-14)

A `terraform plan` of `examples/tier4-factory` (666 resources, 286 of them
placeable) put every ghost in the world in about three seconds and the
provider read each verdict back. Two things came out of it:

1. **`FGCDInitializing` never cleared**, on every hologram, on every read.
   Root cause: a hologram starts life with that disqualifier in
   `mConstructDisqualifiers`, and `CheckValidPlacement` only ever appends.
   The build gun's `ValidatePlacementAndCost` calls
   `ResetConstructDisqualifiers()` before each check; `DriveHologram` now
   does the same. (Fix in source; the build that carries it is the first
   thing to re-check live.)
2. **`FGCDInvalidFloor` on every machine** is expected: the foundations
   under them are ghosts too. The provider treats that disqualifier as
   informational when it is the only one.

Real findings the ghosts surfaced, which is the point of the feature: a
leftover test miner occupying the node the plan wanted
(`FGCDIdenticalOverlappingBuildable` + `FGCDNeedsResourceNode`), a water
extractor overlapping a leftover probe (`FGCDEncroachingClearance`), and the
first pole of each miner's pole chain sitting inside the miner
(`FGCDInvalidPlacement`).

## Session control and auto-load

Tooling that drives the game through this API needs to get *out* of a
session and back *in* without a person clicking through menus: save, quit,
relaunch, load. Three pieces:

- **`POST /api/v1/session/save`** `{name?}` writes a save through
  `UFGSaveSystem::SaveGame`. The save system is asynchronous and reports
  back through `FOnSaveMgrInterfaceSaveGameComplete`, so the handler parks
  the request's `FHttpResultCallback` in `PendingSave` and answers from
  `OnSaveComplete` (UE's HTTP server is fine with a handler that returns
  `true` and completes later). One save in flight at a time (409 otherwise);
  a 50 s timer answers 504 if the delegate never fires, under the provider's
  60 s client timeout. The default name is `<session name>_terraform`
  (`AFGGameState::GetSessionName`), validated with
  `UFGSaveSystem::IsValidSaveName`.
- **`POST /api/v1/session/quit`** `{save?: true, name?}` does the same
  save, answers 202, and one second later calls
  `FPlatformMisc::RequestExit(false)`. The delay is so the HTTP server's
  tick writes the response before the engine tears down.
- **Auto-load** lives in `USTFMenuWorldModule`, a root SML menu-world module,
  because the API subsystem does not exist in the main menu. At
  `POST_INITIALIZATION` it reads `-STFLoad=<name>` from the command line
  or `SATISFACTORY_TERRAFORM_LOAD` from the environment, waits four seconds
  for the menu to settle, then takes the menu's own load path: read the
  file, `UFGSaveSystem::ParseSaveGameHeader`, `LoadSaveFile(header,
  params, firstPlayerController)`. If any of that is unavailable it falls
  back to `UGameplayStatics::OpenLevel(map, "?loadgame=<name>")`, the
  option `AFGGameMode` parses itself. A static flag makes this run once per
  process so quitting a session back to the menu does not reload it.

Launching with the argument from Steam:
`steam://run/526870//-STFLoad=<name>/`; or run `FactoryGameSteam.exe`
directly with the environment variable set.

What has not been checked: everything above is written against the headers
and is live-tested as the first thing after the build that carries it.

## Inventory grants (`POST /api/v1/players/:name/inventory`)

`{item: "Desc_Coal_C", count: 100}` resolves the item descriptor class the
same way buildable classes are resolved (loaded class, else the asset
registry index) and adds it to the named player's `AFGCharacterPlayer`
inventory with `UFGInventoryComponent::AddStack`, one stack of
`UFGItemDescriptor::GetStackSize` at a time, partial adds allowed. The
answer reports `added`, which is less than `requested` when the inventory
fills; nothing added at all is a 409. No unlock or cost check, like every
other write on this API.
