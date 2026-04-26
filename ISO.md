# ISO Usage

This guide covers writing the Boot1oot ISO to USB media and booting it on
physical machines or virtual machines.

Boot1oot is a RAM-only live ISO. It does not need persistence and does not
write to the USB after boot.

## Before You Boot

- Back up anything important on the USB drive. Flashing the ISO overwrites it.
- Use a USB drive you can dedicate to Boot1oot.
- If Secure Boot is enabled, the unsigned Boot1oot ISO may not boot. Disable
  Secure Boot or use a firmware profile that allows unsigned external media.
- If Windows uses BitLocker, external boot or firmware changes may trigger a
  BitLocker recovery prompt on the next Windows boot. Keep the recovery key
  available for managed-device audit work.
- Prefer a full shutdown before testing physical hardware:

```powershell
shutdown /s /t 0
```

Windows Fast Startup can leave disks in a hibernated state, which can make
read-write NTFS work unsafe or fail.

## Windows USB Flashing

### Rufus

1. Insert the USB drive.
2. Open Rufus.
3. Select the USB drive under `Device`.
4. Select `boot1oot.iso` under `Boot selection`.
5. Use the default partition scheme Rufus suggests for the target firmware.
   - `GPT` for modern UEFI systems.
   - `MBR` for legacy BIOS or older mixed systems.
6. Start the write process.
7. If Rufus asks between ISO mode and DD mode, use DD mode if ISO mode does not
   boot on the target hardware.

Rufus normally needs administrator approval because it writes directly to a
physical USB device.

### Windows Built-In ISO Mounting

Windows can mount an ISO, but mounting is not the same as making a bootable USB.
Use Rufus or another raw-image writer for USB boot media.

For VMware, Hyper-V, or another hypervisor, attach `boot1oot.iso` directly as a
virtual CD/DVD image.

## Linux USB Flashing

Find the USB device:

```sh
lsblk
```

Unmount any mounted partitions on that USB:

```sh
sudo umount /dev/sdX*
```

Write the ISO:

```sh
sudo dd if=dist/boot1oot.iso of=/dev/sdX bs=4M status=progress oflag=sync
sync
```

Replace `/dev/sdX` with the whole USB disk, not a partition such as
`/dev/sdX1`.

## macOS USB Flashing

List disks:

```sh
diskutil list
```

Unmount the USB disk:

```sh
diskutil unmountDisk /dev/diskN
```

Write the ISO:

```sh
sudo dd if=dist/boot1oot.iso of=/dev/rdiskN bs=4m status=progress
sync
```

Eject when done:

```sh
diskutil eject /dev/diskN
```

Use `rdiskN` for the write path because it is the raw disk device and is usually
faster. Replace `N` with the USB disk number from `diskutil list`.

## Booting From Windows

### Shift-Restart

1. Hold `Shift`.
2. Click `Restart` from the Windows power menu.
3. Select `Use a device`.
4. Pick the USB drive or USB UEFI entry.

This is the least invasive OS-side path when the firmware exposes USB boot
entries to Windows Recovery.

### Advanced Startup

1. Open Settings.
2. Go to `System` -> `Recovery`.
3. Select `Advanced startup`.
4. Choose `Restart now`.
5. Select `Use a device`.
6. Pick the USB drive.

On older Windows layouts, this may be under `Update & Security` -> `Recovery`.

### Reboot Directly To Firmware

From an elevated PowerShell or Command Prompt:

```powershell
shutdown /r /fw /t 0
```

This requires administrator rights. It asks UEFI firmware to open setup on the
next reboot. It only works on UEFI systems that support the firmware reboot
request.

## Firmware Boot Options

### One-Time Boot Menu

Most systems have a one-time boot menu key shown briefly during startup.
Common keys include:

- `F12`
- `F11`
- `F10`
- `Esc`
- `F8`

Choose the USB device. On UEFI machines, prefer the entry that starts with
`UEFI:` if both legacy and UEFI entries are shown.

### UEFI/BIOS Boot Order

Enter firmware setup, then move the USB device above the internal disk in the
boot order. Common setup keys include:

- `Del`
- `F2`
- `F10`
- `Esc`

After the audit, restore the internal Windows disk as the first boot option.

## Booting An ISO In A VM

Attach `boot1oot.iso` as a virtual CD/DVD image and put the virtual CD/DVD drive
above the virtual hard disk in the VM boot order.

For VMware testing:

1. Edit VM settings.
2. Set CD/DVD to use `boot1oot.iso`.
3. Connect it at power on.
4. Boot with BIOS or UEFI firmware.

Boot1oot supports both BIOS and UEFI boot paths.

## After Boot

The system drops to a minimal `boot1oot` shell. Start with:

```sh
boot1oot scan
boot1oot mount
```

For write-capable Windows filesystem work:

```sh
boot1oot mount -rw
```

Before returning to Windows:

```sh
cd /
boot1oot unmount
poweroff
```
