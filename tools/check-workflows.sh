#!/bin/sh
# Checks the workflow files with actionlint before CI sees them.
#
# It exists because of a specific failure. `.github/workflows/wine-arm64ec-build.yml`
# used `${{ runner.temp }}` in a job-level `env:` block, where the `runner` context
# does not exist. GitHub rejected the whole file, created a run with *no jobs*, and
# gave the run no log -- so the failure had no evidence attached to it, and the only
# way to read the message was to fetch the run's HTML page and pull it out of the
# markup. `actionlint` reports the same problem in a second, offline:
#
#   (Line: 33, Col: 17): Unrecognized named-value: 'runner'
#
# pyyaml accepts that file, which is why the project's YAML check was not enough:
# valid YAML and a valid workflow are different things.
#
# Exit codes: 1 when actionlint finds something, 0 otherwise, including when
# actionlint is not installed. A missing checker is not a passing check, and the
# message says so rather than staying quiet.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

if command -v actionlint >/dev/null 2>&1; then
  ACTIONLINT=actionlint
elif [ -x "$HOME/.local/bin/actionlint" ]; then
  ACTIONLINT="$HOME/.local/bin/actionlint"
else
  printf 'check-workflows: no actionlint on PATH, so there is nothing to check with.\n'
  printf 'check-workflows: install it from https://github.com/rhysd/actionlint/releases\n'
  printf 'check-workflows: at least v1.7, then run this again.\n'
  exit 0
fi

# shellcheck is left out on purpose: it would be another tool to install, and the
# class of error this guards against is workflow schema, not shell linting.
if "$ACTIONLINT" -shellcheck= .github/workflows/*.yml; then
  printf 'check-workflows: %s agrees with every workflow file.\n' "$("$ACTIONLINT" -version | head -1)"
else
  printf 'check-workflows: actionlint found problems above.\n'
  exit 1
fi
