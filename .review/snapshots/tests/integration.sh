#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
fixture=$(mktemp -d /tmp/fcc-test.XXXXXX)
daemon_pid=
transport=socket
service_in=
service_out=
cleanup() {
    local result=$?
    trap - EXIT
    if [[ -n $daemon_pid ]]; then
        kill -TERM "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
    if ((result)); then
        printf '\nDaemon diagnostics:\n'
        cat "$fixture/daemon.log" 2>/dev/null || true
    fi
    rm -rf -- "$fixture"
    exit "$result"
}
trap cleanup EXIT
put() {
    mkdir -p -- "$(dirname -- "$fixture$1")"
    printf '%s\n' "$2" > "$fixture$1"
}
expect_file() {
    local actual
    actual=$(<"$fixture$1")
    [[ $actual == "$2" ]] || { printf 'FAIL %s: expected %s, got %s\n' "$1" "$2" "$actual"; exit 1; }
}
ctl() {
    if [[ $transport == socket ]]; then
        build/fanctl --socket "$fixture/control.sock" "$@"
    else
        local line response= result=0
        printf '%s\n' "$*" >&"$service_in"
        while IFS= read -r -t 5 line <&"$service_out"; do
            [[ $line == END ]] && { printf '%s' "$response"; return "$result"; }
            [[ $line == ERR* ]] && result=1
            response+=$line$'\n'
        done
        printf 'FAIL: test daemon response timed out\n' >&2
        return 1
    fi
}
expect_status() {
    local response
    response=$(ctl status)
    if ! rg -F -x -- "$1=$2" <<<"$response" >/dev/null; then
        printf 'FAIL status: expected %s=%s\n%s\n' "$1" "$2" "$response"
        exit 1
    fi
}
reject() {
    if ctl "$@" >"$fixture/rejected" 2>&1; then
        printf 'FAIL: accepted invalid request: %s\n' "$*"; exit 1
    fi
}
wait_file() {
    for ((attempt=0; attempt<60; ++attempt)); do
        [[ $(<"$fixture$1") == "$2" ]] && return 0
        sleep 0.1
    done
    expect_file "$1" "$2"
}
start_daemon() {
    if [[ $transport == socket ]]; then
        build/fanctl --simulate "$fixture" --socket "$fixture/control.sock" \
            --state "$fixture/settings" --uid "$(id -u)" --daemon >>"$fixture/daemon.log" 2>&1 &
        daemon_pid=$!
        for ((attempt=0; attempt<50; ++attempt)); do
            if [[ -S $fixture/control.sock ]] && ctl status | rg -q '^daemon=online$'; then return 0; fi
            kill -0 "$daemon_pid" 2>/dev/null || break
            sleep 0.1
        done
        wait "$daemon_pid" 2>/dev/null || true
        daemon_pid=
        if rg -q 'Operation not permitted' "$fixture/daemon.log"; then
            transport=stdio
            printf 'Socket binding denied by sandbox; exercising the same hardware engine over test-only pipes.\n'
        else
            printf 'FAIL: daemon did not start\n'; exit 1
        fi
    fi
    coproc FCC_TEST {
        exec build/fanctl --simulate "$fixture" --socket "$fixture/control.sock" \
            --state "$fixture/settings" --uid "$(id -u)" --stdio 2>>"$fixture/daemon.log"
    }
    daemon_pid=$FCC_TEST_PID
    exec {service_in}>&"${FCC_TEST[1]}"
    exec {service_out}<&"${FCC_TEST[0]}"
    local ready
    IFS= read -r -t 5 ready <&"$service_out"
    [[ $ready == READY ]] || { printf 'FAIL: no test daemon handshake\n'; exit 1; }
}
stop_daemon() {
    kill -TERM "$daemon_pid"
    wait "$daemon_pid"
    daemon_pid=
    if [[ $transport == stdio ]]; then
        exec {service_in}>&-
        exec {service_out}<&-
    fi
}
pstate=/sys/devices/system/cpu/intel_pstate
fan_path=/sys/class/hwmon/hwmon4
sensor_path=/sys/class/hwmon/hwmon0
put "$sensor_path/name" coretemp
put "$sensor_path/temp1_input" 70000
put "$fan_path/name" asus
put "$fan_path/pwm1_enable" 2
put "$fan_path/fan1_input" 4200
put /sys/class/hwmon/hwmon8/name nvme
put /sys/class/hwmon/hwmon8/temp1_input 105000
put /sys/class/power_supply/AC0/type Mains
put /sys/class/power_supply/AC0/online 1
put /sys/class/power_supply/BAT0/type Battery
put /sys/class/dmi/id/product_name 'ASUS ExpertBook P2451FB (simulated)'
put /sys/firmware/acpi/platform_profile quiet
put /sys/firmware/acpi/platform_profile_choices 'quiet balanced performance'
put "$pstate/max_perf_pct" 100
put "$pstate/min_perf_pct" 8
put "$pstate/no_turbo" 0
put /sys/devices/system/cpu/cpufreq/policy0/scaling_governor powersave
put /sys/devices/system/cpu/cpufreq/policy0/energy_performance_preference power
put /sys/devices/system/cpu/cpufreq/policy0/energy_performance_available_preferences 'default performance balance_performance balance_power power'
printf 'Integration tests: writes target %s; real hardware is untouched.\n' "$fixture"
start_daemon
expect_status daemon online
expect_status profile automatic
expect_status effective performance
expect_status gpu_fan_control 0
expect_status cpu_rated_rpm -1
expect_status fan_desired_min_rpm 7900
expect_status fan_desired_max_rpm 8000
expect_status fan_rpm_target_supported 0
expect_status fan_trigger_millic 80000
expect_status guard 0
if [[ $transport == socket ]]; then
    [[ $(stat -c %a "$fixture/control.sock") == 600 ]]
    printf 'PASS private control socket permissions\n'
