#!/usr/bin/env sh
# POSIX-shell wrapper so agents using Git Bash can build: forwards all arguments to build.ps1.
#   scripts/build.sh -Preset release -Target pq_tests_core
here="$(cd "$(dirname "$0")" && pwd)"
exec pwsh -NoProfile -ExecutionPolicy Bypass -File "$here/build.ps1" "$@"
