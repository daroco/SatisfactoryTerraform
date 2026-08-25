# Mod CI: self-hosted runner setup

`mod-build.yml` compiles the UE mod. It cannot run on GitHub-hosted runners:
the custom engine is tens of gigabytes extracted (over the hosted runner disk
and far over the 10 GB actions/cache limit) and a cold build takes 1–2 hours.
The community answer — used by SML itself and FicsIt-Networks — is a
self-hosted runner that keeps the engine on disk between runs.

## One-time setup

### 1. Runner machine (done ✅ if the runner shows green in Settings → Actions → Runners)

A Windows 10/11 machine following
[Panakotta00's Satisfactory Modding CI Setup gist](https://gist.github.com/Panakotta00/2a30df9297ed3d6f7e0e11f30314bcae):

- Git, Git LFS, 7zip, GitHub CLI (`gh`)
- Visual Studio 2022 with the workloads from the modding docs
  (import the `.vsconfig` from docs.ficsit.app)
- Install the runner to a **short path** like `C:\CI` — UE breaks on long paths
- Run it as a service so builds work while you're logged out

### 2. Engine access → `ENGINE_GH_TOKEN` secret

The custom engine lives in the **private** repo
`satisfactorymodding/UnrealEngine`. Access is granted to GitHub accounts that
have linked an Epic Games account:

1. Link Epic ↔ GitHub per the [modding docs beginner guide](https://docs.ficsit.app/)
   (Epic account settings → Connections → GitHub, then accept the EpicGames
   org invite).
2. Confirm you can open https://github.com/satisfactorymodding/UnrealEngine.
3. Create a fine-grained PAT with read access to that repo (classic PAT with
   `repo` scope also works) and add it as repo secret **`ENGINE_GH_TOKEN`**.

### 3. Wwise → `WWISE_EMAIL` / `WWISE_PASSWORD` secrets

The game project needs the Wwise UE integration to compile. Create a free
[Audiokinetic account](https://www.audiokinetic.com/) and store its
credentials as the two secrets. CI uses
[`mircearoata/wwise-cli`](https://github.com/mircearoata/wwise-cli) (the same
tool SML's own CI uses) to download and integrate the SDK, cached under
`C:\CI\Wwise`.

## What a run does

1. Checks out `satisfactorymodding/SatisfactoryModLoader` (the modding starter
   project) and copies this repo (the plugin root) into
   `Mods/SatisfactoryTerraform/`.
2. Downloads + extracts the engine to `C:\CI\UE` (first run only; ~1 h).
3. Integrates Wwise, generates project files, builds editor binaries.
4. Runs UAT `PackagePlugin` for Win64 client + Windows dedicated server.
   (Linux dedicated server is a follow-up: it needs a separate Linux
   cross-compile toolchain/sysroot on the runner, not installed by default.)
5. Uploads the packaged mod zips as workflow artifacts — download from the
   run page and drop into your game/server's `Mods` folder (or install via
   Satisfactory Mod Manager pointing at the zip).

Expect the first run to need a couple of iterations on flags/paths (UAT step
names and the engine release layout shift between versions). The canonical
references when something breaks:
[SML's build.yml](https://github.com/satisfactorymodding/SatisfactoryModLoader/blob/master/.github/workflows/build.yml)
and
[FicsIt-Networks' build.yml](https://github.com/Panakotta00/FicsIt-Networks/blob/development/.github/workflows/build.yml).

## Releasing

`release.yml` publishes a tagged release. It runs on a **GitHub-hosted** runner
(never the self-hosted machine) and does **not** build — it reuses the `.zip`
that `mod-build` already packaged, so cutting a release costs no runner time.

Release steps:

1. Land the changes on `main` and let `mod-build` package them (it runs on
   pushes touching `Source/**`, or trigger it manually via **workflow_dispatch**
   on the release commit). Confirm that run is green.
2. Bump `SemVersion`/`VersionName` in `SatisfactoryTerraform.uplugin` and add a
   `CHANGELOG.md` entry for the new version.
3. Tag and push: `git tag v0.1.0 && git push origin v0.1.0`.

`release.yml` then downloads the newest successful `mod-build` artifact, creates
a GitHub Release with the `.zip` attached, and — if configured — uploads to
ficsit.app.

### ficsit.app (SMR) publishing — one-time owner setup

The SMR upload uses the official [`ficsit-cli`](https://github.com/satisfactorymodding/ficsit-cli)
(`ficsit smr upload <mod-id> <file> <changelog>`). To enable it:

1. **Create the mod listing once** on <https://ficsit.app> (Upload a mod). SMR
   reads `SatisfactoryTerraform.uplugin` for the mod reference, name,
   description, SemVersion, and `GameVersion`; it also expects a mod icon
   (`Resources/Icon128.png`) — see the icon note below. Creating the listing
   yields the **mod ID**.
2. Add a repo **variable** `SMR_MOD_ID` = that mod ID (Settings → Secrets and
   variables → Actions → **Variables**). While it is unset, `release.yml` still
   creates the GitHub Release and just skips the SMR upload.
3. Create a ficsit.app **API key** (ficsit.app → profile → API keys) and add it
   as a repo **secret** `SMR_API_KEY`. Never commit or echo it.

Uploaded versions are virus-scanned and approved asynchronously on SMR; the
GitHub Release is created immediately regardless.

### Mod icon (owner to-do)

SMR listings show a 128×128 icon at `Resources/Icon128.png`. This repo does not
ship one yet — add a real PNG there before (or when) creating the listing.
`.gitattributes` already routes `*.png` through Git LFS, so ensure Git LFS is
installed (`git lfs install`) before committing it.

## Security

The build workflow triggers only on pushes to `main`/`claude/**` and manual
dispatch. **Never add `pull_request` triggers**: a self-hosted runner executes
whatever the workflow checks out, and fork PRs would run arbitrary code on it.
`release.yml` is safe to tag-trigger because it runs on a hosted runner and only
consumes an already-built artifact.
