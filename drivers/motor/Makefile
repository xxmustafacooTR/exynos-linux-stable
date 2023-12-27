#
# Makefile for the vibrator driver ICs with vibrator driver
#
subdir-ccflags-y := -Wformat

obj-$(CONFIG_MOTOR_DRV_MAX77865)	+= max77865_haptic.o
obj-$(CONFIG_MOTOR_DRV_MAX77705)	+= max77705_haptic.o
obj-$(CONFIG_SEC_HAPTIC)	+= sec_haptic.o
GCOV_PROFILE_sec_haptic.o		:=y
