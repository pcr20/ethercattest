CC      = gcc
CFLAGS  = -D_GNU_SOURCE -O2 -Wall -Wextra -std=c11 -msse4.2 -flto
LDFLAGS = -flto -lm -lpthread
OBJS    = crc.o stats.o frame.o nic.o threads.o main.o

ecat_ber: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c ecat_common.h crc.h stats.h frame.h nic.h threads.h
	$(CC) $(CFLAGS) -c $< -o $@

TESTS = t_wirefmt t_framesz t_txok t_crc t_resid3 t_escgate t_plsem t_ring
test: $(TESTS:%=tests/%)
	@for t in $(TESTS); do ./tests/$$t || exit 1; done
	@echo "ALL TEST SUITES PASS"

tests/%: tests/%.c crc.c stats.c frame.c ecat_common.h crc.h stats.h frame.h
	$(CC) -D_GNU_SOURCE -O2 -Wall -std=c11 -msse4.2 -I. -o $@ $< -lm -lpthread

clean:
	rm -f ecat_ber $(OBJS) $(TESTS:%=tests/%)

.PHONY: test clean
