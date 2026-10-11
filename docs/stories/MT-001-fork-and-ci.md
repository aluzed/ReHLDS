# MT-001 — Fork, branching model and CI on the fork

**Epic:** E0 Foundations · **Priority:** P0 · **Size:** S · **Depends on:** –

## Context

Work happens on a personal fork of `rehlds/ReHLDS`. Upstream keeps moving (security
fixes such as `550f2d6` dlfile flood fix), so the fork must stay easy to rebase and
any generic improvement should be upstreamable as a small PR.

The existing workflow `.github/workflows/build.yml` builds Linux and Windows.

## User story

As a developer, I want a fork with a clear branching model and green CI on every push,
so that each story can be developed, reviewed and benchmarked in isolation.

## Scope

* Fork `rehlds/ReHLDS` to the personal GitHub account; keep `upstream` as a second remote.
* Branching: `master` mirrors upstream, `mt/main` is the integration branch, one
  `mt/MT-xxx-short-name` branch per story, PRs into `mt/main`.
* Enable GitHub Actions on the fork; make sure the existing build workflow runs on
  `mt/**` branches and PRs into `mt/main`.
* Add a PR template referencing the story ID and requiring benchmark numbers when
  the story touches the frame.
* Document the upstream sync procedure (rebase `mt/main` on `upstream/master`).

## Acceptance criteria

* [ ] Fork exists, `origin` = fork, `upstream` = `rehlds/ReHLDS`.
* [ ] CI builds Linux (i386) and Windows on push to `mt/**` and on PRs.
* [ ] `docs/` is present on `mt/main`.
* [ ] `CONTRIBUTING`-style section in `docs/stories/README.md` or a separate file
      explains branches and sync.

## Risks

* Upstream CI uses secrets for signing releases; release jobs must be disabled on the
  fork so they do not fail.

## Out of scope

* Publishing releases from the fork.
