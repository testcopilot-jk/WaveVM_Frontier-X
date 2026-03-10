#!/usr/bin/env bash
set -euo pipefail

ROOT=/workspaces/WaveVM_Frontier-X
TMPD="/artifacts/single-node-test-$(date +%Y%m%d-%H%M%S)"
mkdir -p "${TMPD}"

# Clean up old processes
pkill -f 'wavevm_node_master 1024 19100' 2>/dev/null || true
pkill -f 'wavevm_node_slave 19105' 2>/dev/null || true
pkill -f 'qemu-system-x86_64.*hostfwd=tcp::2226' 2>/dev/null || true
sleep 1
rm -f /tmp/wvm_user_0.sock /dev/shm/wvm_single_* 2>/dev/null || true

QPATH="${ROOT}/wavevm-qemu/build-native:${PATH}"

# Single-node swarm config
cat > "${TMPD}/swarm.conf" << 'EOF'
NODE 0 127.0.0.1 19100 1 1
EOF

echo "[INFO] Starting Slave..."
(
  env PATH="${QPATH}" WVM_SHM_FILE=/wvm_single_slave0     stdbuf -oL -eL "${ROOT}/slave_daemon/wavevm_node_slave" 19105 1 1024 0 19101
) > "${TMPD}/slave0.log" 2>&1 &
S0=$!

sleep 1

echo "[INFO] Starting Master..."
(
  env PATH="${QPATH}" WVM_INSTANCE_ID=0 WVM_SHM_FILE=/wvm_single_master0     stdbuf -oL -eL "${ROOT}/master_core/wavevm_node_master" 1024 19100 "${TMPD}/swarm.conf" 0 19101 19105 1
) > "${TMPD}/master0.log" 2>&1 &
M0=$!

sleep 3

echo "[INFO] Starting QEMU (smp=2, split=1: vCPU0 local, vCPU1 remote)..."
(
  env PATH="${QPATH}" WVM_INSTANCE_ID=0 WVM_LOCAL_SPLIT=1     stdbuf -oL -eL "${ROOT}/wavevm-qemu/build-native/qemu-system-x86_64"       -accel wavevm -machine q35 -m 1024 -smp 2       -drive file="${ROOT}/artifacts/images/cirros-0.6.2-x86_64-disk.img",if=virtio,format=qcow2,snapshot=on       -netdev user,id=ne,hostfwd=tcp::2226-:22 -device e1000,netdev=ne       -display none -vga none       -serial file:"${TMPD}/vm-serial.log" -monitor none
) > "${TMPD}/vm.log" 2>&1 &
Q=$!

echo "[INFO] QEMU PID=${Q}, waiting 120s for boot..."
echo "[INFO] Logs in: ${TMPD}"

# Monitor serial output every 10s
for i in $(seq 1 12); do
  sleep 10
  echo "[INFO] ${i}0s elapsed..."
  
  # Check serial for kernel boot messages
  if [ -f "${TMPD}/vm-serial.log" ]; then
    lines=$(wc -l < "${TMPD}/vm-serial.log")
    echo "  serial: ${lines} lines"
    tail -3 "${TMPD}/vm-serial.log" 2>/dev/null || true
  fi
  
  # Check for SSH banner
  BANNER=$(timeout 3 bash -c 'exec 3<>/dev/tcp/127.0.0.1/2226; head -c 80 <&3' 2>/dev/null || true)
  if [ -n "${BANNER}" ]; then
    echo "[PASS] SSH BANNER: ${BANNER}"
    break
  fi
  
  # Check QEMU still alive
  if ! kill -0 "${Q}" 2>/dev/null; then
    echo "[FAIL] QEMU exited early"
    break
  fi
done

echo ""
echo "=== vm.log tail ==="
tail -30 "${TMPD}/vm.log" 2>/dev/null || true
echo ""
echo "=== master0.log tail ==="
tail -20 "${TMPD}/master0.log" 2>/dev/null || true
echo ""
echo "=== slave0.log tail ==="
tail -20 "${TMPD}/slave0.log" 2>/dev/null || true
echo ""
echo "=== serial log ==="
cat "${TMPD}/vm-serial.log" 2>/dev/null || echo "(empty)"

# Cleanup
kill "${Q}" "${M0}" "${S0}" 2>/dev/null || true
wait 2>/dev/null || true
echo "[INFO] Done. Artifacts: ${TMPD}"
