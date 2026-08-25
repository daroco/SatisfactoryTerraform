# Security Policy

## The threat model in one line

This mod runs an **HTTP control plane inside your game**: the API can build and
dismantle anything in the world. It is not a read-only feed. Treat the port the
way you would treat any local admin socket.

The full, layer-by-layer rationale (loopback bind, CSRF/DNS-rebinding
hardening, the bearer token, and exactly what each layer stops) lives in the
[**Security** section of the README](README.md#security). Read that before
exposing the port beyond a single-player, single-user machine.

Default posture, summarized:

- The listener binds **loopback only** (`127.0.0.1`) and is not reachable off
  the machine.
- Every request passes a CSRF/transport gate (loopback `Host` allowlist,
  `Origin` rejection, JSON `Content-Type` requirement on mutating verbs).
- A bearer token (`SATISFACTORY_TOKEN`) is optional and empty by default; set
  one before forwarding or tunnelling the port anywhere that presents as
  loopback.

Binding beyond loopback (e.g. a dedicated server on `0.0.0.0`) is **not a
supported configuration yet** — see the README note.

## Supported versions

This project is pre-1.0 and moves fast. Security fixes land on `main` and in
the next tagged release; only the latest release is supported. There is no
back-porting to older tags.

## Reporting a vulnerability

**Please do not open a public GitHub issue for a security vulnerability**,
especially one that could let a local web page or another local process drive
the API.

Instead, report it privately using GitHub's
[**Report a vulnerability**](https://github.com/daroco/SatisfactoryTerraform/security/advisories/new)
flow (Security → Advisories on this repo). If that is unavailable to you, open
a minimal issue asking for a private contact channel — without technical
details — and a maintainer will follow up.

Please include:

- affected version(s) and platform (single-player client, dedicated server);
- whether the token was set and how the port was reached (loopback, tunnel,
  bind override);
- a proof of concept or the request(s) that trigger the issue;
- the impact you observed.

You can expect an acknowledgement within a few days. Because this is a
hobby project maintained in spare time, please allow reasonable time for a fix
before any public disclosure.
