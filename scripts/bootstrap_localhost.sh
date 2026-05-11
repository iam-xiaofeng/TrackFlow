#!/usr/bin/env bash
# One-command bootstrap for a new localhost-only TrackFlow machine.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

echo "[TrackFlow] Step 1/4: install system dependencies"
if [[ $EUID -eq 0 ]]; then
  ./scripts/install_deps.sh
else
  sudo ./scripts/install_deps.sh
fi

echo "[TrackFlow] Step 2/4: install ONNX Runtime (third_party)"
./scripts/install_onnxruntime.sh

echo "[TrackFlow] Step 3/4: install uWebSockets (third_party)"
./scripts/install_uwebsockets.sh

echo "[TrackFlow] Step 4/4: build project"
./scripts/build.sh Release

echo ""
echo "Bootstrap complete."
echo "Run locally with: ./scripts/run_localhost.sh"
