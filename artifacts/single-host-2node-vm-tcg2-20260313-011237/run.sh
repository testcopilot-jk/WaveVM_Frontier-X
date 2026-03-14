#!/usr/bin/env bash
set -euo pipefail
ROOT=/workspaces/WaveVM_Frontier-X
ART_DIR="$(cd "$(dirname "$0")" && pwd)"
CFG="$ART_DIR/flat_2node.conf"
cat > "$CFG" <<'EOCFG'
NODE 0 127.0.0.1 19100 1 1
NODE 1 127.0.0.1 19200 1 1
EOCFG

pkill -f "wavevm_node_master 1024 19100" 2>/dev/null || true
pkill -f "wavevm_node_master 1024 19200" 2>/dev/null || true
pkill -f "wavevm_node_slave 19105" 2>/dev/null || true
pkill -f "wavevm_node_slave 19205" 2>/dev/null || true
pkill -f "qemu-system-x86_64.*hostfwd=tcp::2226-:22" 2>/dev/null || true
pkill -f "wavevm_gateway 19120" 2>/dev/null || true
pkill -f "wavevm_gateway 19220" 2>/dev/null || true
rm -f /tmp/wvm_user_0.sock /tmp/wvm_user_1.sock 2>/dev/null || true

QPATH="$ROOT/wavevm-qemu/build-native:$PATH"

TCG_WRAP="$ART_DIR/qemu-tcg-wrapper.sh"
cat > "$TCG_WRAP" <<'EOF'
#!/usr/bin/env bash
export WVM_DISABLE_AUTO_KVM=1
exec qemu-system-x86_64 "$@"
EOF
chmod +x "$TCG_WRAP"

(env PATH="$QPATH" stdbuf -oL -eL "$ROOT/gateway_service/wavevm_gateway" 19120 127.0.0.1 19100 "$CFG" 19101) >"$ART_DIR/gw0.log" 2>&1 &
G0=$!
(env PATH="$QPATH" stdbuf -oL -eL "$ROOT/gateway_service/wavevm_gateway" 19220 127.0.0.1 19200 "$CFG" 19201) >"$ART_DIR/gw1.log" 2>&1 &
G1=$!
(env PATH="$QPATH" WVM_FORCE_TCG=1 WVM_TCG_QEMU_BIN="$TCG_WRAP" WVM_SHM_FILE=/wvm_flat_node0 stdbuf -oL -eL "$ROOT/slave_daemon/wavevm_node_slave" 19105 2 2048 0 19101) >"$ART_DIR/slave0.log" 2>&1 &
S0=$!
(env PATH="$QPATH" WVM_FORCE_TCG=1 WVM_TCG_QEMU_BIN="$TCG_WRAP" WVM_SHM_FILE=/wvm_flat_node1 stdbuf -oL -eL "$ROOT/slave_daemon/wavevm_node_slave" 19205 1 1024 1 19201) >"$ART_DIR/slave1.log" 2>&1 &
S1=$!
(env PATH="$QPATH" WVM_INSTANCE_ID=0 WVM_SHM_FILE=/wvm_flat_node0 stdbuf -oL -eL "$ROOT/master_core/wavevm_node_master" 2048 19100 "$CFG" 0 19101 19105 1) >"$ART_DIR/master0.log" 2>&1 &
M0=$!
(env PATH="$QPATH" WVM_INSTANCE_ID=1 WVM_SHM_FILE=/wvm_flat_node1 stdbuf -oL -eL "$ROOT/master_core/wavevm_node_master" 1024 19200 "$CFG" 1 19201 19205 1) >"$ART_DIR/master1.log" 2>&1 &
M1=$!

sleep 8
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

sleep 180
LISTEN=$(ss -ltnp | rg ':2226' || true)
BANNER=$(timeout 6 bash -lc 'exec 3<>/dev/tcp/127.0.0.1/2226; head -c 120 <&3' || true)
TIMEOUTS=$(rg -n "RPC Timeout\] Type: 5" "$ART_DIR/master0.log" | wc -l || true)

echo "ARTIFACT_DIR=$ART_DIR"
echo "PIDS G0=$G0 G1=$G1 S0=$S0 S1=$S1 M0=$M0 M1=$M1 Q=$Q"
echo "PORT_2226_LISTEN=${LISTEN:-none}"
echo "VCPU_TIMEOUT_COUNT=$TIMEOUTS"
if [[ -n "$BANNER" ]]; then
  echo "SSH_BANNER=$BANNER"
  echo "RESULT=PASS_BANNER"
else
  echo "SSH_BANNER="
  echo "RESULT=NO_BANNER"
fi
