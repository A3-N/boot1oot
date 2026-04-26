# Setup and Local Builds

This document is for building Boot1oot locally. If you only want to use the ISO,
download the release image and read [README.md](README.md).

Boot1oot builds through Buildroot. The host system needs enough tooling to build
a Linux kernel, Buildroot host tools, GRUB rescue media, and the target packages
selected by the external tree.

## Supported Build Host

- Linux, or WSL on Windows
- Debian, Ubuntu, or Kali are the tested package-family targets
- Buildroot must live in one of these approved locations:
  - `./buildroot`
  - `../buildroot`
  - `~/buildroot`

When the repository is under `/mnt/*` in WSL, generated Buildroot output defaults
to `~/.cache/boot1oot/buildroot-output` instead of the Windows filesystem. This
keeps large compiler output off `/mnt/c`. Downloads are cached in
`~/.cache/boot1oot/dl`.

The setup and build scripts sanitize WSL's `PATH` before invoking Buildroot.
This avoids Buildroot failures caused by Windows path entries such as
`C:\Program Files\...` being appended inside WSL.

## Host Dependencies

Install these inside Linux or WSL:

```sh
sudo apt update
sudo apt install -y \
  build-essential bc bison flex cpio file git libncurses-dev libelf-dev \
  patch perl python3 rsync tar unzip wget grub-common grub-pc-bin \
  grub-efi-amd64-bin xorriso mtools
```

Why these are needed:

- `build-essential`, `gcc`, `g++`, `make`: compiler toolchain for Buildroot host
  utilities and the target cross-toolchain.
- `bc`, `bison`, `flex`, `perl`, `python3`: Linux kernel and Buildroot build
  helpers.
- `cpio`, `rsync`, `tar`, `unzip`, `wget`, `file`, `patch`: source extraction,
  patching, rootfs generation, and downloads.
- `libncurses-dev`: Buildroot/Kconfig menu tooling.
- `libelf-dev`: required by Linux `objtool` on x86 builds.
- `grub-common`, `grub-pc-bin`, `grub-efi-amd64-bin`, `xorriso`, `mtools`:
  BIOS/UEFI ISO generation through `grub-mkrescue`.

## Run Setup

From Linux or WSL:

```sh
bash scripts/setup.sh
```

From PowerShell using WSL:

```powershell
.\scripts\setup.ps1
```

To use a specific WSL distro:

```powershell
.\scripts\setup.ps1 -Distro Kali-linux
```

Setup checks host tools, validates Buildroot line endings, and searches only the
approved Buildroot locations. If no usable Buildroot checkout exists, setup
clones a clean copy to `~/buildroot` with `core.autocrlf=false`.

## Build the ISO

From Linux or WSL:

```sh
bash scripts/build.sh
```

From PowerShell using WSL:

```powershell
.\scripts\build.ps1
```

To force one approved Buildroot location:

```sh
BUILDROOT_DIR=~/buildroot bash scripts/build.sh
```

From PowerShell:

```powershell
.\scripts\build.ps1 -BuildrootDir ~/buildroot
```

If the toolchain configuration changes, clean generated output once:

```sh
CLEAN=1 bash scripts/build.sh
```

From PowerShell:

```powershell
.\scripts\build.ps1 -Clean
```

The finished ISO is written to:

```text
dist/boot1oot.iso
```

By default, the ISO also includes a 64 MiB FAT loot partition image appended to
the hybrid ISO for USB artifact storage. Override its size with:

```sh
LOOT_SIZE=128M bash scripts/build.sh
```

Disable the appended loot partition with:

```sh
LOOT_SIZE=0 bash scripts/build.sh
```

## Regenerate Only the ISO

After bootloader/menu-only changes:

```sh
bash scripts/make-iso.sh
```

From PowerShell:

```powershell
.\scripts\make-iso.ps1
```

## Local Build Config

Optional local build config lives at:

```text
config/boot1oot.local.env
```

That file is ignored by Git because it may contain BitLocker recovery keys.
Start from:

```sh
cp config/boot1oot.env.example config/boot1oot.local.env
```

Example:

```sh
BOOT1OOT_EXPORT_CONFIG=1
BOOT1OOT_BITLOCKER_RECOVERY_KEY=000000-000000-000000-000000-000000-000000-000000-000000
BOOT1OOT_LOOT_PASSPHRASE=change-this-for-encrypted-fallback-archives
```

When `BOOT1OOT_EXPORT_CONFIG=1`, the build passes the key into the `boot1oot`
binary and writes it into the ISO environment as
`BOOT1OOT_BITLOCKER_RECOVERY_KEY`. If the variable is not present or is empty,
`boot1oot` falls back to dislocker's interactive recovery-password prompt.
`BOOT1OOT_LOOT_PASSPHRASE` is used only when `boot1oot loot` cannot write to
the USB loot partition and needs to place an encrypted archive under
`C:\Users\Public\Boot1oot`. If it is not set, Boot1oot uses the built-in
fallback archive passphrase `boot1oot`.

Only enable config export for lab images where baking the recovery key into the
ISO is intentional.

## Dislocker Build Notes

You do not need to install `dislocker` on the host to build the ISO. Boot1oot
builds dislocker inside Buildroot and includes only the target binaries it
needs.

The target ISO currently builds:

- `bash`
- `chntpw`
- `reged`
- `sampasswd`
- `samusrgrp`
- `dislocker-fuse`
- `dislocker-metadata`
- `libdislocker`
- `ntfs-3g`
- `openssl`
- target `libfuse3`
- target `mbedtls`

The dislocker package is fetched from:

```text
https://github.com/Aorimn/dislocker.git
```

It is pinned in `buildroot-external/package/dislocker/dislocker.mk` to a fixed
commit rather than tracking a moving branch.

## Chntpw Build Notes

You do not need to install `chntpw` on the host to build the ISO. Boot1oot
downloads the upstream source archive during the Buildroot build:

```text
https://pogostick.net/~pnh/ntpasswd/chntpw-source-140201.zip
```

The target ISO builds and installs:

- `chntpw`
- `reged`
- `sampasswd`
- `samusrgrp`

`boot1oot users` uses `reged -x` to export the relevant SAM user and alias
branches under `/tmp`, copies `SAM` to `/tmp` for read-only decoding, then
displays the decoded local SAM user table.
Bare `boot1oot chntpw` lists users first, prompts for a username, then launches
upstream chntpw interactively with the detected `SAM`, `SYSTEM`, and `SECURITY`
hives. The wrapper no longer exposes direct `-l` or `-u` shortcuts and no
longer attempts to create or modify users itself.

If you want to compile or test dislocker directly on the host outside Buildroot,
install the native development packages too:

```sh
sudo apt install -y cmake pkgconf libfuse3-dev libmbedtls-dev ruby-dev
```

`ruby-dev` is only needed for dislocker's optional Ruby bindings and
`dislocker-find`; Boot1oot disables Ruby support for a smaller target.

## Generated Files

Do not commit generated artifacts:

- `dist/`
- `buildroot/`
- `buildroot-output/`
- `dl/`
- `config/boot1oot.local.env`
