# boot1oot

Boot1oot is a minimal RAM-only Linux live ISO for auditing Windows filesystems
from outside the installed OS. It is built to boot alongside Windows, discover
Windows volumes, and mount them for inspection without persistence.

Most users should download the release ISO and boot it directly. Build and setup
instructions live in [SETUP.md](SETUP.md).

## ISO Profile

- x86_64 Linux kernel
- BIOS and UEFI boot support
- BusyBox init and shell
- RAM-only initramfs root filesystem
- no configured persistence
- no kernel networking support
- no editors
- VMware-friendly storage drivers: SATA/AHCI, NVMe, SCSI, VMware PVSCSI, VirtIO

## Baked-In Tools

- `boot1oot`: Windows volume scanner, mounter, BitLocker wrapper, and unmount
  helper.
- `dislocker-fuse`: unlocks BitLocker volumes through FUSE.
- `dislocker-metadata`: prints BitLocker metadata before unlock attempts.
- `ntfs-3g`: mounts dislocker's decrypted `dislocker-file`.
- kernel `ntfs3`: mounts normal unencrypted NTFS partitions.
- BusyBox userland: minimal shell and basic Unix commands.

The ISO credits dislocker from `github.com/Aorimn/dislocker` and chntpw from
`pogostick.net/~pnh/ntpasswd`.

## What It Does

`boot1oot scan` identifies candidate Windows filesystems by boot-sector
signatures:

- `NTFS    ` for normal NTFS
- `-FVE-FS-` for BitLocker/FVE

`boot1oot mount` mounts every detected Windows candidate under:

```text
/mnt/windows/<device>
```

Examples:

```text
/mnt/windows/sda3
/mnt/windows/nvme0n1p3
```

Mounted NTFS volumes are classified from filesystem contents as:

- `windows-root`
- `windows-recovery`
- `windows-boot`
- `windows-data`
- `unknown-ntfs`

For BitLocker volumes, Boot1oot checks metadata with `dislocker-metadata`,
unlocks with `dislocker-fuse`, validates that the decrypted virtual volume is
NTFS, then mounts the exposed `dislocker-file` with `ntfs-3g`.

## Commands

Scan without mounting:

```sh
boot1oot scan
```

Mount read-only. This is the default:

```sh
boot1oot mount
boot1oot mount -r
```

Mount read-write:

```sh
boot1oot mount -rw
```

Only process BitLocker candidates:

```sh
boot1oot dislocker
boot1oot dislocker -rw
```

Unmount Boot1oot-managed Windows and dislocker mountpoints:

```sh
cd /
boot1oot unmount
```

Shut down:

```sh
poweroff
```

## Output

Boot1oot prefixes its own status lines:

- green `[+]`: success
- red `[!]`: failure
- blue `[*]`: informational

Output from wrapped tools, such as dislocker's own `[INFO]` lines, is left
unchanged.

## VMware Test Notes

Create a VM with:

- Guest OS: Other Linux 5.x or later kernel, 64-bit
- Firmware: BIOS or UEFI
- Memory: 256 MB minimum
- Network adapter: removed
- CD/DVD: release `boot1oot.iso`

The ISO boots into a minimal shell after showing the Boot1oot banner.
