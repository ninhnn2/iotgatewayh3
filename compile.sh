#!/bin/bash

sudo make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- iot_defconfig
sudo make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j28
sudo make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules_install INSTALL_MOD_PATH=./fanning

