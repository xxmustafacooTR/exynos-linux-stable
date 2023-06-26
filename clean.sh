#!/bin/bash

. variables.sh

printf "Cleaning\n"
cd $CUR_DIR
if [ ! -z "$1" ]
then 
  if [ "$1" == "all" ]; then
	clean_prebuilt
	clean
	git reset --hard
  elif [ "$1" == "lite" ]; then
    clean_temp
    clean_external
  elif [ "$1" == "zip" ]; then
    clean_prebuilt
  fi
else
  clean
fi
