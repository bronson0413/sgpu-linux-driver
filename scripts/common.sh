# Shared settings for the VM scripts. Sourced, not executed.
WORK=$HOME/work
SGPU=$WORK/sgpu
SYSROOT=$WORK/qemu-env/sysroot
QEMU=$WORK/qemu-env/build/qemu-system-x86_64
KERNEL=$WORK/linux-lockdep/arch/x86/boot/bzImage
IMAGE=$WORK/img/rootfs-v2.img
KEY=$WORK/img/id_rsa
PORT=10031
SMP=${SMP:-4}
BAD_PATTERN='KASAN|UBSAN|BUG:|WARNING:|Oops|circular locking|inconsistent lock|possible recursive|suspicious RCU|unbalanced'

SSH="ssh -i $KEY -p $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR root@localhost"

boot_vm() {	# boot_vm <serial-log>
	"$QEMU" -m 2G -smp "$SMP" -accel tcg,thread=multi -cpu max -kernel "$KERNEL" \
		-append "console=ttyS0 root=/dev/sda rw earlyprintk=serial net.ifnames=0 panic=1 ${EXTRA_APPEND:-}" \
		-drive "file=$IMAGE,format=raw" -netdev "user,id=n0,hostfwd=tcp::$PORT-:22" \
		-device e1000,netdev=n0 -device sgpu -nographic -snapshot > "$1" 2>&1 &
	QP=$!
	trap 'kill $QP 2>/dev/null' EXIT
	for _ in $(seq 1 200); do
		if $SSH true 2>/dev/null; then return 0; fi
		kill -0 $QP 2>/dev/null || { echo "QEMU exited early"; tail -5 "$1"; return 1; }
		sleep 3
	done
	echo "VM boot timed out"
	return 1
}
