# boot1oot

Boot1oot is a minimal RAM-only Linux live ISO for auditing Windows filesystems
from outside the installed OS. It is built to boot alongside Windows, discover
Windows volumes, and mount them for inspection without persistence.

Most users should download the release ISO and boot it directly. Build and setup
instructions live in [SETUP.md](SETUP.md). USB flashing and boot instructions
live in [ISO.md](ISO.md).

![alt text](img/boot1oot.gif)

## ISO Profile

- x86_64 Linux kernel
- BIOS and UEFI boot support
- BusyBox init with an interactive bash console
- RAM-only initramfs root filesystem
- optional `/loot` artifact storage partition on USB-written images
- no kernel networking support
- no editors
- VMware-friendly storage drivers: SATA/AHCI, NVMe, SCSI, VMware PVSCSI, VirtIO

## Baked-In Tools

- `boot1oot`: Windows volume scanner, mounter, BitLocker wrapper, and unmount
  helper.
- `dislocker-fuse`: unlocks BitLocker volumes through FUSE.
- `dislocker-metadata`: prints BitLocker metadata before unlock attempts.
- `chntpw`: interactive offline Windows SAM/registry editor and user lister.
- `samusrgrp`: local SAM group membership helper for manual workflows.
- `sampasswd`: noninteractive local SAM password reset helper.
- `reged`: offline registry export/import/editor helper.
- `ntfs-3g`: mounts dislocker's decrypted `dislocker-file`.
- `openssl`: encrypts Windows-hosted fallback loot archives.
- kernel `ntfs3`: mounts normal unencrypted NTFS partitions.
- `bash`: console shell with `boot1oot` command in path.
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
![alt text](img/booty.gif)

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

The live OS root filesystem remains RAM-only. `boot1oot loot` stages offline
Windows-at-rest audit artifacts under `/tmp`, tries to write them to the USB
loot partition, then unmounts `/loot`. If no writable USB loot partition is
available, it writes an encrypted archive under `C:\Users\Public\Boot1oot`.

## Commands

The console supports tab completion for `boot1oot` subcommands:

```sh
boot1oot <tab>
```

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

Collect offline Windows secret material at rest:

```sh
boot1oot loot
```

The command collects local registry hives (`SAM`, `SYSTEM`, `SECURITY`,
`SOFTWARE`, `DEFAULT`) with transaction logs, RegBack if present, machine/user
DPAPI material, Credential Manager/Vault paths, local crypto material, and
`NTDS.dit` when the host is a domain controller. It does not collect live
runtime memory such as `lsass.exe`.

USB-written images save the staged directory to `/loot/<collection-id>` and
unmount `/loot` afterward. Virtual ISO boots normally have no writable USB loot
partition, so the fallback creates `/tmp/<collection-id>.tar.enc` with OpenSSL
AES-256-CBC/PBKDF2 and copies it to `C:\Users\Public\Boot1oot`. Set
`BOOT1OOT_LOOT_PASSPHRASE` for the fallback archive passphrase; if unset,
Boot1oot uses the built-in fallback passphrase `boot1oot`.

## Extracting Loot

![alt text](img/boot-T.gif)

USB path:

1. Boot Windows, Linux, or macOS after shutting Boot1oot down cleanly.
2. Open the USB partition labeled `BOOT1OOT_LOOT`.
3. Copy the newest `boot1oot-loot-<time>-<pid>` directory.

The USB loot path is a normal directory, not encrypted by Boot1oot.

Fallback path:

If Boot1oot could not mount the USB loot partition, it writes an encrypted file
to the audited Windows filesystem:

```text
C:\Users\Public\Boot1oot\boot1oot-loot-<time>-<pid>.tar.enc
```

Decrypt it on Linux/macOS/WSL:

```sh
openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000 \
  -pass pass:boot1oot \
  -in boot1oot-loot-<time>-<pid>.tar.enc \
  -out boot1oot-loot-<time>-<pid>.tar

tar -xf boot1oot-loot-<time>-<pid>.tar
```

Use `BOOT1OOT_LOOT_PASSPHRASE` as the passphrase. If that was not configured,
use the built-in fallback passphrase:

```text
boot1oot
```

If `BOOT1OOT_LOOT_PASSPHRASE` was set to a custom value when the archive was
created, replace `-pass pass:boot1oot` with your custom passphrase or omit
`-pass ...` and let OpenSSL prompt for it.

Run Impacket secretsdump against the extracted local hives:

```sh
cd boot1oot-loot-<time>-<pid>/windows/Windows/System32/config
secretsdump.py -sam SAM -system SYSTEM -security SECURITY LOCAL
```

Export the relevant SAM user/group branches with `reged`, then show the
decoded local SAM user table:

```sh
boot1oot users
```

Launch chntpw interactively. This lists users first, prompts for a username,
then runs chntpw with the detected `SAM`, `SYSTEM`, and `SECURITY` hives:

```sh
boot1oot chntpw
```

`boot1oot users` mounts read-only if needed, exports SAM branches with
`reged -x`, copies `SAM` to `/tmp` for read-only decoding, and then shows the
decoded user table. `ADMIN` and `*BLANK*` come from the bundled SAM decoder.

```text
reged -x SAM HKEY_LOCAL_MACHINE\SAM \SAM\Domains\Account\Users /tmp/boot1oot-sam-users.reg
```

The interactive edit path mounts read-write if needed, then passes the selected
user plus the detected `SAM`, `SYSTEM`, and `SECURITY` hives to upstream chntpw.

The raw `chntpw`, `sampasswd`, `samusrgrp`, and `reged` tools are also included
for manual research workflows.

Unmount Boot1oot-managed Windows and dislocker mountpoints:

```sh
cd /
boot1oot unmount
```

Shut down:

```sh
poweroff
```

## VMware Test Notes

Create a VM with:

- Guest OS: Other Linux 5.x or later kernel, 64-bit
- Firmware: BIOS or UEFI
- Memory: 256 MB minimum
- Network adapter: removed
- CD/DVD: release `boot1oot.iso`

The ISO boots into a minimal shell after showing the Boot1oot banner.
