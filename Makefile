# csqlpage - portable C99 CGI build
#
#   make            SQLite only (default)
#   make PG=1       also build the PostgreSQL backend (needs libpq / pkg-config)
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -pedantic
LDLIBS  ?= -lsqlite3

# Optional PostgreSQL backend (db_pg.c is empty unless WITH_PG is defined).
ifdef PG
CFLAGS  += -DWITH_PG $(shell pkg-config --cflags libpq)
LDLIBS  += $(shell pkg-config --libs libpq)
endif

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := csqlpage

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
