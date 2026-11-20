CC = gcc
CFLAGS = -Wall -Werror -O2 -Iinclude
OBJS = src/main.o src/stats.o src/format.o
LDLIBS = -lm

stats: $(OBJS)
	$(CC) $(CFLAGS) -o stats $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f stats $(OBJS) src/format.o

.PHONY: clean
