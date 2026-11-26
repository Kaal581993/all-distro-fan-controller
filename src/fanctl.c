#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Native hardware service. No shell execution, raw EC access or network port. */
#define SOCKET_PATH "/run/fan-control-center/control.sock"
#define STATE_PATH "/var/lib/fan-control-center/settings"
#define PSTATE "/sys/devices/system/cpu/intel_pstate/"
#define PLATFORM "/sys/firmware/acpi/platform_profile"
#define MAX_SNAPSHOTS 128
#define FAN_TRIGGER_MILLIC 80000
#define FAN_RELEASE_MILLIC 75000
/* Requested range is informational: this driver has no numeric RPM target. */
#define DESIRED_MIN_RPM 7900
#define DESIRED_MAX_RPM 8000
enum { SAVING, MEDIUM, PERFORMANCE, AUTOMATIC };
static const char *profiles[] = {"power-saving", "medium", "performance", "automatic"};
typedef struct {
    int profile, ac_profile, battery_profile, fan_full, cap, boost, enabled;
} Settings;
typedef struct { char path[PATH_MAX], value[256]; } Snapshot;
typedef struct {
    int cpu_temp, gpu_temp, rpm[2], rated[2], mode[2], ac, cap, turbo, min_cap;
    int peak[2], guard, intel, fan_boost, intel_write, platform_write;
    int fan_write[2];
    char fan[2][PATH_MAX], platform[64], model[256];
} Hardware;
static Settings config = {AUTOMATIC, PERFORMANCE, SAVING, -1, 0, -1, 1};
static Hardware hw;
static Snapshot originals[MAX_SNAPSHOTS];
static size_t original_count;
static char simulation[PATH_MAX], state_path[PATH_MAX] = STATE_PATH;
static char socket_path[PATH_MAX] = SOCKET_PATH, last_error[512];
static uid_t owner;
static volatile sig_atomic_t stopping;
static bool user_daemon, socket_explicit;
static char user_socket[PATH_MAX], user_state[PATH_MAX], user_log[PATH_MAX];

static void format(char *out, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out, size, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= size) {
        fprintf(stderr, "Path or response too long\n");
        exit(1);
    }
}

static void error_at(const char *path)
{
    if (!last_error[0])
        format(last_error, sizeof last_error, "%s: %s", path, strerror(errno));
}

static bool read_text(const char *path, char *out, size_t size)
{
    FILE *f = fopen(path, "re");
    if (!f) return false;
    bool ok = fgets(out, (int)size, f) != NULL;
    fclose(f);
    if (!ok) return false;
    size_t len = strlen(out);
    while (len && isspace((unsigned char)out[len - 1])) out[--len] = '\0';
    for (size_t i = 0; i < len; ++i)
        if ((unsigned char)out[i] < 32) out[i] = ' ';
    return true;
}

static int number(const char *text, int low, int high)
{
    char *end;
    errno = 0;
    long n = strtol(text, &end, 10);
    return errno || end == text || *end || n < low || n > high ? -1 : (int)n;
}

static int read_number(const char *path)
{
    char value[64];
    return read_text(path, value, sizeof value) ? number(value, 0, 100000000) : -1;
}

static void syspath(char *out, const char *suffix)
{
    format(out, PATH_MAX, "%s%s", simulation, suffix);
}

static bool sys_writable(const char *suffix)
{
    char path[PATH_MAX];
    syspath(path, suffix);
    return access(path, W_OK) == 0;
}

static int sys_number(const char *suffix)
{
    char path[PATH_MAX];
    syspath(path, suffix);
    return read_number(path);
}

static bool write_text(const char *path, const char *value)
{
    int flags = O_WRONLY | O_CLOEXEC | O_NOFOLLOW;
    /* Fixtures are ordinary files; sysfs attributes must not be truncated. */
    if (simulation[0]) flags |= O_TRUNC;
    int fd = open(path, flags);
    if (fd < 0) { error_at(path); return false; }
    char data[300];
    format(data, sizeof data, "%s\n", value);
    size_t len = strlen(data);
    bool ok = write(fd, data, len) == (ssize_t)len;
    if (!ok) error_at(path);
    if (close(fd) && ok) { error_at(path); ok = false; }
    return ok;
}

