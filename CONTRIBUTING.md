# Contributing to OTNS

## Contributor License Agreement (CLA)

Contributions to this project must be accompanied by a Contributor License Agreement. You (or your employer) retain the copyright to your contribution; this simply gives us permission to use and redistribute your contributions as part of the project. Head over to <https://cla.developers.google.com/> to see your current agreements on file or to sign a new one.

You generally only need to submit a CLA once, so if you've already submitted one (even if it was for a different project), you probably don't need to do it again.

## Code of Conduct

Help us keep OTNS open and inclusive. Please read and follow our [Code of Conduct](CODE_OF_CONDUCT.md).

## Bugs

If you find a bug in the source code, you can help us by [submitting a GitHub Issue](https://github.com/openthread/ot-ns/issues/new). Even better, you can [submit a Pull Request](#pull-requests) with a fix.

## New Features

You can request a new feature by [submitting a GitHub Issue](https://github.com/openthread/ot-ns/issues/new).

If you would like to implement a new feature, please consider its scope:

- *Large feature*: [Submit a GitHub Issue](https://github.com/openthread/ot-ns/issues/new) with your proposal so the community can review and provide feedback first. Early feedback helps align design decisions and avoid duplicated work.
- *Small feature*: Implement and [submit a Pull Request](#pull-requests) directly without a prior proposal.

## Contributing Code

The OTNS Project follows the "Fork-and-Pull" model for accepting contributions.

### Initial Setup

Set up your GitHub fork and local checkout:

1. Fork the [ot-ns repository](https://github.com/openthread/ot-ns) by clicking **Fork** on GitHub.
2. Clone your fork with submodules initialized (`openthread` is required by CMake):

```bash
# Clone your fork (v3 branch with openthread submodule)
git clone --branch v3 --recurse-submodules git@github.com:<username>/ot-ns.git
cd ot-ns

# Configure upstream alias
git remote add upstream git@github.com:openthread/ot-ns.git
```

### Pull Requests

#### Branch

Create a working branch from `v3` (use `main` only for legacy OTNS v2 changes):

```bash
git checkout -b <branch-name> origin/v3
```

#### Create Commits

```bash
git add <file1> <file2>
git commit
```

This opens a text editor where you can write a clear, focused commit message.

#### Upstream Sync and Clean Up

Before submitting your pull request, sync with upstream `v3` and rebase your working branch so it applies cleanly:

```bash
git fetch upstream
git checkout v3
git pull upstream v3

git checkout <branch-name>
git rebase v3
```

Consider squashing small fixup commits into cohesive logical commits using an interactive rebase:

```bash
git checkout <branch-name>
git rebase -i v3
```

#### Coding Conventions and Style

OTNS v3 is written in C++17 and follows OpenThread's host-C++ style (`openthread/STYLE_GUIDE.md` naming, Allman braces, `-fno-exceptions -fno-rtti`), matching `ot-br-posix` and `openthread/tests/nexus`.

- **C/C++ formatting**: Enforced via `.clang-format` (clang-format 19). Run `./script/make-pretty` to format in place and `./script/make-pretty check` to verify compliance.
- **Compiler checks**: Keep builds clean under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror` and AddressSanitizer + UndefinedBehaviorSanitizer (`-DOTNS_ENABLE_SANITIZERS=ON`).
- **Web UI and Python**: Run `npm --prefix web run typecheck && npm --prefix web run build` when modifying `web/`, and run `OTNS_BIN=./build-release/otns python3 -m unittest discover -s python/tests -v` when modifying `python/` or `script/`.

#### Push and Test

```bash
git checkout <branch-name>
git push origin <branch-name>
```

Pushing triggers GitHub Actions CI checks. You can monitor status and logs under the **Actions** tab in your fork.

#### Submit the Pull Request

Once CI checks pass, open your fork on GitHub, select your working branch, target `v3`, and click **Pull Request**. Any follow-up commits pushed to that branch update the pull request automatically.
