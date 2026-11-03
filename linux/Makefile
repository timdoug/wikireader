ROOT := $(shell cd .. && pwd)
VM ?= wr-linux
LIMACTL ?= limactl
SHELL_IN_VM = $(LIMACTL) shell --workdir $(ROOT) $(VM)

.PHONY: vm provision toolchain fetch libc musl-toolchain musl-libc musl-test rootfs build boot-test app-test signal-test core-test x11-test wayland-test libffi-test dt-check binutils-test gcc-test ltp-build ltp-test ltp-all-build ltp-all-test ltp-port-test shell

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
	$(SHELL_IN_VM) ./linux/toolchain.sh final
	$(SHELL_IN_VM) ./linux/uclibc/tests.sh

# musl beside uClibc-ng: the c33-linux-musl compiler, the library and the
# final compiler built against it, and its regression programs, run from
# the card through Grifo.
musl-toolchain: vm
	$(SHELL_IN_VM) env C33_LIBC=musl ./linux/toolchain.sh linux

musl-libc: vm
	$(SHELL_IN_VM) ./linux/musl/build.sh
	$(SHELL_IN_VM) env C33_LIBC=musl ./linux/toolchain.sh final
	$(SHELL_IN_VM) ./linux/musl/tests.sh

musl-test:
	python3 ./musl/run-tests.py

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

# The toolchain's own suites, for c33-linux-uclibc: binutils' in its build
# tree; GCC's execute torture (or the runtest selectors in GCC_TESTS), its
# programs run in Linux in wremu.  Both use the existing toolchain, kernel
# and rootfs.
binutils-test: vm
	$(SHELL_IN_VM) sh -c 'make -C $${WR_LINUX_WORK:-$$HOME/wr-linux}/toolchain/binutils-2.47/build-c33-linux-uclibc check-gas check-binutils check-ld'

gcc-test:
	python3 ./gcc-test/run.py $(GCC_TESTS)

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
