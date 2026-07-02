#!/usr/bin/env bash
# Remove everything install.sh set up.
set -u

SRC="$(cd "$(dirname "$0")" && pwd)"
BIN="$HOME/.local/bin"
UNIT_DIR="$HOME/.config/systemd/user"
SUDOERS="/etc/sudoers.d/50-tw-safe-update"

say() { printf '\033[1;32m==>\033[0m %s\n' "$*"; }

say "Stopping and removing systemd user units"
systemctl --user disable --now tw-safe-update-tray.service 2>/dev/null || true
systemctl --user disable --now tw-safe-update.timer 2>/dev/null || true
rm -f "$UNIT_DIR/tw-safe-update-tray.service" \
      "$UNIT_DIR/tw-safe-update.timer" "$UNIT_DIR/tw-safe-update.service"
systemctl --user daemon-reload 2>/dev/null || true
pkill -x twsu-tray 2>/dev/null || true

say "Removing the old plasmoid, if present"
command -v kpackagetool6 >/dev/null && \
    kpackagetool6 --type Plasma/Applet --remove org.opensuse.twsafeupdate >/dev/null 2>&1 || true

say "Removing scripts, tray binary, launcher entry and notification config"
rm -f "$BIN/twsu-check" "$BIN/twsu-update" "$BIN/twsu-details" "$BIN/twsu-tray"
rm -f "$HOME/.local/share/applications/org.opensuse.twsafeupdate.desktop"
rm -f "$HOME/.local/share/knotifications6/twsafeupdate.notifyrc"
command -v update-desktop-database >/dev/null && \
    update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true

say "Removing the compiled tray build tree"
rm -rf "$SRC/tray/build"

if [ -e "$SUDOERS" ] || sudo -n test -e "$SUDOERS" 2>/dev/null; then
    say "Removing the sudoers rule (requires your password)"
    sudo rm -f "$SUDOERS" || say "  (sudoers rule not removed)"
else
    say "No sudoers rule to remove"
fi

say "Removing cached status and config"
rm -rf "$HOME/.cache/tw-safe-update" "$HOME/.config/tw-safe-update"

echo
say "Done."
echo "Note: build dependencies the installer may have added (cmake, gcc-c++,"
echo "      kf6-*-devel, qt6-*-devel) were NOT removed. Remove them yourself if"
echo "      unwanted, e.g.:  sudo zypper rm --clean-deps kf6-kstatusnotifieritem-devel kf6-kparts-devel"
