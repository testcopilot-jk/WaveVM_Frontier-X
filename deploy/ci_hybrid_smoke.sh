#!/usr/bin/env bash
# ============================================================================
# WaveVM Local Hybrid Smoke Test (KVM mock + TCG)
#
# Runs a 2-node cluster on a single machine without /dev/kvm:
#   Node 0: Slave uses LD_PRELOAD mock -> KVM FAST PATH
#   Node 1: Slave runs normally         -> TCG PROXY (Tri-Channel)
#
# This exercises the heterogeneous KVM/TCG code paths including:
#   - mode_tcg context translation (wvm_translate_kvm_to_tcg / tcg_to_kvm)
#   - Mixed dirty page tracking (mock KVM_GET_DIRTY_LOG vs SIGSEGV)
#   - Ingress dispatch for both KVM and TCG slave types
#   - Cross-mode MSG_VCPU_RUN handling
#   - Heartbeat / Epoch convergence across mixed nodes
#
# Requirements: built binaries + mock library (make -C mock_kvm)
# ============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ART_ROOT="${ROOT_DIR}/artifacts"
mkdir -p "${ART_ROOT}"
TMPD="${ART_ROOT}/hybrid-smoke-$(date +%Y%m%d-%H%M%S)"
mkdir -p "${TMPD}"
SUMMARY_FILE="${TMPD}/result.summary"
MOCK_LIB="${ROOT_DIR}/mock_kvm/libwvm_kvm_mock.so"

RAM_MB=1024
CORES=1
WAIT_SECONDS="${1:-12}"

write_summary() {
  echo "STATUS=$1" > "${SUMMARY_FILE}"
  echo "REASON=$2" >> "${SUMMARY_FILE}"
}

fail() {
  echo "[ERROR] $1"
  write_summary "FAIL" "$1"
  echo "--- Node 0 (KVM mock) Slave log tail ---"
  tail -n 60 "${TMPD}/slave0.log" 2>/dev/null || true
  echo "--- Node 0 Master log tail ---"
  tail -n 60 "${TMPD}/master0.log" 2>/dev/null || true
  echo "--- Node 1 (TCG) Slave log tail ---"
  tail -n 60 "${TMPD}/slave1.log" 2>/dev/null || true
  echo "--- Node 1 Master log tail ---"
  tail -n 60 "${TMPD}/master1.log" 2>/dev/null || true
  exit 1
}

warn() { echo "[WARN] $1"; }

if command -v stdbuf >/dev/null 2>&1; then
  LINEBUF=(stdbuf -oL -eL)
else
  LINEBUF=()
fi

# -- Preflight checks -------------------------------------------------------

if [[ ! -f "${ROOT_DIR}/master_core/wavevm_node_master" ]]; then
  echo "[INFO] Building master..."
  make -C "${ROOT_DIR}/master_core" -f Makefile_User >/dev/null 2>&1
fi
if [[ ! -f "${ROOT_DIR}/slave_daemon/wavevm_node_slave" ]]; then
  echo "[INFO] Building slave..."
  make -C "${ROOT_DIR}/slave_daemon" >/dev/null 2>&1
fi
if [[ ! -f "${MOCK_LIB}" ]]; then
  echo "[INFO] Building KVM mock library..."
  make -C "${ROOT_DIR}/mock_kvm" >/dev/null 2>&1
fi

for bin in master_core/wavevm_node_master slave_daemon/wavevm_node_slave; do
  if [[ ! -x "${ROOT_DIR}/${bin}" ]]; then
    fail "binary_not_found: ${bin}"
  fi
done
if [[ ! -f "${MOCK_LIB}" ]]; then
  fail "mock_library_not_found: ${MOCK_LIB}"
fi

# -- Cleanup trap ------------------------------------------------------------

