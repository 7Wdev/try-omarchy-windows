#!/bin/bash
# Actual shell output in an OpenGL terminal, inside the disposable guest.
set -eu
trap 'exit 0' HUP INT TERM
printf '\033[2J\033[H\033[1;36mOmarchy\033[0m\n\n'
uname -sr
printf '\n'
ls --color=always /usr/share/omarchy
printf '\n'
printf '%s\n' "$$" > /tmp/omarchy-gpu-terminal-ready
while :; do
  printf '\r\033[K%s  |  %s' "$(date '+%A %H:%M:%S')" "$PWD"
  sleep 1
done
