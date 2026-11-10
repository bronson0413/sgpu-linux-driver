#!/bin/bash
# Run both drivers on a KCSAN kernel to look for data races. KCSAN cannot be
# combined with KASAN, so this uses its own kernel and module builds.
# Usage: kcsan_test.sh [stress_seconds]
set -u
. "$(dirname "$0")/common.sh"
STRESS=${1:-120}
KC=$WORK/linux-kcsan
KERNEL=$KC/arch/x86/boot/bzImage
EXTRA_APPEND="systemd.mask=sgpu.service systemd.mask=mychar.service"
LOGDIR=$SGPU/logs
STAMP=$(date +%Y%m%d-%H%M%S)
LOG=$LOGDIR/kcsan-$STAMP.log
B=$SGPU/build-kcsan
mkdir -p "$LOGDIR" "$B/sgpu" "$B/mychar"
exec > >(tee "$LOG") 2>&1

cp "$SGPU/driver/sgpu.c" "$SGPU/driver/Makefile" "$B/sgpu/"
sed -i "s#-I\$(src)/../include#-I$SGPU/include#" "$B/sgpu/Makefile"
cp "$WORK"/mychar/{mychar.c,mychar.h,Makefile} "$B/mychar/"
make -C "$B/sgpu" KDIR="$KC" 2>&1 | tail -1
(cd "$B/mychar" && make KDIR="$KC" CC=gcc-14 2>&1 | tail -1)

boot_vm "$LOGDIR/kcsan-serial-$STAMP.log" || { echo "RESULT: VM_BOOT_FAILED"; exit 1; }
$SSH 'cat > /root/sgpu-k.ko' < "$B/sgpu/sgpu.ko"
$SSH 'cat > /root/mychar-k.ko' < "$B/mychar/mychar.ko"
$SSH 'insmod /root/mychar-k.ko && insmod /root/sgpu-k.ko test_ops=1 timeout_ms=1000 && dmesg -C' || { echo "RESULT: MODULE_LOAD_FAILED"; exit 1; }
$SSH "/root/test_mychar $STRESS"; R1=$?
echo "mychar_exit=$R1"
$SSH "/root/test_sgpu $STRESS"; R2=$?
echo "sgpu_exit=$R2"
BDF=$($SSH 'basename $(readlink -f /sys/bus/pci/drivers/sgpu/0000:*)')
$SSH "/root/test_sgpu --unplug $BDF"; R3=$?
echo "unplug_exit=$R3"
$SSH 'dmesg' > "$LOGDIR/kcsan-dmesg-$STAMP.log"

python3 -I "$SGPU/tools/fuzzlog.py" kcsan "$LOGDIR/kcsan-dmesg-$STAMP.log"
echo "log: $LOG"