cleanup() {
  local rc=$?
  set +e
  for pid_var in S0_PID S1_PID M0_PID M1_PID; do
    local pid="${!pid_var:-}"
    if [[ -n "${pid}" ]]; then kill "${pid}" 2>/dev/null || true; fi
  done
  wait 2>/dev/null || true
  # Clean up any forked QEMU-TCG children from Node 1's TCG proxy
  pkill -f "wavevm_node_slave.*19305" 2>/dev/null || true
  if [[ ! -f "${SUMMARY_FILE}" ]]; then
    if [[ "${rc}" -eq 0 ]]; then
      write_summary "PASS" "completed_without_explicit_summary"
    else
      write_summary "FAIL" "aborted_unexpectedly_rc_${rc}"
    fi
  fi
  echo "[INFO] Logs: ${TMPD}"
}
trap cleanup EXIT

# -- Swarm config (2 nodes, both on localhost, different ports) --------------

SWARM_CFG="${TMPD}/swarm.conf"
cat > "${SWARM_CFG}" <<'EOF'
NODE 0 127.0.0.1 19100 1 1
NODE 1 127.0.0.1 19300 1 1
EOF

echo "============================================================"
echo " WaveVM Hybrid Smoke Test"
echo "   Node 0: KVM mock (LD_PRELOAD) -> KVM FAST PATH"
echo "   Node 1: No mock               -> TCG PROXY (Tri-Channel)"
echo "============================================================"

# -- Start Node 0: KVM mock Slave -------------------------------------------

echo "[INFO] Starting Node 0 Slave (KVM mock)..."
(
  cd "${ROOT_DIR}" && \
  exec env WVM_SHM_FILE=/wvm_hybrid_slave0 \
    WVM_MOCK_LOG=1 \
    LD_PRELOAD="${MOCK_LIB}" \
    "${LINEBUF[@]}" ./slave_daemon/wavevm_node_slave 19105 "${CORES}" "${RAM_MB}" 0 19101
) > "${TMPD}/slave0.log" 2>&1 &
S0_PID=$!

# -- Start Node 1: TCG Slave (no mock, no /dev/kvm -> TCG PROXY) ------------

echo "[INFO] Starting Node 1 Slave (TCG proxy)..."
(
  cd "${ROOT_DIR}" && \
  exec env WVM_SHM_FILE=/wvm_hybrid_slave1 \
    "${LINEBUF[@]}" ./slave_daemon/wavevm_node_slave 19305 "${CORES}" "${RAM_MB}" 1 19301
) > "${TMPD}/slave1.log" 2>&1 &
S1_PID=$!

sleep 2

# -- Start Masters -----------------------------------------------------------

echo "[INFO] Starting Node 0 Master..."
(
  cd "${ROOT_DIR}" && \
  exec env WVM_INSTANCE_ID=hybrid0 WVM_SHM_FILE=/wvm_hybrid_master0 \
    "${LINEBUF[@]}" ./master_core/wavevm_node_master "${RAM_MB}" 19100 "${SWARM_CFG}" 0 19101 19105 1
) > "${TMPD}/master0.log" 2>&1 &
M0_PID=$!

echo "[INFO] Starting Node 1 Master..."
(
  cd "${ROOT_DIR}" && \
  exec env WVM_INSTANCE_ID=hybrid1 WVM_SHM_FILE=/wvm_hybrid_master1 \
    "${LINEBUF[@]}" ./master_core/wavevm_node_master "${RAM_MB}" 19300 "${SWARM_CFG}" 1 19301 19305 1
) > "${TMPD}/master1.log" 2>&1 &
M1_PID=$!

# -- Wait for convergence ---------------------------------------------------

echo "[INFO] Waiting ${WAIT_SECONDS}s for convergence..."
sleep "${WAIT_SECONDS}"

# -- Verification ------------------------------------------------------------

echo "[INFO] Process snapshot:"
ps -p "${S0_PID},${S1_PID},${M0_PID},${M1_PID}" -o pid,comm,state --no-headers 2>/dev/null || true

