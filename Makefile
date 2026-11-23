CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?= -Isrc
LDFLAGS ?=

SOURCES = src/main.c src/parser.c src/executor.c src/commands.c
OBJECTS = $(SOURCES:.c=.o)
TARGET = mysh

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

src/%.o: src/%.c src/shell.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)
