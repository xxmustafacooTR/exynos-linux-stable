#!/bin/bash

. variables.sh

clean_temp

cp -vr $CUR_DIR/arch/arm64/configs/exynos9810_defconfig $CUR_DIR/arch/arm64/configs/exynos9810_temp_defconfig
echo "" >> $CUR_DIR/arch/arm64/configs/exynos9810_temp_defconfig
cat $CUR_DIR/arch/arm64/configs/exynos9810-starxlte_defconfig >> $CUR_DIR/arch/arm64/configs/exynos9810_temp_defconfig
echo "" >> $CUR_DIR/arch/arm64/configs/exynos9810_temp_defconfig
cat $CUR_DIR/arch/arm64/configs/exynos9810-star2lte_defconfig >> $CUR_DIR/arch/arm64/configs/exynos9810_temp_defconfig
make exynos9810_temp_defconfig -j$(nproc --all)

make -j$(nproc --all)

cp -vr $CUR_DIR/arch/arm64/boot/Image $ZIP_DIR/Kernel/star2lte/zImage
cp -vr $CUR_DIR/arch/arm64/boot/dtb.img $ZIP_DIR/Kernel/star2lte/dtb.img