static void set_value(const char *path, const char *value, bool remember)
{
    char current[256];
    if (!read_text(path, current, sizeof current)) { error_at(path); return; }
    if (!strcmp(current, value)) return;
    if (remember) {
        size_t i;
        for (i = 0; i < original_count; ++i)
            if (!strcmp(originals[i].path, path)) break;
        if (i == original_count) {
            if (original_count == MAX_SNAPSHOTS) {
                errno = ENOSPC;
                error_at("Original settings snapshot");
                return;
            }
            format(originals[i].path, sizeof originals[i].path, "%s", path);
            format(originals[i].value, sizeof originals[i].value, "%s", current);
            ++original_count;
        }
    }
    write_text(path, value);
}

static void set_sys(const char *suffix, int value)
{
    char path[PATH_MAX], text[32];
    syspath(path, suffix);
    format(text, sizeof text, "%d", value);
    set_value(path, text, true);
}

static void scan(void)
{
    int peaks[2] = {hw.peak[0], hw.peak[1]}, guard = hw.guard, fan_boost = hw.fan_boost;
    memset(&hw, 0, sizeof hw);
    hw.cpu_temp = hw.gpu_temp = -1;
    hw.ac = -1;
    hw.guard = guard;
    hw.fan_boost = fan_boost;
    for (int i = 0; i < 2; ++i) {
        hw.rpm[i] = hw.rated[i] = hw.mode[i] = -1;
        hw.peak[i] = peaks[i];
    }
    char pattern[PATH_MAX], path[PATH_MAX], name[128];
    syspath(pattern, "/sys/class/hwmon/hwmon*");
    glob_t devices = {0};
    if (!glob(pattern, 0, NULL, &devices)) {
        for (size_t i = 0; i < devices.gl_pathc; ++i) {
            const char *dir = devices.gl_pathv[i];
            format(path, sizeof path, "%s/name", dir);
            if (!read_text(path, name, sizeof name)) continue;
            bool cpu = !strcmp(name, "coretemp") || !strcmp(name, "k10temp") ||
                       !strcmp(name, "zenpower") || !strcmp(name, "cpu_thermal");
            bool gpu = !strcmp(name, "amdgpu") || !strcmp(name, "nouveau") ||
                       !strcmp(name, "nvidia");
            if (cpu || gpu) {
                glob_t temps = {0};
                format(pattern, sizeof pattern, "%s/temp*_input", dir);
                if (!glob(pattern, 0, NULL, &temps))
                    for (size_t t = 0; t < temps.gl_pathc; ++t) {
                        int temp = read_number(temps.gl_pathv[t]);
                        if (temp < 1000 || temp > 150000) continue;
                        int *target = cpu ? &hw.cpu_temp : &hw.gpu_temp;
                        if (temp > *target) *target = temp;
                    }
                globfree(&temps);
            }
            if (strcmp(name, "asus")) continue;
            for (int fan = 0; fan < 2; ++fan) {
                format(path, sizeof path, "%s/fan%d_input", dir, fan + 1);
                hw.rpm[fan] = read_number(path);
                format(path, sizeof path, "%s/fan%d_max", dir, fan + 1);
                hw.rated[fan] = read_number(path);
                format(path, sizeof path, "%s/pwm%d_enable", dir, fan + 1);
                hw.mode[fan] = read_number(path);
                char curve[PATH_MAX], pwm[PATH_MAX];
                format(curve, sizeof curve, "%s/pwm%d_auto_point1_pwm", dir, fan + 1);
                format(pwm, sizeof pwm, "%s/pwm%d", dir, fan + 1);
                /* Only legacy ASUS WMI mode switches. Curve-capable/AGFN
                   hardware needs a different interface and is left alone. */
                if (hw.mode[fan] >= 0 && access(curve, F_OK) && access(pwm, F_OK))
                    format(hw.fan[fan], sizeof hw.fan[fan], "%s", path);
                hw.fan_write[fan] = hw.fan[fan][0] && access(path, W_OK) == 0;
                if (hw.rpm[fan] > hw.peak[fan]) hw.peak[fan] = hw.rpm[fan];
            }
        }
    }
    globfree(&devices);
    /* ACPI temperature is a fallback, never an SSD/DIMM temperature. */
    if (hw.cpu_temp < 0) {
        syspath(pattern, "/sys/class/hwmon/hwmon*");
        devices = (glob_t){0};
        if (!glob(pattern, 0, NULL, &devices))
            for (size_t i = 0; i < devices.gl_pathc; ++i) {
                format(path, sizeof path, "%s/name", devices.gl_pathv[i]);
                if (!read_text(path, name, sizeof name) || strcmp(name, "acpitz")) continue;
                format(path, sizeof path, "%s/temp1_input", devices.gl_pathv[i]);
                int temp = read_number(path);
                if (temp >= 1000 && temp <= 150000 && temp > hw.cpu_temp) hw.cpu_temp = temp;
            }
        globfree(&devices);
    }
    syspath(pattern, "/sys/class/power_supply/*");
    devices = (glob_t){0};
    if (!glob(pattern, 0, NULL, &devices))
        for (size_t i = 0; i < devices.gl_pathc; ++i) {
            format(path, sizeof path, "%s/type", devices.gl_pathv[i]);
            if (!read_text(path, name, sizeof name) || !strcmp(name, "Battery")) continue;
            format(path, sizeof path, "%s/online", devices.gl_pathv[i]);
            int online = read_number(path);
            if (online == 1) hw.ac = 1;
            else if (online == 0 && hw.ac < 0) hw.ac = 0;
        }
    globfree(&devices);
    hw.cap = sys_number(PSTATE "max_perf_pct");
    hw.min_cap = sys_number(PSTATE "min_perf_pct");
    hw.turbo = sys_number(PSTATE "no_turbo");
    hw.intel = hw.cap >= 0 && hw.min_cap >= 0 && hw.turbo >= 0;
    hw.intel_write = hw.intel && sys_writable(PSTATE "max_perf_pct") &&
                     sys_writable(PSTATE "min_perf_pct") && sys_writable(PSTATE "no_turbo");
    hw.platform_write = sys_writable(PLATFORM);
    syspath(path, PLATFORM);
    if (!read_text(path, hw.platform, sizeof hw.platform)) strcpy(hw.platform, "unavailable");
    syspath(path, "/sys/class/dmi/id/product_name");
    if (!read_text(path, hw.model, sizeof hw.model)) strcpy(hw.model, "Laptop");
}