else
    printf 'SKIP socket peer-credential/permission checks: binding is denied in this environment.\n'
fi
expect_file "$fan_path/pwm1_enable" 0
printf 'PASS default AC profile, sensor selection and capability reporting\n'

ctl profile power-saving
expect_file "$pstate/max_perf_pct" 45
expect_file "$pstate/no_turbo" 1
expect_file "$fan_path/pwm1_enable" 0
expect_file /sys/firmware/acpi/platform_profile quiet
ctl profile medium
expect_file "$pstate/max_perf_pct" 70
expect_file "$fan_path/pwm1_enable" 0
expect_file /sys/firmware/acpi/platform_profile balanced
ctl cap 55
expect_file "$pstate/max_perf_pct" 55
ctl boost on
expect_file "$pstate/no_turbo" 0
ctl fan full
expect_file "$fan_path/pwm1_enable" 0
reject fan auto
ctl fan default
expect_file "$fan_path/pwm1_enable" 0
put "$fan_path/fan1_input" 5800
expect_status cpu_peak_rpm 5800
expect_status cpu_rpm 5800
expect_status cpu_rated_rpm -1
printf 'PASS profile presets, manual fan/CPU controls and measured RPM\n'
put "$fan_path/pwm1_enable" 2
wait_file "$fan_path/pwm1_enable" 0
expect_file "$pstate/max_perf_pct" 55
printf 'PASS AC maximum fan is reapplied after a firmware-mode reset, without faking requested RPM\n'

before=$(sha256sum "$fixture/settings")
reject cap 0
reject cap 101
reject cap -1
reject boost maybe
reject profile unknown
reject profile medium extra
reject battery-profile performance
reject ac-profile medium
reject ac-profile power-saving
reject write /etc/passwd
after=$(sha256sum "$fixture/settings")
[[ $before == "$after" ]]
if build/fanctl --simulate "$fixture" --socket "$fixture/control.sock" \
    --state "$fixture/settings" --uid "$(id -u)" \
    "$( [[ $transport == stdio ]] && printf '%s' --stdio || printf '%s' --daemon )" >"$fixture/second.log" 2>&1; then
    printf 'FAIL: duplicate daemon accepted\n'; exit 1
fi
expect_status daemon online
printf 'PASS rejected requests preserve settings; duplicate daemon is rejected\n'

ctl profile performance
put "$sensor_path/temp1_input" 85000
ctl fan default
expect_status guard 1
expect_file "$pstate/max_perf_pct" 60
expect_file "$pstate/no_turbo" 1
expect_file "$fan_path/pwm1_enable" 0
put "$sensor_path/temp1_input" 90000
ctl fan default
expect_status guard 2
expect_file "$pstate/max_perf_pct" 40
put "$sensor_path/temp1_input" 87000
ctl fan default
expect_file "$pstate/max_perf_pct" 40
put "$sensor_path/temp1_input" 85000
ctl fan default
expect_file "$pstate/max_perf_pct" 60
put "$sensor_path/temp1_input" 80000
ctl fan default
expect_status guard 1
put "$sensor_path/temp1_input" 78000
ctl fan default
expect_status guard 0
expect_file "$pstate/max_perf_pct" 100
expect_file "$pstate/no_turbo" 0
expect_file "$fan_path/pwm1_enable" 0
printf 'PASS 85/90 C overrides, hysteresis and restoration of manual selection\n'
put "$sensor_path/temp1_input" 75000
ctl fan default

