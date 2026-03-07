.PHONY: all clean

OS = $(shell uname -s)
CC = cc
CFLAGS = -std=c99 -Wall -Wextra -pedantic -O2
TARGET = netcat
SRC = main.c connect.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)