static int effective_profile(void)
{
    int profile = config.profile;
    if (profile == AUTOMATIC) profile = hw.ac == 1 ? PERFORMANCE : config.battery_profile;
    /* High performance requires confirmed external power. */
    return profile == PERFORMANCE && hw.ac != 1 ? SAVING : profile;
}

static void restore(void)
{
    for (size_t i = original_count; i > 0; --i)
        write_text(originals[i - 1].path, originals[i - 1].value);
    if (!last_error[0]) original_count = 0;
    scan();
    for (int i = 0; i < 2; ++i)
        if (hw.fan_write[i]) set_value(hw.fan[i], "2", false);
}

static void step(void)
{
    last_error[0] = '\0';
    scan();
    if (!config.enabled) return;
    int temp = hw.cpu_temp > hw.gpu_temp ? hw.cpu_temp : hw.gpu_temp;
    if (hw.cpu_temp < 0 || temp >= FAN_TRIGGER_MILLIC) hw.fan_boost = 1;
    else if (temp <= FAN_RELEASE_MILLIC) hw.fan_boost = 0;
    if (hw.cpu_temp < 0 || temp >= 90000) hw.guard = 2;
    else if (temp >= 85000) {
        if (hw.guard < 1 || temp <= 85000) hw.guard = 1;
    } else if (temp <= 78000) hw.guard = 0;
    else if (hw.guard == 2) hw.guard = 1;
    int profile = effective_profile();
    int cap = config.cap ? config.cap : (int[]){45, 70, 100}[profile];
    bool boost = config.boost < 0 ? profile == PERFORMANCE : config.boost == 1;
    bool full = hw.ac == 1 || hw.fan_boost || hw.guard ||
                (config.fan_full < 0 ? profile == PERFORMANCE : config.fan_full == 1);
    const char *platform = (const char *[]){"quiet", "balanced", "performance"}[profile];
    if (hw.guard) {
        int limit = hw.guard == 2 ? 40 : 60;
        if (cap > limit) cap = limit;
        boost = false;
        platform = "balanced";
    }
    char path[PATH_MAX], choices[256];
    syspath(path, PLATFORM "_choices");
    if (hw.platform_write && read_text(path, choices, sizeof choices)) {
        char padded[300], candidate[80];
        format(padded, sizeof padded, " %s ", choices);
        format(candidate, sizeof candidate, " %s ", platform);
        if (strstr(padded, candidate)) {
            syspath(path, PLATFORM);
            set_value(path, platform, true);
        }
    }
    if (hw.intel_write) {
        /* Disable turbo before reducing the ceiling. Lower a conflicting
           minimum first, so the requested ceiling can actually take effect. */
        if (!boost) set_sys(PSTATE "no_turbo", 1);
        if (hw.min_cap > cap) set_sys(PSTATE "min_perf_pct", cap);
        set_sys(PSTATE "max_perf_pct", cap);
        if (boost) set_sys(PSTATE "no_turbo", 0);
        char pattern[PATH_MAX];
        syspath(pattern, "/sys/devices/system/cpu/cpufreq/policy*");
        glob_t policies = {0};
        if (!glob(pattern, 0, NULL, &policies))
            for (size_t i = 0; i < policies.gl_pathc; ++i) {
                format(path, sizeof path, "%s/scaling_governor", policies.gl_pathv[i]);
                if (access(path, W_OK) == 0) set_value(path, "powersave", true);
                const char *epp = profile == PERFORMANCE && !hw.guard ? "balance_performance" :
                                  profile == SAVING ? "power" : "balance_power";
                format(path, sizeof path, "%s/energy_performance_available_preferences", policies.gl_pathv[i]);
                char available[256];
                if (!read_text(path, available, sizeof available) || !strstr(available, epp)) continue;
                format(path, sizeof path, "%s/energy_performance_preference", policies.gl_pathv[i]);
                if (access(path, W_OK) == 0) set_value(path, epp, true);
            }
        globfree(&policies);
    }
    for (int i = 0; i < 2; ++i) {
        if (!hw.fan[i][0]) continue;
        if (!hw.fan_write[i]) {
            errno = EACCES;
            error_at(hw.fan[i]);
            continue;
        }
        /* ASUS reports a cached mode. Reissue full speed on every cycle even
           when the cached value is already 0, so firmware resets are retried. */
        if (full) write_text(hw.fan[i], "0");
        else set_value(hw.fan[i], "2", false);
    }
    scan();
}