ctl profile automatic
put /sys/class/power_supply/AC0/online 0
wait_file "$pstate/max_perf_pct" 45
expect_file "$fan_path/pwm1_enable" 2
expect_status effective power-saving
put /sys/class/power_supply/AC0/online 1
wait_file "$pstate/max_perf_pct" 100
expect_file "$fan_path/pwm1_enable" 0
ctl battery-profile medium
put /sys/class/power_supply/AC0/online 0
wait_file "$pstate/max_perf_pct" 70
ctl profile performance
expect_status effective power-saving
expect_file "$pstate/max_perf_pct" 45
put /sys/class/power_supply/AC0/online 1
ctl profile automatic
printf 'PASS unattended charger switching, configurable battery profile and AC-only performance\n'

put /sys/class/power_supply/AC0/online 0
put "$sensor_path/temp1_input" 75000
ctl profile medium
expect_file "$fan_path/pwm1_enable" 2
put "$sensor_path/temp1_input" 79999
ctl fan auto
expect_status fan_temperature_boost 0
expect_file "$fan_path/pwm1_enable" 2
put "$sensor_path/temp1_input" 80000
ctl fan auto
expect_status fan_temperature_boost 1
expect_status guard 0
expect_file "$fan_path/pwm1_enable" 0
expect_file "$pstate/max_perf_pct" 70
put "$sensor_path/temp1_input" 79000
ctl fan default
expect_status fan_temperature_boost 1
expect_file "$fan_path/pwm1_enable" 0
put "$sensor_path/temp1_input" 75000
ctl fan auto
expect_status fan_temperature_boost 0
expect_file "$fan_path/pwm1_enable" 2
put /sys/class/power_supply/AC0/online 1
ctl profile automatic
expect_file "$fan_path/pwm1_enable" 0
printf 'PASS exact 80 C fan trigger on battery, 75 C recovery and separate CPU overheating thresholds\n'

rm -- "$fixture$sensor_path/temp1_input"
ctl fan default
expect_status guard 2
expect_file "$pstate/max_perf_pct" 40
expect_file "$fan_path/pwm1_enable" 0
put "$sensor_path/temp1_input" 70000
mv -- "$fixture$sensor_path" "$fixture/sys/class/hwmon/hwmon99"
sensor_path=/sys/class/hwmon/hwmon99
ctl profile medium
expect_status guard 0
expect_file "$pstate/max_perf_pct" 70
put "$fan_path/pwm1_auto_point1_pwm" 100
expect_status cpu_fan_control 0
reject fan full
rm -- "$fixture$fan_path/pwm1_auto_point1_pwm"
ctl fan default
put "$fan_path/pwm1_enable" 2
mv -- "$fixture$fan_path/pwm1_enable" "$fixture$fan_path/original_mode"
ln -s original_mode "$fixture$fan_path/pwm1_enable"
reject fan full
expect_file "$fan_path/original_mode" 2
rm -- "$fixture$fan_path/pwm1_enable"
mv -- "$fixture$fan_path/original_mode" "$fixture$fan_path/pwm1_enable"
ctl fan default
printf 'PASS missing sensor fallback, renumbering, curve exclusion and rejected symlink writes\n'

ctl profile medium
stop_daemon
expect_file "$pstate/max_perf_pct" 100
expect_file "$pstate/no_turbo" 0
expect_file /sys/firmware/acpi/platform_profile quiet
expect_file "$fan_path/pwm1_enable" 2
start_daemon
expect_status profile medium
expect_status battery_profile medium
expect_file "$pstate/max_perf_pct" 70
printf 'PASS saved preferences and restoration on clean service stop\n'
stop_daemon
printf 'v1 3 1 1 0 0 -1 1\n' > "$fixture/settings"
start_daemon
expect_status profile automatic
expect_status ac_profile performance
expect_status effective performance
expect_file "$pstate/max_perf_pct" 100
expect_file "$fan_path/pwm1_enable" 0
printf 'PASS older low-power AC and automatic-fan preferences migrate to maximum cooling with AC Performance\n'

