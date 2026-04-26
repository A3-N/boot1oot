#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_DEVS 128
#define DEV_NAME_MAX 64
#define WIN_MOUNT_ROOT "/mnt/windows"
#define DISLOCKER_MOUNTPOINT "/run/boot1oot/dislocker"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_BLUE "\033[34m"
#define COLOR_RESET "\033[0m"
#define BOOT1OOT_RECOVERY_KEY_ENV "BOOT1OOT_BITLOCKER_RECOVERY_KEY"

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

static void print_usage(FILE *out)
{
	fputs("usage: boot1oot <command> [options]\n"
	      "\n"
	      "commands:\n"
	      "  dislocker [-r|-rw] unlock and mount all BitLocker Windows volumes\n"
	      "  mount    [-r|-rw] scan and mount all Windows volumes\n"
	      "  scan     list NTFS and BitLocker candidate volumes\n"
	      "  unmount  unmount Boot1oot Windows and dislocker mountpoints\n"
	      "  init     show the OS banner and start the shell\n",
	      out);
}

static int run_init(void)
{
	const char *shell = "/bin/sh";
	char *const argv[] = { "sh", NULL };

	sethostname("boot1oot", strlen("boot1oot"));
	setenv("HOME", "/root", 1);
	if (BOOT1OOT_BUILTIN_BITLOCKER_RECOVERY_KEY[0])
		setenv(BOOT1OOT_RECOVERY_KEY_ENV, BOOT1OOT_BUILTIN_BITLOCKER_RECOVERY_KEY, 0);
	chdir("/root");

	ensure_runtime_filesystems();
	print_banner();
	fflush(stdout);

	execv(shell, argv);
	fprintf(stderr, "failed to exec %s: %s\n", shell, strerror(errno));
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

	if (strcmp(argv[1], "unmount") == 0 && argc == 2)
		return run_unmount();

	if (strcmp(argv[1], "init") == 0 && argc == 2)
		return run_init();

	fprintf(stderr, "unknown command: %s\n\n", argv[1]);
	print_usage(stderr);
	return 2;
}