static void save_settings(void)
{
    char temporary[PATH_MAX];
    format(temporary, sizeof temporary, "%s.XXXXXX", state_path);
    int fd = mkstemp(temporary);
    if (fd < 0) { error_at("Saving preferences"); return; }
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(temporary); error_at("Saving preferences"); return; }
    int rc = fprintf(f, "v1 %d %d %d %d %d %d %d\n", config.profile, config.ac_profile,
                     config.battery_profile, config.fan_full, config.cap, config.boost, config.enabled);
    bool ok = rc > 0 && fflush(f) == 0 && fsync(fd) == 0;
    if (fclose(f)) ok = false;
    if (!ok || rename(temporary, state_path)) {
        error_at("Saving preferences");
        unlink(temporary);
    }
}

static void load_settings(void)
{
    char line[256], extra;
    Settings s;
    if (!read_text(state_path, line, sizeof line)) return;
    if (sscanf(line, "v1 %d %d %d %d %d %d %d %c", &s.profile, &s.ac_profile,
               &s.battery_profile, &s.fan_full, &s.cap, &s.boost, &s.enabled, &extra) != 7) return;
    if (s.profile < 0 || s.profile > AUTOMATIC || s.ac_profile < 0 || s.ac_profile > PERFORMANCE ||
        s.battery_profile < 0 || s.battery_profile > MEDIUM ||
        s.fan_full < -1 || s.fan_full > 1 || (s.cap != 0 && (s.cap < 20 || s.cap > 100)) ||
        s.boost < -1 || s.boost > 1 || s.enabled < 0 || s.enabled > 1) return;
    s.ac_profile = PERFORMANCE; /* Migrate earlier low-power AC preferences. */
    config = s;
}

