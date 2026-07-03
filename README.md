# TW Safe Update

[![build](https://github.com/Beary-Handsome/tw-safe-update/actions/workflows/build.yml/badge.svg)](https://github.com/Beary-Handsome/tw-safe-update/actions/workflows/build.yml)

A **background system-tray service** for **openSUSE Tumbleweed** (KDE Plasma 6)
that tells you when it is *safe* to run `sudo zypper dup` — and lets you do it
right from the tray. The app shows in your launcher and tray as
**“TW Update Assistant”**.

Tumbleweed is a rolling release: most days `zypper dup` is fine, but sometimes a
fresh snapshot lands **before Packman rebuilt the multimedia stack**, a
third-party package **collides with a newly-introduced official one**, or the
solver simply can't resolve cleanly. Updating then can strip your codecs, swap a
package for an unrelated one that happens to share its name, or leave a
half-upgraded system. This tool watches for exactly those signals and only shows
green when the transaction resolves **cleanly**.

## What it does

1. A **systemd user timer** runs, read-only, every ~3h:
   `zypper refresh` + `zypper dist-upgrade --dry-run`.
2. It parses the proposed transaction and classifies it:

   | Verdict | Meaning |
   |---|---|
   | 🟢 **SAFE** | Clean resolution — no conflicts, no vendor changes, no downgrades, no removals, nothing held back. |
   | 🟢 **UP TO DATE** | Nothing to do. |
   | 🟡 **CAUTION** | Resolvable, but review: held-back packages (Packman lagging), downgrades, non-critical removals, a same-project vendor adoption. |
   | 🔴 **UNSAFE** | Dependency problems, a package **leaving Packman** (codec breakage), a **critical package removal** (kernel/Mesa/systemd/ffmpeg/pipewire…), or a **name collision** (below). |

3. **Smart tray icon.** By default the icon only appears in the tray **when a
   safe update is ready** (set `show=always` to pin it). When the state becomes
   notify-worthy you also get a desktop notification.
   - **Left-click** the icon (or launch *TW Update Assistant*) → a window with
     the verdict, the reasons, any collisions, and buttons.
   - **Update now / Details** run in a **Konsole terminal embedded in that
     window** — the real `zypper dup` asks for your password right there.
   - **Right-click** → a quick menu (Check now / Update now / Details / Quit).

### Smart name-collision detection

The dangerous case that a plain `zypper dup` hides: two *unrelated* projects that
share a package name. Real example:

- installed `polaris` 1.1.0 (vendor **SudoMaker**) = *game stream host for
  Moonlight* — `github.com/papi-ux/polaris`
- repo `polaris` 10.2.0 (vendor **openSUSE**) = *Kubernetes best-practices
  validator* — `github.com/FairwindsOps/polaris`

`dup`'s default would **replace your streamer with a Kubernetes tool.** TW Safe
Update fetches each package's **upstream URL** and compares them:

- **different upstream → “name collision”** → UNSAFE, with the fix:
  `sudo zypper addlock <pkg>` to keep yours and unblock future updates.
- **same upstream → “vendor adoption”** → just CAUTION (openSUSE now ships a
  package you had from a third party — normally fine to take).

## Install

### Prebuilt RPM (openSUSE Tumbleweed)

Every tagged version is built by CI. Grab the latest
`tw-safe-update-*.x86_64.rpm` from the
[Releases page](https://github.com/Beary-Handsome/tw-safe-update/releases) and:

```bash
sudo zypper install ./tw-safe-update-*.x86_64.rpm
```

Then, once per user (a system package can't know which user wants it):

```bash
sudo usermod -aG twsafeupdate "$USER"      # allow the passwordless read-only dry-run
# log out and back in, then:
systemctl --user enable --now tw-safe-update-tray.service tw-safe-update.timer
```

You can also pull the RPM from the Artifacts of any green run on the
[Actions tab](https://github.com/Beary-Handsome/tw-safe-update/actions).

### From source

```bash
cd tw-safe-update
./install.sh
```

The installer (per-user, except the sudoers rule):

- installs any missing **build dependencies** (cmake, gcc-c++,
  `kf6-kstatusnotifieritem-devel`, `kf6-kparts-devel`, `qt6-*-devel`) — this
  step asks for your password;
- copies `twsu-check`, `twsu-update`, `twsu-details` to `~/.local/bin`;
- **builds** the tray daemon (`tray/twsu-tray.cpp`, Qt6 Widgets + KF6
  StatusNotifierItem/Parts) → `~/.local/bin/twsu-tray`;
- installs a **launcher entry** (*TW Update Assistant*);
- installs + enables two **systemd user units**: the ~3-hourly check timer and
  the tray service;
- installs **one narrow read-only `sudoers` rule** (asks for your password).

> **Autostart:** the tray service is `WantedBy=graphical-session.target`, which
> Plasma's systemd-managed session pulls in. If your session doesn't start it
> automatically, run `systemctl --user enable --now tw-safe-update-tray.service`
> or launch *TW Update Assistant* once.

An RPM is also available (see the project's OBS/releases); it installs the same
files system-wide and enables the user units on first login.

### The sudoers rule — exactly what is granted

`/etc/sudoers.d/50-tw-safe-update` grants your user passwordless sudo for **only**
these two commands, with **exactly** these arguments (sudo requires an exact
match — no extra flags can be appended):

```
/usr/bin/zypper --non-interactive refresh
/usr/bin/zypper --non-interactive --no-refresh dist-upgrade --dry-run
```

`--dry-run` never changes the system; `refresh` only updates repo metadata
caches under `/var/cache/zypp` (it cannot install, remove, or upgrade anything).
The real update (`twsu-update`) uses ordinary `sudo` and still prompts for your
password.

## From the terminal

```bash
twsu-check --pretty            # refresh + analyze, human-readable
twsu-check --pretty --cached   # show the last saved verdict, no re-check
twsu-check                     # JSON (what the tray consumes)
twsu-update                    # run the real dist-upgrade (asks for password)
```

Status is cached at `~/.cache/tw-safe-update/status.json`; the raw dry-run is
kept next to it as `last-dup-output.txt` for debugging (world-readable at the
default umask — it lists your pending package transaction, nothing secret).

Non-`--offline` runs also fetch `download.opensuse.org` and each enabled repo's
`repomd.xml` to report the latest snapshot and whether Packman is lagging. Pass
`--offline` to skip those network calls.

## Config

`~/.config/tw-safe-update/tray.conf`:

```
terminal=konsole                  # fallback terminal if the embedded one is unavailable
notify=safe                       # safe | review | any   (collisions always notify once)
icon=software-update-available    # any freedesktop icon name
show=safe                         # safe = only when a safe update is ready | always
```

Change the check interval by editing `OnUnitActiveSec` in
`~/.config/systemd/user/tw-safe-update.timer`, then
`systemctl --user daemon-reload && systemctl --user restart tw-safe-update.timer`.

## Manage the service

```bash
systemctl --user status  tw-safe-update-tray.service   # the tray icon
systemctl --user restart tw-safe-update-tray.service
systemctl --user list-timers tw-safe-update.timer      # background checks
```

## Uninstall

```bash
cd tw-safe-update
./uninstall.sh
```

(It does not remove the system build dependencies the installer added; the
script prints the command to do so if you want.)

## Layout

```
bin/twsu-check      analyzer + verdict + collision engine (Python 3, stdlib only)
bin/twsu-update     interactive real `zypper dup` wrapper
bin/twsu-details    show the cached verdict in a terminal
tray/twsu-tray.cpp  the tray daemon (Qt6 Widgets + KF6 StatusNotifierItem/KParts)
tray/CMakeLists.txt
systemd/            user timer (checks) + tray service
sudoers/…in         template for the read-only NOPASSWD rule
share/…desktop      launcher entry
tests/              offline parser/verdict regression tests
```

## Tests

```bash
tests/run-tests.sh   # parses recorded zypper outputs + regex unit checks. No root.
```

## Notes / limitations

- KDE Plasma 6 / Wayland is the target. The tray uses `KStatusNotifierItem` (so
  left-click opens the window) and embeds a **Konsole KPart** (package
  `konsole`) for the in-window terminal; if that part is missing it falls back to
  an external terminal.
- Build needs Qt6 Widgets/DBus + KF6 StatusNotifierItem/KParts + cmake/g++; the
  installer pulls the `-devel` packages. The built binary only needs the runtime
  libraries (already present on a Plasma desktop).
- The "leaving Packman = unsafe", collision, and critical-package heuristics are
  deliberately conservative; tune `CRITICAL_RE` / `MULTIMEDIA_RE` in
  `bin/twsu-check` to taste.
- Collision detection compares upstream URLs (falling back to summaries); a
  package with no URL metadata is reported as "unknown" rather than guessed.

## License

MIT — see [LICENSE](LICENSE).
