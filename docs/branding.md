# Product names and compatibility

Use **Hermes** in host UI text, diagnostics, owned artwork filenames, and new documentation. The web stylesheet and image assets use `hermes` names. The old favicon and logo HTTP routes and the `apollo.css` stylesheet remain aliases for cached pages.

The host build uses `HERMES_*` for private compile definitions and reports the
Hermes repository in its publisher and support metadata.

- The Hestia compatibility schema retains `base: Apollo` and `sunshine`. GameStream clients also consume the legacy server-state values.
- Existing installs retain the `sunshine_name` configuration field, legacy migration and service names, cached image routes, and device names used by Linux audio/input rules.
- Commands launched by Hermes still receive the `APOLLO_*` and `SUNSHINE_*` compatibility variables. Vite accepts the previous asset-path environment variables as fallbacks.
- Recommend Hestia for desktop streaming. Use “client” for behavior shared by Hestia, Artemis, Moonlight, and other compatible clients. References to Moonlight still identify its upstream library, documented compatibility, and behavior specific to that client.
- The preserved Flatpak templates pair `sunshine.sh` with an older desktop entry. The native mcpp package uses the Hermes desktop entry and service.
- Upstream library names, documentation links, license notices, and historical changelog entries retain their original names.

Changing these interfaces needs a separate migration with old-client and existing-installation tests. Do not perform a global Sunshine/Apollo replacement.
