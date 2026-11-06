#!/usr/bin/env bash
# Makes "PGR2 Test.app" in ~/Applications, for the Dock. Opening it rebuilds what changed and starts the latest game, with
# its saves, settings and shader cache in build/macapp. If the build fails, the last good one starts.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
CMAKE=$(command -v cmake)
APP="$HOME/Applications/PGR2 Test.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cat > "$APP/Contents/MacOS/launch" <<SCRIPT
#!/bin/bash
cd "$REPO"
if ! "$CMAKE" --build build/macapp --target PGR2 > build/macapp/launch-build.log 2>&1 && [ -x build/macapp/PGR2 ]; then
    /usr/bin/osascript -e 'display notification "The build failed, starting the last good one. See build/macapp/launch-build.log" with title "PGR2"'
fi
exec build/macapp/PGR2
SCRIPT
chmod +x "$APP/Contents/MacOS/launch"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>PGR2 Test</string>
<key>CFBundleIdentifier</key><string>local.pgr2.test</string>
<key>CFBundleExecutable</key><string>launch</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleIconFile</key><string>icon</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
# the launcher's icon, its largest picture
TMP=$(mktemp -d)
if sips -s format png "$REPO/launcher/icon.ico" --out "$TMP/icon.png" >/dev/null 2>&1; then
    mkdir "$TMP/icon.iconset"
    for size in 16 32 128 256 512; do
        sips -z $size $size "$TMP/icon.png" --out "$TMP/icon.iconset/icon_${size}x${size}.png" >/dev/null
    done
    iconutil -c icns "$TMP/icon.iconset" -o "$APP/Contents/Resources/icon.icns" || true
fi
rm -rf "$TMP"
echo "$APP"
