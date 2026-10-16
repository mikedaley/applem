#!/bin/bash
# Build the native ApplEm (ImGui + Metal) for macOS, signed with Developer ID,
# notarised and stapled: an .app and a .dmg that open on any Mac without a
# Gatekeeper warning.
#
# Prerequisites:
#   1. A "Developer ID Application" certificate in the login keychain.
#   2. A notarytool credential profile for the Apple ID; NOTARY_PROFILE names
#      it, and a profile another app already stored for the same Apple ID is
#      used when there is no "applem-notary".
#   3. Sparkle's EdDSA private key in the login keychain (made once with the
#      build's sparkle-*/bin/generate_keys; its public half is SUPublicEDKey in
#      native/resources/Info.plist.in). Export a copy with `generate_keys -x`
#      and keep it safe: without it no update can be signed for the copies
#      already out there.
#
# A notarised build ends with appcast.xml beside the .dmg: the one release
# Sparkle offers, signed with that key. Both go in the site's downloads/
# (RETROTECH71_DIR, ../retrotech71 by default, is given copies). The notes
# are RELEASE_NOTES (an HTML file) if set, or the commits since the last
# "Release" commit.
#
# Usage:
#   scripts/build-native-mac.sh                 # build, sign, notarise, staple
#   NOTARIZE=0 scripts/build-native-mac.sh      # build and sign only
#   NOTARY_PROFILE=other scripts/build-native-mac.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUILD="build-macos-release"
OUT="$BUILD/dist"
NOTARIZE="${NOTARIZE:-1}"
NOTARY_PROFILE="${NOTARY_PROFILE:-applem-notary}"
if [ "$NOTARIZE" = "1" ] && ! xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null 2>&1; then
  for candidate in markwell-notary limelight-notary windowsizer-notary; do
    if xcrun notarytool history --keychain-profile "$candidate" >/dev/null 2>&1; then
      echo "> No '$NOTARY_PROFILE' profile; using '$candidate' (same Apple ID)"
      NOTARY_PROFILE="$candidate"
      break
    fi
  done
fi

# The Developer ID identity, found rather than written down, so the repository
# names no certificate. APPLE_SIGNING_IDENTITY overrides it.
IDENTITY="${APPLE_SIGNING_IDENTITY:-$(security find-identity -v -p codesigning \
  | grep "Developer ID Application" | head -1 | sed -E 's/.*"(.*)"/\1/')}"
if [ -z "$IDENTITY" ]; then
  echo "x No 'Developer ID Application' certificate in the keychain." >&2
  exit 1
fi
echo "> Signing identity: $IDENTITY"

# The version the release process bumps.
VERSION="$(head -n 1 VERSION | tr -d '[:space:]')"
if [ -z "$VERSION" ]; then
  echo "x No version in VERSION" >&2
  exit 1
fi
echo "> Version: $VERSION"

# A Release build in its own directory, so a release never picks up a
# development build's settings and the development build is left alone.
echo "> Building..."
git submodule update --init
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" --target ApplEmNative -j 4

APP="$BUILD/native/ApplEm.app"
PLIST="$APP/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$PLIST"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $VERSION" "$PLIST"

# The hardened runtime and a secure timestamp, which notarisation requires.
# Nothing here needs an entitlement: no JIT, no microphone, no sandbox.
# Sparkle's helpers first, from the inside out, as Sparkle's documentation
# orders them; the Downloader keeps the entitlements it ships with.
echo "> Signing Sparkle..."
SPARKLE="$APP/Contents/Frameworks/Sparkle.framework"
SIGN=(codesign --force --options runtime --timestamp --sign "$IDENTITY")
"${SIGN[@]}" "$SPARKLE/Versions/B/XPCServices/Installer.xpc"
"${SIGN[@]}" --preserve-metadata=entitlements "$SPARKLE/Versions/B/XPCServices/Downloader.xpc"
"${SIGN[@]}" "$SPARKLE/Versions/B/Autoupdate"
"${SIGN[@]}" "$SPARKLE/Versions/B/Updater.app"
"${SIGN[@]}" "$SPARKLE"

