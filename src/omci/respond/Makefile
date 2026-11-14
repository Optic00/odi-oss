# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# omcid, the OMCI daemon.
#
# Reuses the freestanding runtime of diag (_start, syscall wrappers, output
# layer), so the two binaries share one copy of the o32 plumbing.

# The freestanding toolchain image, pinned by digest in toolchain/images.env
# and pulled on first use; DIAG_IMAGE overrides it (toolchain/image.sh).
TOP := $(CURDIR)/../../..
-include $(TOP)/toolchain/images.env
include $(TOP)/src/harden.mk
DIAG_IMAGE ?= $(DIAG_IMAGE_PINNED)
IMAGE := $(DIAG_IMAGE)
CROSS := mips-linux-gnu-
BUILD := build
BIN   := $(BUILD)/omcid
DIAG  := ../../diag
# The OMCI driver ABI, owned by the kernel side.
UAPI  := ../../../kernel/extra/drivers/net/ethernet/odi/uapi

ifeq ($(IN_CONTAINER),1)

CC    := $(CROSS)gcc
STRIP := $(CROSS)strip

CFLAGS := -std=c11 -Os -Wall -Wextra \
          -march=mips1 -mabi=32 -EB -msoft-float -G0 \
          -fno-pic -mno-abicalls -ffreestanding -fno-builtin $(HARDEN_CFLAGS) \
          -ffunction-sections -fdata-sections \
          -I$(DIAG)/src -I../generated -I$(UAPI)
LDFLAGS := -nostdlib -nostartfiles -static -Wl,-e,_start -Wl,--build-id=none \
           -Wl,--gc-sections $(HARDEN_LDFLAGS)

# The translation units of omcid, divided by what each one talks to: the
# store, the driver, the renderers, one file per transport. omcid.h declares
# everything that crosses.
SRCS := main mibstore drv apply apply_qos apply_uni apply_bridge show clisrv vqsrv omcimsg cfgstore snapshot events swimage reload rereg alarm mcast pm voip

OBJS := $(BUILD)/start.o $(BUILD)/io.o $(SRCS:%=$(BUILD)/%.o) \
        $(BUILD)/extvlan.o $(BUILD)/mcastvlan.o $(BUILD)/omci_mib.o \
        $(BUILD)/omci_autonomous.o $(BUILD)/omci_defaults.o $(BUILD)/omci_drv.o $(BUILD)/omci_bdgconn.o

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	$(HARDEN_CHECK) $@
	$(STRIP) $@

# The driver-call golden (../drv-test.sh): the same objects, except drv.c
# built with OMCI_DRV_TRACE, where every driver call is logged and succeeds.
# Never shipped; omcid itself is unchanged by it.
$(BUILD)/omcid-drvtrace: $(filter-out $(BUILD)/drv.o,$(OBJS)) $(BUILD)/drv-drvtrace.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	$(HARDEN_CHECK) $@
	$(STRIP) $@