static void status(FILE *out, bool online)
{
    fprintf(out,
        "daemon=%s\nmodel=%s\ncpu_temp=%d\ngpu_temp=%d\ncpu_rpm=%d\ngpu_rpm=%d\n"
        "cpu_fan_control=%d\ngpu_fan_control=%d\ncpu_fan_mode=%d\ngpu_fan_mode=%d\n"
        "cpu_rated_rpm=%d\ngpu_rated_rpm=%d\ncpu_peak_rpm=%d\ngpu_peak_rpm=%d\n"
        "ac=%d\nintel_control=%d\nactual_cap=%d\nno_turbo=%d\nplatform=%s\n"
        "profile=%s\neffective=%s\nac_profile=%s\nbattery_profile=%s\n"
        "fan_override=%s\nrequested_cap=%d\nrequested_boost=%d\nenabled=%d\nguard=%d\n"
        "fan_temperature_boost=%d\nfan_trigger_millic=%d\n"
        "fan_desired_min_rpm=%d\nfan_desired_max_rpm=%d\nfan_rpm_target_supported=0\n"
        "controller_mode=%s\ncpu_fan_write_access=%d\ngpu_fan_write_access=%d\n"
        "cpu_controls_locked=%d\nerror=%s\n",
        online ? "online" : "offline", hw.model, hw.cpu_temp, hw.gpu_temp, hw.rpm[0], hw.rpm[1],
        hw.fan[0][0] != 0, hw.fan[1][0] != 0, hw.mode[0], hw.mode[1], hw.rated[0], hw.rated[1],
        hw.peak[0], hw.peak[1], hw.ac, hw.intel_write, hw.cap, hw.turbo, hw.platform,
        online ? profiles[config.profile] : "unmanaged",
        online && config.enabled ? profiles[effective_profile()] : "firmware",
        profiles[config.ac_profile], profiles[config.battery_profile],
        config.fan_full < 0 ? "profile" : config.fan_full ? "full" : "auto",
        config.cap, config.boost, online ? config.enabled : 0, online && config.enabled ? hw.guard : 0,
        online && config.enabled ? hw.fan_boost : 0, FAN_TRIGGER_MILLIC,
        DESIRED_MIN_RPM, DESIRED_MAX_RPM, online ? (geteuid() == 0 ? "system" : "user") : "offline",
        hw.fan_write[0], hw.fan_write[1], hw.intel && !hw.intel_write, last_error);
}

static int profile_index(const char *name, int maximum)
{
    for (int i = 0; i <= maximum; ++i) if (!strcmp(name, profiles[i])) return i;
    return -1;
}

static bool command(const char *request)
{
    char verb[32], value[64], extra;
    /* Validate against the current charger/permission state, including
       changes that happened since the last polling interval. */
    scan();
    if (!strcmp(request, "release")) {
        last_error[0] = '\0';
        restore();
        config.enabled = 0;
        save_settings();
        return true;
    }
    if (sscanf(request, "%31s %63s %c", verb, value, &extra) != 2) return false;
    Settings next = config;
    if (!strcmp(verb, "profile")) {
        int p = profile_index(value, AUTOMATIC);
        if (p < 0) return false;
        next.profile = p; next.cap = 0; next.boost = -1;
        next.fan_full = -1; next.enabled = 1;
    } else if (!strcmp(verb, "fan")) {
        if (!strcmp(value, "full")) {
            if (!hw.fan[0][0] && !hw.fan[1][0]) return false;
            next.fan_full = 1;
        } else if (!strcmp(value, "auto")) {
            if (hw.ac == 1) return false;
            next.fan_full = 0;
        }
        else if (!strcmp(value, "default")) next.fan_full = -1;
        else return false;
        next.enabled = 1;
    } else if (!strcmp(verb, "cap")) {
        if (!hw.intel_write) return false;
        next.cap = !strcmp(value, "default") ? 0 : number(value, 20, 100);
        if (next.cap < 0) return false;
        next.enabled = 1;
    } else if (!strcmp(verb, "boost")) {
        if (!hw.intel_write) return false;
        if (!strcmp(value, "on")) next.boost = 1;
        else if (!strcmp(value, "off")) next.boost = 0;
        else if (!strcmp(value, "default")) next.boost = -1;
        else return false;
        next.enabled = 1;
    } else if (!strcmp(verb, "ac-profile")) {
        if (strcmp(value, "performance")) return false;
        next.ac_profile = PERFORMANCE;
    } else if (!strcmp(verb, "battery-profile")) {
        next.battery_profile = profile_index(value, MEDIUM);
        if (next.battery_profile < 0) return false;
    } else return false;
    config = next;
    step();
    save_settings();
    return true;
}

