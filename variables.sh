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
CUR_DIR=$PWD

clean_temp() {
	cd $CUR_DIR
	rm -rf vmlinux.* drivers/gator_5.27/gator_src_md5.h scripts/dtbtool_exynos/dtbtool arch/arm64/boot/dtb.img arch/arm64/boot/dts/exynos/*dtb* arch/arm64/configs/exynos9810_temp_defconfig
}

patch_stock() {
		printf "Patching Cached Defconfig For Stock Rom\n"
		sed -i 's/CONFIG_SECURITY_SELINUX_NEVER_ENFORCE=y/# CONFIG_SECURITY_SELINUX_NEVER_ENFORCE is not set/g' "$CUR_DIR"/.config
		sed -i 's/CONFIG_HALL_NEW_NODE=y/# CONFIG_HALL_NEW_NODE is not set/g' "$CUR_DIR"/.config
		sed -i 's/CONFIG_NETFILTER_XT_MATCH_OWNER=y/# CONFIG_NETFILTER_XT_MATCH_OWNER is not set/g' "$CUR_DIR"/.config
		sed -i 's/CONFIG_NETFILTER_XT_MATCH_L2TP=y/# CONFIG_NETFILTER_XT_MATCH_L2TP is not set/g' "$CUR_DIR"/.config
		sed -i 's/CONFIG_L2TP=y/# CONFIG_L2TP is not set/g' "$CUR_DIR"/.config
		sed -i 's/# CONFIG_NET_SCH_NETEM is not set/CONFIG_NET_SCH_NETEM=y/g' "$CUR_DIR"/.config
		sed -i 's/# CONFIG_NET_CLS_CGROUP is not set/CONFIG_NET_CLS_CGROUP=y/g' "$CUR_DIR"/.config
		sed -i 's/CONFIG_NET_CLS_BPF=y/# CONFIG_NET_CLS_BPF is not set/g' "$CUR_DIR"/.config
		sed -i 's/CONFIG_VSOCKETS=y/# CONFIG_VSOCKETS is not set/g' "$CUR_DIR"/.config
		sed -i 's/# CONFIG_CGROUP_NET_CLASSID is not set/CONFIG_CGROUP_NET_CLASSID=y/g' "$CUR_DIR"/.config
		sed -i 's/CONFIG_CUSTOM_FORCETOUCH=y/# CONFIG_CUSTOM_FORCETOUCH is not set/g' "$CUR_DIR"/.config
		sed -i 's/CONFIG_BCMDHD_101_16=y/# CONFIG_BCMDHD_101_16 is not set/g' "$CUR_DIR"/.config
		sed -i 's/# CONFIG_BCMDHD_100_15 is not set/CONFIG_BCMDHD_100_15=y/g' "$CUR_DIR"/.config
		echo "" >> "$CUR_DIR"/.config
		echo "CONFIG_TCP_CONG_LIA=y" >> "$CUR_DIR"/.config
		echo "CONFIG_TCP_CONG_OLIA=y" >> "$CUR_DIR"/.config
		echo "CONFIG_NETFILTER_XT_MATCH_QTAGUID=y" >> "$CUR_DIR"/.config
		echo "CONFIG_NETFILTER_XT_MATCH_ONESHOT=y" >> "$CUR_DIR"/.config
}
