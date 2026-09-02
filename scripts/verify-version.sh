#!/bin/sh
# Verifies a release tag (e.g. v0.1) matches src/version.h and devsoak.readme
# before release.yml hands off to sidick/amiga-workflows' aminet-release.yml
# - the calling repo's own version-file format is project-specific, so this
# check stays here rather than in the shared workflow (see that repo's own
# aminet-release.yml header comment for why).
#
# Usage: scripts/verify-version.sh vX.Y
set -eu

tag_ref="${1:?usage: verify-version.sh <tag, e.g. v0.1>}"
tag="${tag_ref#v}"

src=$(sed -n 's/^#define DEVSOAK_VERSION[[:space:]]*"\(.*\)"$/\1/p' src/version.h)
readme=$(sed -n 's/^Version:[[:space:]]*\(.*\)$/\1/p' devsoak.readme)
date=$(sed -n 's/^#define DEVSOAK_VERSION_DATE[[:space:]]*"\(.*\)"$/\1/p' src/version.h)

echo "tag=$tag src/version.h=$src ($date) devsoak.readme=$readme"
[ "$tag" = "$src" ]    || { echo "::error file=src/version.h::Tag v$tag does not match DEVSOAK_VERSION \"$src\""; exit 1; }
[ "$tag" = "$readme" ] || { echo "::error file=devsoak.readme::Tag v$tag does not match Version: \"$readme\""; exit 1; }

case "$date" in
    [0-9][0-9].[0-9][0-9].[0-9][0-9][0-9][0-9]) ;;
    *) echo "::error file=src/version.h::DEVSOAK_VERSION_DATE \"$date\" is not DD.MM.YYYY"; exit 1 ;;
esac

# A release PR that bumps DEVSOAK_VERSION but forgets the date would
# otherwise ship a binary whose $VER string still carries the previous
# release's date. Compare against the previous release tag's own
# src/version.h (skipped gracefully if there isn't one, e.g. this is the
# first release).
prev_tag=$(git tag -l 'v*' --sort=-v:refname | grep -vFx "v$tag" | head -n1 || true)
if [ -n "$prev_tag" ]; then
    prev_date=$(git show "$prev_tag:src/version.h" 2>/dev/null | \
        sed -n 's/^#define DEVSOAK_VERSION_DATE[[:space:]]*"\(.*\)"$/\1/p')
    if [ -n "$prev_date" ] && [ "$date" = "$prev_date" ]; then
        echo "::error file=src/version.h::DEVSOAK_VERSION_DATE \"$date\" is unchanged since $prev_tag - bump it too"
        exit 1
    fi
fi
