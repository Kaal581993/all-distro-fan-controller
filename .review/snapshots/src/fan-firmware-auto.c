#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Recovery for this laptop's legacy ASUS mode switch: 2 = firmware automatic.
 * Never writes a PWM duty, CPU control, platform profile, or raw EC register. */
static void path(char *out, const char *base, const char *suffix)
{
    int length = snprintf(out, PATH_MAX, "%s%s", base, suffix);
    if (length < 0 || length >= PATH_MAX) {
        fputs("Recovery path is too long\n", stderr);
        exit(EXIT_FAILURE);
    }
}

static bool read_line(const char *file, char *value, size_t size)
{
    FILE *input = fopen(file, "re");
    if (!input) return false;
    bool ok = fgets(value, (int)size, input) != NULL;
    fclose(input);
    if (ok) value[strcspn(value, "\r\n")] = '\0';
    return ok;
}

/* Treat an inaccessible or unexpected interface conservatively. */
static bool absent(const char *file)
{
    struct stat metadata;
    return lstat(file, &metadata) < 0 && errno == ENOENT;
}

int main(int argc, char **argv)
{
    char root[PATH_MAX] = "", file[PATH_MAX], value[256];
    bool simulated = false;
    if (argc == 3 && !strcmp(argv[1], "--simulate")) {
        if (geteuid() == 0 || argv[2][0] != '/' || !realpath(argv[2], root) ||
            !strcmp(root, "/")) {
            fputs("Simulation requires a non-root user and an absolute fixture directory\n", stderr);
            return EXIT_FAILURE;
        }
        simulated = true;
    } else if (argc != 1) {
        fputs("Usage: fan-firmware-auto [--simulate /absolute/fixture]\n", stderr);
        return EXIT_FAILURE;
    }
    if (!simulated) {
        if (!read_line("/sys/class/dmi/id/product_name", value, sizeof value) ||
            !strstr(value, "P2451FB")) {
            fputs("Recovery is restricted to the ASUS P2451FB\n", stderr);
            return EXIT_FAILURE;
        }
    }

    glob_t monitors = {0};
    path(file, root, "/sys/class/hwmon/hwmon*");
    int result = glob(file, 0, NULL, &monitors);
    if (result != 0) {
        globfree(&monitors);
        fputs("No hwmon interfaces available for fan recovery\n", stderr);
        return EXIT_FAILURE;
    }
    bool restored = false, failed = false;
    for (size_t i = 0; i < monitors.gl_pathc; ++i) {
        const char *monitor = monitors.gl_pathv[i];
        path(file, monitor, "/name");
        if (!read_line(file, value, sizeof value) || strcmp(value, "asus")) continue;
        path(file, monitor, "/pwm1");
        if (!absent(file)) continue;
        path(file, monitor, "/pwm1_auto_point1_pwm");
        if (!absent(file)) continue;
        path(file, monitor, "/pwm1_enable");
        struct stat metadata;
        if (lstat(file, &metadata) || !S_ISREG(metadata.st_mode) ||
            !read_line(file, value, sizeof value) ||
            (strcmp(value, "0") && strcmp(value, "1") && strcmp(value, "2"))) {
            fprintf(stderr, "Refusing unexpected fan-mode attribute: %s\n", file);
            failed = true;
            continue;
        }
        int flags = O_WRONLY | O_CLOEXEC | O_NOFOLLOW;
        if (simulated) flags |= O_TRUNC;
        int descriptor = open(file, flags);
        if (descriptor < 0) {
            perror(file);
            failed = true;
            continue;
        }
        bool ok = write(descriptor, "2\n", 2) == 2;
        if (close(descriptor)) ok = false;
        if (!ok || !read_line(file, value, sizeof value) || strcmp(value, "2")) {
            fprintf(stderr, "Failed to restore firmware fan mode: %s\n", file);
            failed = true;
            continue;
        }
        printf("Firmware automatic fan control restored: %s\n", file);
        restored = true;
    }
    globfree(&monitors);
    if (!restored) fputs("No eligible legacy ASUS fan control was restored\n", stderr);
    return restored && !failed ? EXIT_SUCCESS : EXIT_FAILURE;
}
