#!/bin/bash

export CROSS_COMPILE=/home/$USER/Android/Toolchains/aarch64-linux-gnu/bin/aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=/home/$USER/Android/Toolchains/arm-eabi/bin/arm-eabi-
export CLANG_TRIPLE=/home/$USER/Android/Toolchains/aarch64-elf/bin/aarch64-elf-
export CC=/home/$USER/Android/Toolchains/clang/bin/clang
export CLANG_TRIPLE=/home/$USER/Android/Toolchains/clang/bin/aarch64-linux-gnu-

export ARCH=arm64 && export SUBARCH=arm64
CUR_DIR=$PWD
printf "Cleaning\n"
cd $CUR_DIR
rm -rf vmlinux.* drivers/gator_5.27/gator_src_md5.h scripts/dtbtool_exynos/dtbtool arch/arm64/boot/dtb.img arch/arm64/boot/dts/exynos/*dtb* arch/arm64/configs/exynos9810_temp_defconfig
make -j$(nproc) clean
make -j$(nproc) mrproper
