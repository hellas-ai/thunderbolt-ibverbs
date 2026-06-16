#!/usr/bin/env bash
# Build thunderbolt_ibverbs.ko for a NixOS target's booted kernel and,
# optionally, copy or reload it on that target.

set -euo pipefail

usage() {
	cat <<'EOF'
Usage:
  tools/tbv-target-module.sh TARGET [options]

Builds this worktree's thunderbolt_ibverbs.ko against TARGET's NixOS kernel
package, then refuses to proceed unless the result matches TARGET's booted
kernel.

Options:
  --nixos-config PATH          NixOS config flake, default ../nixos-config.
  --config ATTR                nixosConfigurations attr, default TARGET.
  --booted-kernel              Build directly against TARGET's currently
                              booted kernel store derivation, skipping the
                              NixOS config evaluation.
  --ssh HOST                   SSH host, default TARGET.
  --copy-to STORE             Nix copy destination, default ssh-ng://SSH_HOST.
  --copy                      Copy the verified module closure to TARGET.
  --reload                    Copy and reload thunderbolt_ibverbs on TARGET.
                              Reload is non-atomic: a later insmod failure
                              leaves the target without thunderbolt_ibverbs.
  --options STRING            Module options for --reload. Required unless
                              --allow-empty-options is set.
  --allow-empty-options       Permit --reload with no module options.
  --wait SECONDS              Seconds to wait after reload, default 8.
  --override-input NAME PATH  Pass a flake input override to nix commands.
                              May be repeated.
  --allow-kernel-path-mismatch
                              Do not require config kernel path to equal
                              TARGET's booted /run/booted-system/kernel.
  --check-sigs                Do not pass --no-check-sigs to nix copy.
  -h, --help                  Show this help.

Examples:
  tools/tbv-target-module.sh router --nixos-config ../nixos-config
  tools/tbv-target-module.sh router --copy
  tools/tbv-target-module.sh strix-1 --booted-kernel --reload --options 'profile=linux_perf ...'
  tools/tbv-target-module.sh router --reload --options 'profile=mac_compat ...'
EOF
}

die() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}

sh_quote() {
	local s=${1//\'/\'\\\'\'}
	printf "'%s'" "$s"
}

abs_dir() {
	(CDPATH=; cd -- "$1" && pwd)
}

repo_root="$(abs_dir "$(dirname -- "${BASH_SOURCE[0]}")/..")"
target=""
nixos_config="${TBV_NIXOS_CONFIG:-$repo_root/../nixos-config}"
config_attr=""
ssh_host=""
copy_to=""
copy=0
reload=0
booted_kernel=0
allow_empty_options=0
allow_kernel_path_mismatch=0
check_sigs=0
wait_secs="${TBV_WAIT_SECS:-8}"
module_options="${TBV_OPTIONS:-}"
nix_override_args=()

while (($#)); do
	case "$1" in
	-h|--help)
		usage
		exit 0
		;;
	--nixos-config)
		shift
		[[ $# -gt 0 ]] || die "--nixos-config requires a path"
		nixos_config="$1"
		;;
	--config)
		shift
		[[ $# -gt 0 ]] || die "--config requires an attr name"
		config_attr="$1"
		;;
	--booted-kernel)
		booted_kernel=1
		;;
	--ssh)
		shift
		[[ $# -gt 0 ]] || die "--ssh requires a host"
		ssh_host="$1"
		;;
	--copy-to)
		shift
		[[ $# -gt 0 ]] || die "--copy-to requires a Nix store URI"
		copy_to="$1"
		;;
	--copy)
		copy=1
		;;
	--reload)
		reload=1
		copy=1
		;;
	--options)
		shift
		[[ $# -gt 0 ]] || die "--options requires a string"
		module_options="$1"
		;;
	--allow-empty-options)
		allow_empty_options=1
		;;
	--wait)
		shift
		[[ $# -gt 0 ]] || die "--wait requires seconds"
		wait_secs="$1"
		;;
	--override-input)
		shift
		[[ $# -gt 1 ]] || die "--override-input requires NAME PATH"
		nix_override_args+=(--override-input "$1" "$2")
		shift
		;;
	--allow-kernel-path-mismatch)
		allow_kernel_path_mismatch=1
		;;
	--check-sigs)
		check_sigs=1
		;;
	--*)
		die "unknown option: $1"
		;;
	*)
		if [[ -n "$target" ]]; then
			die "unexpected positional argument: $1"
		fi
		target="$1"
		;;
	esac
	shift
done

[[ -n "$target" ]] || die "missing TARGET"
[[ "$wait_secs" =~ ^[0-9]+$ ]] || die "--wait must be an unsigned integer"

command -v nix >/dev/null 2>&1 || die "nix not found"
command -v ssh >/dev/null 2>&1 || die "ssh not found"
command -v find >/dev/null 2>&1 || die "find not found"
command -v modinfo >/dev/null 2>&1 || die "modinfo not found"

nixos_config="$(abs_dir "$nixos_config")"
config_attr="${config_attr:-$target}"
ssh_host="${ssh_host:-$target}"
copy_to="${copy_to:-ssh-ng://$ssh_host}"

tmpdir="$(mktemp -d -t tbv-target-module-XXXXXX)"
cleanup() {
	rm -rf "$tmpdir"
}
trap cleanup EXIT

expr="$tmpdir/module.nix"
kernel_expr="$tmpdir/kernel-path.nix"

find_booted_kernel_dev() {
	local kernel_path=$1
	local drv
	local output
	local -a dev_outputs=()

	[[ -n "$kernel_path" ]] || die "remote booted kernel path is empty"
	[[ -e "$kernel_path" ]] ||
		die "remote booted kernel path is not present in the local store: $kernel_path"

	drv="$(nix-store -q --deriver "$kernel_path" 2>/dev/null || true)"
	[[ -n "$drv" && "$drv" != "unknown-deriver" ]] ||
		die "could not find local deriver for booted kernel $kernel_path"

	while IFS= read -r output; do
		[[ -n "$output" ]] || continue
		case "$output" in
		*-dev)
			dev_outputs+=("$output")
			;;
		esac
	done < <(nix-store -q --outputs "$drv")

	if [[ "${#dev_outputs[@]}" -ne 1 ]]; then
		printf 'kernel derivation outputs for %s:\n' "$drv" >&2
		nix-store -q --outputs "$drv" >&2 || true
		die "expected exactly one booted-kernel dev output, found ${#dev_outputs[@]}"
	fi

	printf '%s\n' "${dev_outputs[0]}"
}

