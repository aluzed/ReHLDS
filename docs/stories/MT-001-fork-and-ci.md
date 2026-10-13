# MT-001 — Fork, branching model and CI on the fork

**Epic:** E0 Foundations · **Priority:** P0 · **Size:** S · **Depends on:** –

## Context

Work happens on a personal fork of `rehlds/ReHLDS`. Upstream keeps moving (security
fixes such as `550f2d6` dlfile flood fix), so the fork must stay easy to rebase and
any generic improvement should be upstreamable as a small PR.

The existing workflow `.github/workflows/build.yml` builds Linux and Windows.

## User story

As a developer, I want a fork with a simple branching model and green CI on every push,
so that each story can be developed and benchmarked on top of the previous ones.

## Scope

* Fork `rehlds/ReHLDS` to the personal GitHub account; keep `upstream` as a second remote.
* Branching: `master` mirrors upstream; all multithreading work lives on the single
  branch `feature/multithreading`, one or more commits per story, each commit message
  referencing the story ID (e.g. `feat: ... (MT-002)`).
* Enable GitHub Actions on the fork; make sure the existing build workflow runs on
  pushes to `feature/**` branches.
* Commit convention: story ID in the subject; commits touching the frame include
  before/after `sv_profile` numbers in the body or in the story file.
* Document the upstream sync procedure (rebase `feature/multithreading` on `upstream/master`).

## Acceptance criteria

* [x] Fork exists, `origin` = fork, `upstream` = `rehlds/ReHLDS`.
* [ ] CI builds Linux (i386) and Windows on push to `feature/**`.
* [x] `docs/` is present on `feature/multithreading`.
* [x] The "Branching" section of `docs/stories/README.md` explains branches and sync.

## Risks

* Upstream CI uses secrets for signing releases; release jobs must be disabled on the
  fork so they do not fail.

## Out of scope

* Publishing releases from the fork.
