#!/usr/bin/env bash
export WVM_DISABLE_AUTO_KVM=1
exec qemu-system-x86_64 "$@"
