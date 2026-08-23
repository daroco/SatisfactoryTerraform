# satisfacto-form

Factory-as-code for Satisfactory: a Go Terraform provider plus a companion UE
C++ mod (SML) exposing a localhost REST API. `terraform apply` places
foundations, machines, belts and power lines in a running game session.

## Architecture in one paragraph

The provider (`internal/provider`) assigns each resource a UUID (`tf_id`) and
talks to the mod's HTTP API (contract: `api/openapi.yaml` — the single source
of truth; change it first, then client, mock, and mod together). The mod keeps
a `tf_id → actor` registry persisted in the save game, so plans stay clean
across save/load, and in-game dismantling shows up as drift (404 on Read →
state removal → recreate plan). `internal/mockserver` is an in-memory
implementation of the same contract so everything provider-side is developed
and CI-gated without launching the game. See `docs/architecture.md`.

## Commands

```sh
go build ./... && go vet ./...            # must stay clean
go test ./...                             # unit tests (mockserver, client)
TF_ACC=1 go test ./internal/provider/ -v  # acceptance tests (needs terraform in PATH
                                          #   or TF_ACC_TERRAFORM_PATH=<binary>)
go run ./cmd/mockserver                   # fake world on :8090
```

Full dev loop for applying the example against the mock: see the
`mock-stack` skill (`.claude/skills/mock-stack/`).

## Layout

- `api/openapi.yaml` — REST contract (source of truth)
- `internal/api` — wire types shared by client + mock
- `internal/client` — HTTP client; `NotFoundError`/`IsNotFound` is the drift signal
- `internal/mockserver` — in-memory mod API; mirror every contract change here
- `internal/provider` — terraform-plugin-framework provider; 4 resources
- `cmd/mockserver` — runnable mock
- `mod/` — UE plugin source (SML 3.x). **Not compiled in this repo's dev
  environment** — it builds on a self-hosted Windows runner (mod-build
  workflow) or locally in the modding starter project. Treat it as
  write-carefully code: no way to typecheck it here.
- `examples/iron-plate-line` — canonical minimal example; CI applies it against the mock
- `examples/factory-floor` — range/grid placement example (`modules/grid-2d`);
  every `examples/*` directory is applied/planned/destroyed in CI
- `modules/grid-2d` — reusable local module: bounding box + spacing → a
  `for_each`-ready map of positions. Pure HCL, no provider/mod changes.
- `docs/` — architecture, mod CI runner setup, GitOps-factory design notes

## Conventions

- Resource identity is `tf_id` (provider-generated UUID), passed on create,
  persisted by the mod in the save. Never derive identity from position/class.
- Transform units are Unreal centimetres; `yaw` in degrees.
- Class/recipe names are opaque strings validated by the game at apply time
  (e.g. `Build_ConstructorMk1_C`, `Recipe_IronPlate_C`). Do not bake game
  content tables into the provider.
- Only `recipe` and `clock_speed` update in place; everything else
  `RequiresReplace`.
- Adding a resource or endpoint: follow the `add-resource` skill
  (`.claude/skills/add-resource/`).
- Error contract: 404 unknown tf_id, 409 duplicate tf_id, 422 validation.
  Delete is idempotent client-side (404 on DELETE is success).

## CI

- `provider-ci` (hosted): build, vet, tests, acceptance tests, and
  apply/plan(-detailed-exitcode)/destroy of the example against the mock.
  This must stay green; it is the merge gate.
- `mod-build` (self-hosted Windows runner on the owner's PC): compiles and
  packages the UE mod. Engine cached at `C:\CI\UE`, Wwise at `C:\CI\Wwise`.
  Secrets: `ENGINE_GH_TOKEN`, `WWISE_EMAIL`, `WWISE_PASSWORD`.
  **Never add `pull_request` triggers to mod-build** — self-hosted runner.
  Babysitting guidance: `.claude/skills/steward/SKILL.md` and `docs/mod-ci.md`.

## Current milestones

M0 (done): provider + mock + tests + example + provider-ci green.
M1 (done): mod skeleton compiles and packages; mod-build workflow green
  (Windows client + Windows dedicated server; Linux server descoped for now,
  needs a cross-compile toolchain on the runner - see docs/mod-ci.md).
M2 (done): PATCH recipe/clock, asset-registry class resolution, proper
  dismantle, and lightweight-instance tracking for structural buildables
  (foundations/walls/etc. - see mod/README.md) - functionally verified
  end-to-end against a live game session via `terraform apply`.
M3 (in progress): belts + power lines in the mod (provider side already done).
M4+: import/drift polish, dedicated-server docs, registry release, GitOps mode.
