ROOT := $(shell cd .. && pwd)
VM ?= wr-linux
LIMACTL ?= limactl
SHELL_IN_VM = $(LIMACTL) shell --workdir $(ROOT) $(VM)

.PHONY: vm provision toolchain fetch libc rootfs build boot-test app-test signal-test core-test x11-test wayland-test libffi-test dt-check ltp-build ltp-test ltp-all-build ltp-all-test ltp-port-test shell

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

signal-test: build
	python3 ./signal-test.py

core-test: build
	$(SHELL_IN_VM) ./linux/debug/build.sh
	./debug/core-test.py

x11-test:
	$(SHELL_IN_VM) ./linux/x11/build.sh
	python3 ./x11/run.py

wayland-test:
	python3 ./wayland/run.py

dt-check: vm
	$(SHELL_IN_VM) ./linux/dt-check.sh

libffi-test:
	$(SHELL_IN_VM) ./linux/libffi/build.sh
	python3 ./libffi/run.py

# Uses the existing compiler, kernel and rootfs; no full system rebuild.
ltp-build:
	$(SHELL_IN_VM) python3 ./linux/ltp/build.py

ltp-test: ltp-build
	python3 ./ltp/run.py

ltp-all-build:
	$(SHELL_IN_VM) python3 ./linux/ltp/build.py --all --output ./linux/artifacts/ltp-all

ltp-all-test: ltp-all-build
	python3 ./ltp/sweep.py --timeouts ./ltp/timeouts.json --wall-timeout 2400

ltp-port-test:
	$(SHELL_IN_VM) python3 ./linux/ltp/build.py --port-tests --output ./linux/artifacts/ltp-port
	python3 ./ltp/run.py --binaries ./artifacts/ltp-port --output ./artifacts/ltp-port-run

shell: vm
	$(LIMACTL) shell --workdir $(ROOT) $(VM)