echo "> Signing the app..."
codesign --force --options runtime --timestamp --sign "$IDENTITY" "$APP"
codesign --verify --strict --verbose=2 "$APP"
codesign --display --verbose=2 "$APP" 2>&1 | grep -E "Authority=Developer|TeamIdentifier|Runtime" || true

# The disk image: the app and a link to Applications, to drag it onto.
echo "> Making the disk image..."
mkdir -p "$OUT"
DMG="$OUT/ApplEm-Native-$VERSION.dmg"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
ditto "$APP" "$STAGE/ApplEm.app"
ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
hdiutil create -volname "ApplEm $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
codesign --force --timestamp --sign "$IDENTITY" "$DMG"

if [ "$NOTARIZE" != "1" ]; then
  echo "> Signed, not notarised (NOTARIZE=0):"
  echo "  $APP"
  echo "  $DMG"
  exit 0
fi

# The .dmg is what is distributed and it carries the signed app, so it is what
# goes to Apple; the ticket then covers the app inside it as well.
echo "> Notarising $DMG with profile '$NOTARY_PROFILE'..."
xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait

echo "> Stapling..."
xcrun stapler staple "$DMG"
xcrun stapler staple "$APP"
xcrun stapler validate "$DMG"
xcrun stapler validate "$APP"

echo "> Gatekeeper:"
spctl --assess --type execute --verbose "$APP"
spctl --assess --type open --context context:primary-signature --verbose "$DMG"

# The appcast: this release alone, which is all Sparkle needs to offer it.
# sign_update reads the private key from the keychain and prints the
# enclosure's signature and length.
echo "> Making the appcast..."
SPARKLE_BIN="$(ls -d "$BUILD"/sparkle-*/bin | tail -1)"
DOWNLOADS="https://retrotech71.co.uk/downloads"
SIGNATURE="$("$SPARKLE_BIN/sign_update" "$DMG")"
MINIMUM="$(/usr/libexec/PlistBuddy -c "Print :LSMinimumSystemVersion" "$PLIST")"
if [ -n "${RELEASE_NOTES:-}" ]; then
  NOTES="$(cat "$RELEASE_NOTES")"
else
  # The last "Release" commit for another version: this one's own commit,
  # whether made yet or not, is not where its notes start.
  LAST_RELEASE="$(git log --format='%H %s' | awk -v this="Release $VERSION" \
    '!found && $2 == "Release" && substr($0, 42) != this { print $1; found = 1 }')"
  NOTES="<ul>$(git log --format='%s' --no-merges "${LAST_RELEASE:+$LAST_RELEASE..}HEAD" \
    | { grep -v '^Release ' || true; } | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g' \
    -e 's#.*#<li>&</li>#' | tr -d '\n')</ul>"
fi
APPCAST="$OUT/appcast.xml"
cat > "$APPCAST" <<APPCAST
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>ApplEm</title>
    <link>$DOWNLOADS/appcast.xml</link>
    <item>
      <title>ApplEm $VERSION</title>
      <pubDate>$(LC_ALL=C date -u "+%a, %d %b %Y %H:%M:%S +0000")</pubDate>
      <sparkle:version>$VERSION</sparkle:version>
      <sparkle:shortVersionString>$VERSION</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>$MINIMUM</sparkle:minimumSystemVersion>
      <description><![CDATA[<h2>ApplEm $VERSION</h2>$NOTES]]></description>
      <enclosure url="$DOWNLOADS/$(basename "$DMG")" type="application/octet-stream" $SIGNATURE />
    </item>
  </channel>
</rss>
APPCAST
xmllint --noout "$APPCAST"

SITE="${RETROTECH71_DIR:-$ROOT/../retrotech71}"
if [ -d "$SITE/downloads" ]; then
  cp "$DMG" "$APPCAST" "$SITE/downloads/"
  echo "> Copied the disk image and the appcast to $SITE/downloads"
  echo "  (applem.html's download links still name the old version; deploy the site to publish)"
fi

echo "> Done:"
echo "  $APP"
echo "  $DMG"
echo "  $APPCAST"
