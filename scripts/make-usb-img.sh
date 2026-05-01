#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo_root/scripts/lib/buildroot-common.sh"

output_dir="${OUTPUT_DIR:-$(boot1oot_default_output_dir "$repo_root")}"
dist_dir="${DIST_DIR:-$repo_root/dist}"
usb_img="${USB_IMG_PATH:-$dist_dir/boot1oot.img}"
work_dir="${USB_IMG_WORK_DIR:-$output_dir/usb-img}"
bios_size="${USB_BIOS_SIZE:-${USB_BIOS_BOOT_SIZE:-128M}}"
efi_size="${USB_EFI_SIZE:-128M}"
loot_size="${LOOT_SIZE:-768M}"
bios_label="${USB_BIOS_LABEL:-B1OOTBIOS}"
efi_label="${USB_EFI_LABEL:-BOOT1OOTEFI}"
loot_label="${LOOT_LABEL:-B1OOT_LOOT}"
syslinux_mbr="${SYSLINUX_GPTMBR:-}"
loot_sentinel="$work_dir/boot1oot-loot.sentinel"

kernel="$output_dir/images/bzImage"
initramfs="$output_dir/images/rootfs.cpio.gz"

bytes_from_size() {
	local value="$1"
	local number
	local suffix

	if [[ "$value" =~ ^([0-9]+)([KkMmGg])?$ ]]; then
		number="${BASH_REMATCH[1]}"
		suffix="${BASH_REMATCH[2]:-}"
	else
		echo "Unsupported size '$value'. Use an integer with optional K, M, or G suffix." >&2
		return 1
	fi

	case "$suffix" in
		[Kk]) printf '%s\n' "$((number * 1024))" ;;
		[Mm]) printf '%s\n' "$((number * 1024 * 1024))" ;;
		[Gg]) printf '%s\n' "$((number * 1024 * 1024 * 1024))" ;;
		*) printf '%s\n' "$number" ;;
	esac
}

align_sectors() {
	local sectors="$1"
	local align="${2:-2048}"

	printf '%s\n' "$((((sectors + align - 1) / align) * align))"
}

require_command() {
	local command="$1"
	local package="$2"

	if ! command -v "$command" >/dev/null 2>&1; then
		echo "$command is required to create the USB disk image." >&2
		echo "On Debian/Ubuntu/WSL: sudo apt install $package" >&2
		exit 1
	fi
}

if [[ ! -f "$kernel" ]]; then
	echo "Missing kernel: $kernel" >&2
	exit 1
fi

if [[ ! -f "$initramfs" ]]; then
	echo "Missing initramfs: $initramfs" >&2
	exit 1
fi

require_command grub-mkstandalone grub-common
require_command syslinux syslinux
require_command sfdisk util-linux
require_command mformat mtools
require_command mcopy mtools
require_command mmd mtools

if [[ -z "$syslinux_mbr" ]]; then
	for candidate in \
		/usr/lib/syslinux/mbr/gptmbr.bin \
		/usr/lib/SYSLINUX/gptmbr.bin; do
		if [[ -f "$candidate" ]]; then
			syslinux_mbr="$candidate"
			break
		fi
	done
fi

if [[ ! -f "$syslinux_mbr" ]]; then
	echo "Missing SYSLINUX GPT MBR image: gptmbr.bin" >&2
	echo "On Debian/Ubuntu/WSL: sudo apt install syslinux syslinux-common" >&2
	echo "Set SYSLINUX_GPTMBR if your distribution stores gptmbr.bin elsewhere." >&2
	exit 1
fi

bios_bytes="$(bytes_from_size "$bios_size")"
efi_bytes="$(bytes_from_size "$efi_size")"
loot_bytes="$(bytes_from_size "$loot_size")"
if ((bios_bytes <= 0 || efi_bytes <= 0 || loot_bytes <= 0)); then
	echo "USB image partition sizes must be greater than zero." >&2
	exit 1
fi
sector_size=512
alignment=2048

bios_sectors="$(align_sectors "$(((bios_bytes + sector_size - 1) / sector_size))" "$alignment")"
efi_sectors="$(align_sectors "$(((efi_bytes + sector_size - 1) / sector_size))" "$alignment")"
loot_sectors="$(align_sectors "$(((loot_bytes + sector_size - 1) / sector_size))" "$alignment")"
bios_start="$alignment"
efi_start="$((bios_start + bios_sectors))"
loot_start="$((efi_start + efi_sectors))"
end_padding="$alignment"
total_sectors="$((loot_start + loot_sectors + end_padding))"
total_bytes="$((total_sectors * sector_size))"
bios_offset="$((bios_start * sector_size))"
efi_offset="$((efi_start * sector_size))"
loot_offset="$((loot_start * sector_size))"