static bool private_directory(const char *path)
{
    char parent[PATH_MAX];
    format(parent, sizeof parent, "%s", path);
    for (char *p = parent + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(parent, 0700) && errno != EEXIST) return false;
        *p = '/';
    }
    if (mkdir(parent, 0700) && errno != EEXIST) return false;
    struct stat info;
    if (lstat(parent, &info) || !S_ISDIR(info.st_mode) || info.st_uid != getuid()) {
        errno = EACCES;
        return false;
    }
    return chmod(parent, 0700) == 0;
}

static bool user_paths(bool create)
{
    const char *account_home = getenv("HOME"), *runtime = getenv("XDG_RUNTIME_DIR");
    const char *state_base = getenv("XDG_STATE_HOME");
    if (!account_home || account_home[0] != '/') { errno = EINVAL; return false; }
    char runtime_dir[PATH_MAX], state_dir[PATH_MAX];
    if (runtime && runtime[0] == '/')
        format(runtime_dir, sizeof runtime_dir, "%s/fan-control-center", runtime);
    else
        format(runtime_dir, sizeof runtime_dir, "/tmp/fan-control-center-user-%lu", (unsigned long)getuid());
    if (state_base && state_base[0] == '/')
        format(state_dir, sizeof state_dir, "%s/fan-control-center", state_base);
    else
        format(state_dir, sizeof state_dir, "%s/.local/state/fan-control-center", account_home);
    format(user_socket, sizeof user_socket, "%s/control.sock", runtime_dir);
    format(user_state, sizeof user_state, "%s/user-settings", state_dir);
    format(user_log, sizeof user_log, "%s/controller.log", state_dir);
    return !create || (private_directory(runtime_dir) && private_directory(state_dir));
}

static int connect_at(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct timeval timeout = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof addr.sun_path) { close(fd); errno = ENAMETOOLONG; return -1; }
    strcpy(addr.sun_path, path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr)) { close(fd); return -1; }
    return fd;
}

static int connect_service(void)
{
    int fd = connect_at(socket_path);
    if (fd >= 0 || socket_explicit || geteuid() == 0) return fd;
    return user_paths(false) ? connect_at(user_socket) : -1;
}

