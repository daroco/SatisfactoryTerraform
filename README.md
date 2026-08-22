# satisfacto-form

Factory-as-code for [Satisfactory](https://www.satisfactorygame.com/): a
Terraform provider plus a companion
[SML](https://ficsit.app/) mod, so `terraform apply` places foundations,
machines, belts and power lines in a **running game**.

```hcl
resource "satisfactory_building" "smelter" {
  class  = "Build_SmelterMk1_C"
  x      = 200
  y      = 0
  z      = 20100
  recipe = "Recipe_IngotIron_C"
}

resource "satisfactory_belt" "ingots" {
  class          = "Build_ConveyorBeltMk1_C"
  from_id        = satisfactory_building.smelter.id
  from_connector = 1
  to_id          = satisfactory_building.constructor.id
  to_connector   = 0
}
```

## How it works

```mermaid
flowchart TD
    A["terraform apply"] -->|"localhost REST, api/openapi.yaml"| B["SatisfactoTerraform mod (UE C++/SML)<br/>spawns/dismantles buildables, tags each with its tf_id"]
    B --> C["save game<br/>(tf_id registry persists across save/load)"]
```

- The provider (`internal/provider`, terraform-plugin-framework) assigns each
  resource a UUID (`tf_id`) and stores it in state.
- The mod keeps a `tf_id → actor` registry that is saved with the game.
  Dismantle something in-game and the next `terraform plan` shows it missing
  and offers to rebuild it. Drift detection, but for factories.
- `internal/mockserver` is an in-memory implementation of the same API so the
  provider is fully developed, tested, and CI-gated without launching the game.

## Repo layout

| Path | What |
|---|---|
| `api/openapi.yaml` | The mod⇄provider REST contract (source of truth) |
| `internal/provider` | Terraform provider (4 resources) |
| `internal/client`, `internal/api` | API client + shared wire types |
| `internal/mockserver`, `cmd/mockserver` | In-memory mock of the mod API |
| `mod/` | UE plugin source (SML mod) — compiled by CI/locally, not here |
| `examples/iron-plate-line` | Minimal working example config |
| `examples/factory-floor` | Range/grid placement example (`modules/grid-2d`) |
| `docs/mod-ci.md` | Self-hosted runner setup for the mod build |

## Resources

- `satisfactory_foundation` — passive structural buildables; any change replaces
- `satisfactory_building` — machines; `recipe` and `clock_speed` update in place
- `satisfactory_belt` — conveyor between two factory connectors
- `satisfactory_power_line` — wire between two power connectors

`class`/`recipe` attributes take the game's own class names
(`Build_ConstructorMk1_C`, `Recipe_IronPlate_C`, ...). They are validated by
the live game at apply time, not baked into the provider — new game content
works without a provider release. Class names are enumerated in the game's own
`CommunityResources/Docs/` JSON and on the wikis.

## Placing a range of buildings

Instead of hand-placing every coordinate, use the `grid-2d` module to tile a
bounding box:

```hcl
module "floor" {
  source  = "./modules/grid-2d"
  from    = { x = 0, y = 0 }
  to      = { x = 3200, y = 1600 }
  spacing = 800 # one Build_Foundation_8x4_01_C tile
}

resource "satisfactory_foundation" "floor" {
  for_each = module.floor.positions
  class    = "Build_Foundation_8x4_01_C"
  x        = each.value.x
  y        = each.value.y
  z        = local.base_z
}
```

The same module spaces out repeated buildings too — pass a wider `spacing`
and feed the output into `satisfactory_building` instead. It's pure HCL (no
provider changes): `for`/`range()` compute the grid, `for_each` with stable
`"ix_iy"` keys keeps unrelated cells from being replaced when the bounding
box changes later. See `modules/grid-2d/README.md` and
`examples/factory-floor` for the full pattern, including why it's
`for_each`-shaped rather than `count`-shaped.

Spacing is always something you choose — the module has no notion of a
building's actual in-game footprint (that would need the mod to expose a
class's real collision/clearance size over the API, which isn't built yet).

## Developing without the game

```sh
go run ./cmd/mockserver &                 # fake world on :8090
go build -o /tmp/terraform-provider-satisfactory .

cat > /tmp/dev.tfrc <<EOF
provider_installation {
  dev_overrides { "daroco/satisfactory" = "/tmp" }
  direct {}
}
EOF

cd examples/iron-plate-line
TF_CLI_CONFIG_FILE=/tmp/dev.tfrc terraform apply
```

Tests: `go test ./...`; full acceptance run: `TF_ACC=1 go test ./internal/provider/ -v`.

## CI

- **provider-ci** (hosted runners): build, vet, unit + acceptance tests against
  the mock, and an apply/plan/destroy of the example.
- **mod-build** (self-hosted Windows runner): compiles and packages the UE mod
  for client + dedicated servers. Setup and required secrets
  (`ENGINE_GH_TOKEN`, `WWISE_EMAIL`, `WWISE_PASSWORD`): see
  [docs/mod-ci.md](docs/mod-ci.md).

## Where this can go: the GitOps factory

Because state lives in Terraform and mutations go through a reviewable plan,
some genuinely unhinged things fall out almost for free once M2/M3 land
(design notes in [docs/gitops-factory.md](docs/gitops-factory.md)):

- **The repo is the world** — a dedicated server runs the mod; CI applies
  `main` on merge. The factory is the branch.
- **PRs are governance** — `terraform plan` output posted as a PR comment is
  the review artifact: "adds 4 smelters, rewires belt 12, dismantles nothing."
  CODEOWNERS on `factories/nuclear/`.
- **Twitch-plays mode is a merge policy** — chat votes, the winning PR merges,
  viewers watch the buildings materialize. Griefing is drift; `terraform apply`
  is disaster recovery.
- **Rollbacks are `git revert`** — cursed spaghetti build merged? Revert the
  commit and it un-exists.

## Status

- ✅ M0 — provider + mock + tests + example + provider CI (this works today)
- ✅ M1 — mod skeleton (`mod/`) compiles and packages via `mod-build` CI:
  HTTP server, registry, machine spawn/delete; client + Windows dedicated
  server (Linux server descoped for now — needs a cross-compile toolchain)
- ✅ M2 — recipe/clock patch, robust class resolution, proper dismantle
  (compiles and packages; not yet functionally verified in a live session)
- 🚧 M3 — belts + power lines in the mod (provider side already done)
- ⏳ M4+ — import/drift polish, dedicated-server docs, registry release
