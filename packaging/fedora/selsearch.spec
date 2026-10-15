# SPDX-License-Identifier: MIT
# Built by .github/workflows/build.yml from `git archive` of the tagged commit:
#   rpmbuild -ba --define "_sourcedir $PWD" --define "version_override 0.1.0" selsearch.spec
%{!?version_override: %global version_override 0.1.2}

Name:           selsearch
Version:        %{version_override}
Release:        1%{?dist}
Summary:        Local screen search for Wayland, plus an Alt+Space launcher
License:        MIT
URL:            https://github.com/svyyyaaat/selsearch
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.25
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  pkgconf-pkg-config
BuildRequires:  systemd-rpm-macros
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtbase-private-devel
BuildRequires:  qt6-qtdeclarative-devel
BuildRequires:  qt6-qtsvg-devel
BuildRequires:  qt6-qtwayland-devel
BuildRequires:  qt6-qtshadertools-devel
BuildRequires:  qt6-qtwebengine-devel
BuildRequires:  layer-shell-qt-devel
BuildRequires:  wayland-devel
BuildRequires:  wayland-protocols-devel
BuildRequires:  libicu-devel
BuildRequires:  onnxruntime-devel
BuildRequires:  tesseract-devel
BuildRequires:  leptonica-devel

# Loaded at run time, not linked.
Requires:       qt6-qtwayland
Requires:       qt6-qtsvg
Requires:       layer-shell-qt
Recommends:     tesseract-langpack-eng
Recommends:     xdg-desktop-portal
Suggests:       plocate

%description
Freeze the screen, select text or part of an image, then copy, translate,
search or ask a local AI about it. Plus an Alt+Space launcher for apps,
files, calculations and web search. Everything runs locally.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -G Ninja -DSELSEARCH_BUILD_TESTS=OFF
%cmake_build

%install
%cmake_install
install -Dm644 LICENSE %{buildroot}%{_licensedir}/%{name}/LICENSE

%post
%systemd_user_post selsearch.service

%preun
%systemd_user_preun selsearch.service

%files
%license LICENSE
%doc README.md INSTALL.md
%{_bindir}/selsearch
%{_bindir}/selsearchd
%{_bindir}/selsearch-web
%{_userunitdir}/selsearch.service
%{_datadir}/applications/selsearch.desktop

%changelog
* Thu Oct 15 2026 Svyyyaaat - 0.1.2-1
- Lens: language menu closes on a second click; selecting on another
  monitor on Hyprland; options to read text on one screen or only inside
  the selection; dot brightness setting
* Tue Oct 13 2026 Svyyyaaat - 0.1.1-1
- Windows: lens no longer shows black screens; Alt/Win shortcuts no longer
  open menus; the shortcut recorder takes any key combination
* Tue Oct 13 2026 Svyyyaaat - 0.1.0-1
- First release
