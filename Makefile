CC     = gcc
CFLAGS = -Wall -Wextra -O2

nvram_write: nvram_write.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f nvram_write
