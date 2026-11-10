#!/bin/bash
# Full driver test run on the KASAN + lockdep + UBSAN kernel.
# Usage: vm_test.sh [stress_seconds]
set -u
. "$(dirname "$0")/common.sh"
STRESS=${1:-60}
LOGDIR=$SGPU/logs
STAMP=$(date +%Y%m%d-%H%M%S)
LOG=$LOGDIR/vmtest-$STAMP.log
SERIAL=$LOGDIR/serial-$STAMP.log
mkdir -p "$LOGDIR"
exec > >(tee "$LOG") 2>&1

boot_vm "$SERIAL" || { echo "RESULT: VM_BOOT_FAILED"; exit 1; }
RESULT=0
step() {	# step <name> <guest command>
	echo "### $1"
	$SSH "$2"
	local rc=$?
	echo "exit=$rc"
	[ $rc = 0 ] || RESULT=1
}

$SSH 'uname -r; nproc; grep -c . /proc/lockdep_stats; ls -l /dev/sgpu /dev/mychar; lsmod | grep -E "sgpu|mychar"'
# A report during boot switches lockdep off for the rest of the run, and the
# dmesg -C below would hide it. Such a run proves nothing, so stop here.
BOOT_HITS=$($SSH "dmesg | grep -cE '$BAD_PATTERN'")
echo "boot_hits=$BOOT_HITS"
if [ "$BOOT_HITS" != 0 ]; then
	$SSH "dmesg | grep -E '$BAD_PATTERN' | head -20"
	echo "RESULT: INVALID (report during boot, before the drivers were tested)"
	echo "log: $LOG"
	exit 3
fi
$SSH 'dmesg -C'

step "mychar functional + stress" "/root/test_mychar $STRESS"
step "mychar repeat run" "/root/test_mychar 0"
step "mychar reload x5" 'for i in 1 2 3 4 5; do rmmod mychar && insmod /root/mychar.ko || exit 1; done'

step "sgpu reload with test commands" 'rmmod sgpu && insmod /root/sgpu.ko test_ops=1 timeout_ms=1000'
step "sgpu suite with test commands + stress" "/root/test_sgpu $STRESS"
step "sgpu repeat run" "/root/test_sgpu 0"

step "sgpu reload without test commands" 'rmmod sgpu && insmod /root/sgpu.ko'
step "sgpu suite without test commands" "/root/test_sgpu 0"

BDF=$($SSH 'basename $(readlink -f /sys/bus/pci/drivers/sgpu/0000:*)')
echo "device: $BDF"
step "sgpu hot-unplug with open context" "/root/test_sgpu --unplug $BDF"
step "sgpu rescan after unplug" 'echo 1 > /sys/bus/pci/rescan; sleep 3; ls -l /dev/sgpu'
step "sgpu suite after rescan" "/root/test_sgpu 0"
step "sgpu unload" 'rmmod sgpu'

echo "### dmesg scan"
HITS=$($SSH "dmesg | grep -cE '$BAD_PATTERN'")
$SSH "dmesg | grep -E '$BAD_PATTERN' | head -40"
$SSH 'grep -E "lock-classes|dependency chains|debug_locks" /proc/lockdep_stats; cat /proc/sys/kernel/tainted'
$SSH "dmesg" > "$LOGDIR/dmesg-$STAMP.log"
DEBUG_LOCKS=$($SSH "awk '/debug_locks/{print \$2}' /proc/lockdep_stats")
TAINT=$($SSH 'cat /proc/sys/kernel/tainted')
echo "dmesg_hits=$HITS debug_locks=$DEBUG_LOCKS taint=$TAINT"
# 4096 is TAINT_OOT_MODULE, expected for these drivers. Anything else, or lockdep
# having switched itself off, means the clean scan above cannot be trusted.
if [ "$RESULT" != 0 ] || [ "$HITS" != 0 ]; then
	echo "RESULT: FAIL"
elif [ "$DEBUG_LOCKS" != 1 ] || [ "$TAINT" != 4096 ]; then
	echo "RESULT: INVALID (lockdep off or unexpected taint)"
else
	echo "RESULT: PASS"
fi
echo "log: $LOG"
