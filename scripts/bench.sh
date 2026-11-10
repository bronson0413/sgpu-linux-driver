#!/bin/bash
# Latency and batching benchmark. Runs on the debug kernel under full emulation,
# so results are only meaningful relative to each other.
set -u
. "$(dirname "$0")/common.sh"
LOGDIR=$SGPU/logs
STAMP=$(date +%Y%m%d-%H%M%S)
LOG=$LOGDIR/bench-$STAMP.log
exec > >(tee "$LOG") 2>&1
boot_vm "$LOGDIR/bench-serial-$STAMP.log" || exit 1
$SSH 'rmmod sgpu; insmod /root/sgpu.ko; /root/bench_sgpu'
echo "log: $LOG"
