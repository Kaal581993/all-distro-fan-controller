# Fan Control Center

A native desktop GUI and background hardware service for this ASUS ExpertBook
P2451FB, with a live KDE taskbar/system-tray fan monitor.
Select **Power Saving**, **Medium Throttle**, **Performance**, or
**Automatic** in the window. It shows live CPU temperature and fan RPM.
The service keeps running when you close the window. All active profiles request
maximum fan speed while plugged in; on battery maximum cooling starts at 80 °C.

## Install once

From a terminal in your desktop session:

~~~sh
cd /home/kaal/fan-control-center
./install.sh
~~~

Then open **Fan Control Center** from the application/start menu, or run
~/.local/bin/fan-control-center. The GUI and taskbar monitor automatically start
a controller under your normal desktop account. **Fan operation requires no
administrator password or system service.** Click **Maximum fan** in the main
window to request the firmware's highest speed.

Each desktop account has its own controller connection and preferences.
This laptop's fan-mode attribute already grants ordinary users write access
(mode 0666), so fan operation is available to ordinary users. The app does
not change device permissions, use sudo/pkexec, or bypass operating-system policy.
On a system where the fan attribute is read-only, that hardware permission
must be configured separately; the app reports the restriction.

Dependencies on Garuda/Arch, if missing:

~~~sh
sudo pacman -S --needed gcc make pkgconf gtk3 qt6-base desktop-file-utils
~~~

The installer prints build commands, the installed launcher, binary sizes and
SHA-256 checksums. It does not download or install packages. It refuses to
start alongside active power-profiles-daemon, TLP, tuned, asusd, NBFC or
fancontrol services. Choose one hardware controller before installation.
The conflict check applies only to the optional system-service installation.
Ordinary fan control does not require installing that service.

## Select a profile

| Selection | Firmware profile | Intel CPU ceiling | Turbo | Fan |
| --- | --- | --- | --- | --- |
| Power Saving | Quiet | 45% | Off | Maximum on AC; automatic on cool battery |
| Medium Throttle | Balanced | 70% | Off | Maximum on AC; automatic on cool battery |
| Performance | Performance | 100% | On | Firmware maximum |
| Automatic | Performance on AC; Power Saving on battery | Follows profile | Follows profile | Follows profile |

These ceilings limit Intel P-state performance, not CPU utilization or a fixed
clock speed. Power Saving and Medium Throttle are application presets,
not manufacturer-certified limits. All settings remain subject to firmware
limits and the temperature override.
The CPU ceilings in the table apply only when CPU power files are writable.
On a normal account where those files are protected, CPU profile cards and
throttle settings remain unavailable; **Maximum fan** and automatic cooling work
independently. The app reports actual CPU settings without claiming to change them.

Performance requires confirmed external power. If selected on battery,
Power Saving runs until the charger is connected. Automatic mode rechecks
the source every two seconds. Its plugged-in profile is fixed to Performance;
the battery profile can be selected in the expandable settings panel.
Manually choosing Power Saving or Medium Throttle still changes the CPU ceiling,
while the fan stays at maximum on AC. CPU overheating protection remains active.

That panel also has Firmware Auto/Maximum buttons, a 20–100% CPU ceiling slider,
turbo controls, and actual firmware/CPU settings. Firmware Auto is unavailable
while plugged in, and while a temperature override is active. Selecting a profile clears
manual overrides and applies the preset. Preferences are remembered in
/var/lib/fan-control-center/settings.

## Taskbar fan monitor

Open **Fan Monitor** from the application menu, click **Show taskbar fan monitor**
in the main GUI, or run:

~~~sh
~/.local/bin/fan-monitor
~~~

The taskbar/system-tray icon shows measured fan speed in thousands of RPM
(for example, 6.3k means 6,300 RPM) and CPU temperature. Hover for exact RPM,
temperature, power source and whether the hardware service is active.
Click the icon to open the GUI; right-click for profile shortcuts.
The icon changes color at 80 °C and 90 °C. Quitting the monitor leaves the
hardware service running.

The installer adds an XDG autostart entry at
~/.config/autostart/fan-monitor.desktop, respecting XDG_CONFIG_HOME.
The monitor starts on the next desktop login. On KDE, use
**System Tray Settings → Entries → Fan Monitor → Always shown** if you want
the icon pinned outside the hidden-icons area.
The monitor also starts the normal-user fan controller. No root service is needed.

## Manufacturer maximum RPM

Initial discovery found ASUS WMI fan control, about 82 °C CPU temperature and
4,200 RPM, automatic/full-speed fan modes, and quiet/balanced/performance
firmware profiles. This machine has no writable fan percentage/speed attribute,
no rated maximum RPM attribute, no separate GPU fan control, and no GPU
temperature sensor exposed to hwmon.