rm -rf "$work_dir"
mkdir -p "$work_dir" "$dist_dir"
rm -f "$usb_img"
truncate -s "$total_bytes" "$usb_img"

sfdisk "$usb_img" >/dev/null <<EOF
label: gpt
unit: sectors

start=$bios_start, size=$bios_sectors, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, attrs=LegacyBIOSBootable, name="BOOT1OOT_BIOS"
start=$efi_start, size=$efi_sectors, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="BOOT1OOT_EFI"
start=$loot_start, size=$loot_sectors, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="BOOT1OOT_LOOT"
EOF

cat > "$work_dir/grub-usb.cfg" <<EOF
set pager=0
set timeout_style=menu
set default=0
set timeout=3

insmod part_gpt
insmod fat
insmod search
insmod search_label
insmod search_fs_file

if search --no-floppy --label "$efi_label" --set=root; then
	echo "Using Boot1oot EFI partition: \$root"
else
	search --no-floppy --file /boot/bzImage --set=root
fi

menuentry "Boot1oot Linux - RAM only" {
	linux /boot/bzImage console=ttyS0,115200n8 console=tty0 earlyprintk=vga,keep loglevel=7 panic=10
	initrd /boot/rootfs.cpio.gz
}
EOF

cat > "$work_dir/syslinux.cfg" <<'EOF'
DEFAULT boot1oot
PROMPT 0
TIMEOUT 30

LABEL boot1oot
	LINUX /boot/bzImage
	INITRD /boot/rootfs.cpio.gz
	APPEND console=ttyS0,115200n8 console=tty0 earlyprintk=vga,keep loglevel=7 panic=10
EOF

grub-mkstandalone \
	-O x86_64-efi \
	-o "$work_dir/BOOTX64.EFI" \
	"boot/grub/grub.cfg=$work_dir/grub-usb.cfg" >/dev/null

mformat -i "$usb_img@@$bios_offset" -F -v "$bios_label" ::
mmd -i "$usb_img@@$bios_offset" ::/boot ::/boot/syslinux
mcopy -i "$usb_img@@$bios_offset" "$kernel" ::/boot/bzImage
mcopy -i "$usb_img@@$bios_offset" "$initramfs" ::/boot/rootfs.cpio.gz
mcopy -i "$usb_img@@$bios_offset" "$work_dir/syslinux.cfg" ::/boot/syslinux/syslinux.cfg
syslinux --install --offset "$bios_offset" --directory /boot/syslinux "$usb_img" >/dev/null
dd if="$syslinux_mbr" of="$usb_img" bs=440 count=1 conv=notrunc status=none

mformat -i "$usb_img@@$efi_offset" -F -v "$efi_label" ::
mmd -i "$usb_img@@$efi_offset" ::/EFI ::/EFI/BOOT ::/boot ::/boot/grub
mcopy -i "$usb_img@@$efi_offset" "$work_dir/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$usb_img@@$efi_offset" "$kernel" ::/boot/bzImage
mcopy -i "$usb_img@@$efi_offset" "$initramfs" ::/boot/rootfs.cpio.gz
mcopy -i "$usb_img@@$efi_offset" "$work_dir/grub-usb.cfg" ::/boot/grub/grub.cfg

mformat -i "$usb_img@@$loot_offset" -F -v "$loot_label" ::
cat > "$loot_sentinel" <<'EOF'
Boot1oot persistent loot partition.
Do not remove this file; boot1oot uses it to identify /loot safely.
EOF
mcopy -i "$usb_img@@$loot_offset" "$loot_sentinel" ::/.boot1oot-loot

printf 'USB image written to %s\n' "$usb_img"
printf '  BIOS partition: %s (%s, label %s)\n' "$bios_size" "$((bios_sectors * sector_size)) bytes" "$bios_label"
printf '  EFI partition:  %s (%s, label %s)\n' "$efi_size" "$((efi_sectors * sector_size)) bytes" "$efi_label"
printf '  Loot partition: %s (%s, label %s)\n' "$loot_size" "$((loot_sectors * sector_size)) bytes" "$loot_label"
