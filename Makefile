obj-m	:= hid-shanwan.o
PWD	:= $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

TOOLCHAIN := 
ARCH := x86
KDIR := /lib/modules/$(shell uname -r)
TCPATH :=

PATH := $(TCPATH):$(PATH)

all:
	ARCH=$(ARCH) CROSS_COMPILE=$(TOOLCHAIN) $(MAKE) -C $(KDIR)/build M=$(PWD) modules
clean:
	ARCH=$(ARCH) CROSS_COMPILE=$(TOOLCHAIN) $(MAKE) -C $(KDIR)/build M=$(PWD) clean
