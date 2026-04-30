#!/usr/bin/env bash

BOOT1OOT_BUILDROOT_URL="${BOOT1OOT_BUILDROOT_URL:-https://gitlab.com/buildroot.org/buildroot.git}"
BOOT1OOT_CLEAN_PATH="${BOOT1OOT_CLEAN_PATH:-/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin}"

boot1oot_sanitize_path() {
	export PATH="$BOOT1OOT_CLEAN_PATH"
}

boot1oot_repo_root() {
	cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd
}

boot1oot_abs_path() {
	local path="$1"

	case "$path" in
		~) path="$HOME" ;;
		~/*) path="$HOME/${path#~/}" ;;
	esac

	realpath -m "$path"
}

boot1oot_default_output_dir() {
	local repo_root="$1"

	case "$repo_root" in
		/mnt/*)
			printf '%s\n' "$HOME/.cache/boot1oot/buildroot-output"
			;;
		*)
			printf '%s\n' "$repo_root/buildroot-output"
			;;
	esac
}

boot1oot_default_dl_dir() {
	printf '%s\n' "$HOME/.cache/boot1oot/dl"
}

boot1oot_allowed_buildroot_dirs() {
	local repo_root="$1"

	printf '%s\n' \
		"$repo_root/buildroot" \
		"$repo_root/../buildroot" \
		"$HOME/buildroot"
}

boot1oot_is_allowed_buildroot_dir() {
	local repo_root="$1"
	local candidate
	local candidate_abs
	local allowed

	candidate_abs="$(boot1oot_abs_path "$2")"
	while IFS= read -r allowed; do
		[[ "$candidate_abs" == "$(boot1oot_abs_path "$allowed")" ]] && return 0
	done < <(boot1oot_allowed_buildroot_dirs "$repo_root")

	return 1
}

boot1oot_has_crlf() {
	local file="$1"

	[[ -f "$file" ]] && LC_ALL=C grep -q $'\r' "$file"
}

boot1oot_buildroot_problem() {
	local dir="$1"

	if [[ ! -d "$dir" ]]; then
		echo "missing"
		return 0
	fi

	if [[ ! -f "$dir/Makefile" || ! -f "$dir/support/scripts/mkmakefile" ]]; then
		echo "not a Buildroot checkout"
		return 0
	fi

	if boot1oot_has_crlf "$dir/support/scripts/mkmakefile" ||
		boot1oot_has_crlf "$dir/support/scripts/setlocalversion"; then
		echo "CRLF line endings"
		return 0
	fi

	echo ""
}

boot1oot_find_buildroot() {
	local repo_root="$1"
	local dir
	local problem

	while IFS= read -r dir; do
		problem="$(boot1oot_buildroot_problem "$dir")"
		if [[ -z "$problem" ]]; then
			boot1oot_abs_path "$dir"
			return 0
		fi
	done < <(boot1oot_allowed_buildroot_dirs "$repo_root")

	return 1
}

boot1oot_resolve_buildroot_for_build() {
	local repo_root="$1"
	local requested="${2:-}"
	local problem

	if [[ -n "$requested" ]]; then
		if ! boot1oot_is_allowed_buildroot_dir "$repo_root" "$requested"; then
			cat >&2 <<EOF
Unsupported Buildroot path:
  $requested

Allowed locations only:
  $repo_root/buildroot
  $repo_root/../buildroot
  ~/buildroot
EOF
			return 1
		fi

		problem="$(boot1oot_buildroot_problem "$requested")"
		if [[ -n "$problem" ]]; then
			echo "Buildroot at $requested is not usable: $problem" >&2
			echo "Run: bash scripts/setup.sh" >&2
			return 1
		fi

		boot1oot_abs_path "$requested"
		return 0
	fi

	if boot1oot_find_buildroot "$repo_root"; then
		return 0
	fi

	echo "No usable Buildroot checkout found. Run: bash scripts/setup.sh" >&2
	return 1
}

boot1oot_missing_commands() {
	local missing=()
	local commands=(
		awk
		bash
		bc
		bison
		cpio
		file
		flex
		gcc
		g++
		git
		grub-mkrescue
		grub-mkstandalone
		make
		mcopy
		mformat
		patch
		perl
		python3
		realpath
		rsync
		sfdisk
		syslinux
		tar
		unzip
		wget
		xorriso
	)
	local cmd

	for cmd in "${commands[@]}"; do
		if ! command -v "$cmd" >/dev/null 2>&1; then
			missing+=("$cmd")
		fi
	done

	if ((${#missing[@]})); then
		printf '%s\n' "${missing[@]}"
	fi
}

boot1oot_missing_headers() {
	local missing=()
	local headers=(
		"/usr/include/gelf.h:libelf-dev"
	)
	local item
	local header
	local package

	for item in "${headers[@]}"; do
		header="${item%%:*}"
		package="${item#*:}"
		if [[ ! -f "$header" ]]; then
			missing+=("$package ($header)")
		fi
	done

	if ((${#missing[@]})); then
		printf '%s\n' "${missing[@]}"
	fi
}

boot1oot_print_dependency_help() {
	cat <<'EOF'
Install missing Debian/Ubuntu/Kali packages with:

  sudo apt update
  sudo apt install -y build-essential bc bison flex cpio file git libncurses-dev \
    libelf-dev patch perl python3 rsync tar unzip wget util-linux \
    grub-common grub-pc-bin grub-efi-amd64-bin xorriso mtools syslinux \
    syslinux-common

EOF
}

boot1oot_clone_buildroot() {
	local repo_root="$1"
	local target="$HOME/buildroot"

	if [[ -e "$target" ]]; then
		echo "Cannot clone Buildroot because $target already exists but is not usable." >&2
		echo "Move it aside or repair it, then run setup again." >&2
		return 1
	fi

	echo "Cloning Buildroot to $target" >&2
	git -c core.autocrlf=false clone "$BOOT1OOT_BUILDROOT_URL" "$target"

	if [[ -n "$(boot1oot_buildroot_problem "$target")" ]]; then
		echo "Buildroot clone completed but failed validation." >&2
		return 1
	fi

	boot1oot_abs_path "$target"
}

boot1oot_clean_output_dir() {
	local repo_root="$1"
	local output_dir="$2"
	local repo_abs
	local output_abs

	repo_abs="$(boot1oot_abs_path "$repo_root")"
	output_abs="$(boot1oot_abs_path "$output_dir")"

	case "$output_abs" in
		"$repo_abs"/buildroot-output|"$repo_abs"/buildroot-output-*|"$HOME"/.cache/boot1oot/buildroot-output)
			echo "Removing generated Buildroot output: $output_abs"
			rm -rf -- "$output_abs"
			;;
		*)
			echo "Refusing to clean unsupported output directory: $output_abs" >&2
			echo "Only these generated output paths can be cleaned automatically:" >&2
			echo "  $repo_abs/buildroot-output" >&2
			echo "  $repo_abs/buildroot-output-*" >&2
			echo "  $HOME/.cache/boot1oot/buildroot-output" >&2
			return 1
			;;
	esac
}

boot1oot_output_needs_clean_for_musl() {
	local output_dir="$1"

	if [[ -f "$output_dir/.config" ]] &&
		grep -q '^BR2_TOOLCHAIN_BUILDROOT_GLIBC=y$' "$output_dir/.config"; then
		return 0
	fi

	compgen -G "$output_dir/build/glibc-*" >/dev/null
}
