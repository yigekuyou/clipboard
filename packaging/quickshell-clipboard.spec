#
# spec file for package quickshell-clipboard
#
# Copyright (c) 2026 yigekuyou
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon.
#

Name:           quickshell-clipboard
Version:        0.1.0
Release:        0
Summary:        Wayland data-control clipboard manager as a Qt 6 QML module
License:        GPL-3.0-or-later
Group:          System/GUI/Other
URL:            https://github.com/yigekuyou/clipboard
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  ninja
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  qt6-base-devel
BuildRequires:  qt6-declarative-devel
Provides:       qml(io.yigekuyou.clipboard)

%description
A Qt 6 QML module (io.yigekuyou.clipboard) that implements a Wayland
clipboard manager on top of ext-data-control-v1 and
wlr-data-control-unstable-v1.  It observes and replaces the selection
without requiring keyboard focus, keeps a de-duplicated history (text and
images) in memory, and exposes it to QML as the ClipboardManager singleton.

The service only manages the clipboard while its host process is running:
when the process exits, the data-control connection is closed and the
selection is released.

%prep
%autosetup

%build
%cmake_qt6
%qt6_build

%install
%qt6_install

%files
%license LICENSE
%doc README.md
%dir %{_qt6_qmldir}/io
%dir %{_qt6_qmldir}/io/yigekuyou
%{_qt6_qmldir}/io/yigekuyou/clipboard/

%changelog
* Mon Oct 19 2026 yigekuyou <yigekuyou@users.noreply.github.com> - 0.1.0-0
- Initial package
