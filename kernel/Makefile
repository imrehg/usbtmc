ifneq ($(KERNELRELEASE),)
obj-m	:= usbtmc.o

else
#KDIR	:= /lib/modules/$(shell uname -r)/build
KDIR	:= /lib/modules/2.6.30-ARCH/build
PWD	:= $(shell pwd)

default:
	$(MAKE)	-C $(KDIR)	SUBDIRS=$(PWD) modules
endif
