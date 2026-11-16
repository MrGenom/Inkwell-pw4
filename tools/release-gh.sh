#!/bin/sh
# Publishes a GitHub release from the release branch.
#
#   tools/release-gh.sh 1.2.1-pw4 [notes.md]
#
# Checks that you are on the release branch with a clean tree that has
# everything origin has, and that the tag v<version> is new. Then builds
# dist/Inkwell-<version>.zip with tools/docker-build.sh, tags the commit,
# pushes the branch and the tag, and creates the GitHub release with the zip
# and SHA256SUMS. Without a notes file GitHub writes the notes itself.
# DRY_RUN=1 runs the checks only. RELEASE_BRANCH overrides "release".
#
# The usual round: git switch release && git merge --ff-only dev, then this.
set -e
cd "$(dirname "$0")/.."
V=${1:?usage: tools/release-gh.sh version [notes.md]}
NOTES=$2
BR=${RELEASE_BRANCH:-release}
TAG=v$V
GH=$(command -v gh 2>/dev/null || echo "/c/Program Files/GitHub CLI/gh.exe")
REPO=$(git remote get-url origin | sed -e 's#^.*github\.com[:/]##' -e 's#\.git$##')

die() { echo "release-gh: $*" >&2; exit 1; }
[ "$(git rev-parse --abbrev-ref HEAD)" = "$BR" ] || die "switch to $BR first: git switch $BR && git merge --ff-only dev"
[ -z "$(git status --porcelain)" ] || die "the working tree has changes; commit or stash them"
git fetch -q origin
git merge-base --is-ancestor "origin/$BR" HEAD || die "$BR is behind origin/$BR; pull first"
git rev-parse -q --verify "refs/tags/$TAG" >/dev/null && die "tag $TAG already exists here"
git ls-remote --exit-code --tags origin "refs/tags/$TAG" >/dev/null && die "tag $TAG already exists on origin"
[ -z "$NOTES" ] || [ -f "$NOTES" ] || die "no notes file $NOTES"
"$GH" auth status >/dev/null 2>&1 || die "gh is not logged in: gh auth login"

echo "release-gh: $TAG from $BR at $(git rev-parse --short HEAD) to $REPO"
[ -n "$DRY_RUN" ] && { echo "release-gh: dry run, stopping here"; exit 0; }

tools/docker-build.sh "$V"
Z=dist/Inkwell-$V.zip
[ -f "$Z" ] || die "the build did not produce $Z"

git tag -a "$TAG" -m "Inkwell $V"
git push origin "$BR" "$TAG"
if [ -n "$NOTES" ]; then set -- --notes-file "$NOTES"; else set -- --generate-notes; fi
"$GH" release create "$TAG" -R "$REPO" --title "Inkwell $V" --verify-tag "$@" "$Z" dist/SHA256SUMS
