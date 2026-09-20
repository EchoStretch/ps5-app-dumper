#!/bin/sh
# Installs the webhb release a project is pinned to into ext/webhb. Copy this
# file into the project (tools/new-app.sh does) next to a webhb.lock:
#
#   version=0.1.0
#   url=https://github.com/<owner>/webhb/releases/download/v0.1.0/webhb-0.1.0.tar.gz
#   sha256=<what `make dist` printed>
#
# The archive is checked against the sha256 before anything is unpacked, so a
# release cannot change under a project's feet. To work on webhb and a project
# side by side, skip all this: make WEBHB_DIR=../webhb
#
#   usage: tools/get-webhb.sh [lock file] [target dir]      (WEBHB_TARBALL=<file> uses a local archive)
set -eu
LOCK="${1:-webhb.lock}"; DEST="${2:-ext/webhb}"
val() { sed -n "s/^$1=//p" "$LOCK" | head -1; }
VERSION="$(val version)"; URL="$(val url)"; SHA="$(val sha256)"
[ -n "$VERSION" ] && [ -n "$SHA" ] || { echo "get-webhb: $LOCK needs version= and sha256=" >&2; exit 1; }

if [ -f "$DEST/VERSION" ] && [ "$(cat "$DEST/VERSION")" = "$VERSION" ]; then
    echo "get-webhb: webhb $VERSION is in $DEST already"; exit 0
fi

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
if [ -n "${WEBHB_TARBALL:-}" ]; then
    cp "$WEBHB_TARBALL" "$TMP/webhb.tar.gz"
else
    [ -n "$URL" ] || { echo "get-webhb: $LOCK has no url= - set one, or WEBHB_TARBALL=<file>, or build with WEBHB_DIR=<checkout>" >&2; exit 1; }
    echo "get-webhb: fetching $URL"
    curl -fsSL -o "$TMP/webhb.tar.gz" "$URL"
fi

if command -v sha256sum >/dev/null 2>&1; then GOT="$(sha256sum "$TMP/webhb.tar.gz" | cut -d' ' -f1)"
else GOT="$(shasum -a 256 "$TMP/webhb.tar.gz" | cut -d' ' -f1)"; fi
[ "$GOT" = "$SHA" ] || { echo "get-webhb: checksum mismatch - expected $SHA, got $GOT" >&2; exit 1; }

mkdir -p "$TMP/x" && tar -xzf "$TMP/webhb.tar.gz" -C "$TMP/x"
ROOT="$(find "$TMP/x" -mindepth 1 -maxdepth 1 -type d | head -1)"
[ -f "$ROOT/webhb/webhb.mk" ] || { echo "get-webhb: this archive is not a webhb release" >&2; exit 1; }
rm -rf "$DEST"; mkdir -p "$(dirname "$DEST")"; mv "$ROOT" "$DEST"
echo "get-webhb: webhb $VERSION installed in $DEST"