$(BUILD)/drv-drvtrace.o: drv.c $(OMCID_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -DOMCI_DRV_TRACE -c -o $@ $<

$(BUILD)/start.o: $(DIAG)/src/start.S | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/io.o: $(DIAG)/src/io.c $(DIAG)/src/io.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

OMCID_HDRS := omcid.h $(DIAG)/src/sys.h $(DIAG)/src/io.h \
              ../generated/omci_mib.h ../nl.h ../omci.h $(UAPI)/omci_gemflow.h \
              $(UAPI)/omci_caps.h $(UAPI)/omci_bdgconn.h $(UAPI)/omci_bridgeport.h \
              ../omci_cli_proto.h \
              ../omci_msgq.h ../omci_tmpfile.h ../extvlan.h ../mcastvlan.h \
              ../procparse.h ../redirect_guard.h

$(BUILD)/%.o: %.c $(OMCID_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/extvlan.o: ../extvlan.c ../extvlan.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/mcastvlan.o: ../mcastvlan.c ../mcastvlan.h ../extvlan.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# The class 171 entry decode, on its own. No message queue, no store, no
# device -- which is why it is a separate binary rather than another case in
# qemu-test.sh.
$(BUILD)/extvlan_test: ../test/extvlan_test.c ../extvlan.h \
                       $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/extvlan.o
	$(CC) $(CFLAGS) $(LDFLAGS) -I.. -o $@ ../test/extvlan_test.c \
	      $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/extvlan.o

# Class 309 attribute 16 as bridge rules, the same way: a pure generator and
# its own binary.
$(BUILD)/mcastvlan_test: ../test/mcastvlan_test.c ../mcastvlan.h ../extvlan.h \
                         $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/extvlan.o $(BUILD)/mcastvlan.o
	$(CC) $(CFLAGS) $(LDFLAGS) -I.. -o $@ ../test/mcastvlan_test.c \
	      $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/extvlan.o $(BUILD)/mcastvlan.o

# Real downstream ownership paths, with fault injection at the command boundary.
$(BUILD)/downstream_ownership_test: ../test/downstream_ownership_test.c $(filter-out $(BUILD)/main.o,$(OBJS))
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--wrap=omci_drv_call -I. -o $@ $^

# The real teardown and service table, with the driver call wrapped to count
# the deactivates. No device or netlink is opened.
$(BUILD)/service_ownership_test: ../test/service_ownership_test.c $(filter-out $(BUILD)/main.o,$(OBJS))
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--wrap=omci_drv_call -I. -o $@ $^

# The MIB data sync counter across its wrap.
$(BUILD)/mib_sync_test: ../test/mib_sync_test.c $(filter-out $(BUILD)/main.o,$(OBJS))
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--wrap=omci_drv_call -I. -o $@ $^

$(BUILD)/omci_mib.o: ../generated/omci_mib.c ../generated/omci_mib.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/omci_autonomous.o: ../generated/omci_autonomous.c \
                           ../generated/omci_autonomous.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/omci_defaults.o: ../generated/omci_defaults.c \
                           ../generated/omci_defaults.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/omci_drv.o: ../generated/omci_drv.c ../generated/omci_drv.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/omci_bdgconn.o: ../generated/omci_bdgconn.c $(UAPI)/omci_bdgconn.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

else

RUN := docker run --rm --user $$(id -u):$$(id -g) -e HOME=/tmp -v $(CURDIR)/../../..:/src -w /src/src/omci/respond $(IMAGE)

.PHONY: all image cap drvtrace run size test clean
all: cap

image:
	@DIAG_IMAGE='$(IMAGE)' $(TOP)/toolchain/image.sh diag >/dev/null

cap: image
	$(RUN) make IN_CONTAINER=1 $(BIN)
	@ls -l $(BIN)

drvtrace: image
	$(RUN) make IN_CONTAINER=1 $(BUILD)/omcid-drvtrace

run: cap
	@$(RUN) qemu-mips-static $(BIN) $(ARGS)

size: cap
	@$(RUN) sh -c 'ls -l $(BIN); $(CROSS)size $(BIN)'

test: image
	$(RUN) make IN_CONTAINER=1 $(BUILD)/extvlan_test $(BUILD)/mcastvlan_test $(BUILD)/downstream_ownership_test $(BUILD)/service_ownership_test $(BUILD)/mib_sync_test
	@$(RUN) qemu-mips-static $(BUILD)/extvlan_test
	@$(RUN) qemu-mips-static $(BUILD)/mcastvlan_test
	@$(RUN) qemu-mips-static $(BUILD)/downstream_ownership_test
	@$(RUN) qemu-mips-static $(BUILD)/service_ownership_test
	@$(RUN) qemu-mips-static $(BUILD)/mib_sync_test

clean:
	rm -rf $(BUILD)

endif

# Exercise the responder with no stick: omcid under qemu, serving its message
# queues with no line side, checked against output captured from a real one.
# See ../qemu-test.sh for why netlink cannot work there.
# `cap`, not $(BIN): outside the container there is no rule to build $(BIN),
# so depending on the file would pass silently against a stale binary.
.PHONY: qemu-test
qemu-test: cap
	$(MAKE) -C ../cli
	docker run --rm -v $(CURDIR)/../../..:/src -w /src/src/omci \
	        $(IMAGE) sh qemu-test.sh
