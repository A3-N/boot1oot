#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo_root/scripts/lib/buildroot-common.sh"

output_dir="${OUTPUT_DIR:-$(boot1oot_default_output_dir "$repo_root")}"
dist_dir="${DIST_DIR:-$repo_root/dist}"
iso_root="${ISO_ROOT:-$output_dir/iso-root}"
iso_path="${ISO_PATH:-$dist_dir/boot1oot.iso}"
loot_size="${LOOT_SIZE:-64M}"
loot_img="$output_dir/images/loot.img"
loot_sentinel="$output_dir/images/boot1oot-loot.sentinel"

kernel="$output_dir/images/bzImage"
initramfs="$output_dir/images/rootfs.cpio.gz"

if [[ ! -f "$kernel" ]]; then
	echo "Missing kernel: $kernel" >&2
	exit 1
fi

if [[ ! -f "$initramfs" ]]; then
	echo "Missing initramfs: $initramfs" >&2
	exit 1
fi

if ! command -v grub-mkrescue >/dev/null 2>&1; then
	echo "grub-mkrescue is required to create the BIOS/UEFI ISO." >&2
	echo "On Debian/Ubuntu/WSL: sudo apt install grub-common grub-pc-bin grub-efi-amd64-bin xorriso mtools" >&2
	exit 1
fi

if [[ "$loot_size" != "0" ]] && ! command -v mformat >/dev/null 2>&1; then
	echo "mformat is required to create the persistent loot partition image." >&2
	echo "On Debian/Ubuntu/WSL: sudo apt install mtools" >&2
	exit 1
fi

rm -rf "$iso_root"
mkdir -p "$iso_root/boot/grub/fonts" "$dist_dir"
cp "$kernel" "$iso_root/boot/bzImage"
cp "$initramfs" "$iso_root/boot/rootfs.cpio.gz"

grub_font=""
for font in \
	/usr/share/grub/unicode.pf2 \
	/usr/share/grub/ascii.pf2 \
	/boot/grub/fonts/unicode.pf2 \
	/boot/grub/fonts/ascii.pf2; do
	if [[ -f "$font" ]]; then
		grub_font="$font"
		break
	fi
done

if [[ -n "$grub_font" ]]; then
	cp "$grub_font" "$iso_root/boot/grub/fonts/unicode.pf2"
fi

grub_extra_args=()
if [[ "$loot_size" != "0" ]]; then
	rm -f "$loot_img" "$loot_sentinel"
	truncate -s "$loot_size" "$loot_img"
	mformat -i "$loot_img" -F -v BOOT1OOT_LOOT ::
	cat > "$loot_sentinel" <<'EOF'
Boot1oot persistent loot partition.
Do not remove this file; boot1oot uses it to identify /loot safely.
EOF
	mcopy -i "$loot_img" "$loot_sentinel" ::/.boot1oot-loot
	grub_extra_args=(-append_partition 4 0x0c "$loot_img" -partition_cyl_align all)
fi

cat > "$iso_root/boot/grub/grub.cfg" <<'EOF'
set pager=0
set timeout_style=menu
terminal_input console
set gfxmode=1024x768,800x600,640x480,auto
set gfxpayload=keep

insmod all_video
if [ "$grub_platform" = "efi" ]; then
	insmod efi_gop
	insmod efi_uga
fi
if [ "$grub_platform" = "pc" ]; then
	insmod vbe
fi
insmod video_bochs
insmod video_cirrus
insmod gfxterm

if loadfont /boot/grub/fonts/unicode.pf2; then
	terminal_output gfxterm
else
	terminal_output console
	set gfxpayload=text
fi

set default=0
set timeout=3

menuentry "Boot1oot Linux - RAM only" {
	linux /boot/bzImage console=ttyS0,115200n8 console=tty0 earlyprintk=vga,keep loglevel=7 panic=10
	initrd /boot/rootfs.cpio.gz
}
EOF

grub-mkrescue -o "$iso_path" "$iso_root" "${grub_extra_args[@]}" >/dev/null
printf 'ISO written to %s\n' "$iso_path"
if [[ "$loot_size" != "0" ]]; then
	printf 'Persistent loot partition appended: %s (%s, label BOOT1OOT_LOOT)\n' "$loot_img" "$loot_size"
fi
