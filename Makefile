CC     = gcc
CFLAGS = -Wall -Wextra -O2

all: nvram_write nvram_read nvram_reset

nvram_write: nvram_write.c
	$(CC) $(CFLAGS) -o $@ $<

nvram_read: nvram_read.c
	$(CC) $(CFLAGS) -o $@ $<

nvram_reset: nvram_reset.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f nvram_write nvram_read nvram_reset
