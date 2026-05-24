#!/usr/bin/env bash
# Boot a real distro cloud image under QEMU/KVM, copy this source tree into the
# guest, and run tools/ci/vm-guest-smoke.sh there.

set -euo pipefail

usage() {
	cat <<'EOF'
Usage:
  tools/ci/vm-smoke.sh debian|fedora|arch

Environment:
  TBV_VM_CACHE_DIR=/path      Cache for downloaded base qcow2 images.
  TBV_VM_WORK_DIR=/path       Scratch directory for overlays and cloud-init.
  TBV_VM_MEMORY=3072          Guest RAM in MiB.
  TBV_VM_CPUS=2               Guest vCPU count.
  TBV_VM_TIMEOUT=300          SSH boot timeout in seconds.
  TBV_VM_DEBIAN_IMAGE_URL=... Override Debian qcow2 URL.
  TBV_VM_FEDORA_IMAGE_URL=... Override Fedora qcow2 URL.
  TBV_VM_ARCH_IMAGE_URL=...   Override Arch qcow2 URL.
EOF
}

die() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}

repo_root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
distro="${1:-}"

if [[ "$distro" == "-h" || "$distro" == "--help" ]]; then
	usage
	exit 0
fi
[[ -n "$distro" ]] || die "missing distro"

case "$distro" in
debian)
	image_url="${TBV_VM_DEBIAN_IMAGE_URL:-https://cloud.debian.org/images/cloud/trixie/latest/debian-13-generic-amd64.qcow2}"
	image_name="debian-13-generic-amd64.qcow2"
	;;
fedora)
	image_url="${TBV_VM_FEDORA_IMAGE_URL:-https://download.fedoraproject.org/pub/fedora/linux/releases/44/Cloud/x86_64/images/Fedora-Cloud-Base-Generic-44-1.7.x86_64.qcow2}"
	image_name="fedora-44-cloud-base-generic.qcow2"
	;;
arch)
	image_url="${TBV_VM_ARCH_IMAGE_URL:-https://fastly.mirror.pkgbuild.com/images/latest/Arch-Linux-x86_64-cloudimg.qcow2}"
	image_name="arch-linux-cloudimg.qcow2"
	;;
*)
	die "unsupported distro: $distro"
	;;
esac

for cmd in qemu-system-x86_64 qemu-img ssh scp curl; do
	command -v "$cmd" >/dev/null 2>&1 || die "$cmd not found"
done

if [[ ! -e /dev/kvm ]]; then
	die "/dev/kvm is missing; this smoke test requires KVM"
fi
if [[ ! -r /dev/kvm || ! -w /dev/kvm ]]; then
	die "/dev/kvm is not readable/writable by this user"
fi

cache_dir="${TBV_VM_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/thunderbolt-ibverbs/vm-images}"
work_dir="${TBV_VM_WORK_DIR:-$(mktemp -d)}"
memory="${TBV_VM_MEMORY:-3072}"
cpus="${TBV_VM_CPUS:-2}"
timeout="${TBV_VM_TIMEOUT:-300}"
guest_user="ci"
port="${TBV_VM_SSH_PORT:-$((20000 + RANDOM % 20000))}"
base_image="$cache_dir/$image_name"
disk="$work_dir/disk.qcow2"
seed="$work_dir/seed.iso"
ssh_key="$work_dir/id_ed25519"
serial_log="$work_dir/qemu-serial.log"
qemu_pid=""

cleanup() {
	if [[ -n "$qemu_pid" ]] && kill -0 "$qemu_pid" >/dev/null 2>&1; then
		kill "$qemu_pid" >/dev/null 2>&1 || true
		wait "$qemu_pid" >/dev/null 2>&1 || true
	fi
	if [[ -z "${TBV_VM_WORK_DIR:-}" ]]; then
		rm -rf "$work_dir"
	fi
}
trap cleanup EXIT

make_seed_iso() {
	local user_data="$1"
	local meta_data="$2"
	local iso="$3"

	if command -v cloud-localds >/dev/null 2>&1; then
		cloud-localds "$iso" "$user_data" "$meta_data"
	elif command -v genisoimage >/dev/null 2>&1; then
		genisoimage -quiet -output "$iso" -volid cidata -joliet -rock \
			"$user_data" "$meta_data"
	elif command -v mkisofs >/dev/null 2>&1; then
		mkisofs -quiet -output "$iso" -volid cidata -joliet -rock \
			"$user_data" "$meta_data"
	else
		die "cloud-localds, genisoimage, or mkisofs is required"
	fi
}

