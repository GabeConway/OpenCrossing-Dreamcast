#!/usr/bin/env bash
#
# install-flycast.sh -- idempotent Flycast installer for the DC harness (macOS).
#
#   ./install-flycast.sh            # install if missing, else verify and exit 0
#   ./install-flycast.sh --check    # verify only, never install
#   ./install-flycast.sh --force    # reinstall even if already present
#
# Emits JSON on stdout. Exit 0 = a usable Flycast is on this machine.
#
# Facts this encodes (harness/dc/README.md, all verified on this host):
#   * The cask installs /Applications/Flycast.app (v2.6, bundle 392a429e8,
#     universal x86_64+arm64, ad-hoc signed with NO Team ID).
#   * Because it is ad-hoc signed the com.apple.quarantine xattr MUST be
#     stripped or every launch is blocked by Gatekeeper. Done once, not per-run.
#   * No BIOS ROM (dc_boot.bin) is needed; Flycast falls back to its HLE BIOS
#     (reios), which is also what makes direct .elf boot work.
#   * The cask is DEPRECATED in Homebrew ("does not pass the macOS Gatekeeper
#     check") and is scheduled for disable on 2026-09-01. After that date this
#     script's brew path stops working -- keep a copy of the .app or the release
#     DMG, or point OC_DC_FLYCAST at a hand-installed binary.
#
# Env:
#   OC_DC_FLYCAST   override the binary path (default below)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

APP="/Applications/Flycast.app"
BIN="${OC_DC_FLYCAST:-$APP/Contents/MacOS/Flycast}"
CASK_DISABLE_DATE="2026-09-01"

MODE=install
while [ $# -gt 0 ]; do
    case "$1" in
        --check) MODE=check; shift ;;
        --force) MODE=force; shift ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

json_out() {  # status action version bundle quarantine note
    printf '{\n'
    printf '  "harness": "install-flycast",\n'
    printf '  "repo": "%s",\n'          "$REPO"
    printf '  "flycast": "%s",\n'       "$BIN"
    printf '  "installed": %s,\n'       "$1"
    printf '  "action": "%s",\n'        "$2"
    printf '  "version": "%s",\n'       "$3"
    printf '  "bundle_version": "%s",\n' "$4"
    printf '  "quarantined": %s,\n'     "$5"
    printf '  "cask_disable_date": "%s",\n' "$CASK_DISABLE_DATE"
    printf '  "note": "%s"\n'           "$6"
    printf '}\n'
}

plist_get() {  # key
    /usr/libexec/PlistBuddy -c "Print :$1" "$APP/Contents/Info.plist" 2>/dev/null || echo "unknown"
}

strip_quarantine() {
    [ -d "$APP" ] || return 0
    if xattr -p com.apple.quarantine "$APP" >/dev/null 2>&1; then
        xattr -dr com.apple.quarantine "$APP"
    fi
}

is_quarantined() {
    [ -d "$APP" ] && xattr -p com.apple.quarantine "$APP" >/dev/null 2>&1
}

# ---------------------------------------------------------------- already here
if [ -x "$BIN" ] && [ "$MODE" != force ]; then
    strip_quarantine
    q=false; is_quarantined && q=true
    json_out true already-present "$(plist_get CFBundleShortVersionString)" \
             "$(plist_get CFBundleVersion)" "$q" \
             "already installed; quarantine xattr verified clear"
    exit 0
fi

if [ "$MODE" = check ]; then
    json_out false missing unknown unknown false \
             "not installed; rerun without --check to install"
    exit 1
fi

# ------------------------------------------------------------------- installing
if ! command -v brew >/dev/null 2>&1; then
    json_out false failed unknown unknown false \
             "Homebrew not found; install Flycast manually into $APP"
    exit 1
fi

if [ "$MODE" = force ]; then
    brew reinstall --cask flycast >&2 || true
else
    brew install --cask flycast >&2 || true
fi

if [ ! -x "$BIN" ]; then
    json_out false failed unknown unknown false \
             "brew ran but $BIN is still missing (cask disabled after $CASK_DISABLE_DATE?)"
    exit 1
fi

# REQUIRED: ad-hoc signed app, Gatekeeper blocks every launch without this.
strip_quarantine
q=false; is_quarantined && q=true

json_out true installed "$(plist_get CFBundleShortVersionString)" \
         "$(plist_get CFBundleVersion)" "$q" "installed and de-quarantined"
[ "$q" = false ]
