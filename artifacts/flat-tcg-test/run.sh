#!/usr/bin/env bash
set -euo pipefail
ROOT=/workspaces/WaveVM_Frontier-X
ART_DIR="$ROOT/artifacts/tmp/flat-tcg-test-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ART_DIR"

mount -o remount,size=8G /dev/shm

CFG="$ART_DIR/flat_2node.conf"
cat > "$CFG" <<'EOCFG'
NODE 0 127.0.0.1 19120 1 1
NODE 1 127.0.0.1 19220 1 1
EOCFG

# L2 gateway configs — each only knows its own local node → local master
CFG_GW0="$ART_DIR/gw0.conf"
cat > "$CFG_GW0" <<'EOCFG'
ROUTE 0 1 127.0.0.1 19100
EOCFG

CFG_GW1="$ART_DIR/gw1.conf"
cat > "$CFG_GW1" <<'EOCFG'
ROUTE 1 1 127.0.0.1 19200
EOCFG

# L1 gateway config — full routing table pointing to L2 gateways
CFG_L1="$ART_DIR/l1.conf"
cat > "$CFG_L1" <<'EOCFG'
ROUTE 0 1 127.0.0.1 19120
ROUTE 1 1 127.0.0.1 19220
EOCFG

pkill -f "wavevm_node_master" 2>/dev/null || true
pkill -f "wavevm_node_slave" 2>/dev/null || true
pkill -f "wavevm_gateway" 2>/dev/null || true
pkill -f "qemu-system-x86_64" 2>/dev/null || true
rm -f /tmp/wvm_user_0.sock /tmp/wvm_user_1.sock 2>/dev/null || true
sleep 1

mv /dev/kvm /dev/kvm.off 2>/dev/null || true
trap 'mv /dev/kvm.off /dev/kvm 2>/dev/null || true; pkill -f wavevm_node_master 2>/dev/null || true; pkill -f wavevm_node_slave 2>/dev/null || true; pkill -f wavevm_gateway 2>/dev/null || true; pkill -f qemu-system-x86_64 2>/dev/null || true' EXIT

QPATH="$ROOT/wavevm-qemu/build-native:$PATH"

echo "=== Starting L1 gateway ==="
(env PATH="$QPATH" stdbuf -oL -eL "$ROOT/gateway_service/wavevm_gateway" 19320 127.0.0.1 19399 "$CFG_L1" 19321) >"$ART_DIR/l1.log" 2>&1 &
L1=$!

echo "=== Starting L2 gateways ==="
(env PATH="$QPATH" stdbuf -oL -eL "$ROOT/gateway_service/wavevm_gateway" 19120 127.0.0.1 19320 "$CFG_GW0" 19101) >"$ART_DIR/gw0.log" 2>&1 &
G0=$!
(env PATH="$QPATH" stdbuf -oL -eL "$ROOT/gateway_service/wavevm_gateway" 19220 127.0.0.1 19320 "$CFG_GW1" 19201) >"$ART_DIR/gw1.log" 2>&1 &
G1=$!

echo "=== Starting slaves ==="
(env PATH="$QPATH" WVM_SHM_FILE=/wvm_flat_node0 stdbuf -oL -eL "$ROOT/slave_daemon/wavevm_node_slave" 19105 2 2048 0 19101) >"$ART_DIR/slave0.log" 2>&1 &
S0=$!
(env PATH="$QPATH" WVM_SHM_FILE=/wvm_flat_node1 stdbuf -oL -eL "$ROOT/slave_daemon/wavevm_node_slave" 19205 1 1024 1 19201) >"$ART_DIR/slave1.log" 2>&1 &
S1=$!

echo "=== Starting masters ==="
(env PATH="$QPATH" WVM_INSTANCE_ID=0 WVM_SHM_FILE=/wvm_flat_node0 stdbuf -oL -eL "$ROOT/master_core/wavevm_node_master" 2048 19100 "$CFG" 0 19101 19105 1) >"$ART_DIR/master0.log" 2>&1 &
M0=$!
(env PATH="$QPATH" WVM_INSTANCE_ID=1 WVM_SHM_FILE=/wvm_flat_node1 stdbuf -oL -eL "$ROOT/master_core/wavevm_node_master" 1024 19200 "$CFG" 1 19201 19205 1) >"$ART_DIR/master1.log" 2>&1 &
M1=$!

echo "=== Waiting 8s for convergence ==="
sleep 8

echo "=== Starting QEMU (TCG mode, no KVM) ==="
(env PATH="$QPATH" WVM_INSTANCE_ID=0 stdbuf -oL -eL "$ROOT/wavevm-qemu/build-native/qemu-system-x86_64" \
  -accel wavevm -machine q35 -m 3072 -smp 3 \
  -object memory-backend-ram,id=ram0,size=2048M \
  -object memory-backend-ram,id=ram1,size=1024M \
  -numa node,memdev=ram0,cpus=0-1,nodeid=0 \
  -numa node,memdev=ram1,cpus=2,nodeid=1 \
  -drive file="$ROOT/artifacts/images/cirros-0.6.2-x86_64-disk.img",if=virtio,format=qcow2 \
  -netdev user,id=ne,hostfwd=tcp::2226-:22 -device e1000,netdev=ne \
  -display none -vga none \
  -serial file:"$ART_DIR/vm-serial.log" -monitor none) >"$ART_DIR/vm.log" 2>&1 &
Q=$!

echo "=== Waiting 300s (5 min) for QEMU TCG boot ==="
for i in 1 2 3 4 5; do
  sleep 60
  echo "  ${i}m elapsed — Q alive: $(kill -0 $Q 2>/dev/null && echo yes || echo NO)"
done

echo ""
echo "=== Checking processes ==="
for p in L1 G0 G1 S0 S1 M0 M1 Q; do
  pid=${!p}
  if kill -0 $pid 2>/dev/null; then
    echo "  $p ($pid): alive"
  else
    echo "  $p ($pid): DEAD"
  fi
done

echo ""
echo "=== Checking port 2226 ==="
ss -tln | grep 2226 || echo "  Port 2226 not listening"

echo ""
echo "=== vCPU distribution ==="
echo "master0:" && grep -oP "vcpu=\d+" "$ART_DIR/master0.log" | sort | uniq -c || true
echo "vm.log:" && grep -oP "vcpu=\d+" "$ART_DIR/vm.log" | sort | uniq -c || true

echo ""
echo "=== SSH banner check ==="
timeout 5 bash -c 'echo "" | nc 127.0.0.1 2226' 2>/dev/null || echo "  (no banner / timeout)"

echo ""
echo "=== Checking for crashes ==="
if grep -rEi "Segmentation fault|Bus error" "$ART_DIR"/*.log 2>/dev/null; then
  echo "CRASHES DETECTED"
else
  echo "No crashes found (sigsegv from DSM excluded)"
fi

echo ""
echo "=== Log tails ==="
for f in l1 gw0 gw1 slave0 slave1 master0 master1 vm; do
  echo "--- $f.log (last 15 lines) ---"
  tail -15 "$ART_DIR/$f.log" 2>/dev/null || echo "  (empty)"
done

echo ""
echo "=== VM serial log (last 50 lines) ==="
tail -50 "$ART_DIR/vm-serial.log" 2>/dev/null || echo "  (empty)"

echo ""
echo "=== Log sizes ==="
wc -l "$ART_DIR"/*.log 2>/dev/null

echo ""
echo "=== ART_DIR: $ART_DIR ==="
