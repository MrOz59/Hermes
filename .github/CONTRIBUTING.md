# Contributing Guidelines

Thank you for your interest in contributing to this project! We welcome contributions from the community and appreciate your efforts to make this project better.

## Branches

- **`main`** — the released line. It takes reported bug fixes and small,
  self-contained features. It is the default branch, so this is where a pull
  request lands unless you say otherwise, and it is the right target for
  anything users are waiting on.
- **`dev`** — the transport and virtual-display migration (the `H*` phases in
  `HERMES-PRODUCT-ROADMAP.md`). It reorganizes internals and moves faster than
  a release can absorb, which is exactly why it is kept separate. Help is
  welcome here too; just say in the pull request that you are targeting `dev`,
  so review can account for ground that is still moving.

`dev` merges `main` regularly; `main` never merges `dev`. A fix that users are
waiting on therefore belongs in `main` first and reaches `dev` on the next
merge — landing it only in `dev` means it misses the release.

Add changelog entries under `## [Unreleased]` in `CHANGELOG.md`. Dated release
sections are created by `scripts/bump-version.sh` when a version is cut, so
please do not add one by hand.

## How to Contribute

### Reporting Issues

Issues are opened through the forms at
[New issue](https://github.com/MrOz59/Hermes/issues/new/choose). Each form asks
for the three things a report needs to be actionable: the Hermes version, the
output of one copy-paste block of host information, and a debug-level log.

Please fill those in rather than deleting them. Hermes sits between your kernel,
your compositor and your GPU driver, and which of the three failed is only
visible in that data — without it a report cannot be investigated, only guessed
at, and a guess costs you another round trip before anything can be looked at.

- **Bug report** — streaming, audio, input, pairing, apps, the Web UI.
- **Virtual display problem** — black screen, the client mirroring the physical
  monitor, wrong resolution, no output created. Also asks what your compositor
  reports about its outputs, which is where these failures are decided.
- **Crash or freeze** — asks for `coredumpctl` output. Install the matching
  `hermes-streaming-debug` package first, so the backtrace has names on it
  instead of raw offsets into a stripped binary.
- **Install, build or packaging problem** — the exact command and its complete
  output, not only the line that looks like the error.
- **Compatibility report** — how Hermes behaved on your distribution,
  compositor and GPU. A report that it simply *works* is as valuable as a
  failure: it is what moves an entry in `docs/compatibility.md` from "expected
  to work" to "verified".
- **Feature request** and **Question** for everything else.

Search the open and closed issues first, and read
[docs/compatibility.md](../docs/compatibility.md) — your combination may already
be recorded there as verified, expected to work, or known broken.

### Code Contributions

#### Getting Started

1. Fork the repository
2. Create a new branch for your feature or bugfix: `git checkout -b feature/your-feature-name`
3. Make your changes
4. Test your changes thoroughly
5. Commit your changes with clear, descriptive messages
6. Push your branch to your fork
7. Create a pull request

#### Code Standards

- Follow the existing code style and formatting conventions
- Write clear, readable code with appropriate comments
- Ensure all changes are sound
- Keep commits focused and atomic

#### Pull Request Guidelines

- Provide a clear description of what your PR does
- Reference any related issues using keywords like "Fixes #123"
- Include screenshots or examples if your changes affect the UI
- Be responsive to feedback and suggestions during code review

## Important Rules

### AI-Generated Code Policy

**AI-generated code is acceptable, but please make sure you have thoroughly reviewed and understand what it does.**

When using AI tools to generate code:
- Review every line of generated code carefully
- Understand the logic and potential implications
- Test the code thoroughly in your environment
- Ensure it follows project conventions and best practices
- Take responsibility for any issues that may arise from the generated code

***IMPORTANT: Don't trust AI generated tests. Test each modifications manually.***

### Code Review Process

- All contributions must go through code review
- Maintainers will review your pull request and provide feedback
- Address all feedback before the PR can be merged
- Be patient and respectful during the review process

## Development Setup

Please refer to the README.md file for instructions on setting up your development environment.

## Questions?

If you have questions about contributing, feel free to:
- Open an issue for discussion
- Contact the maintainers
- Check existing documentation

Thank you for contributing!

