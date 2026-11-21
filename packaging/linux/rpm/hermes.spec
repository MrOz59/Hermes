Name:           hermes
Version:        %{hermes_version}
Release:        %{?hermes_release}%{!?hermes_release:1}%{?dist}
Summary:        Self-hosted game stream host with Linux virtual display support
License:        GPLv3
URL:            https://github.com/MrOz59/Hermes
Source0:        hermes-%{version}.tar.gz
Source1:        hermes-%{version}.files
# PyroWave loads Vulkan through volk, so declare what ELF scanning may miss.
Requires:       vulkan-loader, libcap

%if 0%{?hermes_bundled_pyrowave}
# This SONAME is resolved via RUNPATH inside the Hermes package. Keep that
# private codec out of RPM's public dependency graph.
%global __requires_exclude ^libpyrowave-shared[.]so[.]0[(][)][(]64bit[)]$
%global __provides_exclude %{__requires_exclude}
%endif

# The payload is built before rpmbuild and has no sources for a debug package.
%global debug_package %{nil}

%description
Hermes is an Apollo-derived game streaming server with low-latency
Linux virtual displays via Hermes-KMS (EVDI also supported).

%prep
%setup -q

%install
mkdir -p %{buildroot}
cp -a usr %{buildroot}/
%if 0%{?hermes_bundled_pyrowave}
test -f "%{buildroot}%{_libdir}/hermes/libpyrowave-shared.so.0"
%endif

%files -f %{SOURCE1}
%defattr(-,root,root,-)

%post
setcap cap_sys_admin,cap_mknod,cap_sys_chroot+p /usr/bin/hermes || true
