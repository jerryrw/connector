.PHONY: all clean

UNAME_S := $(shell uname -s)

CC ?= cc
TARGET ?= connect

SRC := main.c connect.c
OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d)

CPPFLAGS += -MMD -MP
CFLAGS += -std=c99 -Wall -Wextra -pedantic -O2
LDFLAGS +=
LDLIBS +=

# pthread flags (needed on Linux; accepted on macOS/clang too)
ifeq ($(UNAME_S),Linux)
CFLAGS += -pthread
LDFLAGS += -pthread
endif
ifeq ($(UNAME_S),Darwin)
CFLAGS += -pthread
LDFLAGS += -pthread
endif

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) $(LDLIBS) -o $@

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJ) $(DEP)

-include $(DEP)