#
# timeshift
#
# Author: Tomas Janousek <tomi@nomi.cz>
# License: GPL
#

CFLAGS=-Wall -std=c99 -pedantic -g

.PHONY: all clean

all: timeshift

clean:
	$(RM) timeshift
