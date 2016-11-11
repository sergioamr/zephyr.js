BOARD ?= arduino_101
KERNEL ?= micro
UPDATE ?= exit

# pass TRACE=y to trace malloc/free in the ZJS API
TRACE ?= n

ifndef ZJS_BASE
$(error ZJS_BASE not defined. You need to source zjs-env.sh)
endif

OCF_ROOT ?= deps/iotivity-constrained
JERRY_BASE ?= $(ZJS_BASE)/deps/jerryscript
JS ?= samples/HelloWorld.js
VARIANT ?= release
# Dump memory information: on = print allocs, full = print allocs + dump pools
TRACE ?= off
# Specify pool malloc or heap malloc
MALLOC ?= pool
# Print callback statistics during runtime
CB_STATS ?= off
# Make target (linux or zephyr)
# MAKECMDGOALS is a Make variable that is set to the target your building for.
TARGET = $(MAKECMDGOALS)

# Build for zephyr, default target
.PHONY: zephyr
zephyr: $(PRE_ACTION) analyze generate
	@make -f Makefile.zephyr BOARD=$(BOARD) KERNEL=$(KERNEL) VARIANT=$(VARIANT) MEM_STATS=$(MEM_STATS) CB_STATS=$(CB_STATS)

.PHONY: analyze
analyze:
	@echo "% This is a generated file" > prj.mdef
ifeq ($(DEV), ashell)
	@cat prj.mdef.dev >> prj.mdef
else
	@cat prj.mdef.base >> prj.mdef
endif
	@echo "# This is a generated file" > src/Makefile
	@cat src/Makefile.base >> src/Makefile
	@if [ "$(TRACE)" = "on" ] || [ "$(TRACE)" = "full" ]; then \
		echo "ccflags-y += -DZJS_TRACE_MALLOC" >> src/Makefile; \
	fi
	@if [ $(MALLOC) = "pool" ]; then \
		echo "obj-y += zjs_pool.o" >> src/Makefile; \
		echo "ccflags-y += -DZJS_POOL_CONFIG" >> src/Makefile; \
		if [ "$(TRACE)" = "full" ]; then \
			echo "ccflags-y += -DDUMP_MEM_STATS" >> src/Makefile; \
		fi; \
		cat prj.mdef.pool >> prj.mdef; \
	else \
		if [ "$(DEV)" != "ashell" ]; then \
			cat prj.mdef.heap >> prj.mdef; \
		fi; \
	fi
	@echo "ccflags-y += $(shell ./scripts/analyze.sh $(BOARD) $(JS))" >> src/Makefile
	@# Add the include for the OCF Makefile only if the script is using OCF
	@if grep BUILD_MODULE_OCF src/Makefile; then \
		echo "include \$$(ZJS_BASE)/Makefile.ocf_zephyr" >> src/Makefile; \
	fi

.PHONY: all
all: zephyr arc

# This is how we can check if we are building for linux and if clean is needed.
# The linux target does not use the BOARD variable, so without this special
# case, the linux target would clean every time.
ifneq ($(TARGET), linux)
# Building for Zephyr, check for .$(BOARD).last_build to see if clean is needed
ifeq ("$(wildcard .$(BOARD).last_build)", "")
PRE_ACTION=clean
endif
else
# Building for Linux, check for .linux.last_build to see if a clean is needed
ifeq ("$(wildcard .linux.last_build)", "")
PRE_ACTION=clean
endif
endif

# Update dependency repos using deps/repos.txt
.PHONY: update
update:
	@git submodule update --init
	@if ! env | grep -q ^ZEPHYR_BASE=; then \
		echo; \
		echo "ZEPHYR_BASE has not been set! It must be set to build"; \
		echo "e.g. export ZEPHYR_BASE=$(ZJS_BASE)/deps/zephyr"; \
		echo; \
		exit 1; \
	fi
	@cd $(OCF_ROOT); git submodule update --init;

# Sets up prj/last_build files
.PHONY: setup
setup: update
	@echo "# This is a generated file" > prj.conf
ifeq ($(BOARD), qemu_x86)
	@cat prj.conf.qemu_x86 >> prj.conf
else
ifeq ($(DEV), ashell)
	@cat prj.conf.arduino_101_dev >> prj.conf
else
	@cat prj.conf.base >> prj.conf
endif
ifeq ($(BOARD), arduino_101)
	cat prj.conf.arduino_101 >> prj.conf
ifeq ($(ZJS_PARTITION), 256)
	@cat prj.conf.partition_256 >> prj.conf
endif
endif
endif
# Append script specific modules to prj.conf
	@if [ -e prj.conf.tmp ]; then \
		cat prj.conf.tmp >> prj.conf; \
	fi
# Remove .last_build file
	@rm -f .*.last_build
	@echo "" > .$(BOARD).last_build