static int start_user_daemon(void)
{
    if (geteuid() == 0 || simulation[0] || socket_explicit) {
        fprintf(stderr, "Start the user controller from your normal desktop account.\n");
        return 1;
    }
    int existing = connect_service();
    if (existing >= 0) { close(existing); return 0; }
    if (!user_paths(true)) { perror("User controller directories"); return 1; }
    char self[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", self, sizeof self - 1);
    if (length <= 0) { perror("Controller executable"); return 1; }
    self[length] = '\0';
    pid_t child = fork();
    if (child < 0) { perror("Starting user controller"); return 1; }
    if (!child) {
        if (setsid() < 0) _exit(1);
        int log = open(user_log, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
        int input = open("/dev/null", O_RDONLY);
        if (log < 0 || input < 0 || dup2(input, STDIN_FILENO) < 0 ||
            dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) _exit(1);
        close(input); close(log);
        execl(self, self, "--user-daemon", (char *)NULL);
        _exit(1);
    }
    for (int attempt = 0; attempt < 40; ++attempt) {
        int fd = connect_at(user_socket);
        if (fd >= 0) { close(fd); return 0; }
        struct timespec pause = {.tv_nsec = 50000000};
        nanosleep(&pause, NULL);
    }
    int result;
    (void)waitpid(child, &result, WNOHANG);
    fprintf(stderr, "Could not start the user fan controller. Details: %s\n", user_log);
    return 1;
}

static void respond(const char *request, FILE *out)
{
    if (!strcmp(request, "status")) { scan(); status(out, true); }
    else if (!command(request)) fprintf(out, "ERR Unsupported command or unavailable control\n");
    else if (last_error[0]) fprintf(out, "ERR %s\n", last_error);
    else fprintf(out, "OK\n");
}

static void handle_client(int fd)
{
    struct ucred cred;
    socklen_t size = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) || (cred.uid != owner && cred.uid != 0)) {
        close(fd); return;
    }
    struct timeval timeout = {.tv_sec = 1};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    char request[256];
    size_t used = 0;
    while (used < sizeof request - 1) {
        ssize_t n = recv(fd, request + used, sizeof request - 1 - used, 0);
        if (n <= 0) break;
        used += (size_t)n;
        if (memchr(request, '\n', used)) break;
    }
    request[used] = '\0';
    char *newline = strchr(request, '\n');
    FILE *out = fdopen(fd, "w");
    if (!out) { close(fd); return; }
    if (!newline || newline != request + used - 1) fprintf(out, "ERR Invalid request\n");
    else {
        *newline = '\0';
        respond(request, out);
    }
    fclose(out);
}

static void on_signal(int sig) { (void)sig; stopping = 1; }

static time_t tick_time(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_BOOTTIME, &now)) {
        perror("Monotonic clock");
        stopping = 1;
        return 0;
    }
    return now.tv_sec;
}

static int daemon_main(bool stdio_test)
{
    if (!simulation[0] && !user_daemon && (geteuid() != 0 || owner == 0)) {
        fprintf(stderr, "Install the system service; it needs root and a non-root --uid.\n");
        return 1;
    }
    char lock_path[PATH_MAX];
    format(lock_path, sizeof lock_path, "%s.lock", socket_path);
    int lock = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) { perror("Daemon lock"); return 1; }
    int fd = stdio_test ? STDIN_FILENO : socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); close(lock); return 1; }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(socket_path) >= sizeof addr.sun_path) {
        fprintf(stderr, "Socket path too long\n"); close(fd); close(lock); return 1;
    }
    strcpy(addr.sun_path, socket_path);
    if (!stdio_test) {
        unlink(socket_path);
        mode_t old = umask(0077);
        int rc = bind(fd, (struct sockaddr *)&addr, sizeof addr);
        umask(old);
        if (rc || chmod(socket_path, 0600) ||
            (geteuid() == 0 && chown(socket_path, owner, (gid_t)-1)) || listen(fd, 8)) {
            perror("Creating control socket"); close(fd); close(lock); return 1;
        }
    }
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    load_settings();
    step();
    fprintf(stderr, "Fan Control Center ready; authorized uid=%lu; %s\n",
            (unsigned long)owner, simulation[0] ? "simulated hardware" : hw.model);
    if (stdio_test) {
        setvbuf(stdin, NULL, _IONBF, 0);
        setvbuf(stdout, NULL, _IONBF, 0);
        puts("READY");
    }
    time_t last_tick = tick_time();
    char previous_error[512] = "";
    while (!stopping) {
        struct pollfd item = {.fd = fd, .events = POLLIN};
        if (poll(&item, 1, 250) > 0) {
            if (stdio_test && (item.revents & (POLLIN | POLLHUP))) {
                char request[256];
                if (!fgets(request, sizeof request, stdin)) break;
                char *end = strchr(request, '\n');
                if (!end) { puts("ERR Invalid request"); puts("END"); }
                else {
                    *end = '\0';
                    respond(request, stdout);
                    puts("END");
                }
            } else if (item.revents & POLLIN) {
                int client = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
                if (client >= 0) handle_client(client);
            }
        }
        if (tick_time() - last_tick >= 2) {
            step();
            last_tick = tick_time();
        }
        if (strcmp(previous_error, last_error)) {
            fprintf(stderr, "%s%s\n", last_error[0] ? "Hardware error: " : "Hardware controls OK", last_error);
            strcpy(previous_error, last_error);
        }
    }
    last_error[0] = '\0';
    restore();
    if (last_error[0]) fprintf(stderr, "Restore error: %s\n", last_error);
    if (!stdio_test) { close(fd); unlink(socket_path); }
    close(lock);
    return last_error[0] ? 1 : 0;
}

