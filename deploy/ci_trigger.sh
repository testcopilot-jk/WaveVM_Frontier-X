#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

REPO_020_DIR="${WAVEVM_REPO_020_DIR:-${ROOT_DIR}/../wavevm-test-020}"
REPO_021_DIR="${WAVEVM_REPO_021_DIR:-${ROOT_DIR}/../wavevm-test-021}"

if [[ ! -d "${REPO_020_DIR}/.git" ]]; then
  echo "[ERROR] 020 repo not found: ${REPO_020_DIR}" >&2
  exit 1
fi
if [[ ! -d "${REPO_021_DIR}/.git" ]]; then
  echo "[ERROR] 021 repo not found: ${REPO_021_DIR}" >&2
  exit 1
fi

if ! command -v ssh >/dev/null 2>&1; then
  echo "[ERROR] ssh not found" >&2
  exit 1
fi

if [[ -z "${GIT_SSH_COMMAND:-}" ]]; then
  export GIT_SSH_COMMAND="ssh -F ${HOME}/.ssh/config"
fi

push_empty_commit() {
  local repo_dir="$1"
  local message="$2"
  git -C "${repo_dir}" add -A
  git -C "${repo_dir}" commit --allow-empty -m "${message}" >/dev/null
  git -C "${repo_dir}" push origin main
}

echo "[INFO] triggering 020 pipeline from ${REPO_020_DIR}"
push_empty_commit "${REPO_020_DIR}" "ci: trigger pipeline for 020"

echo "[INFO] triggering 021 pipeline from ${REPO_021_DIR}"
push_empty_commit "${REPO_021_DIR}" "ci: trigger pipeline for 021"

echo "[INFO] done"
