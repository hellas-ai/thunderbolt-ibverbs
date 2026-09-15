#!/usr/bin/env bash
#
# Prove the COMPAT PROBE in kernel/Makefile behaves against synthetic kernel
# trees, without needing a real kernel to build against.
#
# Why this test exists. The probe decides whether to compile the code that
# writes struct tb_ring.interval_nsec, a member the Asahi USB4 tree carries and
# stock kernels do not. It used to be:
#
#     ifneq ($(shell grep -c interval_nsec $(KDIR)/include/linux/thunderbolt.h),0)
#
# which has two defects that combine into a build failure:
#
#   * it looks only under $(KDIR)/include, and a kernel packaged as a SPLIT
#     build/source tree (nixpkgs, and therefore the Hydra builders) keeps the
#     real headers under $(KDIR)/source/include; and
#   * grep on a missing file prints nothing, so $(shell ...) is "", "" != "0"
#     is true, and the flag was defined precisely when the probe could not
#     check -- a probe whose failure mode is to assert what it exists to doubt.
#
# Against linux 6.18.37 that produced:
#     path.c: error: 'struct tb_ring' has no member named 'interval_nsec'
#
# The rule this encodes: a capability probe MUST fail closed.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
makefile=${1:-$repo_root/kernel/Makefile}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# $1 name  $2 layout(flat|split|nested|none)  $3 header body
fixture() {
  local name=$1 layout=$2 body=$3 inc
  case $layout in
    flat)  inc=$work/$name/include/linux ; kdir=$work/$name ;;
    split) inc=$work/$name/source/include/linux ; kdir=$work/$name/build ;;
    nested) inc=$work/$name/build/source/include/linux ; kdir=$work/$name/build ;;
    none)  mkdir -p "$work/$name/build" ; echo "$work/$name/build" ; return ;;
  esac
  mkdir -p "$inc" "$kdir"
  printf '%s\n' "$body" > "$inc/thunderbolt.h"
  echo "$kdir"
}

probe() {
  make -s -f "$makefile" KDIR="$1" compat-report \
    | sed -n 's/^interval_nsec=//p'
}

fail=0
expect() { # $1 description  $2 KDIR  $3 expected(yes|no)
  local got; got=$(probe "$2")
  if [ "$got" = "$3" ]; then
    printf '  ok      %-46s -> %s\n' "$1" "$got"
  else
    printf '  FAIL    %-46s -> %s (want %s)\n' "$1" "$got" "$3"
    fail=1
  fi
}

HAS='struct tb_ring { int hop; unsigned int interval_nsec; };'
HASNT='struct tb_ring { int hop; };'
COMMENT='/* the kernel programs the throttle from ring->interval_nsec */
struct tb_ring { int hop; };'

echo "COMPAT PROBE matrix ($makefile)"
expect "split tree, member absent (the Hydra case)" "$(fixture split-absent split "$HASNT")"  no
expect "split tree, member present"                 "$(fixture split-has    split "$HAS")"    yes
expect "nested source/ packaging, member present"    "$(fixture nested-has   nested "$HAS")"    yes
expect "flat tree, member absent (stock kernel)"    "$(fixture flat-absent  flat  "$HASNT")"  no
expect "flat tree, member present (Asahi USB4)"     "$(fixture flat-has     flat  "$HAS")"    yes
expect "header mentions it only in a comment"       "$(fixture comment-only flat  "$COMMENT")" no
expect "no header at all (must fail CLOSED)"        "$(fixture missing      none  '')"        no

if [ "$fail" -ne 0 ]; then
  echo "kernel compat probe FAILED" >&2
  exit 1
fi
echo "kernel compat probe: all cases correct"
