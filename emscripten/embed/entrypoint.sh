#!/bin/sh
# Writes config.js and the frame-ancestors header from PARENT_ORIGINS, the comma-separated origins of the pages that embed the editor.
# Then starts Static Web Server on PORT (Railway sets it; 8602 otherwise).
set -euf

: "${PARENT_ORIGINS:?Set PARENT_ORIGINS to the lesson page origins, comma-separated}"

json=""
ancestors=""
for origin in $(printf %s "$PARENT_ORIGINS" | tr ',' ' '); do
  origin=${origin%/}
  if ! printf %s "$origin" | grep -Eq '^https?://[A-Za-z0-9.-]+(:[0-9]+)?$'; then
    echo "PARENT_ORIGINS: '$origin' is not an origin" >&2
    exit 1
  fi
  json="$json${json:+, }\"$origin\""
  ancestors="$ancestors${ancestors:+ }$origin"
done
if [ -z "$json" ]; then
  echo "PARENT_ORIGINS is empty" >&2
  exit 1
fi

printf 'self.ART_EDITOR_CONFIG = {"parentOrigins": [%s]};\n' "$json" > /public/config.js
sed -e "s|__PORT__|${PORT:-8602}|" -e "s|__FRAME_ANCESTORS__|$ancestors|" \
  /etc/sws/sws.toml.template > /tmp/sws.toml

exec static-web-server --config-file /tmp/sws.toml