# Explicit clean
# Update is here because on a fresh checkout, clean will fail. So we want to
# initialize submodules first so clean will succeed in that case. We should find
# a way to make clean work from the start, but for now this should be harmless.
.PHONY: clean
clean: update
	@if [ -d $(ZEPHYR_SDK_INSTALL_DIR) ]; then \
		if [ -d deps/jerryscript ]; then \
			make -C $(JERRY_BASE) -f targets/zephyr/Makefile clean BOARD=$(BOARD); \
			rm -rf deps/jerryscript/build/$(BOARD)/; \
			rm -rf deps/jerryscript/build/lib; \
		fi; \
		if [ -d deps/zephyr ] && [ -e src/Makefile ]; then \
			cd deps/zephyr; make clean BOARD=$(BOARD); \
			cd arc/; make clean; \
		fi; \
	fi
	make -f Makefile.linux clean
	@rm -f src/*.o
	@rm -f src/Makefile
	@rm -f arc/prj.conf
	@rm -f prj.conf
	@rm -f prj.conf.tmp
	@rm -f prj.mdef

.PHONY: pristine
pristine:
	@if [ -d deps/zephyr ] && [ -e outdir ]; then \
		make -f Makefile.zephyr pristine; \
		cd arc; make pristine; \
	fi

# Flash Arduino 101 x86 image
.PHONY: dfu
dfu:
	dfu-util -a x86_app -D outdir/arduino_101/zephyr.bin

# Flash Arduino 101 ARC image
.PHONY: dfu-arc
dfu-arc:
	dfu-util -a sensor_core -D arc/outdir/arduino_101_sss/zephyr.bin

# Flash both
.PHONY: dfu-all
dfu-all: dfu dfu-arc

# Generate the script file from the JS variable
.PHONY: generate
generate: setup
	@echo Creating C string from JS application...
	@./scripts/convert.sh $(JS) src/zjs_script_gen.c

# Run QEMU target
.PHONY: qemu
qemu: $(PRE_ACTION) analyze generate
	make -f Makefile.zephyr BOARD=qemu_x86 KERNEL=$(KERNEL) MEM_STATS=$(MEM_STATS) CB_STATS=$(CB_STATS) qemu

# Builds ARC binary
.PHONY: arc
arc:
	@echo "# This is a generated file" > arc/prj.conf
	@cat arc/prj.conf.base >> arc/prj.conf
ifeq ($(ZJS_PARTITION), 256)
	@cat arc/prj.conf.partition_256 >> arc/prj.conf
endif
	@cd arc; make BOARD=arduino_101_sss

# Run debug server over JTAG
.PHONY: debug
debug:
	make -f Makefile.zephyr BOARD=arduino_101 debugserver

# Run gdb to connect to debug server for x86
.PHONY: gdb
gdb:
	gdb outdir/zephyr.elf -ex "target remote :3333"

# Run gdb to connect to debug server for ARC
.PHONY: arcgdb
arcgdb:
	$$ZEPHYR_SDK_INSTALL_DIR/sysroots/i686-pokysdk-linux/usr/bin/arc-poky-elf/arc-poky-elf-gdb arc/outdir/zephyr.elf -ex "target remote :3334"

# Linux target
.PHONY: linux
# Linux command line target, script can be specified on the command line
linux: $(PRE_ACTION) generate
	rm -f .*.last_build
	echo "" > .linux.last_build
	make -f Makefile.linux JS=$(JS) VARIANT=$(VARIANT) CB_STATS=$(CB_STATS) V=$(V)

.PHONY: help
help:
	@echo "Build targets:"
	@echo "    zephyr:    Build the main Zephyr target (default)"
	@echo "    arc:       Build the ARC Zephyr target for Arduino 101"
	@echo "    all:       Build the zephyr and arc targets"
	@echo "    linux:     Build the Linux target"
	@echo "    dfu:       Flash the x86 core binary with dfu-util"
	@echo "    dfu-arc:   Flash the ARC binary with dfu-util"
	@echo "    dfu-all:   Flash both binaries with dfu-util"
	@echo "    debug:     Run debug server using JTAG"
	@echo "    gdb:       Run gdb to connect to debug server for x86"
	@echo "    arcgdb:    Run gdb to connect to debug server for ARC"
	@echo "    qemu:      Run QEMU after building"
	@echo "    clean:     Clean stale build objects"
	@echo "    setup:     Sets up dependencies"
	@echo "    update:    Updates dependencies"
	@echo
	@echo "Build options:"
	@echo "    BOARD=     Specify a Zephyr board to build for"
	@echo "    JS=        Specify a JS script to compile into the binary"
	@echo "    KERNEL=    Specify the kernel to use (micro or nano)"
	@echo