module_option_value() {
	local name=$1
	local default=$2
	local opt opt_name opt_value

	read -r -a opt_args <<< "$module_options"
	for opt in "${opt_args[@]}"; do
		opt_name="${opt%%=*}"
		[[ "$opt_name" == "$name" ]] || continue
		if [[ "$opt" == *=* ]]; then
			opt_value="${opt#*=}"
		else
			opt_value=1
		fi
		printf '%s\n' "$opt_value"
		return 0
	done

	printf '%s\n' "$default"
}

option_truthy() {
	case "$1" in
	1|y|Y|yes|YES|true|TRUE|on|ON)
		return 0
		;;
	*)
		return 1
		;;
	esac
}

validate_module_options() {
	local module=$1
	local opt opt_name
	local known_params

	known_params="$(modinfo -p "$module" | sed 's/:.*//' | sort -u)"
	read -r -a opt_args <<< "$module_options"
	for opt in "${opt_args[@]}"; do
		[[ -n "$opt" ]] || continue
		opt_name="${opt%%=*}"
		if ! grep -qxF "$opt_name" <<< "$known_params"; then
			die "--options includes unknown module parameter '$opt_name' for $module"
		fi
	done
}

validate_kernel_feature_options() {
	local kernel_dev=$1
	local tbnet_identity
	local bind_services
	local -a header_roots=()

	tbnet_identity="$(module_option_value tbnet_identity "")"
	bind_services="$(module_option_value bind_services "0")"

	[[ -d "$kernel_dev/include" ]] && header_roots+=("$kernel_dev/include")
	[[ -d "$kernel_dev/lib/modules" ]] && header_roots+=("$kernel_dev/lib/modules")
	if [[ "${#header_roots[@]}" -eq 0 ]]; then
		header_roots+=("$kernel_dev")
	fi

	if [[ "$tbnet_identity" == "minimal_packet" ]] &&
	   option_truthy "$bind_services" &&
	   ! grep -Rqs "TB_PROTOCOL_HANDLER_UNREGISTER_DRAINS" \
		"${header_roots[@]}"; then
		die "tbnet_identity=minimal_packet with bind_services requires target kernel headers to define TB_PROTOCOL_HANDLER_UNREGISTER_DRAINS"
	fi
}

