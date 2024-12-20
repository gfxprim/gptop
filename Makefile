CFLAGS?=-W -Wall -Wextra -O2 -ggdb
CFLAGS+=$(shell gfxprim-config --cflags)
LDLIBS=-lgfxprim $(shell gfxprim-config --libs-widgets) -lsysinfo
BIN=gptop
DEP=$(BIN:=.dep)

all: $(DEP) $(BIN)

%.dep: %.c
	$(CC) $(CFLAGS) -M $< -o $@

-include $(DEP)

install:
	install -m 644 -D layout.json $(DESTDIR)/etc/gp_apps/$(BIN)/layout.json
	install -D $(BIN) -t $(DESTDIR)/usr/bin/

clean:
	rm -f $(BIN) *.dep *.o
