#!/bin/bash
# Rufus for Mac: builds the installer DMG, a window with Rufus.app next to an
# Applications shortcut (drag to install), a background and the Rufus volume icon.
#
#   tools/make-dmg.sh <Rufus.app> <output.dmg> <version>
#
# The window layout is set through Finder. If Finder can't be scripted (no GUI
# session, or automation not allowed), the DMG is still created, just with
# Finder's default layout.
set -euo pipefail

APP="$1"; OUT="$2"; VERSION="$3"
VOLNAME="Rufus $VERSION"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d /tmp/rufus-dmg.XXXXXX)"
trap 'hdiutil detach -quiet "/Volumes/$VOLNAME" 2>/dev/null || true; rm -rf "$WORK"' EXIT

# Staging folder
mkdir -p "$WORK/stage/.background"
ditto "$APP" "$WORK/stage/Rufus.app"
ln -s /Applications "$WORK/stage/Applications"
cp "$HERE/app/dmg-background.tiff" "$WORK/stage/.background/background.tiff"
cp "$HERE/app/Rufus.icns" "$WORK/stage/.VolumeIcon.icns"

# Writable image, with some room for Finder's metadata
hdiutil detach -quiet "/Volumes/$VOLNAME" 2>/dev/null || true
hdiutil create -quiet -volname "$VOLNAME" -srcfolder "$WORK/stage" -fs HFS+ -format UDRW -ov "$WORK/rw.dmg"
DEV=$(hdiutil attach -readwrite -noverify -noautoopen "$WORK/rw.dmg" | awk '/Apple_HFS/{print $1}')

# Custom volume icon (SetFile comes with the Command Line Tools)
if command -v SetFile >/dev/null; then
	SetFile -a C "/Volumes/$VOLNAME" || true
fi

# Window layout: 640x400, icons on either side of the background's arrow
# (perl's alarm: never let a Finder/automation prompt hang the build, e.g. on CI)
if perl -e 'alarm 60; exec @ARGV' osascript <<OSA >/dev/null 2>&1
tell application "Finder"
	tell disk "$VOLNAME"
		open
		set current view of container window to icon view
		set toolbar visible of container window to false
		set statusbar visible of container window to false
		set the bounds of container window to {200, 120, 840, 548}
		set opts to the icon view options of container window
		set arrangement of opts to not arranged
		set icon size of opts to 128
		set text size of opts to 13
		set background picture of opts to file ".background:background.tiff"
		set position of item "Rufus.app" of container window to {170, 190}
		set position of item "Applications" of container window to {470, 190}
		update without registering applications
		delay 1
		close
	end tell
end tell
OSA
then
	echo "DMG window layout applied"
else
	echo "warning: could not script Finder, the DMG will use the default layout" >&2
fi

sync
hdiutil detach -quiet "$DEV" || hdiutil detach -force -quiet "$DEV"
rm -f "$OUT"
hdiutil convert -quiet "$WORK/rw.dmg" -format UDZO -imagekey zlib-level=9 -o "$OUT"
echo "Created $OUT"
