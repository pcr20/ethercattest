CC      = gcc
CFLAGS  = -D_GNU_SOURCE -O2 -Wall -Wextra -std=c11 -msse4.2 -flto
LDFLAGS = -flto -lm -lpthread
# ecat_ber links the fault-capture module and the ESC/MII transport it needs.
# The objects stay separate and ecat_ber includes only faultcap.h.
OBJS    = crc.o stats.o frame.o nic.o threads.o main.o faultcap.o escreg.o escmii.o

DUMP_OBJS = escreg.o nic.o regdump_main.o
OP_OBJS   = escreg.o escmii.o nic.o faultcap.o ecat_master.o op_main.o
# ecat_phy WRITES to the slave (ESC MII management). It links escmii.o; the
# read-only ecat_regdump deliberately does NOT.
PHY_OBJS  = escreg.o escmii.o nic.o phy_main.o
# Clears ESC error counters (APWR). Separate binary; ecat_regdump stays
# read-only by construction and links neither escmii.o nor this main.
RESET_OBJS = escreg.o escmii.o nic.o escreset_main.o

all: ecat_ber ecat_regdump ecat_phy ecat_escreset ecat_op

ecat_ber: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

# Read-only ESC register dump. Shares ecat_common.h and nic.c with the BER
# tester; contains NO write (APWR) code path by construction.
ecat_regdump: $(DUMP_OBJS) stats.o crc.o
	$(CC) -o $@ $(DUMP_OBJS) stats.o crc.o $(LDFLAGS)

# PHY register access over ESC MII. Writes ESC registers even to read a PHY
# register; writing a PHY register additionally requires --allow-phy-write.
ecat_phy: $(PHY_OBJS) stats.o crc.o
	$(CC) -o $@ $(PHY_OBJS) stats.o crc.o $(LDFLAGS)

ecat_escreset: $(RESET_OBJS) stats.o crc.o
	$(CC) -o $@ $(RESET_OBJS) stats.o crc.o $(LDFLAGS)

# soem_check is an INDEPENDENT master built against an external SOEM tree; it
# is not part of `all`. Run tools/soem_setup.sh first.
SOEM_DIR ?= $(HOME)/soem
soem_check: soem_check.c
	@test -f $(SOEM_DIR)/include/soem/ec_options.h || \
	  { echo "SOEM not set up: run tools/soem_setup.sh"; exit 1; }
	$(CC) -O2 -Wall -std=gnu11 -I$(SOEM_DIR)/include -I$(SOEM_DIR)/osal \
	  -I$(SOEM_DIR)/osal/linux -I$(SOEM_DIR)/oshw/linux -o $@ $< \
	  $(SOEM_DIR)/src/*.c $(SOEM_DIR)/osal/linux/osal.c \
	  $(SOEM_DIR)/oshw/linux/nicdrv.c $(SOEM_DIR)/oshw/linux/oshw.c -lpthread -lrt

ecat_op: $(OP_OBJS) stats.o crc.o
	$(CC) -o $@ $(OP_OBJS) stats.o crc.o $(LDFLAGS)

%.o: %.c ecat_common.h crc.h stats.h frame.h nic.h threads.h escreg.h escmii.h phy_regs.h faultcap.h
	$(CC) $(CFLAGS) -c $< -o $@

TESTS = t_escframe t_miiframe t_physweep t_hostrx t_escclear t_pause t_faultcap t_wirefmt t_framesz t_txok t_crc t_resid3 t_escgate t_plsem t_ring t_pace t_escpanel t_opseq t_acyclic t_recovery
test: $(TESTS:%=tests/%)
	@for t in $(TESTS); do ./tests/$$t || exit 1; done
	@echo "ALL TEST SUITES PASS"

tests/%: tests/%.c crc.c stats.c frame.c nic.c escreg.c escmii.c faultcap.c ecat_common.h crc.h stats.h frame.h escreg.h escmii.h phy_regs.h faultcap.h
	$(CC) -D_GNU_SOURCE -O2 -Wall -std=c11 -msse4.2 -I. -o $@ $< -lm -lpthread

clean:
	rm -f ecat_ber ecat_regdump ecat_phy ecat_escreset ecat_op soem_check $(OBJS) $(OP_OBJS) $(DUMP_OBJS) $(PHY_OBJS) $(RESET_OBJS) $(TESTS:%=tests/%)

.PHONY: all test clean
