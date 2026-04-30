# Boot and USB Usage

Boot1oot ships as two artifacts:

- `boot1oot.iso` for VM or virtual CD/DVD boot.
- `boot1oot.img` for raw USB writing with persistent loot storage.

The live root filesystem is RAM-only in both modes. The USB image adds a
writable FAT partition labeled `B1OOT_LOOT`.

## Before Booting

- Use a dedicated USB drive; writing `boot1oot.img` overwrites it.
- Disable Secure Boot if the unsigned image is blocked.
- Keep BitLocker recovery keys available for systems that enforce recovery
  after external boot or firmware changes.
- Prefer a full Windows shutdown before read-write filesystem work:

```powershell
shutdown /s /t 0
```

## Write USB Media

### Windows

Use Rufus or another raw-image writer:

1. Select the USB drive.
2. Select `boot1oot.img`.
3. Use raw image mode when prompted.
4. Start the write.

### Linux

```sh
lsblk
sudo umount /dev/sdX*
sudo dd if=dist/boot1oot.img of=/dev/sdX bs=4M status=progress oflag=sync
sync
```

Replace `/dev/sdX` with the whole USB disk, not a partition.

### macOS

```sh
diskutil list
diskutil unmountDisk /dev/diskN
sudo dd if=dist/boot1oot.img of=/dev/rdiskN bs=4m status=progress
sync
diskutil eject /dev/diskN
```

## Boot Options

For physical machines, use the firmware one-time boot menu and choose the USB
entry. On UEFI systems, prefer the entry beginning with `UEFI:` when both legacy
and UEFI entries are shown.

From Windows, Shift-Restart can also expose firmware boot entries:

1. Hold `Shift` while selecting `Restart`.
2. Choose `Use a device`.
3. Select the USB drive.

For VMs, attach `boot1oot.iso` as a virtual CD/DVD and boot from it.

## After Boot

```sh
boot1oot scan
boot1oot mount -r
boot1oot loot
```

For write-capable filesystem work:

```sh
boot1oot mount -rw
```

Before returning to Windows:

```sh
cd /
boot1oot unmount
poweroff
```

## Retrieving Loot

USB image boots write to:

```text
B1OOT_LOOT:/boot1oot-loot-<time>-<pid>
```

ISO boots or systems without writable USB loot storage use the encrypted fallback:

```text
C:\Users\Public\Boot1oot\boot1oot-loot-<time>-<pid>.tar.enc
```

Decrypt fallback archives with:

```sh
openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000 \
  -pass pass:boot1oot \
  -in boot1oot-loot-<time>-<pid>.tar.enc \
  -out boot1oot-loot-<time>-<pid>.tar

tar -xf boot1oot-loot-<time>-<pid>.tar
```

Use your configured `BOOT1OOT_LOOT_PASSPHRASE` instead of `boot1oot` if you set
one before collection.
