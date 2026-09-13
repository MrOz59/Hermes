# Contributing
Read [CONTRIBUTING.md](https://github.com/MrOz59/Hermes/blob/main/.github/CONTRIBUTING.md)
first: it covers which branch a pull request targets (`main` for fixes, `dev`
for the transport and virtual-display migration), how issues are reported, and
the changelog entry every change carries. This page covers the project's
internals — the Web UI, localization and the test suites.

## Recommended Tools

| Tool                                                                                                                                                                           | Description                                                                                                                                                                           |
|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------|
| <a href="https://www.jetbrains.com/clion/"><img src="https://resources.jetbrains.com/storage/products/company/brand/logos/CLion_icon.svg" width="30" height="30"></a><br>CLion | Recommended IDE for C and C++ development. Free for non-commercial use. |

## Project Patterns

### Web UI
* The Web UI uses [Vite](https://vitejs.dev) as its build system.
* The HTML pages used by the Web UI are found in `./src_assets/common/assets/web`.
* [EJS](https://www.npmjs.com/package/vite-plugin-ejs) is used as a templating system for the pages
  (check `template_header.html` and `template_header_main.html`).
* The Style System is provided by [Bootstrap](https://getbootstrap.com).
* The JS framework used by the more interactive pages is [Vue.js](https://vuejs.org).

#### Building

@tabs{
  @tab{CMake | ```bash
    cmake -B build -G Ninja -S . --target web-ui
    ninja -C build web-ui
    ```}
  @tab{Manual | ```bash
    npm run dev
    ```}
}

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

Strings are extracted from the code to the `locale/sunshine.po` template file. Upstream generates it from a
`localize.yml` workflow; this fork has no such workflow, so extract locally when you add a translatable C++ string.

When testing locally it may be desirable to manually extract, initialize, update, and compile strings. Python is
required for this, along with the python dependencies in the `./scripts/requirements.txt` file. Additionally,
[xgettext](https://www.gnu.org/software/gettext) must be installed.

* Extract, initialize, and update
  ```bash
  python ./scripts/_locale.py --extract --init --update
  ```

* Compile
  ```bash
  python ./scripts/_locale.py --compile
  ```

> [!IMPORTANT]
> Do not include extracted or compiled localization files in pull requests.

### Testing

There are two suites, and the `test` job in
[.github/workflows/build.yml](https://github.com/MrOz59/Hermes/blob/main/.github/workflows/build.yml)
runs both on every push and pull request. Nothing else is gated: this fork has no
clang-format lint job and no coverage upload, so run these locally before opening a PR.

#### C++ unit tests
Hermes uses [Google Test](https://github.com/google/googletest), included as a submodule. The test sources are in
`./tests`. They are built by the normal build process when `BUILD_TESTS` is `ON`, and can be turned off with `OFF`.

Configure and build the same way CI does:

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSUNSHINE_ENABLE_WAYLAND=ON \
  -DSUNSHINE_ENABLE_X11=ON \
  -DSUNSHINE_ENABLE_DRM=ON \
  -DSUNSHINE_ENABLE_CUDA=OFF \
  -DBUILD_TESTS=ON
cmake --build build --target test_sunshine -j$(nproc)
```

The suite is a single GoogleTest binary with no ctest registration, so run it directly. On a headless machine run it
under `xvfb`, so `platf::init()` finds an X11 capture source — without one the platform-dependent suites fail their
setup rather than skipping:

```bash
xvfb-run -a ./build/tests/test_sunshine --gtest_color=yes
```

To see all available options, run the tests with the `--help` flag.

```bash
./build/tests/test_sunshine --help
```

Cases that need hardware the machine does not have skip themselves. The Wayland suites need a compositor on
`WAYLAND_DISPLAY`; CI starts a headless wlroots compositor for them, and without one they skip.

> [!TIP]
> See the googletest [FAQ](https://google.github.io/googletest/faq.html) for more information on how to use Google Test.

#### Web UI tests
The Web UI is tested with [Vitest](https://vitest.dev), on its own config: `vite.config.js` is driven by the CMake
environment and builds the pages through ejs, neither of which a unit test needs.

```bash
npm install
npm test
```

#### Clang Format
Source code follows the `.clang-format` file. Nothing enforces it in CI, so format before committing.

Option 1:
```bash
find ./ -iname *.cpp -o -iname *.h -iname *.m -iname *.mm | xargs clang-format -i
```

Option 2 (will modify files):
```bash
python ./scripts/update_clang_format.py
```

<div class="section_buttons">

| Previous                |                                                         Next |
|:------------------------|-------------------------------------------------------------:|
| [Building](building.md) | [Source Code](../third-party/doxyconfig/docs/source_code.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