download_image() {
	mkdir -p "$cache_dir"
	if qemu-img info "$base_image" >/dev/null 2>&1; then
		printf '==> Using cached image: %s\n' "$base_image"
		return
	fi

	printf '==> Downloading %s\n' "$image_url"
	curl -fL --retry 3 --retry-delay 5 -o "$base_image.tmp" "$image_url"
	qemu-img info "$base_image.tmp" >/dev/null
	mv "$base_image.tmp" "$base_image"
}

prepare_guest() {
	local pubkey
	local user_data="$work_dir/user-data"
	local meta_data="$work_dir/meta-data"

	ssh-keygen -q -t ed25519 -N '' -f "$ssh_key"
	pubkey="$(<"$ssh_key.pub")"

	cat >"$user_data" <<EOF
#cloud-config
users:
  - name: $guest_user
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    ssh_authorized_keys:
      - $pubkey
ssh_pwauth: false
disable_root: true
growpart:
  mode: auto
runcmd:
  - [ sh, -c, "systemctl enable --now ssh || systemctl enable --now sshd || true" ]
EOF

	cat >"$meta_data" <<EOF
instance-id: thunderbolt-ibverbs-$distro
local-hostname: tbv-$distro
EOF

	make_seed_iso "$user_data" "$meta_data" "$seed"
	qemu-img create -q -f qcow2 -F qcow2 -b "$base_image" "$disk" 20G
}

start_guest() {
	printf '==> Starting %s VM on SSH port %s\n' "$distro" "$port"
	qemu-system-x86_64 \
		-enable-kvm \
		-cpu host \
		-m "$memory" \
		-smp "$cpus" \
		-drive "file=$disk,format=qcow2,if=virtio" \
		-drive "file=$seed,format=raw,if=virtio,media=cdrom,readonly=on" \
		-netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$port-:22" \
		-device virtio-net-pci,netdev=net0 \
		-display none \
		-serial "file:$serial_log" \
		-monitor none &
	qemu_pid="$!"
}

ssh_cmd() {
	ssh -i "$ssh_key" \
		-p "$port" \
		-o BatchMode=yes \
		-o ConnectTimeout=5 \
		-o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null \
		"$guest_user@127.0.0.1" "$@"
}

scp_to_guest() {
	scp -i "$ssh_key" \
		-P "$port" \
		-o BatchMode=yes \
		-o ConnectTimeout=5 \
		-o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null \
		"$1" "$guest_user@127.0.0.1:$2"
}

wait_for_ssh() {
	local waited=0

	while (( waited < timeout )); do
		if ssh_cmd 'true' >/dev/null 2>&1; then
			return 0
		fi
		if ! kill -0 "$qemu_pid" >/dev/null 2>&1; then
			cat "$serial_log" >&2 || true
			die "QEMU exited before SSH became available"
		fi
		sleep 2
		waited=$((waited + 2))
	done

	cat "$serial_log" >&2 || true
	die "timed out waiting for SSH"
}

wait_for_reboot() {
	local waited=0

	while (( waited < timeout )); do
		if ! ssh_cmd 'true' >/dev/null 2>&1; then
			break
		fi
		sleep 2
		waited=$((waited + 2))
	done
	wait_for_ssh
}

copy_source() {
	local archive="$work_dir/source.tar.gz"

	printf '==> Copying source tree into guest\n'
	tar -C "$repo_root" \
		--exclude=.git \
		--exclude=result \
		--exclude=dist \
		-czf "$archive" .
	scp_to_guest "$archive" /tmp/thunderbolt-ibverbs.tar.gz
	ssh_cmd 'sudo rm -rf /work/src &&
		sudo mkdir -p /work/src &&
		sudo chown '"$guest_user:$guest_user"' /work/src &&
		tar -C /work/src -xzf /tmp/thunderbolt-ibverbs.tar.gz'
}

run_guest_smoke() {
	local rebooted="$1"

	ssh_cmd "cd /work/src &&
		sudo env TBV_GUEST_REBOOTED=$rebooted \
			TBV_GUEST_SRC=/work/src \
			bash tools/ci/vm-guest-smoke.sh $distro"
}

download_image
prepare_guest
start_guest
wait_for_ssh
ssh_cmd 'sudo cloud-init status --wait || true'
copy_source

printf '==> Running guest smoke\n'
set +e
run_guest_smoke 0
status="$?"
set -e

if [[ "$status" == "75" ]]; then
	printf '==> Guest requested one reboot\n'
	wait_for_reboot
	set +e
	run_guest_smoke 1
	status="$?"
	set -e
fi

if [[ "$status" != "0" ]]; then
	printf '\n==> Guest smoke failed; serial log follows\n' >&2
	cat "$serial_log" >&2 || true
	exit "$status"
fi

printf '==> VM smoke OK: %s\n' "$distro"
