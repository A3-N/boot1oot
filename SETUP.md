# Setup and Local Builds

This document is for building Boot1oot locally. To use a release build, download
`boot1oot.iso` or `boot1oot.img` and see [ISO.md](ISO.md).

## Build Host

Use Linux or WSL with Debian, Ubuntu, or Kali package tooling.

Buildroot must live in one of these locations:

```text
./buildroot
../buildroot
~/buildroot
```

When the repo is under `/mnt/*` in WSL, generated Buildroot output defaults to:

```text
~/.cache/boot1oot/buildroot-output
```

## Dependencies

Install host packages inside Linux or WSL:

```sh
sudo apt update
sudo apt install -y \
  build-essential bc bison flex cpio file git libncurses-dev libelf-dev \
  patch perl python3 rsync tar unzip wget grub-common grub-pc-bin \
  grub-efi-amd64-bin xorriso mtools syslinux syslinux-common util-linux
```

## Setup

```sh
bash scripts/setup.sh
```

PowerShell via WSL:

```powershell
.\scripts\setup.ps1
```

Setup validates dependencies and clones Buildroot to `~/buildroot` if no
approved checkout exists.

## Build

Default ISO build:

```sh
bash scripts/build.sh
```

Select an artifact:

```sh
ARTIFACT=iso bash scripts/build.sh
ARTIFACT=img bash scripts/build.sh
ARTIFACT=both bash scripts/build.sh
```

PowerShell:

```powershell
.\scripts\build.ps1 -Artifact iso
.\scripts\build.ps1 -Artifact img
.\scripts\build.ps1 -Artifact both
```

Outputs:

```text
dist/boot1oot.iso
dist/boot1oot.img
```

The ISO is RAM-only. The IMG is a raw USB disk image with BIOS boot, UEFI boot,
and a FAT loot partition.

Change the USB loot partition size:

```sh
ARTIFACT=img LOOT_SIZE=128M bash scripts/build.sh
```

Clean generated output:

```sh
CLEAN=1 bash scripts/build.sh
```

## Regenerate Boot Artifacts

After a successful Buildroot build, regenerate only the ISO:

```sh
bash scripts/make-iso.sh
```

Regenerate only the USB image:

```sh
bash scripts/make-usb-img.sh
```

PowerShell wrappers:

```powershell
.\scripts\make-iso.ps1
.\scripts\make-usb-img.ps1
```

## Optional Local Config

Local build config can live at:

```text
config/boot1oot.local.env
```

Start from:

```sh
cp config/boot1oot.env.example config/boot1oot.local.env
```

Example:

```sh
BOOT1OOT_EXPORT_CONFIG=1
BOOT1OOT_BITLOCKER_RECOVERY_KEY=000000-000000-000000-000000-000000-000000-000000-000000
BOOT1OOT_LOOT_PASSPHRASE=change-this-for-fallback-archives
```

Use exported config only for lab images where embedding these values is
intentional. Normal release builds should leave `BOOT1OOT_EXPORT_CONFIG` unset
or set to `0`.

## Generated Files

Do not commit generated output:

```text
dist/
buildroot/
buildroot-output/
dl/
config/boot1oot.local.env
```
