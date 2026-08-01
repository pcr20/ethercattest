CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
LDFLAGS = -lm
TARGET  = ecat_ber

.PHONY: all clean install

all: $(TARGET)

$(TARGET): ecat_ber.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Install with CAP_NET_RAW so it can run without sudo
install: $(TARGET)
	sudo cp $(TARGET) /usr/local/bin/
	sudo setcap cap_net_raw+ep /usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET)
