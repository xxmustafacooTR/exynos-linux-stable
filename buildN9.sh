#!/bin/bash

export CROSS_COMPILE=/home/$USER/Android/Toolchains/aarch64-linux-gnu/bin/aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=/home/$USER/Android/Toolchains/arm-eabi/bin/arm-eabi-
export CLANG_TRIPLE=/home/$USER/Android/Toolchains/aarch64-elf/bin/aarch64-elf-
export CC=/home/$USER/Android/Toolchains/clang/bin/clang
export CLANG_TRIPLE=/home/$USER/Android/Toolchains/clang/bin/aarch64-linux-gnu-

export ARCH=arm64 && export SUBARCH=arm64
ZIP_DIR="/home/$USER/Android/Kernel/Zip"
CUR_DIR=$PWD

rm -rf vmlinux.* drivers/gator_5.27/gator_src_md5.h scripts/dtbtool_exynos/dtbtool arch/arm64/boot/dtb.img arch/arm64/boot/dts/exynos/*dtb* arch/arm64/configs/exynos9810_temp_defconfig

cp -vr $CUR_DIR/arch/arm64/configs/exynos9810_defconfig $CUR_DIR/arch/arm64/configs/exynos9810_temp_defconfig
echo "" >> $CUR_DIR/arch/arm64/configs/exynos9810_temp_defconfig
cat $CUR_DIR/arch/arm64/configs/exynos9810-crownlte_defconfig >> $CUR_DIR/arch/arm64/configs/exynos9810_temp_defconfig
make exynos9810_temp_defconfig -j$(nproc --all)

make -j$(nproc --all)

cp -vr $CUR_DIR/arch/arm64/boot/Image $ZIP_DIR/Kernel/crownlte/zImage
cp -vr $CUR_DIR/arch/arm64/boot/dtb.img $ZIP_DIR/Kernel/crownlte/dtb.img