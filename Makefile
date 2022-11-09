KDIR:=/home/jc/3399pro/3399_722/rk3399-linux/kernel
obj-m:=gd25qxx_driver.o
PWD:=$(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) 
	aarch64-linux-gnu-gcc gd25q64_test.c -o gd25q64_test

clean:
	rm -rf *.ko *.order *.symvers *.cmd *.o *.mod.c *.tmp_versions .*.cmd .tmp_versions gd25q64_test