printf '==> Target: %s via ssh %s\n' "$config_attr" "$ssh_host"
remote_uname="$(ssh "$ssh_host" 'uname -r')"
remote_kernel="$(ssh "$ssh_host" 'readlink -f /run/booted-system/kernel 2>/dev/null || readlink -f /run/current-system/kernel 2>/dev/null || true')"
remote_kernel="${remote_kernel%/bzImage}"
printf '==> Remote uname: %s\n' "$remote_uname"
if [[ -n "$remote_kernel" ]]; then
	printf '==> Remote booted kernel: %s\n' "$remote_kernel"
fi

if [[ "$booted_kernel" == 1 ]]; then
	booted_kernel_dev="$(find_booted_kernel_dev "$remote_kernel")"
	kernel_dev="$booted_kernel_dev"
	printf '==> Booted kernel dev: %s\n' "$booted_kernel_dev"
	cat >"$expr" <<EOF
let
  flake = builtins.getFlake "path:$repo_root";
  pkgs = import flake.inputs.nixpkgs { system = builtins.currentSystem; };
  kernel = {
    modDirVersion = "$remote_uname";
    dev = builtins.storePath "$booted_kernel_dev";
    moduleBuildDependencies = pkgs.linuxPackages.kernel.moduleBuildDependencies;
  };
in
  pkgs.callPackage $repo_root/nix/module.nix {
    inherit kernel;
    source = $repo_root;
  }
EOF
else
	cat >"$expr" <<EOF
let
  flake = builtins.getFlake "path:$nixos_config";
  cfg = (builtins.getAttr "$config_attr" flake.nixosConfigurations).config;
in
  cfg.boot.kernelPackages.callPackage $repo_root/nix/module.nix {
    source = $repo_root;
  }
EOF

	cat >"$kernel_expr" <<EOF
let
  flake = builtins.getFlake "path:$nixos_config";
  cfg = (builtins.getAttr "$config_attr" flake.nixosConfigurations).config;
in
  toString cfg.boot.kernelPackages.kernel
EOF

	config_kernel="$(nix eval --impure --raw "${nix_override_args[@]}" --expr "$(<"$kernel_expr")")"
	printf '==> Config kernel: %s\n' "$config_kernel"
	kernel_dev="$(find_booted_kernel_dev "$config_kernel")"
	printf '==> Config kernel dev: %s\n' "$kernel_dev"
	if [[ -n "$remote_kernel" && "$config_kernel" != "$remote_kernel" ]]; then
		if [[ "$allow_kernel_path_mismatch" != 1 ]]; then
			die "config kernel does not match remote booted kernel; deploy/reboot the target or pass --allow-kernel-path-mismatch for an explicit ABI-only experiment"
		fi
		printf 'warning: config kernel path differs from remote booted kernel; continuing by request\n' >&2
	fi
fi

if [[ "$booted_kernel" == 1 ]]; then
	printf '==> Building module for %s booted kernel\n' "$config_attr"
else
	printf '==> Building module for %s configured kernel\n' "$config_attr"
fi
module_pkg="$(nix build --impure --no-link --print-out-paths "${nix_override_args[@]}" --expr "$(<"$expr")" | tail -n1)"
[[ -n "$module_pkg" ]] || die "nix build produced no output path"

mapfile -t modules < <(find "$module_pkg/lib/modules" -path '*/extra/thunderbolt_ibverbs.ko' -type f)
if [[ "${#modules[@]}" -ne 1 ]]; then
	printf 'found modules:\n' >&2
	printf '  %s\n' "${modules[@]}" >&2
	die "expected exactly one thunderbolt_ibverbs.ko"
fi

module="${modules[0]}"
module_uname="$(modinfo -F vermagic "$module" | awk '{print $1}')"
printf '==> Module: %s\n' "$module"
printf '==> Module vermagic kernel: %s\n' "$module_uname"
if [[ "$module_uname" != "$remote_uname" ]]; then
	die "module vermagic kernel '$module_uname' does not match remote uname '$remote_uname'"
fi

validate_module_options "$module"
validate_kernel_feature_options "$kernel_dev"