[ASUS's P2451FB specifications](https://www.asus.com/bt/laptops/for-work/expertbook/expertbook-p2-p2451/techspec/)
list Fan Profile support but do not give a maximum cooling-fan RPM. A published
rated maximum could not be verified. HDD rotation speeds are unrelated.

**Firmware Maximum** requests ASUS firmware's full-speed mode. Let the fan
settle, then read its actual RPM in the GUI. **Observed peak** is measured
telemetry from the current service run, not a manufacturer rating.
There is no arbitrary RPM setting or fan-off button.

Your requested **7,900–8,000 RPM** range is recorded as a desired range, never
reported as a measured or guaranteed speed. This laptop exposes no numeric RPM
target, so the service sends the full-speed command on every two-second cycle.
It cannot guarantee an exact speed or physically force that range.
It requests maximum on AC regardless of the selected CPU profile and fan override.
Earlier saved low-power AC preferences migrate to Performance in Automatic mode.

The [Linux ASUS WMI driver](https://github.com/torvalds/linux/blob/master/drivers/platform/x86/asus-wmi.c)
maps the legacy mode switch to 0 for full speed and 2 for firmware automatic.
This implementation skips AGFN/PWM and custom-curve interfaces, which have
different semantics. Sensor indexes are rediscovered each cycle.
GPU telemetry/control appears only when corresponding supported interfaces exist.

## Temperature override and restoration

While cooling is active, temperatures are checked every two seconds:

- **80 °C:** request maximum fan on either power source; keep that request
  active until cooling to **75 °C**. This does not lower the CPU ceiling.
- **85 °C:** maximum fan, turbo off, CPU ceiling at most **60%**.
- **90 °C**, or missing CPU temperature: lower that ceiling to at most **40%**.
- The 40% override stays until 85 °C. The 60% override stays until 78 °C.
  Your selected settings then resume.

CPU ceilings/turbo changes are applied only when the operating system grants
write access. Otherwise maximum fan cooling still works, and the machine's
firmware/OS remains responsible for CPU thermal throttling.

These are application thresholds, not an ASUS specification or a guarantee
of temperature. Firmware thermal protection remains active. No raw EC writes,
thermal-trip changes, voltage changes or overclocking are performed.

**Restore firmware control** sets fans to firmware automatic, restores the CPU
and platform values saved before this daemon run changed them, and switches
to monitoring only. The app's temperature override is then inactive until you
select a profile again. A normal service stop also restores those saved settings.

A crash, SIGKILL or power loss cannot run restoration. Systemd restarts the
service after failure. Original-setting snapshots belong to each daemon run;
already-applied settings can become the next run's baseline after a crash.

## Checks and logs

~~~sh
make check
make artifacts
./launch.sh
systemctl status fan-control-center.service
tail -n +1 -F ~/.local/state/fan-control-center/controller.log
tail -n +1 -F .review/operations.log
~~~

Tests write only to a temporary simulated sysfs tree. They cover profiles,
power-source changes, temperature overrides, persistence, rejected requests,
restoration, the 80 °C trigger, AC maximum enforcement, settings migration
ordinary-user fan control with protected CPU files, and monitor/autostart
installation. The Qt monitor's icon and telemetry are
tested offscreen. The main GTK GUI checks use Xvfb when available:

Restricted environments may prohibit socket binding or starting Xvfb. The
hardware engine then runs over pipes limited to unprivileged simulated hardware;
socket/display checks are explicitly reported as skipped. Run make check from
your normal desktop terminal to also exercise those interfaces.

~~~sh
sudo pacman -S --needed xorg-server-xvfb xorg-xauth xdotool
~~~

Source/build/user-install review appends to .review/operations.log.
Optional system installation appends to /var/log/fan-control-center-install.log.
GUI diagnostics append to ~/.local/state/fan-control-center/gui.log, respecting
XDG_STATE_HOME. The monitor is a native C++/Qt desktop component; hardware
access remains in the C service. The service accepts fixed, validated commands through
a private per-account Unix socket. User preferences are kept in
~/.local/state/fan-control-center/user-settings. No network
service or interpreter-based model service is used.

The optional system service remains available for administrators who explicitly
want protected CPU power controls. It is not required for general fan access:

~~~sh
sudo ./install.sh --system --user "$USER"
~~~

## Remove

~~~sh
cd /home/kaal/fan-control-center
./install.sh --remove-user
~~~

Preferences and logs are retained. To update the normal-user app, rerun
./install.sh. If you separately installed the optional system service, remove
it with sudo ./install.sh --remove-system.
