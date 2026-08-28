---
name: live-verify
description: Verify a mod change against a running game session - install the build, probe the API, and audit the world. Use for ANY change touching spawning, the registry, lightweight buildables, or dismantling, where CI passing is not evidence the change works.
---

# Verifying against a live session

CI proves this mod **compiles**. It cannot prove it **works**: the mock server
is a different implementation, and every serious bug in this project's history
was invisible until a real game ran the real code. Budget a live pass for
anything touching spawn, the registry, or dismantle.

## Compile before you push (12 seconds, not 22 minutes)

The self-hosted runner is the same machine you are working on, so the engine
and a working project tree are already here. Compiling directly is a
pre-flight check, not a replacement for CI: **CI still builds, packages,
produces the artifact, logs the run, and gates the merge.** This only answers
"does this even compile" - which a 22-minute round trip is a terrible way to
ask.

Measured on this runner: a cold build is ~12 minutes; rebuilding after a
one-line `.cpp` edit is **~12 seconds**.

```sh
# 1. NEVER build while CI is building - you would fight over the same
#    intermediate files. This must print 0:
powershell -NoProfile -Command "(Get-Process | Where-Object { $_.ProcessName -match 'UnrealBuildTool|^cl$|Runner.Worker' } | Measure-Object).Count"

# 2. Stage your working copy into the project tree
cp -r Source SatisfactoryTerraform.uplugin \
  /d/w/SatisfactoryTerraform/SatisfactoryTerraform/SML/Mods/SatisfactoryTerraform/

# 3. Compile (FactoryEditor Win64 Development against the SML uproject)
powershell -NoProfile -Command "& 'C:\CI\UE\Engine\Build\BatchFiles\Build.bat' FactoryEditor Win64 Development -project='D:\w\SatisfactoryTerraform\SatisfactoryTerraform\SML\FactoryGame.uproject'"
```

Notes:

- Only your own pushes start CI builds, so you control the collision window -
  but check anyway, and do not push while a local build is running.
- Corrupting the tree costs a slow build, nothing more: the next CI run
  re-checks-out and repairs it.
- This compiles the Development editor. Packaging builds Shipping, a separate
  set of objects, so a clean local compile does not prove packaging works -
  one more reason CI stays the gate.

## The gotcha that matters most

**A leaked lightweight instance is invisible to the API.** The registry lists
*records*, and a leaked instance has none - so `GET /api/v1/buildables` looks
perfectly clean while a foundation is standing untracked in the world. A green
audit is not evidence of absence.

This is not hypothetical. A rejected duplicate spawn once leaked an instance,
the audit reported 64/64 tiles with zero duplicates and zero untracked records,
and the tile was plainly there in-game (issue #5). The same blind spot hid a
doubling bug for days: every foundation existed twice, and nothing in the API
could show it.

**Only physical inspection distinguishes them.** Use the isolated protocol:

```sh
# empty ground, far from anything, so anything visible is provably the leak
POST   /api/v1/buildables   tf_id=probe-A  at an isolated transform   # 201
<do the thing under test - e.g. POST a duplicate at the same transform>
DELETE /api/v1/buildables/probe-A                                     # 204
# the registry now believes that spot is empty
```

Then look at the spot in-game. Empty means clean; anything standing is a leak.
Ask the user - this step needs eyes, and there is no API substitute.

## The other blind spot: same-session reads lie

Resolution uses a session-local index hint, so a stale or ambiguous record can
still resolve correctly *within the session that created it*. Identity bugs
only surface after the hint is gone.

**Any change to record identity, resolution, or persistence must be tested
across a full save → quit → relaunch cycle**, not just a save/load, and
certainly not in the session that wrote the records. Three separate bugs
(stale refs, ambiguous binds, resurrecting records) all passed same-session
checks and failed on reload.

## Loop

1. **Install with the game closed.** The `.pak`/`.ucas`/`.utoc` files are
   locked while it runs; a half-install (new DLL, old paks) is an unverified
   combination. Confirm every file in `Mods/SatisfactoryTerraform/` shares a
   build timestamp before trusting a result.
2. **Baseline before touching anything** - counts, positions, drift. You cannot
   tell what a test changed without knowing what preceded it.
3. **Probe the API** for the status code and message.
4. **Audit the world**, then **inspect physically** if lightweight buildables
   are involved (see above).
5. **Restore the world** - delete probes, re-apply the example, confirm
   zero-drift. Leave it as you found it.

A useful audit compares three views that must agree: the registry
(`GET /api/v1/buildables`), Terraform state
(`.resources[].instances[].attributes.id` in `terraform.tfstate`), and the
geometry your config implies. Report duplicate positions and records absent
from state - both were real bugs. A helper lives at
`C:\Users\drcor\AppData\Local\Temp\claude_scratch\audit.py` on the owner's
machine; it is scratch tooling, not repo content.

## Probes worth keeping

- **Drift**: dismantle something by hand, then `terraform plan` must name that
  exact resource. This is the project's core promise and it silently broke once.
- **Duplicate position**: POST at an existing buildable's transform. Expect
  `409` and, critically, no new instance.
- **Security posture** (after any listener change): the LAN IP must refuse the
  connection, `Content-Type: text/plain` must be `415`, and
  `Host: evil.example.com` must be `403`. All three were once exploitable.

## Rules

- Never test destructive behaviour on the user's real build when an isolated
  position will do. A probe that targeted a real tile poisoned it and cost a
  manual dismantle to recover.
- Ask before anything that needs the user's hands (saving, loading, dismantling)
  and tell them exactly what to look for.
- If a result contradicts your model, believe the game. Every wrong diagnosis
  here came from reasoning about code instead of testing.
