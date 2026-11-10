#!/bin/bash
# Add the sgpu device to the QEMU source tree and build a minimal x86 QEMU.
set -eu
E=$HOME/work/qemu-env
S=$E/sysroot
Q=$E/src/qemu-10.2.1
P=$HOME/work/sgpu
export PATH=$S/usr/bin:$PATH
export PYTHONPATH=$E/pydeps
export PKG_CONFIG_PATH=$S/usr/lib/x86_64-linux-gnu/pkgconfig:$S/usr/share/pkgconfig

cp "$P/qemu/sgpu.c" "$Q/hw/misc/sgpu.c"
cp "$P/include/sgpu_hw.h" "$Q/hw/misc/sgpu_hw.h"
grep -q CONFIG_SGPU "$Q/hw/misc/meson.build" ||
	echo "system_ss.add(when: 'CONFIG_SGPU', if_true: files('sgpu.c'))" >> "$Q/hw/misc/meson.build"
grep -q 'config SGPU' "$Q/hw/misc/Kconfig" ||
	printf '\nconfig SGPU\n    bool\n    default y if PCI_DEVICES\n    depends on PCI\n' >> "$Q/hw/misc/Kconfig"

mkdir -p "$E/build"
cd "$E/build"
if [ ! -f build.ninja ]; then
	"$Q/configure" --target-list=x86_64-softmmu --disable-docs --disable-werror \
		--disable-tools --disable-guest-agent --disable-gtk --disable-sdl --disable-vnc \
		--disable-opengl --disable-virglrenderer --disable-curses --disable-brlapi \
		--enable-slirp --enable-tcg --enable-kvm \
		--extra-cflags="-I$S/usr/include -I$S/usr/include/x86_64-linux-gnu" \
		--extra-ldflags="-L$S/usr/lib/x86_64-linux-gnu -Wl,-rpath,$S/usr/lib/x86_64-linux-gnu -Wl,-rpath-link,$S/usr/lib/x86_64-linux-gnu"
fi
ninja -j8 qemu-system-x86_64
./qemu-system-x86_64 --version | head -1
./qemu-system-x86_64 -device help 2>/dev/null | grep -i sgpu
echo QEMU_DONE
