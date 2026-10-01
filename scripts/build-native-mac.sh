#!/bin/bash
# Build the native ApplEm (ImGui + Metal) for macOS, signed with Developer ID,
# notarised and stapled: an .app and a .dmg that open on any Mac without a
# Gatekeeper warning.
#
# The same prerequisites as scripts/build-desktop-mac.sh, the Tauri build's:
#   1. A "Developer ID Application" certificate in the login keychain.
#   2. A notarytool credential profile for the Apple ID; NOTARY_PROFILE names
#      it, and a profile another app already stored for the same Apple ID is
#      used when there is no "applem-notary".
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

# One version for both builds: the one the release process bumps.
VERSION="$(sed -nE 's/.*VERSION = "([^"]+)".*/\1/p' src/js/config/version.js)"
if [ -z "$VERSION" ]; then
  echo "x No VERSION in src/js/config/version.js" >&2
  exit 1
fi
echo "> Version: $VERSION"

# A Release build in its own directory, so a release never picks up a
# development build's settings and the development build is left alone.
echo "> Building..."
git submodule update --init native/third_party/imgui
cmake -S . -B "$BUILD" -DA2E_BUILD_NATIVE=ON -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" --target ApplEmNative -j "$(sysctl -n hw.ncpu)"

APP="$BUILD/native/ApplEm.app"
PLIST="$APP/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$PLIST"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $VERSION" "$PLIST"

# The hardened runtime and a secure timestamp, which notarisation requires.
# Nothing here needs an entitlement: no JIT, no microphone, no sandbox.
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

echo "> Done:"
echo "  $APP"
echo "  $DMG"
