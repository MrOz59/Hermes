# Versioning

Hermes uses [Semantic Versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.

- **MAJOR** — incompatible/breaking changes (protocol, config, behavior).
- **MINOR** — new functionality, backwards-compatible.
- **PATCH** — backwards-compatible bug fixes only.

While Hermes is pre-1.0 (`0.x.y`), minor bumps may still include breaking
changes; treat `0.x` as "fast-moving".

## Single source of truth

The top-level [`VERSION`](VERSION) file holds the current version and nothing
else (e.g. `0.1.0`). Everything derives from it:

| Consumer            | How it reads the version                                  |
|---------------------|-----------------------------------------------------------|
| CMake / C++ build   | `CMakeLists.txt` reads `VERSION` into `project(... VERSION)`; exposed to C++ as the `PROJECT_VERSION` macro (boot log + WebUI/API). |
| WebUI               | `package.json` `version` field (kept in sync by the bump script). |
| `.deb` / `.rpm`     | The CI packaging steps use the build version computed by the `version` job. |
| Arch package        | CI rewrites `pkgver` in the `PKGBUILD` to the build version before `makepkg`. |
| Nightly release     | The `nightly` job titles the release with the build version. |

Because of this, you never edit the version in more than one place.

## Day-to-day: the changelog

As you make changes, add a bullet under **[Unreleased]** in
[`CHANGELOG.md`](CHANGELOG.md) (under `Added` / `Changed` / `Fixed` /
`Removed`). This is the only per-commit habit needed — it keeps the release
notes ready at all times.

## Cutting a release

Run the bump script with the part to increment:

```bash
scripts/bump-version.sh patch    # 0.1.0 -> 0.1.1
scripts/bump-version.sh minor    # 0.1.1 -> 0.2.0
scripts/bump-version.sh major    # 0.2.0 -> 1.0.0
scripts/bump-version.sh 1.4.2    # or set it explicitly
```

The script:

1. updates `VERSION` and `package.json`,
2. moves the `[Unreleased]` changelog entries into a new dated
   `## [X.Y.Z] - YYYY-MM-DD` section,
3. commits as `Release vX.Y.Z`, and
4. creates the annotated tag `vX.Y.Z`.

Then push:

```bash
git push origin HEAD
git push origin vX.Y.Z
```

Pushing the `vX.Y.Z` tag triggers the **release** job in
[`.github/workflows/build.yml`](.github/workflows/build.yml), which builds the
Linux artifacts and publishes a normal (non-prerelease) GitHub Release.

Flags: `--no-tag` (commit but don't tag), `--no-commit` (edit files only).

## Nightly builds

Every push to `main` that builds successfully refreshes a single rolling
`nightly` prerelease on GitHub Releases, with the latest Linux artifacts. The
`nightly` tag is force-moved to the newest commit each time, so it always
reflects the tip of `main`. Nightlies are marked **pre-release** and are not
meant to be stable.

### The commit is part of the version

A stable release (a `v*` tag) ships the bare `VERSION` number. Every other
build — nightly, PR, manual dispatch — appends its short commit:

```
0.5.1          # the v0.5.1 tag
0.5.1+abc1234  # a nightly built from commit abc1234
```

That version is what the boot log prints, what `/api/config` returns, and what
the Web UI home page shows, so a bug report from a nightly identifies the exact
source it was built from. `+` is SemVer build metadata: it sorts *equal* to the
plain version rather than below it, which is correct — a nightly is 0.5.1 plus
commits, not a 0.5.1 pre-release. It is also the one separator that Arch
`pkgver`, `dpkg` and `rpm` all accept.

The `version` job in [`build.yml`](.github/workflows/build.yml) computes it once
and every package job reads it, passing `BRANCH` and `BUILD_VERSION` into the
build so `cmake/prep/build_version.cmake` takes the version from the environment
instead of deriving it from the checkout. A local `makepkg` sets neither and
keeps the bare `pkgver`; a local `cmake` build off a feature branch still gets
the branch-derived `X.Y.Z.<short>` (and `.dirty`) suffix it always did.