int main(int argc, char **argv)
{
    owner = getuid();
    int i = 1;
    while (i < argc && !strncmp(argv[i], "--", 2)) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
            format(socket_path, sizeof socket_path, "%s", argv[i + 1]); socket_explicit = true; i += 2;
        } else if (!strcmp(argv[i], "--state") && i + 1 < argc) {
            format(state_path, sizeof state_path, "%s", argv[i + 1]); i += 2;
        } else if (!strcmp(argv[i], "--uid") && i + 1 < argc) {
            int uid = number(argv[i + 1], 1, INT_MAX);
            if (uid < 0) return 1;
            owner = (uid_t)uid; i += 2;
        } else if (!strcmp(argv[i], "--simulate") && i + 1 < argc) {
            if (geteuid() == 0 || argv[i + 1][0] != '/' ||
                !realpath(argv[i + 1], simulation) || !strcmp(simulation, "/")) {
                fprintf(stderr, "Simulation requires a non-root user and an absolute fixture directory.\n");
                return 1;
            }
            i += 2;
        } else break;
    }
    if (i + 1 == argc && !strcmp(argv[i], "--start-user-daemon")) return start_user_daemon();
    if (i + 1 == argc && !strcmp(argv[i], "--user-daemon")) {
        if (geteuid() == 0 || simulation[0] || socket_explicit || !user_paths(true)) {
            fprintf(stderr, "Cannot prepare the normal-user fan controller.\n");
            return 1;
        }
        user_daemon = true;
        format(socket_path, sizeof socket_path, "%s", user_socket);
        format(state_path, sizeof state_path, "%s", user_state);
        return daemon_main(false);
    }
    if (i < argc && !strcmp(argv[i], "--daemon") && i + 1 == argc) return daemon_main(false);
    if (i < argc && !strcmp(argv[i], "--stdio") && i + 1 == argc) {
        if (!simulation[0]) {
            fprintf(stderr, "Pipe transport is restricted to simulated hardware.\n");
            return 1;
        }
        return daemon_main(true);
    }
    if (i < argc && !strcmp(argv[i], "--probe") && i + 1 == argc) {
        scan(); status(stdout, false); return 0;
    }
    bool getting_status = i == argc || (i + 1 == argc && !strcmp(argv[i], "status"));
    int fd = connect_service();
    if (fd < 0 && !getting_status && !socket_explicit && geteuid() != 0) {
        if (start_user_daemon() == 0) fd = connect_service();
    }
    if (fd < 0) {
        if (getting_status) { scan(); status(stdout, false); return 0; }
        fprintf(stderr, "User fan controller unavailable: %s. No administrator prompt is used.\n", strerror(errno));
        return 1;
    }
    char request[256] = "";
    if (getting_status) strcpy(request, "status");
    else {
        size_t used = 0;
        for (; i < argc; ++i) {
            int n = snprintf(request + used, sizeof request - used, "%s%s", used ? " " : "", argv[i]);
            if (n < 0 || (size_t)n >= sizeof request - used) { close(fd); return 1; }
            used += (size_t)n;
        }
    }
    size_t len = strlen(request);
    request[len++] = '\n';
    if (send(fd, request, len, MSG_NOSIGNAL) != (ssize_t)len) { close(fd); return 1; }
    shutdown(fd, SHUT_WR);
    char response[8192];
    size_t used = 0;
    while (used < sizeof response - 1) {
        ssize_t n = read(fd, response + used, sizeof response - 1 - used);
        if (n < 0) { perror("Service response"); close(fd); return 1; }
        if (!n) break;
        used += (size_t)n;
    }
    close(fd);
    response[used] = '\0';
    fputs(response, stdout);
    return used == 0 || !strncmp(response, "ERR", 3) ? 1 : 0;
}
