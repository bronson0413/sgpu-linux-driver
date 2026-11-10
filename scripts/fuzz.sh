#!/bin/bash
# Coverage-guided fuzzing of sgpu and mychar with syzkaller.
# Usage: fuzz.sh [seconds]    (default 3600)
#
# The VM runs under TCG, without KVM. Two settings exist for that reason:
#  - The manager binary is built without the startup glob expansion. Walking
#    /sys/**/* took longer than the 15 minute no-output timeout, and none of
#    the enabled syscalls use a glob. The syzkaller tree is restored afterwards.
#  - The same build skips network device and other injection setup. Creating
#    them stalled every executor in the kernel's usermode helper for over ten
#    minutes, and the drivers under test do not use the network.
#  - focus_areas keeps only the two drivers' code in the coverage signal.
set -u
. "$(dirname "$0")/common.sh"
SECONDS_TO_RUN=${1:-3600}
REPORT_MARGIN=480
SYZ=$WORK/syzkaller
HTTP_PORT=56742
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=$SGPU/logs/fuzz-$STAMP
WD=$WORK/workdir-sgpu
BASE=http://127.0.0.1:$HTTP_PORT
mkdir -p "$OUT" "$WD"

cp "$SGPU/syz/sgpu.txt" "$SGPU/syz/sgpu.txt.const" "$SYZ/sys/linux/"
cd "$SYZ" || exit 1
sed -i 's/range ctx.target.RequiredGlobs()/range []string{}/' pkg/vminfo/syscalls.go
SKIP='flatrpc.FeatureNetDevices|flatrpc.FeatureNetInjection|flatrpc.FeatureDevlinkPCI|flatrpc.FeatureNicVF|flatrpc.FeatureWifiEmulation|flatrpc.FeatureLRWPANEmulation|flatrpc.FeatureVhciInjection|flatrpc.FeatureUSBEmulation|flatrpc.FeatureBinFmtMisc'
sed -i "s/features := flatrpc.AllFeatures\$/features := flatrpc.AllFeatures \&^ ($SKIP)/" pkg/rpcserver/rpcserver.go
restore() { git -C "$SYZ" checkout -- pkg/vminfo/syscalls.go pkg/rpcserver/rpcserver.go; }
git diff --stat
fail() { restore; echo "$1"; tail -20 "$OUT/build.log"; exit 1; }
make descriptions > "$OUT/build.log" 2>&1 || fail "descriptions failed"
make manager execprog executor db prog2c >> "$OUT/build.log" 2>&1 || fail "build failed"
restore

# Seed programs reach the resource chain (open, create, submit, wait) that
# random generation rarely assembles at TCG execution rates.
SEEDS=$OUT/seeds
mkdir -p "$SEEDS"
for f in "$SGPU"/syz/seeds/*.prog; do
	"$SYZ/bin/syz-prog2c" -prog "$f" > /dev/null 2> "$OUT/seed-check.err" ||
		{ echo "invalid seed $f"; cat "$OUT/seed-check.err"; exit 1; }
	cp "$f" "$SEEDS/$(basename "$f" .prog)"
done
if [ ! -e "$WD/corpus.db" ]; then
	"$SYZ/bin/syz-db" pack "$SEEDS" "$WD/corpus.db" >> "$OUT/build.log" 2>&1 || { echo "seed pack failed"; exit 1; }
fi
echo "seeds=$(ls "$SEEDS" | wc -l)"

cat > "$OUT/syz.cfg" <<EOF
{
	"target": "linux/amd64",
	"http": "127.0.0.1:$HTTP_PORT",
	"workdir": "$WD",
	"kernel_obj": "$WORK/linux-lockdep",
	"module_obj": ["$SGPU/driver", "$WORK/mychar"],
	"image": "$IMAGE",
	"sshkey": "$KEY",
	"syzkaller": "$SYZ",
	"procs": 4,
	"type": "qemu",
	"experimental": {"focus_areas": [{"filter": {"functions": ["^sgpu_", "^mychar_"]}, "weight": 1.0}]},
	"enable_syscalls": [
		"openat\$sgpu", "ioctl\$SGPU_INFO", "ioctl\$SGPU_BO_CREATE", "ioctl\$SGPU_BO_DESTROY",
		"ioctl\$SGPU_BO_WRITE", "ioctl\$SGPU_BO_READ", "ioctl\$SGPU_SUBMIT", "ioctl\$SGPU_WAIT",
		"openat\$mychar", "ioctl\$MYCHAR_RESIZE", "ioctl\$MYCHAR_WRITE_AT", "ioctl\$MYCHAR_READ_AT",
		"ioctl\$MYCHAR_WRITE_AT_RAW", "ioctl\$MYCHAR_READ_AT_RAW", "ioctl\$MYCHAR_RESET", "ioctl\$MYCHAR_STAT",
		"read", "write", "lseek", "close", "dup", "pread64", "pwrite64"
	],
	"vm": {
		"count": 4,
		"kernel": "$KERNEL",
		"cpu": 2,
		"mem": 2048,
		"qemu": "$QEMU",
		"qemu_args": "-accel tcg,thread=multi -cpu max -device sgpu"
	}
}
EOF

timeout -s INT "$SECONDS_TO_RUN" "$SYZ/bin/syz-manager" -config "$OUT/syz.cfg" > "$OUT/syz-manager.log" 2>&1 &
MGR=$!
# Reports are served by the running manager, so collect them before the end.
for _ in $(seq 1 $(((SECONDS_TO_RUN - REPORT_MARGIN) / 5))); do
	kill -0 $MGR 2>/dev/null || { echo "manager exited early"; tail -5 "$OUT/syz-manager.log"; exit 1; }
	sleep 5
done
curl -s --max-time 20 "$BASE/" -o "$OUT/summary.html"
curl -s --max-time 150 "$BASE/funccover" -o "$OUT/funccover.csv"
curl -s --max-time 150 "$BASE/filecover" -o "$OUT/filecover.csv"
curl -s --max-time 150 "$BASE/cover" -o "$OUT/cover.html"
wait $MGR
python3 -I "$SGPU/tools/fuzzlog.py" fuzz "$OUT/syz-manager.log"
echo "crash_dirs=$(ls "$WD/crashes" 2>/dev/null | wc -l)"
for d in "$WD"/crashes/*/; do
	[ -d "$d" ] || continue
	echo "crash: $(head -1 "$d/description")"
	mkdir -p "$OUT/crashes"
	cp -r "$d" "$OUT/crashes/"
done
ls -la "$OUT" | tail -12
echo "out: $OUT"
