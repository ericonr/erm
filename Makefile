OPT = -O2
CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809UL -D_DEFAULT_SOURCE -Wall -Wextra $(OPT)

.PHONY: all

all: erm

erm: erm.c remove.c | erm.h

clean:
	rm -f erm
