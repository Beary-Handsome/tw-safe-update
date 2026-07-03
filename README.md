# TW Safe Update

[![build](https://github.com/Beary-Handsome/tw-safe-update/actions/workflows/build.yml/badge.svg)](https://github.com/Beary-Handsome/tw-safe-update/actions/workflows/build.yml)

Tells you when it's safe to run `sudo zypper dup` on **openSUSE Tumbleweed**, and
lets you do it from the system tray. It shows up as **TW Update Assistant**.

Tumbleweed is a rolling release. Most days a `zypper dup` is fine, but every so
often a snapshot lands before Packman has rebuilt your codecs, or a third-party
package collides with a newly-added official one, or the update just can't
resolve cleanly. Do it at the wrong moment and you can lose media playback, have
a package quietly swapped for an unrelated one, or end up half-upgraded. This
runs the check for you in the background and only shows a green light when the
update comes out clean.

## What it actually checks — and what it can't

The core is real, not a guess. It runs the package manager's own dry run
(`zypper dist-upgrade --dry-run`), so what it reports is exactly what the real
update would do. It reliably catches:

- dependency conflicts (the update can't resolve),
- packages being **removed, downgraded, or held back**,
- **codecs/packages leaving Packman**,
- **name collisions** — two unrelated projects that share a package name. It
  compares their upstream URLs, so a `dup` can't quietly replace, say, your
  Moonlight streaming host with an unrelated Kubernetes tool that happens to be
  named the same thing.

These are the common ways a Tumbleweed update bites you, and it catches them
because it's reading the resolver's own output.

What it **can't** know: a package that installs cleanly but has a runtime bug, a
kernel or driver update that doesn't agree with your hardware, or anything that
only shows up after a reboot. No dry run can predict those.

So a green light means **"this update resolves cleanly and won't remove,
downgrade, or swap your packages"** — not "this update is bug-free." It's a solid
check for the common footguns, not a guarantee.

| | |
|---|---|
| 🟢 **Safe** | Clean — nothing removed, downgraded, held back, or swapped. Good to go. |
| 🟢 **Up to date** | Nothing to install. |
| 🟡 **Review** | Resolvable, but worth a look (held-back packages, a downgrade, a vendor change). |
| 🔴 **Not safe** | Dependency problems, codecs leaving Packman, a critical package removal, or a name collision. |

## Install

**Requires openSUSE Tumbleweed with KDE Plasma 6.**

1. Download the latest `tw-safe-update-*.x86_64.rpm` from the
   [**Releases page**](https://github.com/Beary-Handsome/tw-safe-update/releases/latest)
   and install it:

   ```bash
   sudo zypper install ./tw-safe-update-*.x86_64.rpm
   ```

2. Opt in and start it (a system package can't know which user wants it):

   ```bash
   sudo usermod -aG twsafeupdate "$USER"
   systemctl --user enable --now tw-safe-update-tray.service tw-safe-update.timer
   ```

3. **Log out and back in** — that's needed for the group to take effect.

That's it. It watches quietly in the background; the tray icon appears only when
a safe update is ready. You can open **TW Update Assistant** from your launcher
any time to check on demand.

<details>
<summary>Build from source instead</summary>

```bash
git clone https://github.com/Beary-Handsome/tw-safe-update
cd tw-safe-update
./install.sh
```

The installer pulls the build dependencies, compiles the tray, and sets up a
per-user sudoers rule and the systemd units.
</details>

## Using it

- The tray icon shows **only when a safe update is ready** (set `show=always` to
  pin it).
- **Left-click** the icon, or launch the app, to open a window with the verdict,
  the reasons, and buttons.
- **Update now** and **Details** run in a terminal embedded right in the window —
  the real `zypper dup` asks for your password there.

## Configuration

Optional, in `~/.config/tw-safe-update/tray.conf`:

```
show=safe                         # safe = only when an update is ready | always
notify=safe                       # safe | review | any
icon=software-update-available    # any freedesktop icon name
terminal=konsole                  # fallback terminal, if the embedded one is unavailable
```

## The sudoers rule

The background check needs to run two **read-only** commands without a password:

```
zypper --non-interactive refresh
zypper --non-interactive --no-refresh dist-upgrade --dry-run
```

Neither can change the system — `refresh` only updates metadata, and `--dry-run`
never applies anything. They're granted only to the `twsafeupdate` group. The
real update always prompts for your password.

## Uninstall

```bash
sudo zypper remove tw-safe-update        # installed from the RPM
```

If you built from source, run `./uninstall.sh` from the checkout instead.

## License

MIT — see [LICENSE](LICENSE).
