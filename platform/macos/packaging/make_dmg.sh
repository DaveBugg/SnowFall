#!/bin/sh
# Builds a compressed disk image with SnowFall.app and an /Applications shortcut.
#   make_dmg.sh <path/SnowFall.app> <out.dmg>
set -eu
APP="$1"
OUT="$2"
STAGE="$(mktemp -d /tmp/snowfall-dmg.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT
ditto "$APP" "$STAGE/SnowFall.app"
ln -s /Applications "$STAGE/Applications"
rm -f "$OUT"
# hdiutil occasionally fails with "Resource busy" on CI machines: retry a few times.
n=0
until hdiutil create -quiet -volname "SnowFall" -srcfolder "$STAGE" -ov -fs HFS+ -format UDZO "$OUT"; do
  n=$((n + 1))
  if [ "$n" -ge 4 ]; then
    echo "make_dmg: hdiutil failed" >&2
    exit 1
  fi
  sleep 3
done
echo "make_dmg: $OUT"
