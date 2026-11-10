obj-m += mychar.o

KDIR ?= $(HOME)/work/linux-lockdep
KCC ?= gcc-14

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) CC=$(KCC) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) CC=$(KCC) clean