stop_daemon
chmod 0444 "$fixture$pstate/max_perf_pct" "$fixture$pstate/min_perf_pct" \
    "$fixture$pstate/no_turbo" "$fixture/sys/firmware/acpi/platform_profile" \
    "$fixture/sys/devices/system/cpu/cpufreq/policy0/scaling_governor" \
    "$fixture/sys/devices/system/cpu/cpufreq/policy0/energy_performance_preference"
start_daemon
expect_status controller_mode user
expect_status intel_control 0
expect_status cpu_controls_locked 1
expect_status cpu_fan_write_access 1
expect_status error ""
ctl fan full
expect_file "$fan_path/pwm1_enable" 0
expect_file "$pstate/max_perf_pct" 100
expect_file "$pstate/no_turbo" 0
reject cap 55
reject boost off
put "$sensor_path/temp1_input" 90000
ctl fan default
expect_status guard 2
expect_status error ""
expect_file "$fan_path/pwm1_enable" 0
expect_file "$pstate/max_perf_pct" 100
put "$sensor_path/temp1_input" 75000
put /sys/class/power_supply/AC0/online 0
ctl fan auto
expect_file "$fan_path/pwm1_enable" 2
put "$sensor_path/temp1_input" 80000
ctl fan default
expect_status fan_temperature_boost 1
expect_file "$fan_path/pwm1_enable" 0
stop_daemon
chmod 0644 "$fixture$pstate/max_perf_pct" "$fixture$pstate/min_perf_pct" \
    "$fixture$pstate/no_turbo" "$fixture/sys/firmware/acpi/platform_profile" \
    "$fixture/sys/devices/system/cpu/cpufreq/policy0/scaling_governor" \
    "$fixture/sys/devices/system/cpu/cpufreq/policy0/energy_performance_preference"
put "$sensor_path/temp1_input" 70000
put /sys/class/power_supply/AC0/online 1
start_daemon
printf 'PASS ordinary-user fan controls and 80 C trigger work with protected CPU/firmware files, without administrator access\n'

QT_QPA_PLATFORM=offscreen build/fan-monitor --self-test --screenshot build/tray-preview.png
stat --printf='%n %s bytes\n' build/tray-preview.png
sha256sum build/tray-preview.png

if command -v xvfb-run >/dev/null && command -v xauth >/dev/null; then
    if timeout 10s xvfb-run -a true >"$fixture/display.log" 2>&1; then
        GDK_BACKEND=x11 xvfb-run -a build/fan-control-gui --socket "$fixture/control.sock" \
            --smoke-test --screenshot build/gui-preview.png
        stat --printf='%n %s bytes\n' build/gui-preview.png
        sha256sum build/gui-preview.png
    else
        printf 'SKIP GUI rendering test: Xvfb cannot start in this environment.\n'
        cat "$fixture/display.log"
    fi
else
    printf 'SKIP GUI display test: install Xvfb and xauth to enable it.\n'
fi
ctl release
expect_status enabled 0
expect_status effective firmware
expect_file "$pstate/max_perf_pct" 100
expect_file "$fan_path/pwm1_enable" 2
ctl profile power-saving
expect_file "$pstate/max_perf_pct" 45
printf 'PASS restore-to-firmware action and resuming a profile\n'

printf 'Testing installation in a temporary prefix with spaces.\n'
XDG_CONFIG_HOME="$fixture/user config" bash install.sh --local --prefix "$fixture/user prefix"
desktop-file-validate "$fixture/user prefix/share/applications/fan-control-center.desktop"
[[ -x "$fixture/user prefix/bin/fan-control-center" ]]
cmp build/fanctl "$fixture/user prefix/lib/fan-control-center/fanctl"
cmp build/fan-monitor "$fixture/user prefix/lib/fan-control-center/fan-monitor"
desktop-file-validate "$fixture/user config/autostart/fan-monitor.desktop"
[[ -x "$fixture/user prefix/bin/fan-monitor" ]]
XDG_CONFIG_HOME="$fixture/user config" bash install.sh --remove-user --prefix "$fixture/user prefix"
[[ ! -e "$fixture/user prefix/share/applications/fan-control-center.desktop" ]]
[[ ! -e "$fixture/user config/autostart/fan-monitor.desktop" ]]
printf 'PASS user installation, launcher, binary integrity and removal\n'
printf 'All available integration checks passed. Skipped checks are listed above.\n'
