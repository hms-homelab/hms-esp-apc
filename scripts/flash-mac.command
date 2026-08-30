#!/bin/bash
#
# Flash hms-esp-apc onto an ESP32-S3 N8R8 board, from a Mac, with nothing
# installed beforehand.
#
# Double-click this file in Finder, or run it from a terminal:
#     bash scripts/flash-mac.command
#
# It installs what the build needs (Apple command line tools, then Espressif's
# ESP-IDF and its toolchain), builds the firmware for the N8R8 board profile,
# finds the board on USB, and flashes it. Safe to run again — every step checks
# whether it already did its work.
#
# First run downloads roughly 2GB and takes 15-30 minutes. Later runs take
# about a minute.

set -euo pipefail

IDF_VERSION="v5.3.4"          # what CI builds with; see .github/workflows
BUILD_DIR="build-n8r8"        # kept apart from build/, which may hold another
                              # board's configuration

# ── Presentation ──────────────────────────────────────────────────────────

if [ -t 1 ]; then
    BOLD=$'\033[1m'; DIM=$'\033[2m'; RED=$'\033[31m'
    GREEN=$'\033[32m'; YELLOW=$'\033[33m'; BLUE=$'\033[34m'; OFF=$'\033[0m'
else
    BOLD=""; DIM=""; RED=""; GREEN=""; YELLOW=""; BLUE=""; OFF=""
fi

step()  { printf '\n%s==> %s%s\n' "$BOLD$BLUE" "$*" "$OFF"; }
ok()    { printf '%s  ✓ %s%s\n'   "$GREEN" "$*" "$OFF"; }
note()  { printf '%s    %s%s\n'   "$DIM" "$*" "$OFF"; }
warn()  { printf '%s  ! %s%s\n'   "$YELLOW" "$*" "$OFF"; }

