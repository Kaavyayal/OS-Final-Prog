SHELL := $(shell which bash)
TEST_EXTS = .ok
UTCS_ID ?= $(shell pwd | sed -e 's/.*_//')

MY_TESTS = ${addprefix ${UTCS_ID},${TEST_EXTS}}

TESTS_DIR ?= .

# Where a test set's files go, like the kernel's (kernel/a/x.cc builds to
# build/kernel/a/x.cc.o): the default set (TESTS_DIR=.) has its disk images in
# build/ and its results (.out, .debug, .diff, .result, ...) here, any other set
# has both in build/<TESTS_DIR>/. Two sets can have tests with the same name.
TESTS_SET := ${patsubst /%,%,${patsubst ./%,%,${patsubst %/,%,${TESTS_DIR}}}}
ifeq (${TESTS_SET},.)
TEST_BUILD := build/
TEST_OUT :=
else
TEST_BUILD := build/${TESTS_SET}/
TEST_OUT := ${TEST_BUILD}
endif

POSSIBLE_TESTS = ${notdir ${basename ${wildcard ${TESTS_DIR}/*${firstword ${TEST_EXTS}}}}}
TESTS = ${sort ${POSSIBLE_TESTS}}
TEST_OKS = ${addsuffix .ok,${TESTS}}
TEST_CYCLES = ${addprefix ${TEST_OUT},${addsuffix .cycles,${TESTS}}}
TEST_RESULTS = ${addprefix ${TEST_OUT},${addsuffix .result,${TESTS}}}
TEST_TARGETS = ${addsuffix .test,${TESTS}}
TEST_OUTS = ${addprefix ${TEST_OUT},${addsuffix .out,${TESTS}}}
TEST_DIFFS = ${addprefix ${TEST_OUT},${addsuffix .diff,${TESTS}}}
TEST_LOOPS = ${addsuffix .loop,${TESTS}}
TEST_FAILS = ${addsuffix .fail,${TESTS}}
TEST_DATA = ${addprefix ${TEST_BUILD}, ${addsuffix .data,${TESTS}}}

# One kernel for every test: a test is its disk image (TESTS_DIR/tN.dir, with
# /sbin/init in it), the kernel runs it
KERNEL = build/kernel.elf

# One boot image for every test: Limine and the kernel. The test's disk image
# is the ramdisk, QEMU's ivshmem device (its shared memory is a PCI BAR, so
# its size is a power of two: the image is padded to RAMDISK_BYTES).
# share=off: the kernel's writes stay in QEMU's copy, the file doesn't change.
BOOT_IMAGE = build/boot.img
RAMDISK_BYTES = 16M

# QEMU's disks for the test whose disk image is $1
qemu_disk_flags = -drive file=${BOOT_IMAGE},index=0,media=disk,format=raw,file.locking=off \
	-object memory-backend-file,id=ramdisk,mem-path=$1,size=${RAMDISK_BYTES},share=off \
	-device ivshmem-plain,memdev=ramdisk

ORIGIN_REPO=${shell git config --get remote.origin.url | sed -e s'/.git$$//'}
STUDENT_NAME=${shell echo ${ORIGIN_REPO} | sed -e 's/.*_//'}
TESTS_REPO=${shell echo ${ORIGIN_REPO} | sed -e 's/_${STUDENT_NAME}/__tests/'}

SRCFILES := $(shell find kernel -type f 2>/dev/null | sort)
CFILES := $(filter %.c,$(SRCFILES))
CCFILES := $(filter %.cc,$(SRCFILES))
SFILES := $(filter %.S,$(SRCFILES))
OFILES := $(addprefix build/,$(CFILES:.c=.c.o) $(CCFILES:.cc=.cc.o) $(SFILES:.S=.S.o))

# GCC 14 only, for now: GCC 15 and 16 leak coroutine frames when a coroutine
# runs on another core before the call that started it returns (the ramp
# updates the frame's reference count non-atomically after initial_suspend
# already handed the coroutine to the event loop).
#
# The course's toolchain if it's there (or TOOLS_DIR=<its bin directory>),
# then gcc-14/g++-14, then plain gcc/g++. Checked by check_compiler.
GCC_MAJOR := 14
TOOLS_PREFER = ~gheith/public/tools26/bin
TOOLS_DIR ?= ${shell (test -x ${TOOLS_PREFER}/g++ && echo ${TOOLS_PREFER}) || true}

ifneq (${TOOLS_DIR},)
CC := ${TOOLS_DIR}/gcc
CXX := ${TOOLS_DIR}/g++
else ifneq (${shell which g++-${GCC_MAJOR} 2>/dev/null},)
CC := gcc-${GCC_MAJOR}
CXX := g++-${GCC_MAJOR}
else
CC := gcc
CXX := g++
endif
LD := ${CXX}

CXX_VERSION := ${shell ${CXX} -dumpfullversion 2>/dev/null}

UTCS_OPTS ?= -O3

CFLAGS := ${UTCS_OPTS} -std=gnu18 -Wall -Werror @common.flags
CCFLAGS := ${UTCS_OPTS} -std=gnu++23 -Wall -Werror @common.flags
LDFLAGS = \
    @common.flags \
    -nostdlib \
    -static \
    -T script.ld

# customize by setting environment variables
QEMU_ACCEL ?= tcg,thread=multi
QEMU_CPU ?= max
QEMU_SMP ?= 4
QEMU_MEM ?= 128m
QEMU_TIMEOUT ?= 10
QEMU_TIMEOUT_CMD ?= timeout
QEMU_DEBUG ?= # -d int

QEMU_PREFER = ~gheith/public/tools26/bin/qemu-system-x86_64
QEMU_CMD ?= ${shell (test -x ${QEMU_PREFER} && echo ${QEMU_PREFER}) || echo qemu-system-x86_64}

QEMU_CONFIG_FLAGS = -accel ${QEMU_ACCEL} \
                    -machine q35 \
	            -cpu ${QEMU_CPU} \
	            -smp ${QEMU_SMP} \
	            -m ${QEMU_MEM} \
	            ${QEMU_DEBUG}

# The console (user programs' read and write): the second serial port (COM2).
# It reads TEST_BUILD/tN.console (TESTS_DIR/tN.in, if there is one, then ^D:
# the end of the input), and writes to $2. The first port (COM1) is the
# kernel's, output only: firmware and Limine read and drop COM1's input while
# booting.
qemu_console_flags = -chardev file,id=console,path=$2,input-path=${TEST_BUILD}$1.console \
	-serial chardev:console

QEMU_FLAGS = -no-reboot \
	     ${QEMU_CONFIG_FLAGS} \
	     -nographic\
	     --monitor none \
	     -serial file:${TEST_OUT}$*.debug \
	     ${call qemu_console_flags,$*,${TEST_OUT}$*.out} \
	     ${call qemu_disk_flags,${TEST_BUILD}$*.data} \
	     -device isa-debug-exit,iobase=0xf4,iosize=0x04

TIME = $(shell which time)

.PHONY: ${TESTS} sig test tests all clean lint check_compiler ${TEST_TARGETS} help qemu_config_flags qemu_cmd before_test 

all : ${TESTS};

help:
	@echo ""
	@echo "Makefile for ${ORIGIN_URL}"
	@echo
	@echo "Useful targets:"
	@echo "    make -s all             # build the kernel and all test images"
	@echo "    make -s t0              # build the kernel and t0's image"
	@echo "    make -s t0.test         # run t0 and report results"
	@echo "    make -s t0.loop         # run t0 10 times and report results"
	@echo "    make -s t0.fail         # run t0 until it fails (max LOOP_LIMIT times)"
	@echo "    make -s test            # run all tests once"
	@echo "    make -s test.loop       # use only if you absolutely have to"
	@echo "                            # and after checking that no one else"
	@echo "                            # is using this machine"
	@echo "    make -s get_tests       # get (or update) peer tests from the server"
	@echo "                            # saves the tests in ./all_tests"
	@echo "                            # please don't push back to git"
	@echo "    make -s get_results     # get (or update) your reuslts from the server"
	@echo "                            # saves the reulsts in ./my_results"
	@echo "                            # please don't push back to git"
	@echo "Environment Variables:"
	@echo "    number of loop iterations: LOOP_LIMIT       (${LOOP_LIMIT})"
	@echo "    qemu acceleration flag   : QEMU_ACCEL       (${QEMU_ACCEL})"
	@echo "    qemu command             : QEMU_CMD         (${QEMU_CMD})"
	@echo "    qemu cpu                 : QEMU_CPU         (${QEMU_CPU})"
	@echo "    simulated memory         : QEMU_MEM         (${QEMU_MEM})"
	@echo "    number of cores          : QEMU_SMP         (${QEMU_SMP})"
	@echo "    timeout                  : QEMU_TIMEOUT     (${QEMU_TIMEOUT})"
	@echo "    timeout command          : QEMU_TIMEOUT_CMD (${QEMU_TIMEOUT_CMD})"
	@echo "    tests directory          : TESTS_DIR        (${TESTS_DIR})"
	@echo ""
	@echo "tests: ${TESTS}"

origin:
	@echo "repo       : ${ORIGIN_REPO}"
	@echo "student    : ${STUDENT_NAME}"
	@echo "tests repo : ${TESTS_REPO}"

get_tests:
	test -d all_tests || git clone ${TESTS_REPO} all_tests
	(cd all_tests ; git pull)
	@echo ""
	@echo "Tests copied to all_tests (cd all_tests)"
	@echo "   Please don't add the all_tests directory to git"
	@echo ""

get_summary:
	test -d all_results || git clone ${GIT_SERVER}:${PROJECT_NAME}__results all_results
	(cd all_results ; git pull)
	python tools/summarize.py all_results

get_results:
	test -d my_results || git clone ${ORIGIN_REPO}_results my_results
	(cd my_results ; git pull)
	@(cd my_results;                                                      \
		for i in *.result; do                                         \
			name=$$(echo $$i | sed -e 's/\..*//');                \
			echo "$$name `cat $$name.result` `cat $$name.time`";  \
		done;                                                         \
		echo "";                                                      \
		echo "`grep pass *.result | wc -l` / `ls *.result | wc -l`";  \
	)
	@echo ""
	@echo "More details in my_results (cd my_results)"
	@echo "    Please don't add my_results directory to git"
	@echo ""

qemu_cmd:
	@echo "${QEMU_CMD}"

qemu_config_flags:
	@echo "${QEMU_CONFIG_FLAGS}"

# for run_qemu, debug_qemu and monitor_qemu
%.qemu_disk_flags:
	@echo "${call qemu_disk_flags,${TEST_BUILD}$*.data}"

# the console's output goes to the terminal
%.qemu_console_flags:
	@echo "${call qemu_console_flags,$*,/dev/stdout}"

$(TESTS) : % : ${TEST_BUILD}%.data ${TEST_BUILD}%.console ${BOOT_IMAGE};

# Static checks: pixi run make -s lint (clang-tidy 23 is in the pixi
# environment). What it checks, and why: tools/lint.clang-tidy
CLANG_TIDY ?= clang-tidy
LINT_FLAGS = ${filter-out -MD -MP,${shell cat common.flags}}

lint:
	@${CLANG_TIDY} --quiet --experimental-custom-checks \
	    --config-file=tools/lint.clang-tidy \
	    ${CCFILES} -- -I${shell pwd} -std=gnu++23 ${LINT_FLAGS} \
	    2> >(grep -v '^\[[0-9]*/[0-9]*\] Processing file' >&2)

clean:
	rm -rf *.diff *.out *.result *.failure *.time build *.debug *.cycles
	make -C limine clean

# Stops the build unless CXX is GCC 14 (see GCC_MAJOR)
check_compiler:
	@if [ "${firstword ${subst ., ,${CXX_VERSION}}}" != "${GCC_MAJOR}" ]; then \
	    echo "*** ${CXX} is GCC '${CXX_VERSION}', the kernel needs GCC ${GCC_MAJOR}"; \
	    echo "*** install g++-${GCC_MAJOR}, or set TOOLS_DIR to the bin directory of one"; \
	fi

# Which compiler built build/: changing it rebuilds everything (objects from
# two compilers don't mix)
build/compiler: check_compiler
	@mkdir -p build
	@echo "${CXX} ${CXX_VERSION}" | cmp -s - $@ || echo "${CXX} ${CXX_VERSION}" > $@

# if ccache is installed, CCACHE will be set to its path
CCACHE ?= ${shell command -v ccache 2> /dev/null}

build/%.c.o: %.c Makefile common.flags build/compiler
	@mkdir -p "$(dir $@)"
	${CCACHE} $(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

build/%.cc.o: %.cc Makefile common.flags build/compiler
	@mkdir -p "$(dir $@)"
	${CCACHE} $(CXX) -I${shell pwd} $(CCFLAGS) $(CPPFLAGS) -c $< -o $@

build/%.S.o: %.S Makefile common.flags build/compiler
	@mkdir -p "$(dir $@)"
	${CCACHE} $(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

${KERNEL} : common.flags Makefile script.ld build/compiler $(OFILES)
	@mkdir -p build
	$(LD) $(LDFLAGS) $(OFILES) -o $@

limine/limine: Makefile limine/*.c limine/*.h limine/Makefile
	make -C limine

LIMINE_FILES := limine.conf limine/limine-bios.sys limine/BOOTX64.EFI

# every disk image has 4K blocks
BLOCK_SIZE = 4096

MKFS = mkfs.ext2 -q -b ${BLOCK_SIZE} -i ${BLOCK_SIZE} -d ${TESTS_DIR}/$*.dir  -I 128 -t ext2 ${TEST_BUILD}$*.data 10m

# (made again when the test's init is: make -C tN.dir builds it)
${TEST_DATA} : ${TEST_BUILD}%.data : Makefile ${TESTS_DIR}/%.dir/sbin/init
	@mkdir -p ${TEST_BUILD}
	@rm -f ${TEST_BUILD}$*.data
	(${MKFS} -E revision=0 2> /dev/null) || ${MKFS} -r 0
	truncate -s ${RAMDISK_BYTES} ${TEST_BUILD}$*.data


${BOOT_IMAGE}: Makefile limine/limine ${KERNEL} ${LIMINE_FILES}
	# borrowed from https://codeberg.org/Limine/limine-cxx-template/src/branch/trunk/GNUmakefile
	rm -f $@
	# zero-filled 20MB raw disk image
	dd if=/dev/zero bs=1M seek=20 count=0 of=$@ > image.debug 2>&1
	# create a boot partition
	PATH=$$PATH:/usr/sbin:/sbin sgdisk $@ -n 1:2048 -t 1:ef00 -m 1 >> image.debug 2>&1
	# install BIOS boot sector and stage2
	./limine/limine bios-install $@ >> image.debug 2>&1
	# format the rest as a FAT32 file system (Limine requirement)
	mformat -i $@@@1M >> image.debug 2>&1
	# create required directories (UEFI and Limine specified)
	mmd -i $@@@1M ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine >> image.debug 2>&1
	# copy the kernel executable
	mcopy -i $@@@1M ${KERNEL} ::/boot/kernel >> image.debug 2>&1
	# copy the Limine required files
	mcopy -i $@@@1M ${LIMINE_FILES} ::/boot/limine >> image.debug 2>&1
	touch $@

# (a test's .in, if it has one, is a prerequisite too)
.SECONDEXPANSION:

# What the console reads: the test's .in (if there is one), then ^D (\004),
# which the kernel takes as the end of the input.
${TEST_BUILD}%.console : Makefile $$(wildcard ${TESTS_DIR}/$$*.in)
	@mkdir -p ${TEST_BUILD}
	@(cat $(wildcard ${TESTS_DIR}/$*.in) /dev/null; printf '\004') > $@

# The kernel's output (COM1) goes to .debug, the user programs' (COM2) to .out.
${TEST_OUTS} : ${TEST_OUT}%.out : Makefile % ${TEST_BUILD}%.console
	@rm -f ${TEST_OUT}$*.out ${TEST_OUT}$*.debug ${TEST_OUT}$*.failure
	@touch ${TEST_OUT}$*.failure
	-(${TIME} --quiet -o ${TEST_OUT}$*.time -f "%E" ${QEMU_TIMEOUT_CMD} ${QEMU_TIMEOUT} ${QEMU_CMD} ${QEMU_FLAGS} > ${TEST_OUT}$*.failure 2>&1); if [ $$? -eq 124 ]; then echo "timeout" > ${TEST_OUT}$*.failure; echo "timeout" > ${TEST_OUT}$*.time; fi
	@[ -s ${TEST_OUT}$*.debug ] || echo "failed to run, look in ${TEST_OUT}$*.failure for more details" > ${TEST_OUT}$*.out
	@touch ${TEST_OUT}$*.out ${TEST_OUT}$*.cycles
	@-egrep '^@@@ ' ${TEST_OUT}$*.debug > ${TEST_OUT}$*.cycles 2>&1

${TEST_DIFFS} : ${TEST_OUT}%.diff : Makefile ${TEST_OUT}%.out ${TESTS_DIR}/%.ok
	-(diff -wBb ${TEST_OUT}$*.out ${TESTS_DIR}/$*.ok > ${TEST_OUT}$*.diff 2> /dev/null || true)

${TEST_RESULTS} : ${TEST_OUT}%.result : Makefile ${TEST_OUT}%.diff
	(test -z "`cat ${TEST_OUT}$*.diff`" && echo "pass" > ${TEST_OUT}$*.result) || echo "fail" > ${TEST_OUT}$*.result

${TEST_TARGETS} : %.test : Makefile ${TEST_OUT}%.result
	@echo -n "$* ... "
	@echo "`cat ${TEST_OUT}$*.result` `cat ${TEST_OUT}$*.time`"

OTHER_USERS = ${shell who | sed -e 's/ .*//' | sort | uniq}
HOW_MANY = ${shell who | sed -e 's/ .*//' | sort | uniq | wc -l}
LOOP_LIMIT ?= 10

loop_warning.%:
	@echo "*******************************************************************************"
	@echo "*** This is NOT the sort of thing you run ALL THE TIME on a SHARED MACHINE  ***"
	@echo "*** In particular long running tests and tests that timeout                 ***"
	@echo "*******************************************************************************"
	@echo ""
	@echo "You can use LOOP_LIMIT to control the number if iterations. For example:"
	@echo "   LOOP_LIMIT=7 make -s $*.loop"
	@echo ""
	@echo "::::::: You are 1 of ${HOW_MANY} users on this machine"
	@echo ":::::::         ${OTHER_USERS}"
	@echo ":::::::   all of them value their work and their time as much as you value yours"
	@echo ":::::::"
	@echo ""


${TEST_LOOPS} : %.loop : loop_warning.% %
	@let pass=0; \
	for ((i=1; i<=${LOOP_LIMIT}; i++)); do \
		echo -n  "[$$i/${LOOP_LIMIT}] "; \
		$(MAKE) -s $*.test; \
		if [ "`cat ${TEST_OUT}$*.result`" = "pass" ]; then let pass=pass+1; fi; \
	done; \
	echo ""; \
	echo "$$(basename $$(pwd)) $* $$pass/${LOOP_LIMIT}"; \
	echo ""

${TEST_FAILS} : %.fail : loop_warning.% %
	@let pass=0; \
	for ((i=1; i<=${LOOP_LIMIT}; i++)); do \
		echo -n  "[$$i/${LOOP_LIMIT}] "; \
		$(MAKE) -s $*.test; \
		if [ "`cat ${TEST_OUT}$*.result`" = "pass" ]; then let pass=pass+1; else break; fi; \
	done; \
	echo ""; \
	echo "$$(basename $$(pwd)) $* $$pass/${LOOP_LIMIT}"; \
	echo ""	

before_test:
	rm -f ${addprefix ${TEST_OUT},*.result *.time *.out *.failure *.cycles}

test: before_test Makefile ${TESTS} ${TEST_TARGETS} ;
	-@echo ""
	-@echo -n "$$(basename $$(pwd)) "
	-@echo "pass:`(grep pass ${TEST_OUT}*.result | wc -l) || echo 0`/`(ls ${TEST_OUT}*.result | wc -l) || echo 0`"
	-@echo ""

test.loop: loop_warning.test ${TEST_LOOPS}

failed:
	-@for i in `grep -l fail ${TEST_OUT}*.result`; do \
		t=`echo $$i | sed -e "s/\.result//"`; \
		echo ""; \
		echo "**************** $$t ****************"; \
		cat $$t.diff; \
	done

format:
	find kernel -name '*.cc' -o -name '*.h' | xargs clang-format -i


-include ${shell find build -name '*.d' 2> /dev/null}
-include *.d

