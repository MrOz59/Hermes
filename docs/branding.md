# Product names and compatibility

Use **Hermes** in host UI text, diagnostics, owned artwork filenames, and new documentation. The web stylesheet and image assets use `hermes` names. The old favicon and logo HTTP routes remain aliases for cached pages.

Some inherited names are external interfaces, not display branding:

- Capability `base: Apollo` and the `sunshine` compatibility flag are consumed by existing clients, including Hestia.
- Configuration keys such as `sunshine_name`, the `APOLLO_*` and `SUNSHINE_*` script environments, legacy migration paths, and build options remain compatible.
- Upstream library names, documentation links, license notices, and historical changelog entries retain their original names.

Changing these interfaces needs a separate migration with old-client and existing-installation tests. Do not perform a global Sunshine/Apollo replacement.
