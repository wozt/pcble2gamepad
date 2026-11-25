#!/usr/bin/env python3
"""pcble2gamepad releases, adapted from WUP Stream's tools/publish_release.py.

After successful tests/AppImage build, create a lightweight tag via the GitHub
refs API (no git user.name/email), then publish versioned assets. Supports
annotated tags from the legacy shell publisher, collisions and reruns.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

TAG_PATTERN = re.compile(r"^refs/tags/v1\.0\.0\.(0|[1-9][0-9]*)(\^\{\})?$")
MAX_TAG_ATTEMPTS = 15


def read_version_tags(raw: str) -> dict[int, str]:
    """Index -> *commit* SHA, supporting lightweight and annotated Git tags."""
    tags: dict[int, str] = {}
    peeled: set[int] = set()
    for line in raw.splitlines():
        fields = line.split()
        if len(fields) != 2 or not re.fullmatch(r"[0-9a-f]{40}", fields[0]):
            continue
        match = TAG_PATTERN.fullmatch(fields[1])
        if not match:
            continue
        index = int(match.group(1))
        if match.group(2):
            # The ^{} line is the peeled commit SHA of an annotated tag.
            tags[index] = fields[0]
            peeled.add(index)
        elif index not in peeled:
            tags[index] = fields[0]
    return tags


def choose_tag(tags: dict[int, str], commit_sha: str) -> tuple[int, bool]:
    matching = [n for n, sha in tags.items() if sha == commit_sha]
    if matching:
        return min(matching), True
    return max(tags, default=-1) + 1, False


def command(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(args, text=True, capture_output=True, check=False)
    if check and proc.returncode:
        raise RuntimeError(
            f"Command failed ({proc.returncode}): {' '.join(args)}\n"
            f"{proc.stderr[-3000:]}"
        )
    return proc


def remote_tags() -> dict[int, str]:
    return read_version_tags(
        command("git", "ls-remote", "--tags", "origin",
                "refs/tags/v1.0.0.*").stdout
    )


def tag_exists_on_github(repo: str, tag: str) -> bool:
    return command("gh", "release", "view", tag, "--repo", repo,
                   "--json", "tagName", check=False).returncode == 0


def reserve_tag(repo: str, sha: str) -> str:
    """Reserve the next version atomically, retrying only actual collisions."""
    for attempt in range(MAX_TAG_ATTEMPTS):
        tags = remote_tags()
        index, reused = choose_tag(tags, sha)
        tag = f"v1.0.0.{index}"
        if reused:
            print(f"Reusing {tag} for commit {sha[:12]}", flush=True)
            return tag

        result = command(
            "gh", "api", "-X", "POST", f"repos/{repo}/git/refs",
            "-f", f"ref=refs/tags/{tag}", "-f", f"sha={sha}",
            check=False,
        )
        if result.returncode == 0:
            print(f"Reserved {tag} for commit {sha[:12]}", flush=True)
            return tag

        updated = remote_tags()
        if index in updated:
            if updated[index] == sha:
                return tag
            print(f"{tag} allocated by another build; retrying", flush=True)
            time.sleep(min(1 + attempt, 5))
            continue
        raise RuntimeError(f"Cannot create {tag}: {result.stderr[-2000:]}")
    raise RuntimeError("Too many concurrent tag collisions")


def release_asset_names(repo: str, tag: str) -> set[str]:
    proc = command("gh", "release", "view", tag, "--repo", repo,
                   "--json", "assets", "--jq", ".assets[].name",
                   check=False)
    return set(proc.stdout.splitlines()) if proc.returncode == 0 else set()


def build_assets(original: Path, tag: str) -> tuple[Path, Path]:
    version = tag.removeprefix("v")
    output = original.with_name(f"pcble2gamepad-{version}-x86_64.AppImage")
    if output != original:
        shutil.copy2(original, output)
    digest = hashlib.sha256()
    with output.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    checksums = output.parent / "SHA256SUMS"
    checksums.write_text(f"{digest.hexdigest()}  {output.name}\n", encoding="utf-8")
    return output, checksums


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: publish_release.py dist/pcble2gamepad-x86_64.AppImage")
    repo = os.environ.get("GITHUB_REPOSITORY", "")
    sha = os.environ.get("GITHUB_SHA", "")
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", repo):
        raise RuntimeError("GITHUB_REPOSITORY is absent or invalid")
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise RuntimeError("GITHUB_SHA is absent or invalid")
    if not os.environ.get("GH_TOKEN"):
        raise RuntimeError("GH_TOKEN is required")
    original = Path(sys.argv[1]).resolve()
    if not original.is_file() or original.stat().st_size < 1024:
        raise RuntimeError(f"Missing or empty AppImage: {original}")
    with original.open("rb") as stream:
        if stream.read(4) != b"\x7fELF":
            raise RuntimeError(f"Not an ELF/AppImage: {original}")

    tag = reserve_tag(repo, sha)
    appimage, checksums = build_assets(original, tag)
    expected = {appimage.name, checksums.name}
    existing = release_asset_names(repo, tag)
    if expected.issubset(existing):
        print(f"Release {tag} already contains both assets; nothing to do")
        return

    if tag_exists_on_github(repo, tag):
        command("gh", "release", "upload", tag, str(appimage),
                str(checksums), "--clobber", "--repo", repo)
        print(f"Repaired missing assets of {tag}")
    else:
        command("gh", "release", "create", tag, str(appimage),
                str(checksums), "--verify-tag", "--repo", repo,
                "--title", tag, "--generate-notes")
        print(f"Published {tag}")
    print(f"AppImage: {appimage.name}; checksum: {checksums.name}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"Release failed: {exc}", file=sys.stderr)
        sys.exit(1)
