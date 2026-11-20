CC = gcc
CFLAGS = -Wall -Werror -O2 -Iinc
OBJS = src/main.o src/stats.o
LDLIBS = -lm

stats: $(OBJS)
	$(CC) $(CFLAGS) -o stats $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f stats $(OBJS) src/format.o

.PHONY: clean
