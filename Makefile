CC      = gcc
CFLAGS  = -D_GNU_SOURCE -O2 -Wall -Wextra -std=c11 -msse4.2 -flto
LDFLAGS = -flto -lm -lpthread
OBJS    = crc.o stats.o frame.o nic.o threads.o main.o

DUMP_OBJS = escreg.o nic.o regdump_main.o
# ecat_phy WRITES to the slave (ESC MII management). It links escmii.o; the
# read-only ecat_regdump deliberately does NOT.
PHY_OBJS  = escreg.o escmii.o nic.o phy_main.o

all: ecat_ber ecat_regdump ecat_phy

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

%.o: %.c ecat_common.h crc.h stats.h frame.h nic.h threads.h escreg.h escmii.h
	$(CC) $(CFLAGS) -c $< -o $@

TESTS = t_escframe t_miiframe t_wirefmt t_framesz t_txok t_crc t_resid3 t_escgate t_plsem t_ring
test: $(TESTS:%=tests/%)
	@for t in $(TESTS); do ./tests/$$t || exit 1; done
	@echo "ALL TEST SUITES PASS"

tests/%: tests/%.c crc.c stats.c frame.c nic.c escreg.c escmii.c ecat_common.h crc.h stats.h frame.h escreg.h escmii.h
	$(CC) -D_GNU_SOURCE -O2 -Wall -std=c11 -msse4.2 -I. -o $@ $< -lm -lpthread

clean:
	rm -f ecat_ber ecat_regdump ecat_phy $(OBJS) $(DUMP_OBJS) $(PHY_OBJS) $(TESTS:%=tests/%)

.PHONY: all test clean
