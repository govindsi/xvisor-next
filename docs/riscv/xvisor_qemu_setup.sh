#!/bin/bash
#
# file:        xvisor_qemu_setup.sh
#
# description: a utility script to build/run for riscv xvisor.
#
# Examples:    Use xvisor_qemu_setup.sh -install/-run
#
#
# License:     GPL3+
#
###############################################################################
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
#
###############################################################################


if [ "$1" = "-install" ];
then
if (( $# eq 5 )); then
XVISOR_DIR=$2
KERNEL_DIR=$3
OPEN_SBI_DIR=$4
BUSYBOX_DIR=$5
else
git clone https://github.com/avpatel/xvisor-next

git clone https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git

git clone https://github.com/riscv/opensbi

git clone https://git.busybox.net/busybox/
XVISOR_DIR=$PWD/xvisor-next
KERNEL_DIR=$PWD/linux
OPEN_SBI_DIR=$PWD/opensbi
BUSYBOX_DIR=$PWD/busybox
fi
fi

echo $XVISOR_DIR
echo $KERNEL_DIR
echo $OPEN_SBI_DIR
echo $BUSYBOX_DIR

if [ "$1" = "-run" ];
then
OPEN_SBI_DIR=$PWD/opensbi
XVISOR_DIR=$PWD/xvisor-next
echo $OPEN_SBI_DIR
qemu-system-riscv64 -cpu rv64,x-h=true -M virt -m 512M -nographic -bios $OPEN_SBI_DIR/build/platform/generic/firmware/fw_jump.bin -kernel $XVISOR_DIR/build/vmm.bin -initrd $XVISOR_DIR/build/disk.img -append 'vmm.bootcmd="vfs mount initrd /;vfs run /boot.xscript;vfs cat /system/banner.txt"'
exit
fi


prepare_busybox () {
cd $XVISOR_DIR
cp tests/common/busybox/busybox-1.31.1_defconfig $BUSYBOX_DIR/.config
cd $BUSYBOX_DIR
make oldconfig
#Build Busybox RootFS under _install
make install

mkdir -p ./_install/etc/init.d
mkdir -p ./_install/dev
mkdir -p ./_install/proc
mkdir -p ./_install/sys
ln -sf /sbin/init ./_install/init
cp -f $XVISOR_DIR/tests/common/busybox/fstab ./_install/etc/fstab
cp -f $XVISOR_DIR/tests/common/busybox/rcS ./_install/etc/init.d/rcS
cp -f $XVISOR_DIR/tests/common/busybox/motd ./_install/etc/motd
cp -f $XVISOR_DIR/tests/common/busybox/logo_linux_clut224.ppm ./_install/etc/logo_linux_clut224.ppm
cp -f $XVISOR_DIR/tests/common/busybox/logo_linux_vga16.ppm ./_install/etc/logo_linux_vga16.ppm

#Create a RootFS image

cd ./_install; find ./ | cpio -o -H newc > ../rootfs.img; cd -
#INITRAMFS compressed cpio image
#cd ./_install; find ./ | cpio -o -H newc | gzip -9 > ../rootfs.img; cd -
#INITRD etx2 image
#genext2fs -b 6500 -N 1024 -U -d ./_install ./rootfs.ext2
}


export CROSS_COMPILE=riscv64-unknown-linux-gnu-

cd $XVISOR_DIR

make ARCH=riscv generic-64b-defconfig

#Build Xvisor & DTBs]
make

#Build Basic Firmware]
make -C tests/riscv/virt64/basic

#GoTo OpenSBI source directory
cd $OPEN_SBI_DIR

#Build OpenSBI
make PLATFORM=generic

#GoTo Linux source directory
cd $KERNEL_DIR

#Configure Linux in build directory
cp arch/riscv/configs/defconfig arch/riscv/configs/tmp-virt64_defconfig

$XVISOR_DIR/tests/common/scripts/update-linux-defconfig.sh -p arch/riscv/configs/tmp-virt64_defconfig -f $XVISOR_DIR/tests/riscv/virt64/linux/linux_extra.config

make O=$KERNEL_DIR ARCH=riscv tmp-virt64_defconfig

#Build Linux in build directory to reflect changes in kernel image
make O=$KERNEL_DIR ARCH=riscv Image dtbs

#Create BusyBox RAMDISK to be used as RootFS for Linux kernel]

prepare_busybox


cd $XVISOR_DIR

#Create disk image for Xvisor
mkdir -p ./build/disk/tmp
mkdir -p ./build/disk/system
cp -f ./docs/banner/roman.txt ./build/disk/system/banner.txt
cp -f ./docs/logo/xvisor_logo_name.ppm ./build/disk/system/logo.ppm
mkdir -p ./build/disk/images/riscv/virt64
dtc -q -I dts -O dtb -o ./build/disk/images/riscv/virt64-guest.dtb ./tests/riscv/virt64/virt64-guest.dts
cp -f ./build/tests/riscv/virt64/basic/firmware.bin ./build/disk/images/riscv/virt64/firmware.bin
cp -f ./tests/riscv/virt64/linux/nor_flash.list ./build/disk/images/riscv/virt64/nor_flash.list
cp -f ./tests/riscv/virt64/linux/cmdlist ./build/disk/images/riscv/virt64/cmdlist
cp -f ./tests/riscv/virt64/xscript/one_guest_virt64.xscript ./build/disk/boot.xscript
cp -f $KERNEL_DIR/arch/riscv/boot/Image ./build/disk/images/riscv/virt64/Image
dtc -q -I dts -O dtb -o ./build/disk/images/riscv/virt64/virt64.dtb ./tests/riscv/virt64/linux/virt64.dts
cp -f $BUSYBOX_DIR/rootfs.img ./build/disk/images/riscv/virt64/rootfs.img

genext2fs -B 1024 -b 32768 -d ./build/disk ./build/disk.img


