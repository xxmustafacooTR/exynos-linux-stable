#!/bin/bash

. variables.sh

printf "Cleaning\n"
clean_temp
cd $CUR_DIR
if [ ! -z "$1" ]
then 
  if [ "$1" == "all" ]; then
	clean_prebuilt
	make -j$(nproc) clean
	make -j$(nproc) mrproper
  elif [ "$1" == "zip" ]; then
    clean_prebuilt
  fi
else
  make -j$(nproc) clean
  make -j$(nproc) mrproper
fi
