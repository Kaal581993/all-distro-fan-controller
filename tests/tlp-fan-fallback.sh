#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
fixture=$(mktemp -d /tmp/fcc-fallback.XXXXXX)
daemon_pid=
cleanup() {
    if [[ -n $daemon_pid ]]; then
        kill -KILL "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
    rm -rf -- "$fixture"
}
trap cleanup EXIT
binary=$PWD/build/fan-firmware-auto
monitor=$fixture/sys/class/hwmon/hwmon6
mkdir -p -- "$monitor"
printf 'asus\n' > "$monitor/name"
printf '0\n' > "$monitor/pwm1_enable"
reject() {
    if "$binary" --simulate "$fixture" > "$fixture/rejection.log" 2>&1; then
        printf 'FAIL: unsafe or unsupported fan control was accepted\n' >&2
        exit 1
    fi
}

"$binary" --simulate "$fixture"
[[ $(cat "$monitor/pwm1_enable") == 2 ]]
printf 'PASS full-speed mode returns to firmware automatic\n'

coproc FALLBACK_TEST {
    exec build/fanctl --simulate "$fixture" --socket "$fixture/control.sock" \
        --state "$fixture/settings" --stdio 2> "$fixture/controller.log"
}
daemon_pid=$FALLBACK_TEST_PID
exec {controller_in}>&"${FALLBACK_TEST[1]}"
exec {controller_out}<&"${FALLBACK_TEST[0]}"
IFS= read -r -t 5 ready <&"$controller_out"
[[ $ready == READY ]]
# A missing CPU sensor makes the audited daemon request full cooling.
[[ $(cat "$monitor/pwm1_enable") == 0 ]]
kill -KILL "$daemon_pid"
wait "$daemon_pid" 2>/dev/null || true
daemon_pid=
exec {controller_in}>&-
exec {controller_out}<&-
[[ $(cat "$monitor/pwm1_enable") == 0 ]]
"$binary" --simulate "$fixture"
[[ $(cat "$monitor/pwm1_enable") == 2 ]]
printf 'PASS independent recovery after SIGKILL of the actual controller; missing sensor fails to full cooling\n'

mv -- "$monitor" "$fixture/sys/class/hwmon/hwmon91"
monitor=$fixture/sys/class/hwmon/hwmon91
printf '0\n' > "$monitor/pwm1_enable"
"$binary" --simulate "$fixture"
[[ $(cat "$monitor/pwm1_enable") == 2 ]]
printf 'PASS recovery rediscovers hwmon indexes\n'

printf '0\n' > "$monitor/pwm1_enable"
printf 'nvme\n' > "$monitor/name"
reject
[[ $(cat "$monitor/pwm1_enable") == 0 ]]
printf 'asus\n' > "$monitor/name"
printf '100\n' > "$monitor/pwm1"
reject
[[ $(cat "$monitor/pwm1_enable") == 0 ]]
rm -- "$monitor/pwm1"
printf '100\n' > "$monitor/pwm1_auto_point1_pwm"
reject
[[ $(cat "$monitor/pwm1_enable") == 0 ]]
rm -- "$monitor/pwm1_auto_point1_pwm"
printf 'PASS unrelated sensors, PWM-duty and custom-curve interfaces are untouched\n'

mv -- "$monitor/pwm1_enable" "$monitor/decoy"
ln -s decoy "$monitor/pwm1_enable"
reject
[[ $(cat "$monitor/decoy") == 0 ]]
rm -- "$monitor/pwm1_enable"
mv -- "$monitor/decoy" "$monitor/pwm1_enable"
printf '7\n' > "$monitor/pwm1_enable"
reject
[[ $(cat "$monitor/pwm1_enable") == 7 ]]
printf 'PASS symlink and unknown-mode recovery is rejected\n'

if "$binary" --simulate / > "$fixture/rejection.log" 2>&1; then exit 1; fi
if "$binary" --simulate relative > "$fixture/rejection.log" 2>&1; then exit 1; fi
if "$binary" --unknown > "$fixture/rejection.log" 2>&1; then exit 1; fi
printf 'PASS unsafe simulation arguments are rejected; no live hardware was written\n'
