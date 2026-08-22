CC        := ccache gcc
STANDARDS := -D_POSIX_C_SOURCE=200908L -D_XOPEN_SOURCE=600 -D_XOPEN_SOURCE_EXTENDED
# TINE is an ACE command.  Its executable is still a host process (ACE runs
# it through the Linux command), but pathname semantics come from ACE's live
# DOS session.
# Keep the resolver in ACE rather than growing a second broker client here.
ACE_DIR   ?= $(abspath ../..)
ACE_BUILD ?= $(ACE_DIR)/build
ACE_CFLAGS := -I$(ACE_DIR)/src -I$(ACE_DIR)/compat
CFLAGS    := $(STANDARDS) $(CURSES) $(ACE_CFLAGS) -Os
DESTDIR   ?= /usr/local
CCACHE_PREFIX ?= distcc
export CCACHE_PREFIX

# broker-identity.o and ace-modes.o travel with broker_client.o: together they
# determine which mode-specific broker a client reaches. ACE's own Makefile
# keeps the trio in BROKER_CLIENT_OBJS; tine builds separately, so it names
# all three here.
# ace-crm-retry.o joins them for the same reason: the posix wrappers reach the
# filesystem through the privilege seam now, so a build that has the wrappers
# without the seam does not link.
ACE_PATH_OBJS := $(ACE_BUILD)/ace-amiga-posix.o $(ACE_BUILD)/broker_client.o \
                 $(ACE_BUILD)/broker-identity.o $(ACE_BUILD)/ace-modes.o \
                 $(ACE_BUILD)/ace-crm-retry.o

all: tine
	strip tine

clean:
	rm -rf *.o tine

tine: buffer.o command.o editor.o mode.o parser.o tine_console.o util.o \
      $(ACE_PATH_OBJS)

$(ACE_PATH_OBJS):
	$(MAKE) -C "$(ACE_DIR)" "$(patsubst $(ACE_DIR)/%,%,$@)"

install: all
	mkdir -p "$(DESTDIR)/bin" "$(DESTDIR)/share/man/man1"
	cp tine "$(DESTDIR)/bin/tine"
	cp tine.1 "$(DESTDIR)/share/man/man1/tine.1"
