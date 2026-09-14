#!/usr/bin/env bash
# Usage: dev/create-release.sh [revision]
#
# The version is "<nix major.minor>.<revision>" (see default.nix). Without an
# argument the revision is incremented by one; pass an explicit revision (e.g. 0)
# after bumping to a new Nix major.minor.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null && pwd)"
cd "$SCRIPT_DIR/.."

repo=NixOS/nix-eval-jobs

if [[ "$(git symbolic-ref --short HEAD)" != "main" ]]; then
  echo "must be on main branch" >&2
  exit 1
fi

uncommitted_changes=$(git diff --compact-summary)
if [[ -n $uncommitted_changes ]]; then
  echo -e "There are uncommitted changes, exiting:\n${uncommitted_changes}" >&2
  exit 1
fi
git pull "git@github.com:${repo}" main
unpushed_commits=$(git log --format=oneline origin/main..main)
if [[ -n $unpushed_commits ]]; then
  echo -e "\nThere are unpushed changes, exiting:\n$unpushed_commits" >&2
  exit 1
fi

current_revision=$(sed -n 's/^  revision = "\([0-9]\+\)";$/\1/p' default.nix)
if [[ -z $current_revision ]]; then
  echo "could not find revision in default.nix" >&2
  exit 1
fi
revision="${1:-$((current_revision + 1))}"
if [[ ! $revision =~ ^[0-9]+$ ]]; then
  echo "revision must be a number, got: ${revision}" >&2
  exit 1
fi

sed -i -e "s!^  revision = \".*\";\$!  revision = \"${revision}\";!" default.nix
version=$(nix eval --raw .#default.version)
tag="v${version}"

if git ls-remote --exit-code --tags origin "refs/tags/${tag}" >/dev/null; then
  echo "Tag ${tag} already exists, exiting" >&2
  git checkout -- default.nix
  exit 1
fi

echo "Releasing ${version}"

git add default.nix
git branch -D "release-${version}" 2>/dev/null || true
git checkout -b "release-${version}"
git commit -m "release ${version}"
git push --force origin "release-${version}"
pr_url=$(gh pr create \
  --repo "$repo" \
  --base main \
  --head "release-${version}" \
  --title "Release ${version}" \
  --body "Release ${version} of nix-eval-jobs")
gh pr merge --repo "$repo" "$pr_url" --auto --merge
git checkout main

while [[ "$(gh pr view --repo "$repo" "$pr_url" --json state -q .state)" != "MERGED" ]]; do
  echo "Waiting for PR to be merged..."
  sleep 10
done

git pull "git@github.com:${repo}" main
git tag "${tag}"
git push origin "refs/tags/${tag}"
gh release create --repo "$repo" "${tag}" --title "${tag}" --generate-notes
