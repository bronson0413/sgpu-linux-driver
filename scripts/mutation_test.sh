#!/bin/bash
# Mutation test of the driver's input validation: each mutant removes one
# check and the suite must notice. A surviving mutant is a gap in the tests or
# a check made redundant by another one.
set -u
. "$(dirname "$0")/common.sh"
LOGDIR=$SGPU/logs
STAMP=$(date +%Y%m%d-%H%M%S)
LOG=$LOGDIR/mutation-$STAMP.log
SERIAL=$LOGDIR/mutation-serial-$STAMP.log
B=$SGPU/build-mutants
mkdir -p "$LOGDIR" "$B"
cp "$SGPU/driver/Makefile" "$B/Makefile"
sed -i 's#-I$(src)/../include#-I$(src)/../include#' "$B/Makefile"
exec > >(tee "$LOG") 2>&1

ensure_vm() {
	if ! $SSH true 2>/dev/null; then
		kill "${QP:-0}" 2>/dev/null
		sleep 2
		boot_vm "$SERIAL.$RANDOM" || exit 1
	fi
}

run_suite() {	# run_suite <ko> <test_ops>; prints the guest test exit code
	$SSH 'cat > /root/m.ko' < "$1"
	$SSH "rmmod sgpu 2>/dev/null; insmod /root/m.ko test_ops=$2 timeout_ms=1000" || { echo 200; return; }
	$SSH 'SGPU_SKIP_STATUS=1 timeout 240 /root/test_sgpu 0 > /root/out.txt 2>&1; echo $?' || echo 201
}

boot_vm "$SERIAL.base" || exit 1
$SSH 'cp /root/sgpu.ko /root/base.ko'
for t in 1 0; do
	rc=$(run_suite "$SGPU/driver/sgpu.ko" $t)
	echo "baseline test_ops=$t exit=$rc"
	[ "$rc" = 0 ] || { echo "RESULT: BASELINE_FAILED"; $SSH 'cat /root/out.txt'; exit 1; }
done

total=0; killed=0
while read -r name ops <&3; do
	total=$((total + 1))
	python3 -I "$SGPU/scripts/mutate.py" apply "$name" "$SGPU/driver/sgpu.c" "$B/sgpu.c" || { echo "$name: PATTERN_ERROR"; continue; }
	if ! make -C "$B" >/dev/null 2>&1; then echo "$name: BUILD_ERROR"; continue; fi
	ensure_vm
	rc=$(run_suite "$B/sgpu.ko" "$ops")
	if [ "$rc" = 0 ]; then
		echo "$name: SURVIVED"
	else
		killed=$((killed + 1))
		why=$($SSH 'grep -m1 -E "^FAIL|FAILED" /root/out.txt' 2>/dev/null | cut -c1-110)
		echo "$name: KILLED exit=$rc $why"
	fi
done 3< <(python3 -I "$SGPU/scripts/mutate.py" list)

echo "mutants_total=$total killed=$killed survived=$((total - killed))"
echo "log: $LOG"
