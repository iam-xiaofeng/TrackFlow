#!/usr/bin/env bash
# Start TrackFlow backend + localhost nginx gateway, then print local URL.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SERVER_BIN="$PROJECT_DIR/build/yolo_edge_server"
CONFIG_FILE="$PROJECT_DIR/config/config.yaml"
LOCAL_NGINX_CONF="$PROJECT_DIR/deploy/nginx/trackflow.localhost.conf"
ENABLED_CONF="/etc/nginx/sites-enabled/trackflow-localhost"
AVAILABLE_CONF="/etc/nginx/sites-available/trackflow-localhost"
TMP_CONF=""

cleanup() {
  if [[ -n "${TMP_CONF}" && -f "${TMP_CONF}" ]]; then
    rm -f "${TMP_CONF}"
  fi
}
trap cleanup EXIT

if [[ ! -x "$SERVER_BIN" ]]; then
  echo "Missing binary: $SERVER_BIN"
  echo "Please run: ./scripts/bootstrap_localhost.sh"
  exit 1
fi

if [[ ! -f "$CONFIG_FILE" ]]; then
  echo "Missing config: $CONFIG_FILE"
  exit 1
fi

if [[ ! -f "$LOCAL_NGINX_CONF" ]]; then
  echo "Missing nginx template: $LOCAL_NGINX_CONF"
  exit 1
fi

echo "[TrackFlow] stopping stale backend process (if any)"
pkill -f "${SERVER_BIN}" >/dev/null 2>&1 || true

echo "[TrackFlow] starting backend on ws://127.0.0.1:9001"
nohup "$SERVER_BIN" -c "$CONFIG_FILE" > "$PROJECT_DIR/trackflow.log" 2>&1 &
sleep 1

if ! pgrep -f "${SERVER_BIN}" >/dev/null 2>&1; then
  echo "Backend failed to start. See: $PROJECT_DIR/trackflow.log"
  exit 1
fi

if ! command -v nginx >/dev/null 2>&1; then
  echo "nginx not found. Install it first: sudo apt-get install -y nginx"
  exit 1
fi

echo "[TrackFlow] installing localhost nginx site"
TMP_CONF="$(mktemp)"
sed "s/__TRACKFLOW_ROOT__/${PROJECT_DIR//\//\\/}/g" "$LOCAL_NGINX_CONF" > "$TMP_CONF"
if [[ $EUID -eq 0 ]]; then
  install -m 644 "$TMP_CONF" "$AVAILABLE_CONF"
  ln -sfn "$AVAILABLE_CONF" "$ENABLED_CONF"
  nginx -t
  systemctl reload nginx
else
  sudo install -m 644 "$TMP_CONF" "$AVAILABLE_CONF"
  sudo ln -sfn "$AVAILABLE_CONF" "$ENABLED_CONF"
  sudo nginx -t
  sudo systemctl reload nginx
fi

echo ""
echo "TrackFlow is ready."
echo "Open in browser: http://localhost:8080/trackflow_dashboard.html"
echo "Legacy URL also works: http://localhost:8080/test_v5.html"
echo "Backend log: $PROJECT_DIR/trackflow.log"
