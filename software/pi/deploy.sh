#!/usr/bin/env bash
# From the laptop: copy the Pi code to the box and restart it.
# Usage: ./deploy.sh [pi@spicetracks.local]
set -e
HOST=${1:-pi@spicetracks.local}
rsync -av --delete --exclude .venv --exclude state.json --exclude .spotify_token --exclude __pycache__ \
  "$(dirname "$0")/" "$HOST:spicetracks-software/pi/"
ssh "$HOST" 'cd spicetracks-software/pi && .venv/bin/pip install -q -r requirements.txt && systemctl --user restart spicetracks && sleep 2 && systemctl --user --no-pager status spicetracks | head -5'
