%global modname thunderbolt-ibverbs

Name:           %{modname}-dkms
Version:        @VERSION@
Release:        1%{?dist}
Summary:        Thunderbolt/USB4 host-to-host RDMA verbs kernel module (DKMS)

License:        GPL-2.0-only
URL:            https://github.com/hellas-ai/thunderbolt-ibverbs
Source0:        %{modname}-%{version}.tar.gz

BuildArch:      noarch
BuildRequires:  coreutils
Requires:       dkms >= 2.1.0
Requires:       kmod
Requires(post): dkms
Requires(preun): dkms

%description
Experimental Linux kernel module exposing an RDMA verbs device over
Thunderbolt/USB4 host-to-host links. This package installs the module source
under /usr/src and registers it with DKMS, which builds the module against the
running kernel on install and on every subsequent kernel upgrade.

Requires Linux 6.14 or newer, or the linux-thunderbolt kernel built from the
upstream flake.

%prep
%setup -q -n %{modname}-%{version}

%build
# Source-only package; the kernel module is built on the target system by DKMS.

%install
install -d -m 0755 %{buildroot}/usr/src/%{modname}-%{version}
cp -a . %{buildroot}/usr/src/%{modname}-%{version}/

%post
if [ "$1" = "1" ]; then
    dkms add -m %{modname} -v %{version} >/dev/null 2>&1 || :
fi
dkms autoinstall -m %{modname}/%{version} >/dev/null 2>&1 || :

%preun
if [ "$1" = "0" ]; then
    dkms remove -m %{modname} -v %{version} --all >/dev/null 2>&1 || :
fi

%files
%dir /usr/src/%{modname}-%{version}
/usr/src/%{modname}-%{version}/*

%changelog
* Sat May 30 2026 George Whewell <george@hellas.ai> - 0.2.1-1
- v0.2.1: no DKMS-side changes; release cut alongside Ubuntu 22.04 and
  24.04 usb4-rdma-provider .debs so PyTorch / vllm / llama.cpp users can
  apt-install the provider directly inside stock containers.

* Thu May 28 2026 George Whewell <george@hellas.ai> - 0.2.0-1
- v0.2.0: aarch64-darwin perftest and bench-tools, IOMMU-off transport
  sweep results, Mac↔Linux UC SEND harness, additional kernel and
  userspace fixes since 0.1.0.

* Tue May 26 2026 George Whewell <george@hellas.ai> - 0.1.0-1
- Initial DKMS source package.
