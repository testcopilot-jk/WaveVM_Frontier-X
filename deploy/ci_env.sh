#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SSH_DIR="${HOME}/.ssh"
mkdir -p "${SSH_DIR}"
chmod 700 "${SSH_DIR}"

write_key_from_env_or_path() {
  local env_value_name="$1"
  local env_path_name="$2"
  local out_path="$3"
  local value="${!env_value_name:-}"
  local src_path="${!env_path_name:-}"

  if [[ -n "${value}" ]]; then
    umask 077
    printf '%s\n' "${value}" > "${out_path}"
  elif [[ -n "${src_path}" ]]; then
    if [[ ! -f "${src_path}" ]]; then
      echo "[ERROR] ${env_path_name} points to missing file: ${src_path}" >&2
      exit 1
    fi
    install -m 600 "${src_path}" "${out_path}"
  else
    echo "[ERROR] missing ${env_value_name} or ${env_path_name}" >&2
    exit 1
  fi
  chmod 600 "${out_path}"
}

write_key_from_env_or_path WAVEVM_BB_KEY_020 WAVEVM_BB_KEY_020_PATH "${SSH_DIR}/bb_key_020"
write_key_from_env_or_path WAVEVM_BB_KEY_021 WAVEVM_BB_KEY_021_PATH "${SSH_DIR}/bb_key_021"

cat > "${SSH_DIR}/config" <<'EOF'
Host bb-jktest020
  HostName bitbucket.org
  User git
  IdentityFile ~/.ssh/bb_key_020
  IdentitiesOnly yes
  StrictHostKeyChecking no

Host bb-jktest021
  HostName bitbucket.org
  User git
  IdentityFile ~/.ssh/bb_key_021
  IdentitiesOnly yes
  StrictHostKeyChecking no
EOF
chmod 600 "${SSH_DIR}/config"

echo "[INFO] Wrote SSH config to ${SSH_DIR}/config"
echo "[INFO] Wrote Bitbucket keys to ${SSH_DIR}/bb_key_020 and ${SSH_DIR}/bb_key_021"
echo "[INFO] Use: export GIT_SSH_COMMAND='ssh -F ${SSH_DIR}/config'"
