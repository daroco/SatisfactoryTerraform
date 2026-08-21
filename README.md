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

```
terraform apply
      │  (HTTPS-less localhost REST, api/openapi.yaml)
      ▼
SatisfactoTerraform mod (UE C++/SML) ── spawns/dismantles buildables,
      │                                  tags each with its tf_id
      ▼
save game (tf_id registry persists across save/load)
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
| `examples/iron-plate-line` | Working example config |
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
- 🚧 M1 — mod skeleton (`mod/`): HTTP server, registry, machine spawn/delete;
  needs its first compile pass against the engine
- ⏳ M2 — recipe/clock patch, robust class resolution, proper dismantle
- ⏳ M3 — belts + power lines in the mod (provider side already done)
- ⏳ M4+ — import/drift polish, dedicated-server docs, registry release
