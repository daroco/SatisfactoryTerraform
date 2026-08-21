---
name: add-resource
description: Add or change an API endpoint or Terraform resource (e.g. satisfactory_pipe, satisfactory_train, sign, splitter rules). Use whenever the mod-provider API contract or the provider's resource surface changes.
---

# Adding a resource / changing the contract

The contract has four implementations that must move in lockstep. Change them
in this order:

1. **`api/openapi.yaml`** — spec first. Reuse existing shapes: things placed
   at a transform are Buildables; things linking two buildables' connectors
   are Connections. Keep error semantics: 404 unknown tf_id, 409 duplicate,
   422 validation.
2. **`internal/api/types.go`** — wire types (snake_case JSON tags).
3. **`internal/mockserver/server.go`** — reference implementation with the
   same validation the mod will do (class-name shape checks, range checks,
   endpoint-existence checks). Add unit tests in `server_test.go` covering
   the lifecycle and each validation error.
4. **`internal/client/client.go`** — client methods. DELETE must stay
   idempotent (swallow NotFound). Return `*NotFoundError` for 404 so the
   provider's drift handling works.
5. **`internal/provider/resource_*.go`** — new resource:
   - `tf_id` is the `id` attribute: `Computed`, `UseStateForUnknown`,
     generated with `uuid.NewString()` in Create.
   - Every attribute the game cannot change in place gets `RequiresReplace`.
     In-place-updatable attributes go through a PATCH endpoint.
   - Read must call `resp.State.RemoveResource(ctx)` on `client.IsNotFound`.
   - Register the factory in `provider.go` `Resources()`.
   - If it's a belt/wire-like linker, extend `resource_connection.go`
     instead of writing a new file (see `newPowerLineResource`).
6. **Acceptance tests** in `internal/provider/provider_test.go`: lifecycle +
   no-op re-plan (`PlanOnly`) + one validation-error case. Run
   `TF_ACC=1 go test ./internal/provider/ -v`.
7. **`mod/Source/...`** — implement the real thing in C++ (or leave a
   `TODO(Mn)` marker with a concrete implementation sketch if the milestone
   isn't there yet). Remember: this cannot be compiled in the cloud dev
   environment, so keep C++ changes minimal and pattern-matched to existing
   code.
8. Update `examples/` and README's resource list if user-facing.

Definition of done: `go build ./... && go vet ./... && go test ./...` clean,
acceptance tests green, spec/mock/client/provider all agree, and the example
still applies (see the `mock-stack` skill).
