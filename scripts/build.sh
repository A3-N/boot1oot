#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo_root/scripts/lib/buildroot-common.sh"
boot1oot_sanitize_path

config_file="$repo_root/config/boot1oot.local.env"
if [[ -f "$config_file" ]]; then
	set -a
	# shellcheck disable=SC1090
	source "$config_file"
	set +a
fi

buildroot_dir="$(boot1oot_resolve_buildroot_for_build "$repo_root" "${BUILDROOT_DIR:-}")"
output_dir="${OUTPUT_DIR:-$(boot1oot_default_output_dir "$repo_root")}"
dl_dir="${DL_DIR:-$(boot1oot_default_dl_dir)}"
defconfig="${DEFCONFIG:-boot1oot_x86_64_defconfig}"
jobs="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
clean="${CLEAN:-0}"

if [[ "$clean" == "1" ]]; then
	boot1oot_clean_output_dir "$repo_root" "$output_dir"
elif [[ -d "$output_dir" ]] && boot1oot_output_needs_clean_for_musl "$output_dir"; then
	cat >&2 <<EOF
Existing Buildroot output was configured/built with glibc.
Boot1oot now uses musl for a smaller target image and to avoid the glibc toolchain failure.

Clean generated output once, then rebuild:

  CLEAN=1 bash scripts/build.sh

From PowerShell:

  .\\scripts\\build.ps1 -Clean

EOF
	exit 1
fi

mkdir -p "$output_dir" "$dl_dir"

make -C "$buildroot_dir" O="$output_dir" BR2_EXTERNAL="$repo_root/buildroot-external" BR2_DL_DIR="$dl_dir" "$defconfig"

if [[ -d "$output_dir/build/boot1oot-0.1" ]]; then
	make -C "$buildroot_dir" O="$output_dir" BR2_EXTERNAL="$repo_root/buildroot-external" BR2_DL_DIR="$dl_dir" boot1oot-dirclean
fi
if [[ -d "$output_dir/build/chntpw-140201" ]]; then
	make -C "$buildroot_dir" O="$output_dir" BR2_EXTERNAL="$repo_root/buildroot-external" BR2_DL_DIR="$dl_dir" chntpw-dirclean
fi
rm -f "$output_dir/images/rootfs.cpio" "$output_dir/images/rootfs.cpio.gz"

make -C "$buildroot_dir" O="$output_dir" BR2_EXTERNAL="$repo_root/buildroot-external" BR2_DL_DIR="$dl_dir" -j"$jobs"

OUTPUT_DIR="$output_dir" bash "$repo_root/scripts/make-iso.sh"
