#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo_root/scripts/lib/buildroot-common.sh"
boot1oot_sanitize_path

echo "Boot1oot setup"
echo "Repository: $repo_root"

missing_commands="$(boot1oot_missing_commands || true)"
missing_headers="$(boot1oot_missing_headers || true)"
if [[ -n "$missing_commands" || -n "$missing_headers" ]]; then
	echo
	if [[ -n "$missing_commands" ]]; then
		echo "Missing required host commands:"
		printf '  %s\n' $missing_commands
	fi
	if [[ -n "$missing_headers" ]]; then
		echo "Missing required host headers/packages:"
		printf '  %s\n' $missing_headers
	fi
	echo
	boot1oot_print_dependency_help
	exit 1
fi

echo
echo "Checking approved Buildroot locations:"
while IFS= read -r dir; do
	problem="$(boot1oot_buildroot_problem "$dir")"
	if [[ -z "$problem" ]]; then
		echo "  OK      $(boot1oot_abs_path "$dir")"
	else
		echo "  SKIP    $dir ($problem)"
	fi
done < <(boot1oot_allowed_buildroot_dirs "$repo_root")

if buildroot_dir="$(boot1oot_find_buildroot "$repo_root")"; then
	echo
	echo "Using Buildroot: $buildroot_dir"
	echo "Setup complete. Run: bash scripts/build.sh"
	exit 0
fi

echo
echo "No usable Buildroot checkout found in approved locations."
buildroot_dir="$(boot1oot_clone_buildroot "$repo_root")"
echo
echo "Using Buildroot: $buildroot_dir"
echo "Setup complete. Run: bash scripts/build.sh"
