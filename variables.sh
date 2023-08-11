#!/bin/bash

export PATH=/home/$USER/Android/Toolchains/clang/bin:/home/$USER/Android/Toolchains/clang/lib:${PATH}
export CLANG_TRIPLE=/home/$USER/Android/Toolchains/clang/bin/aarch64-linux-gnu-
export CROSS_COMPILE=/home/$USER/Android/Toolchains/clang/bin/aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=/home/$USER/Android/Toolchains/clang/bin/arm-linux-gnueabi-
export CC=/home/$USER/Android/Toolchains/clang/bin/clang
export REAL_CC=/home/$USER/Android/Toolchains/clang/bin/clang
export LD=/home/$USER/Android/Toolchains/clang/bin/ld.lld
export AR=/home/$USER/Android/Toolchains/clang/bin/llvm-ar
export NM=/home/$USER/Android/Toolchains/clang/bin/llvm-nm
export OBJCOPY=/home/$USER/Android/Toolchains/clang/bin/llvm-objcopy
export OBJDUMP=/home/$USER/Android/Toolchains/clang/bin/llvm-objdump
export READELF=/home/$USER/Android/Toolchains/clang/bin/llvm-readelf
export STRIP=/home/$USER/Android/Toolchains/clang/bin/llvm-strip
export LLVM=1 && export LLVM_IAS=1
export KALLSYMS_EXTRA_PASS=1

export ARCH=arm64 && export SUBARCH=arm64
ZIP_DIR="/home/$USER/Android/Kernel/Zip"
KERNEL_NAME="Kernel"
DTB_NAME="Dtb"
CUR_DIR=$PWD

dts_ext4() {
		printf "EXT4 Dts\n"
		sed -i 's/erofs/ext4/g' "$CUR_DIR"/arch/arm64/boot/dts/exynos/exynos9810-starlte_common.dtsi
		sed -i 's/erofs/ext4/g' "$CUR_DIR"/arch/arm64/boot/dts/exynos/exynos9810-star2lte_common.dtsi
		sed -i 's/erofs/ext4/g' "$CUR_DIR"/arch/arm64/boot/dts/exynos/exynos9810-crownlte_common.dtsi
		DTB_NAME="Dtb"
}

dts_erofs() {
		printf "EROFS Dts\n"
		sed -i 's/ext4/erofs/g' "$CUR_DIR"/arch/arm64/boot/dts/exynos/exynos9810-starlte_common.dtsi
		sed -i 's/ext4/erofs/g' "$CUR_DIR"/arch/arm64/boot/dts/exynos/exynos9810-star2lte_common.dtsi
		sed -i 's/ext4/erofs/g' "$CUR_DIR"/arch/arm64/boot/dts/exynos/exynos9810-crownlte_common.dtsi
		DTB_NAME="Dtb-erofs"
}

clean_external() {
	cd $CUR_DIR
	rm -rf .*-fetch-lock drivers/kernelsu/.check net/wireguard/.check
}

