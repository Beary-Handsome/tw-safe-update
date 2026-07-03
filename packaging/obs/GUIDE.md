# Publishing tw-safe-update on the Open Build Service (OBS)

OBS builds RPMs for openSUSE (and others) from a spec + source. You need an
account on <https://build.opensuse.org> and the `osc` CLI:

```bash
sudo zypper install osc
```

`osc` uses your OBS credentials (`~/.config/osc/oscrc` is created on first use).

There are two ways to feed OBS the source. **Method A (manual tarball)** is the
most reliable and matches exactly what was tested locally. **Method B (`_service`)**
makes OBS pull from GitHub automatically.

---

## Method A — upload the spec + tarball (recommended first time)

1. Create your package under your home project:

   ```bash
   osc meta pkg -e home:Beary-Handsome tw-safe-update
   # (save/exit the editor; this creates the package)
   osc checkout home:Beary-Handsome tw-safe-update
   cd home:Beary-Handsome/tw-safe-update
   ```

2. Copy in the spec and the source tarball:

   ```bash
   cp ~/tw-safe-update/packaging/tw-safe-update.spec .
   cp ~/rpmbuild/SOURCES/tw-safe-update-1.0.0.tar.gz .
   ```

   (Re-create the tarball any time with:
   `git -C ~/tw-safe-update archive --prefix=tw-safe-update-1.0.0/ \
   -o ~/rpmbuild/SOURCES/tw-safe-update-1.0.0.tar.gz HEAD`)

3. Add and commit — this triggers a build:

   ```bash
   osc add tw-safe-update.spec tw-safe-update-1.0.0.tar.gz
   osc commit -m "tw-safe-update 1.0.0"
   ```

4. Add build targets (repositories) in the web UI (e.g. `openSUSE_Tumbleweed`),
   or:

   ```bash
   osc meta prjconf home:Beary-Handsome   # ensure Tumbleweed repo is enabled
   ```

5. Watch the build:

   ```bash
   osc results
   osc buildlog openSUSE_Tumbleweed x86_64   # if it fails, read the log
   ```

---

## Method B — let OBS pull from GitHub (`_service`)

1. Create/checkout the package as in Method A steps 1.
2. Copy the spec and the service file:

   ```bash
   cp ~/tw-safe-update/packaging/tw-safe-update.spec .
   cp ~/tw-safe-update/packaging/obs/_service .
   ```

3. Commit:

   ```bash
   osc add tw-safe-update.spec _service
   osc commit -m "tw-safe-update 1.0.0 (via _service)"
   ```

   OBS runs the service server-side, produces `tw-safe-update-1.0.0.tar.gz`, and
   builds. For tagged releases, change `<revision>` in `_service` from `main` to
   a tag like `v1.0.0` and bump `<versionformat>`.

---

## Build requirements OBS needs (already in the spec)

The spec's `BuildRequires` are: `cmake`, `gcc-c++`, `extra-cmake-modules`,
`cmake(Qt6Widgets)`, `cmake(Qt6DBus)`, `cmake(KF6StatusNotifierItem)`,
`cmake(KF6Parts)`. OBS resolves these automatically for the Tumbleweed repo.

## After it builds

Users install via your repo:

```bash
sudo zypper addrepo https://download.opensuse.org/repositories/home:/Beary-Handsome/openSUSE_Tumbleweed/home:Beary-Handsome.repo
sudo zypper refresh
sudo zypper install tw-safe-update
```

Then each user runs once (the package can't know which user wants it):

```bash
sudo usermod -aG twsafeupdate "$USER"     # allow the passwordless read-only dry-run
# log out and back in, then enable the tray for your session if needed:
systemctl --user enable --now tw-safe-update-tray.service tw-safe-update.timer
```

## Notes

- To submit to the official `openSUSE:Factory` later, use
  `osc submitrequest` / `osc sr` from your home project once it's polished.
- The `%config(noreplace)` on the sudoers file means user edits survive upgrades.
- If a build fails on a missing macro, ensure the project's Tumbleweed repo is
  the build target (older Leap repos won't have the KF6 packages).