# Any unexpected failure lands here rather than scrolling a stack of shell
# errors past someone who has no way to read them.
die() {
    printf '\n%s  ✗ %s%s\n\n' "$RED$BOLD" "$1" "$OFF"
    [ $# -gt 1 ] && printf '%s%s%s\n\n' "$DIM" "$2" "$OFF"
    printf 'Nothing was written to the board. Send the last 20 lines above\n'
    printf 'to whoever gave you this and they will know what happened.\n\n'
    exit 1
}
trap 'die "Something went wrong on line $LINENO." "The command that failed is shown above."' ERR

# ── Locate the repository ─────────────────────────────────────────────────
# A double-clicked .command starts in the home directory, not next to itself,
# so the path has to come from the script rather than the working directory.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_DIR"

[ -f main/version.h ] || die "This script is not inside the hms-esp-apc project." \
    "Expected to find main/version.h next to it, in $REPO_DIR"

VERSION="$(sed -n 's/.*HMS_ESP_APC_VERSION "\([^"]*\)".*/\1/p' main/version.h)"

printf '\n%s  hms-esp-apc  v%s  —  ESP32-S3 N8R8 flasher%s\n' "$BOLD" "$VERSION" "$OFF"
printf '%s  %s%s\n' "$DIM" "$REPO_DIR" "$OFF"

[ "$(uname -s)" = "Darwin" ] || die "This script only runs on a Mac."

# ── 1. Apple command line tools ───────────────────────────────────────────

step "Checking Apple command line tools"
if xcode-select -p >/dev/null 2>&1; then
    ok "already installed"
else
    warn "not installed — a system window will open now"
    note "Click Install, wait for it to finish, then run this script again."
    xcode-select --install >/dev/null 2>&1 || true
    exit 0
fi

# ── 2. ESP-IDF ────────────────────────────────────────────────────────────
# Espressif's SDK. install.sh pulls a compiler, cmake and ninja into
# ~/.espressif, so nothing here depends on Homebrew being present.

step "Checking ESP-IDF $IDF_VERSION"

if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/export.sh" ]; then
    IDF_DIR="$IDF_PATH"                       # respect an existing install
    ok "using the one already set up at $IDF_DIR"
else
    IDF_DIR="$HOME/esp/esp-idf"
    if [ -f "$IDF_DIR/export.sh" ]; then
        ok "found at $IDF_DIR"
    else
        warn "not installed — downloading it now"
        note "About 1GB. This is the slow part; 10-20 minutes is normal."
        mkdir -p "$HOME/esp"
        git clone -b "$IDF_VERSION" --depth 1 --recursive \
            https://github.com/espressif/esp-idf.git "$IDF_DIR" \
            || die "Could not download ESP-IDF." "Check the internet connection and try again."
        ok "downloaded"
    fi
fi

step "Installing the ESP32-S3 compiler toolchain"
note "Skips anything already present, so this is quick on later runs."
"$IDF_DIR/install.sh" esp32s3 >/tmp/hms-esp-apc-install.log 2>&1 \
    || die "The toolchain installer failed." "Full output: /tmp/hms-esp-apc-install.log"
ok "toolchain ready"

step "Loading the build environment"
set +u                                        # export.sh reads unset variables
# shellcheck disable=SC1091
. "$IDF_DIR/export.sh" >/tmp/hms-esp-apc-export.log 2>&1 \
    || { set -u; die "Could not load the build environment." "Full output: /tmp/hms-esp-apc-export.log"; }
set -u
command -v idf.py >/dev/null 2>&1 || die "The build tools did not load correctly." \
    "Full output: /tmp/hms-esp-apc-export.log"
ok "ready"

# ── 3. Build ──────────────────────────────────────────────────────────────
# sdkconfig.n8r8.defaults layers the N8R8 board's status LED pin (GPIO48) over
# the repository defaults, which target a different board and would leave the
# LED dark. Both files are needed, in this order.

step "Building the firmware"
note "First build takes several minutes. Later builds are much faster."
idf.py -B "$BUILD_DIR" \
       -DSDKCONFIG="$BUILD_DIR/sdkconfig" \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.n8r8.defaults" \
       build >/tmp/hms-esp-apc-build.log 2>&1 \
    || die "The firmware did not build." "Full output: /tmp/hms-esp-apc-build.log"

APP_BIN="$BUILD_DIR/apc_usb_mqtt_bridge.bin"
[ -f "$APP_BIN" ] || die "The build finished but produced no firmware file." \
    "Full output: /tmp/hms-esp-apc-build.log"
ok "built v$VERSION ($(( $(stat -f%z "$APP_BIN") / 1024 )) KB)"

# ── 4. Find the board ─────────────────────────────────────────────────────
# Two boards can present the same /dev/cu.usbmodem* name, so the USB vendor is
# what actually identifies them. A CH343 or CP210x is the board's UART port,
# which is the one to flash: it is a separate chip, so it keeps working no
# matter what the firmware does. An Espressif vendor id is the chip's own USB
# port, which the firmware takes over for the UPS once it boots.

USB_TSV="$(mktemp)"
trap 'rm -f "$USB_TSV"' EXIT

ioreg -p IOUSB -w0 -l 2>/dev/null | awk '
    /"USB Vendor Name"/   { v=$0; sub(/.*= "/,"",v); sub(/"$/,"",v) }
    /"USB Product Name"/  { p=$0; sub(/.*= "/,"",p); sub(/"$/,"",p) }
    /"USB Serial Number"/ { s=$0; sub(/.*= "/,"",s); sub(/"$/,"",s)
                            if (s != "") print s "\t" v " " p }
' > "$USB_TSV" || true

describe_port() {
    local base="${1##*/}" ser lbl
    while IFS=$'\t' read -r ser lbl; do
        [ -n "$ser" ] || continue
        case "$base" in *"$ser"*) printf '%s' "$lbl"; return ;; esac
    done < "$USB_TSV"
    printf 'unrecognised device'
}

step "Looking for a board on USB"

PORTS=()
for p in /dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/cu.usbmodem* \
         /dev/cu.SLAB_USBtoUART*; do
    [ -e "$p" ] || continue                   # unmatched glob stays literal
    case "$p" in *Bluetooth*|*debug-console*) continue ;; esac
    PORTS+=("$p")
done

if [ ${#PORTS[@]} -eq 0 ]; then
    die "No board found on USB." \
"Plug the board into this Mac with a USB-C cable, then run this again.

Two things catch people out:
  • The board has TWO USB-C ports. Use the one labelled UART or COM.
  • Some USB-C cables only carry power. If nothing appears, try another cable."
fi

if [ ${#PORTS[@]} -eq 1 ]; then
    PORT="${PORTS[0]}"
    ok "found $PORT"
    note "$(describe_port "$PORT")"
else
    printf '\n  More than one device is connected. Which is the board?\n\n'
    i=1
    for p in "${PORTS[@]}"; do
        printf '    %s%d%s) %s\n       %s%s%s\n' \
               "$BOLD" "$i" "$OFF" "$p" "$DIM" "$(describe_port "$p")" "$OFF"
        i=$((i + 1))
    done
    printf '\n  Enter a number [1]: '
    read -r choice
    choice="${choice:-1}"
    case "$choice" in
        ''|*[!0-9]*) die "\"$choice\" is not one of the numbers listed." ;;
    esac
    [ "$choice" -ge 1 ] && [ "$choice" -le ${#PORTS[@]} ] \
        || die "\"$choice\" is not one of the numbers listed."
    PORT="${PORTS[$((choice - 1))]}"
    ok "using $PORT"
fi

# ── 5. Flash ──────────────────────────────────────────────────────────────

printf '\n  Is this board brand new, or has it run different firmware before?\n'
printf '  %sSaying yes erases everything on it first, including any saved WiFi.%s\n' "$DIM" "$OFF"
printf '  Erase first? [y/N]: '
read -r do_erase

if [ "${do_erase:-n}" = "y" ] || [ "${do_erase:-n}" = "Y" ]; then
    step "Erasing the board"
    idf.py -B "$BUILD_DIR" -p "$PORT" erase-flash >/tmp/hms-esp-apc-erase.log 2>&1 \
        || die "Could not erase the board." \
"Full output: /tmp/hms-esp-apc-erase.log

If it says the port is busy, close anything else using the board
(Arduino IDE, a serial monitor) and try again."
    ok "erased"
fi

step "Flashing v$VERSION — do not unplug the board"
idf.py -B "$BUILD_DIR" \
       -DSDKCONFIG="$BUILD_DIR/sdkconfig" \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.n8r8.defaults" \
       -p "$PORT" flash >/tmp/hms-esp-apc-flash.log 2>&1 \
    || die "Flashing failed." \
"Full output: /tmp/hms-esp-apc-flash.log

Most common causes:
  • The cable is in the board's other USB-C port. Use the UART/COM one.
  • Something else is holding the port. Close it and try again.
  • The cable carries power but not data. Try another cable."
ok "flashed"

# ── 6. What happens next ──────────────────────────────────────────────────

cat <<EOF

${GREEN}${BOLD}  Done. The board is running v$VERSION.${OFF}

  ${BOLD}Setting it up${OFF}
  The board waits 10 seconds, then creates its own WiFi network called
  ${BOLD}APC-XXXX${OFF} (four characters from its address).

    1. On a phone or laptop, join the ${BOLD}APC-XXXX${OFF} network. It has no password.
    2. A setup page should open by itself. If it does not, visit
       ${BOLD}http://192.168.4.1/${OFF}
    3. Pick your WiFi network from the list, enter its password and your
       MQTT broker, then Save. The board restarts and joins your network.

  After that it appears in Home Assistant on its own, and its own page shows
  the UPS readings.

  ${BOLD}Plugging in the UPS${OFF}
  Use the board's ${BOLD}other${OFF} USB-C port, the one you did not flash through.

EOF

printf '  Watch the board start up now? [y/N]: '
read -r do_monitor
if [ "${do_monitor:-n}" = "y" ] || [ "${do_monitor:-n}" = "Y" ]; then
    printf '\n%s  Press Control-] to stop watching.%s\n\n' "$DIM" "$OFF"
    trap - ERR                                # Control-] is a normal exit here
    idf.py -B "$BUILD_DIR" -p "$PORT" monitor || true
fi
