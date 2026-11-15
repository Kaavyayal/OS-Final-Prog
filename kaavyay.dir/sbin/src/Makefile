CC = gcc
CFLAGS = -std=gnu18 -O2 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector -fno-pie
LDFLAGS = -nostdlib -static -no-pie -Wl,-e,_start -Wl,--build-id=none

../init: init.c Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) init.c -o $@

.PHONY: clean
clean:
	rm -f ../init