if [[ "$copy" == 1 ]]; then
	copy_args=(copy --to "$copy_to" "$module_pkg")
	if [[ "$check_sigs" != 1 ]]; then
		copy_args=(copy --no-check-sigs --to "$copy_to" "$module_pkg")
	fi
	printf '==> Copying module closure to %s\n' "$copy_to"
	nix "${copy_args[@]}"

	# shellcheck disable=SC2029 # $module is intentionally expanded locally.
	remote_module_uname="$(ssh "$ssh_host" "modinfo -F vermagic $(sh_quote "$module") | awk '{print \$1}'")"
	[[ "$remote_module_uname" == "$remote_uname" ]] ||
		die "remote copied module vermagic '$remote_module_uname' does not match remote uname '$remote_uname'"
fi

if [[ "$reload" == 1 ]]; then
	if [[ -z "$module_options" && "$allow_empty_options" != 1 ]]; then
		die "--reload requires --options or TBV_OPTIONS; pass --allow-empty-options to load defaults intentionally"
	fi
	printf '==> Reloading thunderbolt_ibverbs on %s\n' "$ssh_host"
	printf 'warning: --reload is non-atomic; a remote insmod failure leaves thunderbolt_ibverbs unloaded\n' >&2
	# shellcheck disable=SC2029 # Module path/options are intentionally expanded locally.
	ssh "$ssh_host" \
		"sudo -n env TBV_MODULE=$(sh_quote "$module") TBV_EXPECTED_UNAME=$(sh_quote "$remote_uname") TBV_OPTIONS=$(sh_quote "$module_options") TBV_WAIT_SECS=$(sh_quote "$wait_secs") bash -s" <<'REMOTE'
set -euo pipefail

fail() {
	printf 'remote error: %s\n' "$*" >&2
	exit 1
}

actual_uname="$(uname -r)"
[[ "$actual_uname" == "$TBV_EXPECTED_UNAME" ]] ||
	fail "uname changed during reload: expected $TBV_EXPECTED_UNAME, got $actual_uname"

module_uname="$(modinfo -F vermagic "$TBV_MODULE" | awk '{print $1}')"
[[ "$module_uname" == "$actual_uname" ]] ||
	fail "module vermagic kernel '$module_uname' does not match uname '$actual_uname'"

modprobe ib_uverbs || true

if grep -q '^thunderbolt_ibverbs ' /proc/modules; then
	rmmod thunderbolt_ibverbs
fi

load_softdeps=0
for dep in $(modinfo -F softdep "$TBV_MODULE" || true); do
	case "$dep" in
	pre:)
		load_softdeps=1
		continue
		;;
	post:)
		load_softdeps=0
		continue
		;;
	esac
	[[ "$load_softdeps" == 1 && -n "$dep" ]] || continue
	modprobe "$dep" || true
done

for dep in $(modinfo -F depends "$TBV_MODULE" | tr ',' ' ' || true); do
	[[ -n "$dep" ]] || continue
	modprobe "$dep"
done

read -r -a opt_args <<< "$TBV_OPTIONS"
roce_netdev=""
for opt in "${opt_args[@]}"; do
	case "$opt" in
	roce_netdev=*)
		roce_netdev="${opt#roce_netdev=}"
		;;
	esac
done

insmod "$TBV_MODULE" "${opt_args[@]}"

if [[ "$TBV_WAIT_SECS" != 0 ]]; then
	sleep "$TBV_WAIT_SECS"
fi

grep -q '^thunderbolt_ibverbs ' /proc/modules ||
	fail "module is not loaded after insmod"

if [[ -r /sys/kernel/debug/thunderbolt_ibverbs/summary ]]; then
	awk -F': *' '$1 == "profile" || $1 == "verbs_registered" || $1 == "tbnet_identity_proxy_ipv4" || $1 == "data_rx_bad_frame" || $1 == "data_cq_overflow" { print }' \
		/sys/kernel/debug/thunderbolt_ibverbs/summary
fi
if [[ -n "$roce_netdev" ]]; then
	if command -v ip >/dev/null 2>&1 && ip link show dev "$roce_netdev" >/dev/null 2>&1; then
		printf 'roce_netdev: %s present\n' "$roce_netdev"
	else
		printf 'warning: roce_netdev %s is absent; verbs registration may be deferred until it appears\n' "$roce_netdev" >&2
	fi
fi
if command -v rdma >/dev/null 2>&1; then
	rdma link show | sed 's/^/rdma: /'
fi
REMOTE
fi

printf '==> OK: module matches %s (%s)\n' "$ssh_host" "$remote_uname"
