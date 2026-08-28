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
