ROOT := $(shell cd .. && pwd)
VM ?= wr-linux
LIMACTL ?= limactl
SHELL_IN_VM = $(LIMACTL) shell --workdir $(ROOT) $(VM)

.PHONY: vm provision toolchain fetch libc busybox console build boot-test app-test shell

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

busybox: libc
	$(SHELL_IN_VM) ./linux/busybox/build.sh

console: libc
	$(SHELL_IN_VM) ./linux/console/build.sh

build: busybox console
	$(SHELL_IN_VM) ./linux/build.sh

boot-test:
	./boot-test.sh

app-test: build
	./app-test.py

shell: vm
	$(LIMACTL) shell --workdir $(ROOT) $(VM)
