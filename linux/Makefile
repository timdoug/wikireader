ROOT := $(shell cd .. && pwd)
VM ?= wr-linux
LIMACTL ?= limactl
SHELL_IN_VM = $(LIMACTL) shell --workdir $(ROOT) $(VM)

.PHONY: vm provision toolchain fetch libc rootfs build boot-test app-test shell

vm:
	./vm.sh

provision: vm
	$(SHELL_IN_VM) ./linux/provision.sh

toolchain: vm
	$(SHELL_IN_VM) ./linux/toolchain.sh

fetch: vm
	$(SHELL_IN_VM) ./linux/fetch.sh

libc: vm
	$(SHELL_IN_VM) ./linux/uclibc/build.sh
	$(SHELL_IN_VM) ./linux/toolchain.sh libstdc++

rootfs: libc
	$(SHELL_IN_VM) ./linux/buildroot/build.sh

build: rootfs
	$(SHELL_IN_VM) ./linux/build.sh

boot-test:
	./boot-test.sh

app-test: build
	./app-test.py

shell: vm
	$(LIMACTL) shell --workdir $(ROOT) $(VM)
