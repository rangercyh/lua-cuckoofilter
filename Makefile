.PHONY: all test clean

TARGET=cuckoofilter.so

all: $(TARGET)

CFLAGS = $(CFLAG)
CFLAGS += -std=c99 -g3 -O2 -rdynamic -Wall -fPIC -shared

$(TARGET): lua-cuckoofilter.c
	gcc $(CFLAGS) -o $@ $^

clean:
	rm $(TARGET)

test:
	lua test.lua
