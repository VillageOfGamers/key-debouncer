Name:           debounced
Version:        2.0
Release:        1%{?dist}
Summary:        Userspace keyboard debounce daemon

License:        GPL-3.0-only
URL:            https://github.com/VillageOfGamers/key-debouncer
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  libevdev-devel
BuildRequires:  pkgconfig

Requires:       libevdev
Requires:       systemd

%description
Simple daemon to debounce keyboard input and provide FlashTap features.

%prep
%autosetup

%build
%make_build

%install
%make_install PREFIX=/usr SYSTEMD_UNITDIR=/usr/lib/systemd/system

%post
%systemd_post debounced.service

%preun
%systemd_preun debounced.service

%postun
%systemd_postun_with_restart debounced.service

%files
%license LICENSE
%{_bindir}/debounced
%{_bindir}/debouncectl
%{_unitdir}/debounced.service
%{_mandir}/man8/debounced.8*
%{_mandir}/man8/debouncectl.8*

%changelog
* Tue Mar 17 2026 Vincent Meadows <giantvince1@protonmail.com> - 2.0-1
- Reworked debounced.c for thread safety: socket thread is now purely
  IPC, all device and key state management moved to main().
- Fixed unsafe function calls in SIGTERM handler; now uses atomic flag
  with deferred cleanup in main loop.
- Fixed startup stuck key issue by force-releasing all physically held
  keys via synthetic UP events before grabbing the device.
- FlashTap now operates on post-debounce events via post_debounce_event
  routing, eliminating duplicate emissions in mode 'b'.
- Fixed mode variable not routing event processing; modes 'd', 'f', and
  'b' now correctly gate debounce and FlashTap independently.
- Extended keycode support to KEY_MAX (767) with passthrough for codes
  above MAX_KEYCODE, fixing silent drops of media and macro keys.
- Removed -march=native from release builds for cross-architecture
  compatibility.
- Added RPM spec and AUR PKGBUILD packaging metadata.
- Added manpages for debounced(8) and debouncectl(8).
- Unified all packaging helpers into main branch.
* Thu Sep 18 2025 Vincent Meadows <giantvince1@protonmail.com> - 1.0-1
- Initial release
