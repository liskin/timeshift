#
# timeshift
#
# Author: Tomas Janousek <tomi@nomi.cz>
# License: GPL
#

CFLAGS=-Wall -std=c99 -pedantic -g -mthreads
LDLIBS=-lws2_32

.PHONY: all clean

all: timeshift

clean:
	$(RM) timeshift
