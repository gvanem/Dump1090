CC     ?= gcc
CFLAGS ? = -O2 -g -Wall $(shell pkg-config --cflags librtlsdr)
LDLIBS += $(shell pkg-config --libs librtlsdr) -lpthread -lm

all: dump1090

%.o: %.c
	$(CC) $(CFLAGS) -c $<

dump1090: dump1090.o mongoose.o
	$(CC) -g -o dump1090 dump1090.o mongoose.o $(LDLIBS)

clean:
	rm -f *.o dump1090
