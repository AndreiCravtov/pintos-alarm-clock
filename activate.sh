#!/usr/bin/env bash

# --------------------------------------------
# THIS SCRIPT COMPILES AND ADDS PINTOS TO PATH
# --------------------------------------------
# RUN `source ./activate.sh` TO MAKE THIS WORK
# --------------------------------------------

# export PINTOS_HOME path
PINTOS_HOME=$(pwd)
export PINTOS_HOME

# get directories
UTILS_DIR="$PINTOS_HOME/src/utils"

# make, and add to path
cd "$UTILS_DIR" || exit
make
export PATH="$UTILS_DIR:$PATH"
cd "$PINTOS_HOME" || exit