# Changelog

All notable changes to SatisfactoryTerraform are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project aims to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Version numbers here match the `SemVersion` in
[`SatisfactoryTerraform.uplugin`](SatisfactoryTerraform.uplugin) and the
ficsit.app release.

## [Unreleased]

## [0.1.0] - 2026-08-25

First tagged release. Functionally verified live end-to-end: `terraform apply`
on the provider's `examples/iron-plate-line` creates all 8 resources
(4 foundations, 2 buildings, a belt, a power line) against a running game
session, and the next `plan` shows zero drift.

### Added
- SML root game-world module and plugin/module scaffolding.
- Loopback HTTP listener (UE `HTTPServer`) with bearer-token auth and JSON
  helpers, implementing the API contract owned by
  [terraform-provider-satisfactory](https://github.com/daroco/terraform-provider-satisfactory).
- Registry subsystem persisting `tf_id → actor` / lightweight-ref /
  connection-endpoint maps in the save game, so identity survives save/load
  and restarts.
- `GET /health`, `GET /world`, and buildable spawn / read / list / delete.
- Path-parameter routing for `/api/v1/buildables/:tf_id` and
  `/api/v1/connections/:tf_id`.
- `PATCH` of recipe and clock speed on manufacturers.
- Asset-registry class resolution shared by buildable and recipe lookups.
- Dismantle via `IFGDismantleInterface`, with a direct-`Destroy()` fallback.
- Lightweight-buildable tracking and session-boundary self-heal for
  structural buildables (foundations, walls, ramps, ...).
- Belts (`UFGBuildableSpawnStrategy_Spline`) and power lines
  (`AFGBuildableWire::Connect`) via `POST /api/v1/connections`.

### Security
- Listener pinned to `127.0.0.1` (was coming up on `0.0.0.0` due to
  FactoryGame's engine config).
- CSRF hardening on every request: loopback `Host` allowlist (defeats DNS
  rebinding), `Origin` rejection, and a JSON `Content-Type` requirement on
  mutating verbs.
- Optional bearer token read from `SATISFACTORY_TOKEN`.

[Unreleased]: https://github.com/daroco/SatisfactoryTerraform/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/daroco/SatisfactoryTerraform/releases/tag/v0.1.0
