#!/bin/sh
# Lays out the site that the editor host serves (see README.md): index.html, source/, and v/<content hash>/ with the engine and bridge.js.
# config.js is not here: the server writes it from PARENT_ORIGINS.
#
# Usage: stage.sh <web build dir> <output dir> <commit> [repository URL]
set -eu

if [ $# -lt 3 ]; then
  echo "Usage: $0 <web build dir> <output dir> <commit> [repository URL]" >&2
  exit 2
fi
build=$1
out=$2
commit=$3
repo=${4:-https://github.com/Triad-Coding/LibreSprite}
here=$(cd "$(dirname "$0")" && pwd)

# The /source/ page must name the exact commit
if ! printf %s "$commit" | grep -Eq '^[0-9a-f]{40}$'; then
  echo "$0: <commit> must be a full git hash, not '$commit'" >&2
  exit 2
fi

files="$build/libresprite.js $build/libresprite.wasm $build/libresprite.data $here/bridge.js"
version=$(cat $files | sha256sum | cut -c1-12)

rm -rf "$out"
mkdir -p "$out/v/$version" "$out/source"
cp $files "$out/v/$version/"
sed "s/__VERSION__/$version/g" "$here/index.html" > "$out/index.html"
sed -e "s/__COMMIT__/$commit/g" -e "s|__REPO__|$repo|g" "$here/source.html" > "$out/source/index.html"
cp "$here/../../LICENSE.txt" "$out/source/LICENSE.txt"

# Pre-compressed copies for the server.
# Cloudflare does not compress the .data package (application/octet-stream).
for f in "$out/v/$version"/*; do
  gzip -9 -k -f "$f"
  if command -v brotli > /dev/null; then
    brotli -q 11 -f "$f"
  fi
done

echo "Staged $out (v/$version, commit $commit)"
