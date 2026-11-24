# Publishing the Hermes documentation

1. After the Documentation workflow lands on `main` in `MrOz59/Hermes`, open
   **Settings → Pages → Build and deployment** and select **GitHub Actions** as
   the source. The workflow uses the project's standard Pages address
   [mroz59.github.io/Hermes](https://mroz59.github.io/Hermes/).
2. If the `github-pages` deployment environment uses a branch restriction,
   allow `main`. The workflow grants Pages write and OIDC permissions to the
   deployment job using the built-in token.
3. To publish immediately after setup, open **Actions → Documentation →
   Run workflow**, select `main`, and run it. Subsequent changes to the
   documentation or its build inputs on `main` trigger publication automatically.

The deployment job publishes the build from the same successful workflow run.
Open its **Deploy GitHub Pages artifact** step for the live URL. Pull requests
and forks still produce a downloadable `hermes-documentation` preview.
If the build fails, the existing site stays online.

Announcements continue to come from
[`feed.json` on the `feed` branch](https://raw.githubusercontent.com/MrOz59/Hermes/feed/feed.json).
Publish notices through the existing [announcements process](../announcements.md).

[GitHub Pages setup documentation](https://docs.github.com/en/pages/getting-started-with-github-pages/configuring-a-publishing-source-for-your-github-pages-site)
