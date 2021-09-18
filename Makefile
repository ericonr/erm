OPT = -O2
CFLAGS = -Wall -Wextra $(OPT)

.PHONY: all

all: erm

erm: erm.c remove.c | erm.h

clean:
	rm -f erm
