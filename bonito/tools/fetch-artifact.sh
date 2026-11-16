#!/bin/bash
# Resumable download of a GitHub Actions artifact (gh run download hung on 60 MB artifacts here).
#   fetch-artifact.sh OWNER/REPO RUN_ID ARTIFACT_NAME OUT_DIR
set -euo pipefail
REPO=$1 RUN=$2 NAME=$3 OUT=$4
mkdir -p "$OUT"
read -r ID SIZE < <(gh api "repos/$REPO/actions/runs/$RUN/artifacts" -q ".artifacts[] | select(.name==\"$NAME\") | \"\(.id) \(.size_in_bytes)\"")
URL=$(curl -s -o /dev/null -w '%{redirect_url}' -H "Authorization: Bearer $(gh auth token)" -H "Accept: application/vnd.github+json" "https://api.github.com/repos/$REPO/actions/artifacts/$ID/zip")
for try in $(seq 1 20); do
	curl -sS -L -C - --max-time 60 --speed-limit 20000 --speed-time 20 -o "$OUT/$NAME.zip" "$URL" 2>/dev/null || true
	[ "$(stat -c %s "$OUT/$NAME.zip" 2>/dev/null || echo 0)" = "$SIZE" ] && break
done
[ "$(stat -c %s "$OUT/$NAME.zip")" = "$SIZE" ] || { echo "download incomplete" >&2; exit 1; }
unzip -q -o "$OUT/$NAME.zip" -d "$OUT/$NAME"
echo "$OUT/$NAME"
