#!/bin/bash

. clean.sh all
. buildN9.sh stock
. buildS9.sh stock
. buildS9+.sh stock
. buildS9.sh
. buildS9.sh a11
. buildS9.sh aosp
. buildN9.sh
. buildN9.sh a11
. buildN9.sh aosp
. buildS9+.sh aosp
. buildS9+.sh a11
. buildS9+.sh
. zip.sh
