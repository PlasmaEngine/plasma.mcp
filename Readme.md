# Plasma MCP

A package for the Plasma engine, published as `plasma.mcp`.

## Layout

Not every package has every one of these; a content-only package has no `Source/`, and one that
ships no content has no `Assets/`.

| Path | What it is |
|---|---|
| `Package.plPackage` | The manifest — id, version, which engine it works with, what it ships. |
| `plasma.mcp.plPluginBundle` | What plugin discovery reads. Without it the package installs and never loads. |
| `plasma.mcp.workspace.cs` | The build targets. `Tools/Pack-Package.ps1` reads them from here. |
| `Source/` | The C++ this package compiles. |
| `Assets/` | Content, mounted as a data directory under the root name `plasma.mcp` when installed. |
| `Configs/` | Build rules resolving the engine SDK. Stamped from the SDK; edit only to add dependencies. |
| `Tools/` | The build and packaging scripts the workflows call. |

## Building locally

```
set PLASMA_SDK_ROOT=<engine root>
pwsh ./Tools/Pack-Package.ps1
```

That builds every target `plasma.mcp.workspace.cs` declares and packs the result. Output lands in
`Bin/Windows_x64_Development/`, which is the layout an installed package uses — so a local build
produces exactly the tree that gets published.

The editor can do this for you: **Package Manager → Add Local…**, point it at this directory, and it
builds and installs the result into the open project.

## Publishing a release

Releases are built by `.github/workflows/release.yml` and carry a single `.plpkg` asset — a zip of
the installed layout, named with the engine ABI tag it was built against. Anyone can then install it
through **Import Repo…** in the Package Manager without a compiler or an SDK.

Point the workflow at an engine SDK first, under **Settings → Secrets and variables → Actions**:

| Variable | Meaning |
|---|---|
| `PLASMA_SDK_REPO` | `owner/repo` whose releases carry the SDK. The tag defaults to `v<MinEngineVersion>` from the manifest. |
| `PLASMA_SDK_TAG` | An explicit SDK release tag, when the default is not what you want. |
| `PLASMA_SDK_URL` | A direct link to an SDK zip, for an SDK published outside GitHub. |
| `PLASMA_SDK_ROOT` | A path on the runner. For a self-hosted machine that already keeps a checkout. |
| `PLASMA_RUNNER` | The runner label to build on. Defaults to `windows-latest`. |

Add `PLASMA_SDK_TOKEN` as a *secret* if the SDK lives in a private repository.

Then, to ship a version:

1. Bump `%Version` in `Package.plPackage`.
2. Commit and push.
3. Tag it and push the tag:

```
git tag v1.0.1
git push origin v1.0.1
```

The tag has to match `%Version` — the workflow checks, because a release named one version while
carrying another is not something an installer can recover from.

Published versions are immutable by convention: fix a mistake with a new patch version rather than by
replacing an asset, because anyone who already installed the old one has no way to tell it changed.

### Private repositories

The editor downloads a release asset without authenticating, so a private repository's `.plpkg` is
not reachable that way — **Import Repo…** falls back to cloning the source and building it, which
needs a compiler and an SDK. Make the repository public when you want people to install without
either.

## Engine compatibility

`%MinEngineVersion`, `%MaxEngineVersion` and `%AbiTag` in the manifest say which engine builds this
package works with. The ABI tag covers platform, compiler, engine version *and* build configuration,
because a Debug and a Development build do not share a binary interface. When you move to a new
engine version, update all three and `Configs/PlasmaPackageSdk.pb.cs`'s
`BUILDSYSTEM_SDKVERSION_MINOR` — that one is compiled into the binaries, so leaving it behind makes
the manifest disagree with what it describes.
