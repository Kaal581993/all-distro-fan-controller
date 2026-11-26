#!/usr/bin/env bash
set -euo pipefail
launcher=$(readlink -f -- "${BASH_SOURCE[0]}")
directory=$(dirname -- "$launcher")
if [[ -x $directory/fan-control-gui ]]; then
    executable=$directory/fan-control-gui
elif [[ -x $directory/build/fan-control-gui ]]; then
    executable=$directory/build/fan-control-gui
else
    printf 'Build Fan Control Center first: ./install.sh\n' >&2
    exit 1
fi
state_directory=${XDG_STATE_HOME:-$HOME/.local/state}/fan-control-center
mkdir -p -- "$state_directory"
exec "$executable" "$@" >>"$state_directory/gui.log" 2>&1
