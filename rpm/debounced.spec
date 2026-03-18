Name:           debounced
Version:        1.0
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

%changelog
* Thu Sep 18 2025 Vincent Meadows <giantvince1@protonmail.com> - 1.0-1
- Initial release
