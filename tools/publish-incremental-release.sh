#!/usr/bin/env bash
set -euo pipefail
[[ -n "${GITHUB_SHA:-}" && -n "${GITHUB_REPOSITORY:-}" ]] || {
  echo 'Requires GitHub Actions context' >&2; exit 2;
}
command -v gh >/dev/null
APPIMAGE="${1:-dist/pcble2gamepad-x86_64.AppImage}"
[[ -s "$APPIMAGE" ]] || { echo "Missing $APPIMAGE" >&2; exit 1; }
sha256sum "$APPIMAGE" > "$APPIMAGE.sha256"
git fetch --tags --prune origin
while IFS= read -r tag; do
  [[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || continue
  if [[ "$(git rev-list -n 1 "$tag")" == "$GITHUB_SHA" ]]; then
    if gh release view "$tag" >/dev/null 2>&1; then
      echo "Already published this commit as $tag; no changes"
      exit 0
    fi
    echo "Existing tag $tag points to this commit; creating its missing release"
    gh release create "$tag" "$APPIMAGE" "$APPIMAGE.sha256" \
      --title "$tag" --notes "Automated Linux x86_64 build from $GITHUB_SHA"
    exit 0
  fi
done < <(git tag --list)
latest="$(git tag --list | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' | sort -V | tail -n1 || true)"
if [[ -z "$latest" ]]; then
  next='v1.0.0.0'
else
  prefix="${latest%.*}"
  last="${latest##*.}"
  next="${prefix}.$((10#$last + 1))"
fi
if git show-ref --verify --quiet "refs/tags/$next"; then
  echo "Tag $next already exists, refusing to overwrite" >&2; exit 1
fi
git tag -a "$next" "$GITHUB_SHA" -m "pcble2gamepad $next"
git push origin "refs/tags/$next"
gh release create "$next" "$APPIMAGE" "$APPIMAGE.sha256" \
  --title "$next" --notes "Automatic x86_64 build tested at $GITHUB_SHA. Bluetooth hardware requires a separately installed, privileged backend."
