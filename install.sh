#!/usr/bin/env bash
# tw-safe-update installer (openSUSE Tumbleweed, KDE Plasma 6).
#
# Installs, per-user:
#   * the checker/updater scripts        -> ~/.local/bin
#   * a compiled tray daemon (SNI icon)  -> ~/.local/bin/twsu-tray
#   * two systemd user units             -> background check timer + tray service
# and one narrow, read-only sudoers rule (the only step that needs root).
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
BIN="$HOME/.local/bin"
UNIT_DIR="$HOME/.config/systemd/user"
CONF_DIR="$HOME/.config/tw-safe-update"
SUDOERS="/etc/sudoers.d/50-tw-safe-update"

say()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m warning:\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m error:\033[0m %s\n' "$*" >&2; exit 1; }

[ -r /etc/os-release ] && grep -qi tumbleweed /etc/os-release \
    || warn "This does not look like openSUSE Tumbleweed — continuing anyway."
command -v zypper >/dev/null || die "zypper not found."

# Use the real login name (not the spoofable $USER) and validate it before it
# is ever written into a root-owned sudoers file.
USERNAME="$(id -un)"
[[ "$USERNAME" =~ ^[a-z_][a-z0-9_-]*$ ]] \
    || die "Refusing to continue: unexpected username '$USERNAME' (would be written into a sudoers rule)."

# 1. scripts -----------------------------------------------------------------
say "Installing scripts to $BIN"
mkdir -p "$BIN"
install -m 0755 "$SRC/bin/twsu-check"   "$BIN/twsu-check"
install -m 0755 "$SRC/bin/twsu-update"  "$BIN/twsu-update"
install -m 0755 "$SRC/bin/twsu-details" "$BIN/twsu-details"
case ":$PATH:" in *":$BIN:"*) : ;; *) warn "$BIN is not in PATH (fine for the tray, add it for CLI use)." ;; esac

