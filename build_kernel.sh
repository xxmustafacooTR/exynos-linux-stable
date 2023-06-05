#!/bin/bash

. clean.sh all
. buildN9.sh stock
. buildN9.sh stock no
. buildS9.sh stock
. buildS9.sh stock no
. buildS9+.sh stock
. buildS9+.sh stock no
. buildS9.sh default
. buildS9.sh default no no
. buildS9.sh a11
. buildS9.sh a11 no
. buildS9.sh aosp
. buildS9.sh aosp no
. buildN9.sh default
. buildN9.sh default no no
. buildN9.sh a11
. buildN9.sh a11 no
. buildN9.sh aosp
. buildN9.sh aosp no
. buildS9+.sh aosp
. buildS9+.sh aosp no
. buildS9+.sh a11
. buildS9+.sh a11 no
. buildS9+.sh default
. buildS9+.sh default no no
. zip.sh
