%global provider_lib_dir /usr/lib64/libibverbs
%global provider_etc_dir /etc/libibverbs.d

Name:           usb4-rdma-provider
Version:        @VERSION@
Release:        1%{?dist}
Summary:        usb4_rdma libibverbs userspace provider

License:        GPL-2.0-only OR BSD-3-Clause
URL:            https://github.com/hellas-ai/thunderbolt-ibverbs

BuildArch:      x86_64
Requires:       libibverbs

%description
Drop-in libibverbs provider that lets libibverbs enumerate and use
thunderbolt_ibverbs kernel devices. Install alongside the matching
thunderbolt-ibverbs-dkms package for full ibv_devices visibility and
downstream RDMA tool support (NCCL, vllm, perftest).

%install
install -d -m 0755 %{buildroot}%{provider_lib_dir}
install -d -m 0755 %{buildroot}%{provider_etc_dir}
install -m 0644 %{_sourcedir}/libusb4_rdma-rdmav*.so %{buildroot}%{provider_lib_dir}/
install -m 0644 %{_sourcedir}/usb4_rdma.driver %{buildroot}%{provider_etc_dir}/

%files
%{provider_lib_dir}/libusb4_rdma-rdmav*.so
%{provider_etc_dir}/usb4_rdma.driver

%changelog
* Tue May 26 2026 George Whewell <george@hellas.ai> - 0.1.0-1
- Initial release.
