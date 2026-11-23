# Contributing
Read [CONTRIBUTING.md](https://github.com/MrOz59/Hermes/blob/main/.github/CONTRIBUTING.md)
first: it covers which branch a pull request targets (`main` for fixes, `dev`
for the transport and virtual-display migration), how issues are reported, and
the changelog entry every change carries. This page covers the project's
internals — the Web UI, localization and the test suites.

## Recommended Tools

| Tool | Description |
|---|---|
| [CLion](https://www.jetbrains.com/clion/) | Recommended IDE for C and C++ development. Free for non-commercial use. |

## Project Patterns

### Web UI
* The Web UI uses [Vite](https://vite.dev) and its Rolldown bundler.
* [Bun](https://bun.com) runs the build, type checks, and tests.
* The HTML pages used by the Web UI are found in `./src_assets/common/assets/web`.
* The Style System is provided by [Bootstrap](https://getbootstrap.com).
* [Vue](https://vuejs.org) single-file components hold the page templates and TypeScript.

#### Building

From `src_assets/common/assets/web`, run `bun install --frozen-lockfile`, then
`bun run dev` for front-end development. Use the
[`web-assets` build feature](building.md#build-locally) to build and stage the
production pages with the Hermes executable.

### Documentation

The documentation site uses [Docusaurus](https://docusaurus.io/). Use the Node.js
version pinned in `docs/site/.node-version` with Doxygen and Graphviz on PATH.
Start a local preview from the repository root.

```bash
npm --prefix docs/site ci
npm --prefix docs/site run start
```

Run `npm --prefix docs/site run build` to check links and generate the
production site in `docs/site/build/`. Preview that build with
`npm --prefix docs/site run serve`.

For a hosted build, set `HERMES_DOCS_URL` to the site's origin and
`HERMES_DOCS_BASE_URL` to its path, such as `/Hermes/`. Use the same values for
the build and preview. CI uploads the built site as an artifact.

Edit guides in `docs/` and navigation in `docs/site/sidebars.ts`. Edit the
C++ source comments to update the generated API reference. Use `.cppm` for
named module interfaces and place a brief Doxygen comment before the
`export module` declaration. Doxygen reads these declarations directly and
lists them in the [C++ module index](pathname:///reference/cpp/modules.htm).

### Localization
The strings Hermes ships are inherited from Sunshine's localization, and the default language is `en` (English).

> [!IMPORTANT]
> Upstream's CrowdIn integration is **not wired up in this fork**: there is no localization workflow in
> `.github/workflows`, so nothing here pushes templates to CrowdIn or opens translation PRs. Add new English strings to
> `en.json` as shown below; translations for them arrive when a fork-side localization route exists, or through a pull
> request that edits a language file directly.

##### Translation Basics
* The brand names *Hermes*, *Hestia* and *Sunshine* should never be translated.
* Other brand names should never be translated. Examples include *AMD*, *Intel*, and *NVIDIA*.

#### Extraction

##### Web UI
Hermes uses [Vue I18n](https://vue-i18n.intlify.dev) for localizing the UI.
The following is a simple example of how to use it.

* Add the string to the `./src_assets/common/assets/web/public/assets/locale/en.json` file, in English.
  ```json
  {
   "index": {
     "welcome": "Hello, Hermes!"
   }
  }
  ```

  > [!NOTE]
  > The JSON keys should be sorted alphabetically. You can use [jsonabc](https://novicelab.org/jsonabc)
  > to sort the keys.

  > [!NOTE]
  > Add new strings only to *en.json*. The other language files come from upstream's translation process; editing
  > them here makes the next merge from upstream conflict.

* Use the string in the Vue component.
  ```html
  <template>
    <div>
      <p>{{ $t('index.welcome') }}</p>
    </div>
  </template>
  ```

  > [!TIP]
  > More formatting examples can be found in the
  > [Vue I18n guide](https://kazupon.github.io/vue-i18n/guide/formatting.html).

##### C++

There should be minimal cases where strings need to be extracted from C++ source code; however it may be necessary in
some situations. For example the system tray icon could be localized as it is user interfacing.

* Wrap the string to be extracted in a function as shown.
  ```cpp
  #include <boost/locale.hpp>
  #include <string>

  std::string msg = boost::locale::translate("Hello world!");
  ```

> [!TIP]
> More examples can be found in the documentation for
> [boost locale](https://www.boost.org/doc/libs/1_70_0/libs/locale/doc/html/messages_formatting.html).

> [!WARNING]
> The below is for information only. Contributors should never include manually updated template files, or
> manually compiled language files in Pull Requests.

Strings are extracted from the code to the `locale/hermes.po` template file. Upstream generates it from a
`localize.yml` workflow; this fork has no such workflow, so extract locally when you add a translatable C++ string.

To maintain native translations locally, install [GNU gettext](https://www.gnu.org/software/gettext), which
supplies `xgettext`, `msginit`, `msgmerge`, and `msgfmt`. This is separate from the Web UI's JSON translations.

* Extract, initialize, and update
  ```bash
  ./scripts/_locale.sh --extract --init --update
  ```

* Compile
  ```bash
  ./scripts/_locale.sh --compile
  ```

> [!IMPORTANT]
> Do not include extracted or compiled localization files in pull requests.

### Testing

There are two suites, and the `test` job in
[.github/workflows/build.yml](https://github.com/MrOz59/Hermes/blob/main/.github/workflows/build.yml)
runs both on every push and pull request. Nothing else is gated: this fork has no
clang-format lint job and no coverage upload, so run these locally before opening a PR.

#### C++ unit tests

Hermes uses [GoogleTest](https://github.com/google/googletest), included as a
submodule. The suite compiles the host sources with test hooks and coverage
instrumentation. Prepare the [local mcpp toolchain](modularization/mcpp-migration.md)
and run from the repository root.

```bash
MCPP_HOME="$PWD/.mcpp/local-home" MCPP_TOOLCHAIN="path:$PWD/.toolchain" \
  xvfb-run -a mcpp test --offline -p hermes-host --features kwin,web-assets \
  --profile coverage -- --gtest_color=yes
```

Xvfb supplies an X11 capture source on a headless machine. Pass GoogleTest
options after `--`, such as `--gtest_filter='SessionBroker*'`. Wayland cases
use the compositor identified by `WAYLAND_DISPLAY`. CI runs a second pass
against a headless compositor to exercise output layout and scaling.
Cases that need unavailable hardware report a skip.

#### Network lifecycle tests without a GPU

A focused target compiles the production `src/network.cpp` and runs the socket
and shared-resource tests without the capture/encoder dependencies or a display
server. It covers failed startup cleanup, repeated reconnects, OS error reporting,
and executed helpers retaining control, video, or audio sockets. Linux clipboard
tests also exercise the actual helper pipe code with deliberately inheritable
sockets, full-size/empty content, failed commands, and broken pipes. The target supplements the full suite;
it does not exercise RTSP negotiation, capture, or actual client sessions.

Use the project-local [mcpp toolchain setup](modularization/mcpp-migration.md).
Run from the repository root.

```bash
git submodule update --init third-party/googletest third-party/moonlight-common-c
git -C third-party/moonlight-common-c submodule update --init enet
MCPP_HOME="$PWD/.mcpp/local-home" MCPP_TOOLCHAIN="path:$PWD/.toolchain" \
  mcpp run --offline --profile sanitize -p hermes-network-lifecycle \
  network_lifecycle_tests -- --gtest_color=yes
```

The `Network lifecycle regressions` workflow runs this target with AddressSanitizer,
UndefinedBehaviorSanitizer, and leak detection. In environments that block
LeakSanitizer's process inspection, `ASAN_OPTIONS=detect_leaks=0` allows the other
sanitizers to run; leak detection must still run on an unrestricted CI runner.

#### Web UI tests
The Web UI tests run with Bun. The production Vite configuration takes asset
paths from the native build and emits eight HTML pages with precompiled Vue
templates.

```bash
cd src_assets/common/assets/web
bun install --frozen-lockfile
bun run typecheck
bun run test
```

#### Clang Format
Source code follows the `.clang-format` file. Nothing enforces it in CI, so format before committing.

Option 1:
```bash
find ./ -iname *.cpp -o -iname *.h -iname *.m -iname *.mm | xargs clang-format -i
```

Option 2 (will modify files):
```bash
./scripts/update_clang_format.sh
```
