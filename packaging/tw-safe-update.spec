#
# spec file for package tw-safe-update
#
# Copyright (c) 2026 Beary-Handsome
#
# MIT-licensed; see the LICENSE file shipped in the source.
#

Name:           tw-safe-update
Version:        1.0.0
Release:        0
Summary:        Tells you when it is safe to run a Tumbleweed "zypper dup"
License:        MIT
URL:            https://github.com/Beary-Handsome/tw-safe-update
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  extra-cmake-modules
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(KF6StatusNotifierItem)
BuildRequires:  cmake(KF6Parts)
%{?systemd_ordering}

# Runtime pieces the tool shells out to / embeds.
Requires:       zypper
Requires:       sudo
Requires:       python3-base
# Konsole provides the KPart embedded in the update window.
Requires:       konsole
Requires(pre):  shadow

%description
TW Safe Update ("TW Update Assistant") is a KDE Plasma 6 background tray
service for openSUSE Tumbleweed. On a systemd user timer it runs a read-only
"zypper refresh" + "zypper dist-upgrade --dry-run", classifies whether updating
is safe (SAFE / CAUTION / UNSAFE), and only shows a tray icon when a safe update
is ready. It detects package name-collisions (an installed third-party package
and an unrelated repo package that share a name) by comparing upstream URLs,
runs the real update in an embedded Konsole terminal, and raises desktop
notifications.

The read-only refresh/dry-run run without a password via a narrow sudoers rule
granted to the "twsafeupdate" group; the real update always prompts.

%prep
%autosetup

%build
cmake -S tray -B build \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS="%{optflags}" \
    -DCMAKE_INSTALL_PREFIX=%{_prefix}
cmake --build build %{?_smp_mflags}

%install
install -Dm0755 build/twsu-tray            %{buildroot}%{_bindir}/twsu-tray
install -Dm0755 bin/twsu-check             %{buildroot}%{_bindir}/twsu-check
install -Dm0755 bin/twsu-update            %{buildroot}%{_bindir}/twsu-update
install -Dm0755 bin/twsu-details           %{buildroot}%{_bindir}/twsu-details

# systemd *user* units — point ExecStart at the packaged binaries.
mkdir -p %{buildroot}%{_userunitdir}
for u in tw-safe-update.service tw-safe-update.timer tw-safe-update-tray.service; do
    sed -e 's#%%h/.local/bin/#%{_bindir}/#g' systemd/$u > %{buildroot}%{_userunitdir}/$u
done

# launcher entry (Exec=twsu-tray resolves via PATH -> %{_bindir}/twsu-tray)
install -Dm0644 share/org.opensuse.twsafeupdate.desktop \
    %{buildroot}%{_datadir}/applications/org.opensuse.twsafeupdate.desktop

# group-scoped read-only sudoers rule (system package can't know the username)
install -d -m0750 %{buildroot}%{_sysconfdir}/sudoers.d
cat > %{buildroot}%{_sysconfdir}/sudoers.d/50-tw-safe-update <<'EOF'
# Installed by the tw-safe-update package.
# Members of the "twsafeupdate" group may run the two READ-ONLY zypper commands
# the checker needs without a password. Neither can modify the system:
#   refresh                    -> only updates repo metadata caches
#   dist-upgrade --dry-run     -> computes, never applies, the upgrade
# The real update uses ordinary sudo and still prompts.
%twsafeupdate ALL=(root) NOPASSWD: /usr/bin/zypper --non-interactive refresh
%twsafeupdate ALL=(root) NOPASSWD: /usr/bin/zypper --non-interactive --no-refresh dist-upgrade --dry-run
EOF
chmod 0440 %{buildroot}%{_sysconfdir}/sudoers.d/50-tw-safe-update

%pre
# The read-only sudoers rule is granted to this group; users opt in with
# `usermod -aG twsafeupdate <user>`.
getent group twsafeupdate >/dev/null || groupadd -r twsafeupdate || :

%post
# Enable the user units for every user's session (next login).
%systemd_user_post tw-safe-update.timer
%systemd_user_post tw-safe-update-tray.service

%preun
%systemd_user_preun tw-safe-update.timer
%systemd_user_preun tw-safe-update-tray.service

%postun
%systemd_user_postun tw-safe-update.timer
%systemd_user_postun tw-safe-update-tray.service

%files
%license LICENSE
%doc README.md
%{_bindir}/twsu-check
%{_bindir}/twsu-update
%{_bindir}/twsu-details
%{_bindir}/twsu-tray
%{_userunitdir}/tw-safe-update.service
%{_userunitdir}/tw-safe-update.timer
%{_userunitdir}/tw-safe-update-tray.service
%{_datadir}/applications/org.opensuse.twsafeupdate.desktop
%config(noreplace) %{_sysconfdir}/sudoers.d/50-tw-safe-update

%changelog
* Thu Jul 02 2026 Beary-Handsome <michael.abballe@gmail.com> - 1.0.0-0
- Initial package.