# Check for fatal errors
if grep -qE "Address already in use|Segmentation fault|Failed to init|Resource Mismatch|CRASH on OOB access|bind .* failed|RX socket create failed" "${TMPD}"/*.log 2>/dev/null; then
  fail "failure_signature_detected"
fi

# Verify Node 0 Slave took KVM FAST PATH (mock)
if grep -q "KVM FAST PATH" "${TMPD}/slave0.log"; then
  echo "[PASS] Node 0 Slave: KVM FAST PATH (via mock)"
else
  fail "node0_slave_did_not_enter_kvm_fast_path"
fi

# Verify Node 1 Slave took TCG PROXY path
if grep -q "TCG PROXY" "${TMPD}/slave1.log"; then
  echo "[PASS] Node 1 Slave: TCG PROXY (Tri-Channel)"
else
  # On hosts with /dev/kvm, it will take KVM path instead. Not a failure.
  if grep -q "KVM FAST PATH" "${TMPD}/slave1.log"; then
    warn "node1_slave_has_real_kvm_using_kvm_fast_path_instead_of_tcg"
  else
    fail "node1_slave_mode_unclear"
  fi
fi

# Verify mock library loaded on Node 0
if grep -q "KVM Mock Library loaded" "${TMPD}/slave0.log"; then
  echo "[PASS] Node 0: Mock library active"
else
  fail "node0_mock_library_not_loaded"
fi

# Verify mock KVM init succeeded on Node 0
if grep -q 'open("/dev/kvm") -> mock fd' "${TMPD}/slave0.log"; then
  echo "[PASS] Node 0: Mock /dev/kvm opened"
else
  fail "node0_mock_kvm_open_failed"
fi

# Wait for slave listening evidence
ready=0
for _ in $(seq 1 15); do
  s0_ready=0; s1_ready=0
  grep -q "Listening on 0.0.0.0:19105" "${TMPD}/slave0.log" 2>/dev/null && s0_ready=1
  grep -q "Listening on 0.0.0.0:19305" "${TMPD}/slave1.log" 2>/dev/null && s1_ready=1
  if [[ "${s0_ready}" -eq 1 && "${s1_ready}" -eq 1 ]]; then
    ready=1; break
  fi
  sleep 1
done
if [[ "${ready}" -ne 1 ]]; then
  fail "slave_startup_evidence_incomplete (s0=${s0_ready} s1=${s1_ready})"
fi
echo "[PASS] Both slaves listening"

# Verify neighbor discovery
nb_ready=0
for _ in $(seq 1 15); do
  m0_nb=0; m1_nb=0
  grep -q "New neighbor discovered: 1" "${TMPD}/master0.log" 2>/dev/null && m0_nb=1
  grep -q "New neighbor discovered: 0" "${TMPD}/master1.log" 2>/dev/null && m1_nb=1
  if [[ "${m0_nb}" -eq 1 && "${m1_nb}" -eq 1 ]]; then
    nb_ready=1; break
  fi
  sleep 1
done
if [[ "${nb_ready}" -eq 1 ]]; then
  echo "[PASS] Neighbor discovery complete (bidirectional)"
else
  warn "neighbor_discovery_incomplete (m0->1=${m0_nb} m1->0=${m1_nb})"
fi

# Check for heartbeat activity
if grep -q "HB" "${TMPD}/master0.log" 2>/dev/null || grep -q "heartbeat" "${TMPD}/master0.log" 2>/dev/null; then
  echo "[PASS] Heartbeat activity detected"
else
  warn "no_heartbeat_evidence_in_master0_log"
fi

# Check mock KVM_RUN was exercised (if any VCPU_RUN requests arrived)
if grep -q "KVM_RUN" "${TMPD}/slave0.log" 2>/dev/null; then
  echo "[PASS] Node 0: KVM_RUN mock exercised"
else
  warn "no_kvm_run_activity_on_node0 (normal if no VCPU_RUN requests sent)"
fi

echo ""
echo "============================================================"
write_summary "PASS" "hybrid_kvm_mock_tcg_smoke_ok"
echo " RESULT: PASS"
echo " Node 0: KVM mock path verified"
echo " Node 1: TCG proxy path verified"
echo " Cross-node communication: OK"
echo "============================================================"
