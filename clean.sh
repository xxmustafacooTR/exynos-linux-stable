#!/bin/bash

. variables.sh

printf "Cleaning\n"
clean_temp
cd $CUR_DIR
make -j$(nproc) clean
make -j$(nproc) mrproper
