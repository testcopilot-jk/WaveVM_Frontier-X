#!/usr/bin/env bash
set -euo pipefail

# One-shot bootstrap for a fresh GitHub Codespace.
# Installs core tooling, Tailscale, and optional sshd.
#
# Usage:
#   bash deploy/codespace_bootstrap.sh
#
# Optional env:
#   TS_AUTHKEY=tskey-...            # If set, script runs `tailscale up`
#   TS_HOSTNAME=wavevm-codespace    # Optional hostname passed to tailscale up
#   ENABLE_SSHD=1                   # Install and start openssh-server

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNTIME_DIR="${ROOT_DIR}/.codespace/runtime"
TAILSCALE_DIR="${RUNTIME_DIR}/tailscale"
TAILSCALED_SOCKET="${TAILSCALE_DIR}/tailscaled.sock"
TAILSCALED_STATE="${TAILSCALE_DIR}/tailscaled.state"
TAILSCALED_LOG="${TAILSCALE_DIR}/tailscaled.log"

if [[ "${EUID}" -ne 0 ]]; then
  echo "[ERROR] Please run as root."
  echo "Try: sudo bash deploy/codespace_bootstrap.sh"
  exit 1
fi

mkdir -p "${TAILSCALE_DIR}"

echo "[1/5] Installing base packages..."
export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y --no-install-recommends \
  ca-certificates curl git jq make tmux unzip \
  openssh-client iproute2 iputils-ping net-tools \
  build-essential pkg-config

if [[ "${ENABLE_SSHD:-0}" == "1" ]]; then
  apt-get install -y --no-install-recommends openssh-server
  mkdir -p /var/run/sshd
  sed -i 's/^#\?PasswordAuthentication.*/PasswordAuthentication no/' /etc/ssh/sshd_config
  sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin prohibit-password/' /etc/ssh/sshd_config
  if command -v service >/dev/null 2>&1; then
    service ssh restart || service ssh start || true
  fi
fi

echo "[2/5] Installing Tailscale (if missing)..."
if ! command -v tailscale >/dev/null 2>&1; then
  curl -fsSL https://tailscale.com/install.sh | sh
fi

echo "[3/5] Starting tailscaled..."
if pgrep -x tailscaled >/dev/null 2>&1; then
  echo "tailscaled is already running."
else
  TS_TUN_MODE="userspace-networking"
  if [[ -c /dev/net/tun ]]; then
    TS_TUN_MODE="auto"
  fi

  nohup tailscaled \
    --state="${TAILSCALED_STATE}" \
    --socket="${TAILSCALED_SOCKET}" \
    --tun="${TS_TUN_MODE}" \
    > "${TAILSCALED_LOG}" 2>&1 &

  sleep 2
fi

echo "[4/5] tailscaled health check..."
if ! tailscale --socket "${TAILSCALED_SOCKET}" version >/dev/null 2>&1; then
  echo "[WARN] tailscaled may not be ready yet."
  echo "Check log: ${TAILSCALED_LOG}"
fi

if [[ -n "${TS_AUTHKEY:-}" ]]; then
  echo "[5/5] Running tailscale up..."
  UP_ARGS=(--socket "${TAILSCALED_SOCKET}" up --authkey "${TS_AUTHKEY}" --accept-routes)
  if [[ -n "${TS_HOSTNAME:-}" ]]; then
    UP_ARGS+=(--hostname "${TS_HOSTNAME}")
  fi
  tailscale "${UP_ARGS[@]}"
else
  echo "[5/5] Skipping tailscale up (TS_AUTHKEY not set)."
  echo "Run manually:"
  echo "  tailscale --socket ${TAILSCALED_SOCKET} up --authkey <TS_AUTHKEY> --accept-routes"
fi

echo
echo "Done."
echo "Tailscale socket: ${TAILSCALED_SOCKET}"
echo "Tailscale state : ${TAILSCALED_STATE}"
echo "Tailscale log   : ${TAILSCALED_LOG}"
echo
echo "Quick checks:"
echo "  tailscale --socket ${TAILSCALED_SOCKET} status"
echo "  tailscale --socket ${TAILSCALED_SOCKET} ip -4"

