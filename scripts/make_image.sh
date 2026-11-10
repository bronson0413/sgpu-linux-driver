#!/bin/bash
# Build the v2 test image from the original one. Needs no root: debugfs edits
# the ext4 image directly. The original image is left untouched.
set -eu
. "$(dirname "$0")/common.sh"
SRC=$WORK/img/rootfs.img
DEBUGFS=/usr/sbin/debugfs
SVC=$(mktemp)
trap 'rm -f "$SVC"' EXIT
cat > "$SVC" <<'EOF'
[Unit]
Description=Load sgpu driver
Before=ssh.service

[Service]
Type=oneshot
ExecStart=/sbin/insmod /root/sgpu.ko test_ops=1 timeout_ms=200
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
cp --sparse=always "$SRC" "$IMAGE"
{
	echo "rm /root/mychar.ko"
	echo "write $WORK/mychar/mychar.ko /root/mychar.ko"
	echo "rm /root/test_mychar"
	echo "write $WORK/mychar/test_mychar /root/test_mychar"
	echo "write $SGPU/driver/sgpu.ko /root/sgpu.ko"
	echo "write $SGPU/tests/test_sgpu /root/test_sgpu"
	echo "write $SGPU/tests/bench_sgpu /root/bench_sgpu"
	echo "sif /root/bench_sgpu mode 0100755"
	echo "write $SVC /etc/systemd/system/sgpu.service"
	echo "symlink /etc/systemd/system/multi-user.target.wants/sgpu.service /etc/systemd/system/sgpu.service"
	for f in /root/mychar.ko /root/test_mychar /root/sgpu.ko /root/test_sgpu /etc/systemd/system/sgpu.service; do
		echo "sif $f uid 0"
		echo "sif $f gid 0"
	done
	echo "sif /root/test_mychar mode 0100755"
	echo "sif /root/test_sgpu mode 0100755"
} > "$SVC.cmds"
"$DEBUGFS" -w -f "$SVC.cmds" "$IMAGE" 2>&1 | grep -iE 'error|not found|failed' || true
rm -f "$SVC.cmds"
"$DEBUGFS" -R "ls -l /root" "$IMAGE" 2>/dev/null | grep -E 'sgpu|mychar'
echo IMAGE_OK