clean_temp() {
	cd $CUR_DIR
	rm -rf vmlinux.* drivers/gator_5.27/gator_src_md5.h scripts/dtbtool_exynos/dtbtool arch/arm64/boot/dtb.img arch/arm64/boot/dts/exynos/*dtb* arch/arm64/configs/exynos9810_temp_defconfig
}

clean_prebuilt() {
	cd $CUR_DIR
	rm -rf $ZIP_DIR/Dtb/crownlte/dtb.img $ZIP_DIR/Dtb/star2lte/dtb.img $ZIP_DIR/Dtb/starlte/dtb.img
	rm -rf $ZIP_DIR/Dtb-erofs/crownlte/dtb.img $ZIP_DIR/Dtb-erofs/star2lte/dtb.img $ZIP_DIR/Dtb-erofs/starlte/dtb.img
	rm -rf $ZIP_DIR/Kernel/crownlte/zImage $ZIP_DIR/Kernel/star2lte/zImage $ZIP_DIR/Kernel/starlte/zImage
	rm -rf $ZIP_DIR/Kernel-ksu/crownlte/zImage $ZIP_DIR/Kernel-ksu/star2lte/zImage $ZIP_DIR/Kernel-ksu/starlte/zImage
	rm -rf $ZIP_DIR/Kernel-a11/crownlte/zImage $ZIP_DIR/Kernel-a11/star2lte/zImage $ZIP_DIR/Kernel-a11/starlte/zImage
	rm -rf $ZIP_DIR/Kernel-a11-ksu/crownlte/zImage $ZIP_DIR/Kernel-a11-ksu/star2lte/zImage $ZIP_DIR/Kernel-a11-ksu/starlte/zImage
	rm -rf $ZIP_DIR/Kernel-aosp/crownlte/zImage $ZIP_DIR/Kernel-aosp/star2lte/zImage $ZIP_DIR/Kernel-aosp/starlte/zImage
	rm -rf $ZIP_DIR/Kernel-aosp-ksu/crownlte/zImage $ZIP_DIR/Kernel-aosp-ksu/star2lte/zImage $ZIP_DIR/Kernel-aosp-ksu/starlte/zImage
	rm -rf $ZIP_DIR/Kernel-stock/crownlte/zImage $ZIP_DIR/Kernel-stock/star2lte/zImage $ZIP_DIR/Kernel-stock/starlte/zImage
	rm -rf $ZIP_DIR/Kernel-stock-ksu/crownlte/zImage $ZIP_DIR/Kernel-stock-ksu/star2lte/zImage $ZIP_DIR/Kernel-stock-ksu/starlte/zImage
}

clean() {
	cd $CUR_DIR
	clean_temp
    clean_external
    make -j$(nproc) clean
    make -j$(nproc) mrproper
}

patch_kernelsu() {
		printf "Enabling KernelSU\n"
		sed -i 's/# CONFIG_KSU is not set/CONFIG_KSU=y/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		KERNEL_NAME="$KERNEL_NAME-ksu"
}

patch_wifi() {
		printf "Patching Wifi to Old Driver\n"
		sed -i 's/CONFIG_BCMDHD_101_16=y/# CONFIG_BCMDHD_101_16 is not set/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		sed -i 's/# CONFIG_BCMDHD_100_15 is not set/CONFIG_BCMDHD_100_15=y/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		KERNEL_NAME="Kernel-a11"
}

patch_stock() {
		printf "Patching Cached Defconfig For Stock Rom\n"
		sed -i 's/CONFIG_SECURITY_SELINUX_NEVER_ENFORCE=y/# CONFIG_SECURITY_SELINUX_NEVER_ENFORCE is not set/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		sed -i 's/CONFIG_HALL_NEW_NODE=y/# CONFIG_HALL_NEW_NODE is not set/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		sed -i 's/CONFIG_NETFILTER_XT_MATCH_OWNER=y/# CONFIG_NETFILTER_XT_MATCH_OWNER is not set/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		sed -i 's/CONFIG_NETFILTER_XT_MATCH_L2TP=y/# CONFIG_NETFILTER_XT_MATCH_L2TP is not set/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		sed -i 's/CONFIG_L2TP=y/# CONFIG_L2TP is not set/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		sed -i 's/# CONFIG_NET_SCH_NETEM is not set/CONFIG_NET_SCH_NETEM=y/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		sed -i 's/# CONFIG_NET_CLS_CGROUP is not set/CONFIG_NET_CLS_CGROUP=y/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		sed -i 's/CONFIG_NET_CLS_BPF=y/# CONFIG_NET_CLS_BPF is not set/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		sed -i 's/CONFIG_VSOCKETS=y/# CONFIG_VSOCKETS is not set/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		sed -i 's/# CONFIG_CGROUP_NET_CLASSID is not set/CONFIG_CGROUP_NET_CLASSID=y/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		sed -i 's/CONFIG_CUSTOM_FORCETOUCH=y/# CONFIG_CUSTOM_FORCETOUCH is not set/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		sed -i 's/# CONFIG_NETFILTER_XT_MATCH_ONESHOT is not set/CONFIG_NETFILTER_XT_MATCH_ONESHOT=y/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		patch_wifi
		echo "" >> "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		echo "CONFIG_TCP_CONG_LIA=y" >> "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		echo "CONFIG_TCP_CONG_OLIA=y" >> "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		echo "CONFIG_NETFILTER_XT_MATCH_QTAGUID=y" >> "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		KERNEL_NAME="Kernel-stock"
}

patch_aosp() {
		printf "Patching Cached Defconfig For AOSP Base\n"
		sed -i 's/CONFIG_USB_ANDROID_SAMSUNG_MTP=y/# CONFIG_USB_ANDROID_SAMSUNG_MTP is not set/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		sed -i 's/CONFIG_EXYNOS_DECON_MDNIE_LITE_TUNE_RESTRICTIONS=y/# CONFIG_EXYNOS_DECON_MDNIE_LITE_TUNE_RESTRICTIONS is not set/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		sed -i 's/CONFIG_ZRAM_LRU_WRITEBACK=y/# CONFIG_ZRAM_LRU_WRITEBACK is not set/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		sed -i 's/CONFIG_ZRAM_LRU_WRITEBACK_LIMIT=5120/CONFIG_ZRAM_LRU_WRITEBACK_LIMIT=1024/g' "$CUR_DIR"/arch/arm64/configs/exynos9810_temp_defconfig
		KERNEL_NAME="Kernel-aosp"
}
