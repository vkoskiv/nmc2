CC=cc
CFLAGS=-g -Wall -Wextra -Wno-missing-field-initializers -std=c99 -D_GNU_SOURCE -O2 $$(pkg-config --cflags uuid sqlite3 zlib libbsd)
LDFLAGS=-lm -lpthread $$(pkg-config --libs uuid sqlite3 zlib libbsd)
BIN=bin/nmc2
OBJDIR=bin/obj
SRCS=$(shell find . -name '*.c')
OBJS=$(patsubst %.c, $(OBJDIR)/%.o, $(SRCS))

all: $(BIN)

$(OBJDIR)/%.o: %.c $(OBJDIR)
	@mkdir -p '$(@D)'
	$(info CC $<)
	@$(CC) $(CFLAGS) -c $< -o $@
$(OBJDIR):
	mkdir -p $@
$(BIN): $(OBJS) $(OBJDIR)
	$(info LD $@)
	@$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

clean:
	rm -rf bin/*

.PHONY: run
run: all
	@bin/nmc2