# 2. build + install the tray daemon ----------------------------------------
# Build deps: cmake, g++, Qt6 Widgets/DBus, and KF6 StatusNotifierItem (the tray
# uses KStatusNotifierItem so a left-click opens the menu on Wayland). Install
# whatever is missing up front (one sudo prompt).
need=()
command -v cmake >/dev/null || need+=(cmake)
command -v g++ >/dev/null   || need+=(gcc-c++)
[ -d /usr/lib64/cmake/KF6StatusNotifierItem ] || need+=(kf6-kstatusnotifieritem-devel)
[ -d /usr/lib64/cmake/KF6Parts ]              || need+=(kf6-kparts-devel)
[ -d /usr/lib64/cmake/Qt6DBus ]               || need+=(qt6-dbus-devel)
[ -d /usr/lib64/cmake/Qt6Widgets ]            || need+=(qt6-widgets-devel)
if [ ${#need[@]} -gt 0 ]; then
    say "Installing tray build dependencies: ${need[*]} (needs your password)"
    sudo zypper --non-interactive install --no-recommends "${need[@]}" \
        || warn "Could not install some build deps — the tray build may fail."
fi

if command -v cmake >/dev/null && command -v g++ >/dev/null; then
    say "Building the tray daemon"
    build="$SRC/tray/build"
    ( cmake -S "$SRC/tray" -B "$build" -DCMAKE_BUILD_TYPE=Release >/dev/null \
      && cmake --build "$build" --parallel >/dev/null ) \
      && install -m 0755 "$build/twsu-tray" "$BIN/twsu-tray" \
      && say "Tray daemon installed." \
      || { warn "Tray build failed — skipping the tray (scripts still work)."; }
else
    warn "cmake/g++ not found — skipping the tray daemon. Install them and re-run."
fi

# 3. default tray config -----------------------------------------------------
mkdir -p "$CONF_DIR"
if [ ! -f "$CONF_DIR/tray.conf" ]; then
    cat > "$CONF_DIR/tray.conf" <<'EOF'
# TW Safe Update tray config
# terminal used for the interactive update/details (must accept "-e <cmd>")
terminal=konsole
# when to raise a notification: safe | review | any
# (name-collisions always notify once, regardless of this setting)
notify=safe
# tray icon name; update-low is the standard "updates ready" panel glyph.
# alternatives: update-none, software-update-available, system-software-update
icon=update-low
# when the tray icon is visible: safe (only when a safe update is ready) | always
show=safe
EOF
fi

# 3b. launcher entry, icon, metadata, man pages ------------------------------
APPS="$HOME/.local/share/applications"
mkdir -p "$APPS"
sed -e "s#^Exec=twsu-tray#Exec=$BIN/twsu-tray#" \
    -e "s#^TryExec=twsu-tray#TryExec=$BIN/twsu-tray#" \
    "$SRC/share/org.opensuse.twsafeupdate.desktop" > "$APPS/org.opensuse.twsafeupdate.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" 2>/dev/null || true
install -Dm 0644 "$SRC/share/icons/org.opensuse.twsafeupdate.svg" \
    "$HOME/.local/share/icons/hicolor/scalable/apps/org.opensuse.twsafeupdate.svg"
install -Dm 0644 "$SRC/share/org.opensuse.twsafeupdate.metainfo.xml" \
    "$HOME/.local/share/metainfo/org.opensuse.twsafeupdate.metainfo.xml"
for m in "$SRC"/man/*.1; do
    install -Dm 0644 "$m" "$HOME/.local/share/man/man1/$(basename "$m")"
done
say "Launcher entry, icon, and man pages installed (search “TW Update Assistant”)."

# 4. sudoers rule (needs root) ----------------------------------------------
say "Installing read-only sudoers rule at $SUDOERS (requires your password)"
tmp="$(mktemp)"
sed "s/__USER__/$USERNAME/g" "$SRC/sudoers/50-tw-safe-update.in" > "$tmp"
if sudo install -m 0440 -o root -g root "$tmp" "$SUDOERS" \
   && sudo visudo -cf "$SUDOERS" >/dev/null; then
    say "sudoers rule installed and validated."
else
    rm -f "$tmp"; sudo rm -f "$SUDOERS" 2>/dev/null || true
    die "Failed to install/validate sudoers rule."
fi
rm -f "$tmp"

# 5. systemd user units ------------------------------------------------------
say "Installing systemd user units (check timer + tray service)"
mkdir -p "$UNIT_DIR"
# Guarded so a non-systemd-user session can't abort the installer after the
# sudoers rule is already in place (set -e is active).
install -m 0644 "$SRC/systemd/tw-safe-update.service"      "$UNIT_DIR/" || warn "unit install failed"
install -m 0644 "$SRC/systemd/tw-safe-update.timer"        "$UNIT_DIR/" || warn "unit install failed"
install -m 0644 "$SRC/systemd/tw-safe-update-tray.service" "$UNIT_DIR/" || warn "unit install failed"
systemctl --user daemon-reload 2>/dev/null || warn "systemctl --user daemon-reload failed (no user session?)."
systemctl --user enable --now tw-safe-update.timer >/dev/null 2>&1 \
    && say "Background check timer enabled (every ~3h)." || warn "Could not enable the timer."
if [ -x "$BIN/twsu-tray" ]; then
    systemctl --user enable --now tw-safe-update-tray.service >/dev/null 2>&1 \
        && say "Tray service enabled and started." || warn "Could not start the tray service."
fi

# 6. retire the old plasmoid, if a previous version installed it ------------
if command -v kpackagetool6 >/dev/null \
   && kpackagetool6 --type Plasma/Applet --list 2>/dev/null | grep -qx org.opensuse.twsafeupdate; then
    kpackagetool6 --type Plasma/Applet --remove org.opensuse.twsafeupdate >/dev/null 2>&1 \
        && say "Removed the old plasmoid widget (replaced by the tray service)."
fi

# 7. first check -------------------------------------------------------------
say "Running an initial check…"
"$BIN/twsu-check" --pretty || warn "Initial check reported an issue (see above)."

cat <<EOF

$(say "Done — look for the update icon in your system tray.")
Left-click the tray icon for the menu (Check now / Update now / Details).
It notifies you when an update is safe, and warns on package-name collisions.

Manual check:  twsu-check --pretty
Update now:    twsu-update              (asks for your password)
Tray service:  systemctl --user status tw-safe-update-tray.service
EOF
