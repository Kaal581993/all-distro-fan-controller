#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
source_directory=$PWD
mode=local
desktop_user=${SUDO_USER:-$(id -un)}
prefix=${HOME}/.local
config_directory=${XDG_CONFIG_HOME:-$HOME/.config}
usage() {
    cat <<'HELP'
Usage:
  ./install.sh                          Build and install the GUI/menu launcher
  ./install.sh --local --prefix PATH    Install to a different user prefix
  sudo ./install.sh --system --user NAME
                                       Optional: enable privileged CPU controls
  ./install.sh --remove-user            Remove the user launcher and GUI
  sudo ./install.sh --remove-system     Stop and remove the hardware service

Install the user GUI and open it. Fan control starts under your desktop account.
Fan operation needs no system service, administrator prompt or password.
No packages are downloaded or installed by this script.
HELP
}
while (($#)); do
    case $1 in
        --local) mode=local; shift ;;
        --system) mode=system; shift ;;
        --remove-user) mode=remove-user; shift ;;
        --remove-system) mode=remove-system; shift ;;
        --user) [[ $# -ge 2 ]] || { usage; exit 1; }; desktop_user=$2; shift 2 ;;
        --prefix) [[ $# -ge 2 ]] || { usage; exit 1; }; prefix=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
done
[[ $prefix == /* && $prefix != / && $prefix != *$'\n'* && $prefix != *$'\r'* ]] ||
    { printf 'The prefix must be an absolute user directory.\n' >&2; exit 1; }
if [[ $mode == system || $mode == remove-system ]]; then
    [[ $EUID == 0 ]] || { printf 'Run this mode with sudo.\n' >&2; exit 1; }
    review_log=/var/log/fan-control-center-install.log
else
    [[ $EUID != 0 ]] || { printf 'Install the GUI as your desktop user, without sudo.\n' >&2; exit 1; }
    mkdir -p .review
    review_log=$source_directory/.review/operations.log
fi
exec > >(tee -a "$review_log") 2>&1
printf '\n[%s] INSTALL mode=%s prefix=%s\n' "$(date -Is)" "$mode" "$prefix"
app_directory=$prefix/lib/fan-control-center
menu_file=$prefix/share/applications/fan-control-center.desktop
icon_file=$prefix/share/icons/hicolor/scalable/apps/fan-control-center.svg
monitor_menu=$prefix/share/applications/fan-monitor.desktop
monitor_autostart=$config_directory/autostart/fan-monitor.desktop

show_artifact() {
    stat --printf='%n %s bytes\n' "$@"
    sha256sum "$@"
}
install_text() {
    local source=$1 target=$2 permissions=$3 previous
    previous=$(mktemp)
    if [[ -f $target ]]; then cp -- "$target" "$previous"; fi
    install -m "$permissions" "$source" "$target"
    printf '\nINSTALLED SOURCE/CONFIGURATION: %s\n' "$target"
    cat -- "$target"
    printf '\nINCREMENTAL DIFF: %s\n' "$target"
    diff -u --label "previous/$target" --label "current/$target" "$previous" "$target" || [[ $? == 1 ]]
    rm -f -- "$previous"
}
desktop_escape() {
    local text=$1 backtick=$'\x60'
    text=${text//\\/\\\\}
    text=${text//\"/\\\"}
    text=${text//\$/\\\$}
    text=${text//"$backtick"/\\$backtick}
    text=${text//%/%%}
    printf '%s' "$text"
}
install_desktop() {
    local source=$1 target=$2 executable=$3 temporary
    temporary=$(mktemp)
    while IFS= read -r line; do
        if [[ $line == Exec=* ]]; then
            printf 'Exec="%s"\n' "$(desktop_escape "$executable")"
        else
            printf '%s\n' "$line"
        fi
    done < "$source" > "$temporary"
    install_text "$temporary" "$target" 0644
    rm -f -- "$temporary"
    desktop-file-validate "$target"
}

case $mode in
    local)
        for tool in cc make pkg-config desktop-file-validate; do
            command -v "$tool" >/dev/null || {
                printf 'Missing %s. On Garuda/Arch install: sudo pacman -S --needed gcc make pkgconf gtk3 qt6-base desktop-file-utils\n' "$tool"
                exit 1
            }
        done
        pkg-config --exists gtk+-3.0 || { printf 'GTK 3 development files are missing.\n'; exit 1; }
        pkg-config --exists Qt6Widgets || { printf 'Qt 6 Widgets is missing. Install qt6-base.\n'; exit 1; }
        printf 'BUILD COMMAND: make all\n'
        make all
        show_artifact build/fanctl build/fan-control-gui build/fan-monitor
        install -d "$app_directory" "$prefix/bin" "$(dirname -- "$menu_file")" \
            "$(dirname -- "$icon_file")" "$(dirname -- "$monitor_autostart")"
        for file in fanctl fan-control-gui fan-monitor; do
            if [[ -f $app_directory/$file ]]; then
                printf '\nPREVIOUS BINARY\n'
                show_artifact "$app_directory/$file"
            fi
            install -m 0755 "build/$file" "$app_directory/$file"
            show_artifact "$app_directory/$file"
        done
        install_text launch.sh "$app_directory/launch.sh" 0755
        install_text README.md "$app_directory/README.md" 0644
        install_text packaging/fan-control-center.svg "$icon_file" 0644
        ln -sfn "$app_directory/launch.sh" "$prefix/bin/fan-control-center"
        ln -sfn "$app_directory/fanctl" "$prefix/bin/fanctl"
        ln -sfn "$app_directory/fan-monitor" "$prefix/bin/fan-monitor"
        install_desktop packaging/fan-control-center.desktop "$menu_file" "$app_directory/launch.sh"
        install_desktop packaging/fan-monitor.desktop "$monitor_menu" "$app_directory/fan-monitor"
        install_desktop packaging/fan-monitor.desktop "$monitor_autostart" "$app_directory/fan-monitor"
        if command -v update-desktop-database >/dev/null; then
            update-desktop-database "$prefix/share/applications"
        fi
        if command -v gtk-update-icon-cache >/dev/null && [[ -f $prefix/share/icons/hicolor/index.theme ]]; then
            gtk-update-icon-cache -f "$prefix/share/icons/hicolor"
        fi
        printf '\nGUI installed. Open “Fan Control Center” from the application menu.\n'
        printf 'Fan Monitor is installed in the menu and will start automatically at desktop login.\n'
        printf 'Start it now using the GUI button or: %q\n' "$prefix/bin/fan-monitor"
        printf 'Fan controls start automatically in your desktop account, without administrator access.\n'
        printf 'User-controller diagnostics: ~/.local/state/fan-control-center/controller.log\n'
        ;;
    system)
        systemctl show --property=Version --value >/dev/null
        desktop_uid=$(id -u -- "$desktop_user")
        [[ $desktop_uid != 0 ]] || { printf 'Choose your non-root desktop user with --user NAME.\n'; exit 1; }
        [[ -s build/fanctl && -x build/fanctl ]] || {
            printf 'Build/install as your desktop user first: ./install.sh\n'; exit 1;
        }
        conflicts=()
        for service in power-profiles-daemon.service tlp.service tuned.service asusd.service nbfc_service.service fancontrol.service; do
            if systemctl is-active --quiet "$service"; then conflicts+=("$service"); fi
        done
        if ((${#conflicts[@]})); then
            printf 'Another active service controls the same hardware: %s\n' "${conflicts[*]}"
            printf 'Choose one controller. To use this app, stop/disable those services yourself, then rerun installation.\n'
            exit 1
        fi
        printf 'Installing service for %s (uid %s)\n' "$desktop_user" "$desktop_uid"
        show_artifact build/fanctl
        if systemctl is-active --quiet fan-control-center.service; then
            systemctl stop fan-control-center.service
        fi
        install -d -m 0755 /usr/local/lib/fan-control-center
        install -o root -g root -m 0755 build/fanctl /usr/local/lib/fan-control-center/fanctl
        install -d -m 0755 /etc/systemd/system
        unit_temporary=$(mktemp)
        sed "s/@UID@/$desktop_uid/g" packaging/fan-control-center.service.in > "$unit_temporary"
        install_text "$unit_temporary" /etc/systemd/system/fan-control-center.service 0644
        rm -f -- "$unit_temporary"
        chown root:root /etc/systemd/system/fan-control-center.service
        chmod 0644 /etc/systemd/system/fan-control-center.service
        printf '\nINSTALLED SERVICE\n'
        cat /etc/systemd/system/fan-control-center.service
        show_artifact /usr/local/lib/fan-control-center/fanctl
        systemd-analyze verify /etc/systemd/system/fan-control-center.service
        systemctl daemon-reload
        systemctl enable --now fan-control-center.service
        systemctl --no-pager --full status fan-control-center.service
        /usr/local/lib/fan-control-center/fanctl status
        ;;
    remove-user)
        rm -f -- "$menu_file" "$monitor_menu" "$monitor_autostart" "$icon_file" \
            "$prefix/bin/fan-control-center" "$prefix/bin/fanctl" "$prefix/bin/fan-monitor"
        rm -f -- "$app_directory/fanctl" "$app_directory/fan-control-gui" "$app_directory/fan-monitor" \
            "$app_directory/launch.sh" "$app_directory/README.md"
        rmdir -- "$app_directory" 2>/dev/null || true
        printf 'Removed the user GUI and launcher. The system service is managed separately.\n'
        ;;
    remove-system)
        systemctl disable --now fan-control-center.service
        rm -f /etc/systemd/system/fan-control-center.service /usr/local/lib/fan-control-center/fanctl
        rmdir /usr/local/lib/fan-control-center 2>/dev/null || true
        systemctl daemon-reload
        printf 'Service removed. Preferences retained in /var/lib/fan-control-center.\n'
        ;;
esac
