#!/bin/bash
# TEMPORARY launcher: copy the BoundingPathData bundle out of the iOS root image (read-only). No arguments needed.
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || exec sudo bash "$0" "$@"
exec bash bonito/tools/copy-from-root.sh /System/Library/PrivateFrameworks/UIKitCore.framework/BoundingPathData.bundle
