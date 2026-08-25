ifeq ($(origin CC),default)
CC := ccache gcc
endif
# ccache handles the local cache; its prefix sends cache misses through
# distcc. Both remain overridable for local or cross builds.
CCACHE_PREFIX ?= distcc
export CCACHE_PREFIX
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-pointer-sign -O2

# Header dependency tracking.  Without this, editing a header in compat/
# rebuilds nothing that includes it, because the rules below list only their
# .c files.  That is not merely a stale build: compat/include/dos/dosextens.h
# defines struct Process, so a half-rebuilt tree links objects that disagree
# about where pr_CurrentDir and pr_CIS live, and the result deadlocks rather
# than failing to compile.  `override` so the tracking survives a CFLAGS
# override on the command line.
# -MF is explicit because distcc rewrites the compile command and, without
# it, drops the dependency file beside the source under its basename -- which
# lands stray .d files in the repository root rather than in build/.
override CFLAGS += -MMD -MP -MF $(BUILD)/$(@F).d

# The broker's wire format is versioned by hashing the header that defines
# it, so the version cannot be forgotten when the layout changes.  Both sides
# fold this into the protocol magic; a mismatch is then caught on the first
# field read rather than after a payload has been misparsed.
ACE_BROKER_PROTOCOL_VERSION := $(shell sha256sum src/broker_protocol.h | cut -c1-8)
# The FMM's protocol version follows the same rule, and for a sharper
# reason: the peers are a user process and a root one, so a version they
# disagree about is a privileged process misreading a payload.
ACE_PRIVILEGE_PROTOCOL_VERSION := $(shell sha256sum src/ace_privilege_protocol.h | cut -c1-8)

# ACE_SYS_DIR used to be given to broker.o alone.  Every client now resolves
# SYS: too, because the broker's socket name is keyed to it, and a client that
# resolved a different root would look for its broker on a different socket
# and never find it.  One definition, given to everything.
override CFLAGS += -DACE_SYS_DIR='"$(SYSDIR)"' \
                   -DAMIGA_BROKER_PROTOCOL_VERSION=0x$(ACE_BROKER_PROTOCOL_VERSION)u \
                   -DACE_PRIVILEGE_PROTOCOL_VERSION=0x$(ACE_PRIVILEGE_PROTOCOL_VERSION)u
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0)
BLKID_CFLAGS := $(shell pkg-config --cflags blkid)
BLKID_LIBS := $(shell pkg-config --libs blkid)

# The broker's over-length filename fallback compresses with raw DEFLATE.
ZLIB_CFLAGS := $(shell pkg-config --cflags zlib)
ZLIB_LIBS := $(shell pkg-config --libs zlib)
GFX_CFLAGS := $(shell pkg-config --cflags cairo fontconfig)
GFX_LIBS := $(shell pkg-config --libs cairo fontconfig)
WAYLAND_CFLAGS := $(shell pkg-config --cflags wayland-client)
WAYLAND_LIBS := $(shell pkg-config --libs wayland-client)
# ACE installs into the user's own prefix, not the system's. Every ACE
# program finds its companions -- the shell, the console, the broker, the AROS
# commands -- beside its own executable, so an install is a set that has to
# stay together, and two sets on one PATH drift silently: the older one keeps
# working, just old, while the newer one never runs. Defaulting here means the
# obvious command is the correct one and needs no sudo.
#
# A system-wide install is still an ordinary PREFIX override --
# `sudo make PREFIX=/usr/local AROS_ROOT="$$HOME/aros" install`, with AROS_ROOT
# passed explicitly because sudo resets HOME -- but it is deliberately not a
# target of its own. Nothing should reach it by accident.
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
APPLICATIONSDIR ?= $(DATADIR)/applications
ICONDIR ?= $(DATADIR)/icons/hicolor/512x512/apps
POLKIT_ACTIONDIR ?= $(DATADIR)/polkit-1/actions
# What SYS: means on this host: the boot volume's root, laid out the Amiga way
# so C: really is SYS:C and S: really is SYS:S. The binaries themselves stay in
# BINDIR, where a Linux user's PATH can reach them and where every "look for
# the console beside me" lookup already resolves; SYS:C holds symlinks to them,
# so both views are true at once and neither is a copy of the other.
SYSDIR ?= $(DATADIR)/ace
INSTALL ?= install
CURL ?= curl
TAR ?= tar
SHA256SUM ?= sha256sum

COMPAT := $(CURDIR)/compat/include
BUILD := $(CURDIR)/build
# Everything that talks to the broker also has to work out where the broker
# is, and must reach the same answer the broker did.  broker_identity.c is
# that shared answer, so it travels with broker_client.o rather than being
# listed separately in sixty link lines.
BROKER_CLIENT_OBJS := $(BUILD)/broker_client.o $(BUILD)/broker-identity.o \
                      $(BUILD)/ace-modes.o $(BUILD)/ace-crm-retry.o
AROS_ROOT ?= $(HOME)/aros
# Third-party source ACE builds but does not own: Regina and Vim live here,
# each in its own directory. Both are built entirely out of tree -- every
# object lands under $(BUILD) -- so a vendored tree stays exactly as it was
# imported, and `git status` staying clean under third_party/ remains a real
# check that ACE was changed rather than the thing it is meant to run.
# See third_party/PROVENANCE.md for where each came from.
THIRD_PARTY := $(CURDIR)/third_party
VIM_SRC ?=
HOST_ARCH ?= $(shell uname -m)
ifeq ($(HOST_ARCH),aarch64)
AROS_CPU_ARCH ?= aarch64-all
else ifeq ($(HOST_ARCH),x86_64)
AROS_CPU_ARCH ?= x86_64-all
else ifeq ($(HOST_ARCH),i686)
AROS_CPU_ARCH ?= i386-all
else ifeq ($(HOST_ARCH),i386)
AROS_CPU_ARCH ?= i386-all
else ifneq (,$(filter arm%,$(HOST_ARCH)))
AROS_CPU_ARCH ?= arm-all
else
AROS_CPU_ARCH ?= x86_64-all
endif
AROS_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Echo.c
AROS_CD_SRC := src/cd.c
AROS_PATHPART_SRC := $(AROS_ROOT)/workbench/c/shellcommands/PathPart.c
AROS_WHICH_SRC := $(AROS_ROOT)/workbench/c/Which.c
AROS_FAULT_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Fault.c
AROS_ASK_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Ask.c
AROS_IF_SRC := $(AROS_ROOT)/workbench/c/shellcommands/If.c
AROS_ELSE_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Else.c
AROS_ENDIF_SRC := $(AROS_ROOT)/workbench/c/shellcommands/EndIf.c
AROS_ENDSKIP_SRC := $(AROS_ROOT)/workbench/c/shellcommands/EndSkip.c
AROS_LAB_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Lab.c
AROS_SKIP_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Skip.c
ACE_QUIT_SRC := src/quit.c
ACE_EDIT_SRC := src/edit.c
ACE_ED_SRC := src/ed_tine.c
AROS_GET_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Get.c
AROS_GETENV_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Getenv.c
AROS_SETENV_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Setenv.c
AROS_UNSETENV_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Unsetenv.c
AROS_SET_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Set.c
AROS_UNSET_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Unset.c
AROS_ALIAS_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Alias.c
AROS_UNALIAS_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Unalias.c
AROS_FAILAT_SRC := $(AROS_ROOT)/workbench/c/shellcommands/FailAt.c
AROS_WHY_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Why.c
AROS_PROMPT_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Prompt.c
INSTALL_LINUX_SRC := src/lnx.c
AROS_MAKEDIR_SRC := $(AROS_ROOT)/workbench/c/MakeDir.c
MAKELINK_SRC := src/makelink.c
AROS_JOIN_SRC := $(AROS_ROOT)/workbench/c/Join.c
AROS_EVAL_SRC := $(AROS_ROOT)/workbench/c/Eval.c
AROS_EVAL_PARSER_SRC := $(AROS_ROOT)/workbench/c/evalParser.y
AROS_ENDCLI_SRC := $(AROS_ROOT)/workbench/c/shellcommands/EndCLI.c
AROS_NEWCLI_SRC := $(AROS_ROOT)/workbench/c/shellcommands/NewCLI.c
AROS_ASSIGN_SRC := $(AROS_ROOT)/workbench/c/Assign.c
AROS_RELABEL_SRC := $(AROS_ROOT)/workbench/c/Relabel.c
AROS_TYPE_SRC := $(AROS_ROOT)/workbench/c/Type.c
AROS_RENAME_SRC := $(AROS_ROOT)/workbench/c/Rename.c
AROS_STACK_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Stack.c
AROS_DOSPATH_DIR := $(AROS_ROOT)/rom/dos
DIR_SRC := src/dir.c
AROS_DELETE_SRC := $(AROS_ROOT)/workbench/c/Delete.c
AROS_PROTECT_SRC := $(AROS_ROOT)/workbench/c/Protect.c
AROS_FILENOTE_SRC := $(AROS_ROOT)/workbench/c/Filenote.c
AROS_COPY_SRC := $(AROS_ROOT)/workbench/c/Copy.c
AROS_LIST_SRC := $(AROS_ROOT)/workbench/c/List.c
AROS_SORT_SRC := $(AROS_ROOT)/workbench/c/Sort.c
AROS_SEARCH_SRC := $(AROS_ROOT)/workbench/c/Search.c
AROS_TOUCH_SRC := $(AROS_ROOT)/workbench/c/Touch.c
AROS_BEEP_SRC := $(AROS_ROOT)/workbench/c/Beep.c
AROS_WAIT_SRC := $(AROS_ROOT)/workbench/c/Wait.c
LHA_AROS_VERSION := 1.14i.1
LHA_AROS_URL := https://github.com/sodero/lha/archive/refs/tags/$(LHA_AROS_VERSION).tar.gz
LHA_AROS_SHA256 := 6776004c56c5fcbbed746517ecf4882e6170d1e5ee553ef228f0e6ff5e4f304b
LHA_AROS_ARCHIVE := $(BUILD)/lha-aros-$(LHA_AROS_VERSION).tar.gz
LHA_AROS_DIR := $(BUILD)/lha-aros-$(LHA_AROS_VERSION)
LHA_AROS_SOURCE_STAMP := $(LHA_AROS_DIR)/.source-ready
LHA_AROS_CONFIG := $(CURDIR)/config/lha-aros/config.h
LHA_AROS_NAMES := append bitio crcio dhuf extract getopt_long header huf indicator \
                  larc lhadd lharc lhext lhlist maketbl maketree patmatch \
                  pm2 pm2hist pm2tree shuf slide support_utf8 util
LHA_AROS_OBJS := $(addprefix $(BUILD)/lha-aros-,$(addsuffix .o,$(LHA_AROS_NAMES)))
LHA_AROS_INCLUDES := -I$(CURDIR)/config/lha-aros -I$(LHA_AROS_DIR) \
                     -I$(LHA_AROS_DIR)/src -Isrc \
                     -I$(CURDIR)/compat/aros-real/include -I$(COMPAT) \
                     -I$(AROS_ROOT)/arch/all-pc/include \
                     -I$(AROS_ROOT)/arch/$(AROS_CPU_ARCH)/include \
                     -I$(AROS_ROOT)/compiler/arossupport/include \
                     -I$(AROS_ROOT)/compiler/include
LHA_AROS_CFLAGS := -D_AMIGA -D__AROS__ -DEXPAND_WILDCARDS \
                   -DHAVE_CONFIG_H -D_XOPEN_SOURCE=700 \
                   -Wno-return-mismatch -Wno-unused-parameter \
                   -Wno-pointer-sign -Wno-unused-variable \
                   -Wno-unused-but-set-variable \
                   -Wno-implicit-function-declaration \
                   -Wno-int-conversion -Wno-int-to-pointer-cast \
                   -Wno-sign-compare -Wno-missing-field-initializers \
                   -Wno-strict-aliasing -Wno-maybe-uninitialized \
                   -Wno-format -Wno-unused-function \
                   -Wno-implicit-fallthrough \
                   -Wno-dangling-pointer \
                   -Wno-parentheses \
                   -Wno-stringop-truncation \
                   -include dos/dos.h -include ace_amiga_posix.h \
                   -Dfopen=ace_amiga_posix_fopen \
                   -Dopen=ace_amiga_posix_open \
                   -Dmkstemp=ace_amiga_posix_mkstemp \
                   -Dstat=ace_amiga_posix_stat \
                   -Dlstat=ace_amiga_posix_lstat \
                   -Daccess=ace_amiga_posix_access \
                   -Dmkdir=ace_amiga_posix_mkdir \
                   -Dopendir=ace_amiga_posix_opendir \
                   -Drename=ace_amiga_posix_rename \
                   -Dunlink=ace_amiga_posix_unlink \
                   -Dremove=ace_amiga_posix_remove \
                   -Drmdir=ace_amiga_posix_rmdir \
                   -Dchmod=ace_amiga_posix_chmod \
                   -Dutime=ace_amiga_posix_utime \
                   -Dutimes=ace_amiga_posix_utimes \
                   -Dsymlink=ace_amiga_posix_symlink \
                   -Dreadlink=ace_amiga_posix_readlink
# Regina, the Rexx interpreter, built from the AROS contrib tree.
#
# The source is deliberately not vendored here: it is a pristine sparse
# checkout of aros-contrib, kept clean, so that what this Makefile proves is
# that ACE implements the contract Regina expects rather than that Regina was
# cut down until it fitted.  docs/regina-amiga-port.md has the clone command;
# AROS_CONTRIB_ROOT overrides where it lives.
AROS_CONTRIB_ROOT ?= $(HOME)/stash/aros-contrib
REGINA_VENDOR_SRC := $(THIRD_PARTY)/regina
# The vendored tree wins when it is there; the external sparse checkout is
# still honoured so a working copy of aros-contrib can be built against
# directly, which is how a vendored tree gets refreshed in the first place.
# os_amiga.c is the marker file because it is the one source that only this
# port has, so a directory merely named "regina" is never mistaken for one.
REGINA_SRC ?= $(if $(wildcard $(REGINA_VENDOR_SRC)/os_amiga.c),$(REGINA_VENDOR_SRC),$(AROS_CONTRIB_ROOT)/regina)
# The file list is regina/mmakefile.src's `rexx` target verbatim: its OFILES,
# plus the three the target adds.  nosaa and mt_notmt are what make this the
# standalone interpreter rather than the shared library.
REGINA_NAMES := funcs builtin error variable interprt debug dbgfuncs \
                memory parsing files misc unxfuncs cmsfuncs os2funcs \
                shell rexxext stack tracing interp cmath convert \
                strings library strmath signals macros envir expr \
                instore yaccsrc lexsrc wrappers options \
                rexxbif arxfuncs amifuncs os_amiga \
                rexx nosaa mt_notmt
REGINA_OBJS := $(addprefix $(BUILD)/regina-,$(addsuffix .o,$(REGINA_NAMES)))
# regina.ver is the upstream version file, sourced rather than duplicated so
# `rexx -v` cannot drift from the source it was built from.
REGINA_VER_FILE := $(REGINA_SRC)/regina.ver
regina_ver = $(shell sed -n 's/^$(1)=//p' $(REGINA_VER_FILE) 2>/dev/null | tr -d '"')
REGINA_VER_DATE := $(call regina_ver,VER_DATE)
REGINA_VER_MAJOR := $(call regina_ver,VER_MAJOR)
REGINA_VER_MINOR := $(call regina_ver,VER_MINOR)
REGINA_VER_SUPP := $(call regina_ver,VER_SUPP)
# compat/aros-real/include must come before compat/include: amifuncs.c needs
# the real exec/execbase.h and exec/lists.h, and the older shadowing copies
# under compat/include/exec/ make it fail.  Reversing the two breaks
# os_amiga.c instead, which is what compat/regina/include exists to repair --
# see the long comment in ace_regina_compat.h.  -I$(REGINA_SRC) comes first so
# Regina's own headers win over anything with the same name.
REGINA_INCLUDES := -I$(REGINA_SRC) \
                   -I$(CURDIR)/compat/aros-real/include -I$(COMPAT) \
                   -I$(CURDIR)/compat/regina/include -Isrc \
                   -I$(AROS_ROOT)/arch/all-pc/include \
                   -I$(AROS_ROOT)/arch/$(AROS_CPU_ARCH)/include \
                   -I$(AROS_ROOT)/compiler/arossupport/include \
                   -I$(AROS_ROOT)/compiler/include
# -Uunix -U__unix__ -U__unix is a correctness flag, not tidiness.  mt_notmt.c
# picks its OS_Dep_funcs from an #if/#elif chain whose unix branch comes
# before its Amiga one, and gcc on a Linux host predefines all three.  Without
# these, Regina quietly links against __regina_OS_Unx and the build is the
# wrong port -- it compiles, links, and runs.  The mt_notmt.o rule below
# asserts the outcome rather than trusting the flags to stay here.
#
# -w rather than a list of -Wno-: this is unmodified third-party source and
# the warnings are not ours to fix.  It also neutralises the -Werror in
# CFLAGS, which is the point.
# -w is not enough on its own: gcc 14 promotes implicit declarations, implicit
# int, and the pointer/int conversions from warnings to errors by default, and
# -w does not demote an error.  Regina is 2009 C and trips all of them, so each
# is named.  They are relaxations for third-party source only; nothing in src/
# is built this way.
REGINA_CFLAGS := -std=gnu99 -w \
                 -Wno-implicit-function-declaration -Wno-implicit-int \
                 -Wno-int-conversion -Wno-incompatible-pointer-types \
                 -Wno-return-mismatch \
                 -Uunix -U__unix__ -U__unix \
                 -D__regina_arexx_import=ace_regina_arexx_import \
                 -D__AROS__ -D_GNU_SOURCE -DNO_EXTERNAL_QUEUES -DAPIENTRY= \
                 -DREGINA_VERSION_DATE='"$(REGINA_VER_DATE)"' \
                 -DREGINA_VERSION_MAJOR='"$(REGINA_VER_MAJOR)"' \
                 -DREGINA_VERSION_MINOR='"$(REGINA_VER_MINOR)"' \
                 -DREGINA_VERSION_SUPP='"$(REGINA_VER_SUPP)"' \
                 -include ace_regina_compat.h

AROS_DOSPAT_DIR := $(AROS_ROOT)/rom/dos
AROS_DOSPAT_NAMES := patternmatching matchpattern parsepattern \
                     matchpatternnocase parsepatternnocase \
                     exall matchfirst matchnext matchend match_misc
AROS_DOSPAT_OBJS := $(addprefix $(BUILD)/aros-dos-,$(addsuffix .o,$(AROS_DOSPAT_NAMES)))
# -funsigned-char is a correctness flag here, not a warning relaxation. Real
# AROS declares STRPTR as UBYTE*, so a parsed pattern's tokens -- P_ANY and
# friends, 0x80..0x88 in dos/dosasl.h -- read back as the values
# patternmatching.c switches on. ACE's compat header declares STRPTR as plain
# char*, which is unsigned on ARM but signed on x86, and on a signed-char host
# those tokens read back negative and every one of those case labels becomes
# unreachable: the pattern matcher silently stops recognising "#?" at all.
# gcc catches it as -Werror=switch-outside-range. Making plain char unsigned
# for these translation units restores the type AROS wrote them against.
AROS_DOSPAT_CFLAGS := -funsigned-char \
                      -Wno-implicit-function-declaration \
                      -Wno-int-conversion -Wno-int-to-pointer-cast \
                      -Wno-sign-compare -Wno-missing-field-initializers \
                      -Wno-unused-but-set-variable -Wno-strict-aliasing \
                      -Wno-maybe-uninitialized \
                      -include ace_dos_intern.h
# AROS declares a command's arguments with the AROS_SHn macros, and AROS's own
# compiler/include/aros/shcommands.h is what expands them -- into the
# AllocDosObject(DOS_RDARGS)/ReadArgs()/FreeArgs() sequence in
# shcommands_notembedded.h. Compiling that header is what puts these commands
# on the real argument parser; ACE used to restate it, and the restatement
# parsed the templates itself.
#
# -I$(COMPAT) stays ahead of the AROS include directory in every rule below,
# so ACE's compat headers win wherever both trees have one and AROS's are
# reached only for what compat does not provide. ace_shcommand_host.h
# supplies the host process entry point the macro expansion assumes.
AROS_SHCOMMAND_CFLAGS := -I$(AROS_ROOT)/compiler/include \
                         -include ace_shcommand_host.h
AROS_SHCOMMAND_INCLUDES := -I$(AROS_ROOT)/workbench/c/shellcommands

AROS_BOOPSI_DIR := $(AROS_ROOT)/rom/intuition
AROS_ALIB_DIR := $(AROS_ROOT)/compiler/alib
AROS_BOOPSI_NAMES := rootclass makeclass freeclass addclass removeclass \
                     findclass newobjecta disposeobject setattrsa getattr \
                     nextobject
AROS_ALIB_NAMES := domethod dosupermethod coercemethod alib_util
AROS_BOOPSI_OBJS := $(addprefix $(BUILD)/aros-boopsi-,$(addsuffix .o,$(AROS_BOOPSI_NAMES))) \
                    $(BUILD)/aros-exec-memory.o \
                    $(addprefix $(BUILD)/aros-alib-,$(addsuffix .o,$(AROS_ALIB_NAMES)))
AROS_GRAPHICS_DIR := $(AROS_ROOT)/rom/devs/console
AROS_GRAPHICS_NAMES := stdconclass consoleclass support
AROS_GRAPHICS_OBJS := $(addprefix $(BUILD)/aros-console-,$(addsuffix .o,$(AROS_GRAPHICS_NAMES)))
AROS_ARSUPPORT_DIR := $(AROS_ROOT)/compiler/arossupport
AROS_ARSUPPORT_NAMES := libfindtagitem libnexttagitem
AROS_ARSUPPORT_OBJS := $(addprefix $(BUILD)/aros-arsupport-,$(addsuffix .o,$(AROS_ARSUPPORT_NAMES)))
AROS_GRAPHICS_INCLUDES := -I$(CURDIR)/compat/aros-real/include \
                          -I$(AROS_ROOT)/arch/all-pc/include \
                          -I$(AROS_ROOT)/arch/$(AROS_CPU_ARCH)/include \
                          -I$(AROS_ROOT)/compiler/arossupport/include \
                          -I$(AROS_ROOT)/compiler/include \
                          -I$(AROS_ROOT)/rom/devs/console/include \
                          -I$(AROS_ROOT)/rom/devs/console
AROS_GRAPHICS_CFLAGS := -Wno-implicit-function-declaration \
                        -Wno-int-conversion -Wno-int-to-pointer-cast \
                        -Wno-pointer-sign -Wno-sign-compare \
                        -Wno-missing-field-initializers \
                        -Wno-unused-but-set-variable -Wno-strict-aliasing \
                        -Wno-maybe-uninitialized -Wno-unused-variable \
                        -include ace_graphics_intern.h
AROS_CON_HANDLER_SRC := $(AROS_ROOT)/rom/filesys/console_handler/con_handler.c
AROS_CON_SUPPORT_SRC := $(AROS_ROOT)/rom/filesys/console_handler/support.c
AROS_CON_COMPLETION_SRC := $(AROS_ROOT)/rom/filesys/console_handler/completion.c
AROS_SHELL_NAMES := buffer cliEcho cliLen cliNan cliPrompt cliVarNum \
                    convertArg convertBackTicks convertLine convertLineDot \
                    convertRedir convertVar interpreter readLine redirection
AROS_SHELL_OBJS := $(addprefix $(BUILD)/aros-shell-,$(addsuffix .o,$(AROS_SHELL_NAMES)))
AROS_REAL_INCLUDES := -I$(CURDIR)/compat/aros-real/include \
                      -I$(AROS_ROOT)/arch/all-pc/include \
                      -I$(AROS_ROOT)/arch/$(AROS_CPU_ARCH)/include \
                      -I$(AROS_ROOT)/compiler/arossupport/include \
                      -I$(AROS_ROOT)/compiler/include \
                      -I$(AROS_ROOT)/rom/devs/console \
                      -I$(AROS_ROOT)/rom/filesys/console_handler
AROS_REAL_CFLAGS := -Wno-implicit-function-declaration \
                    -Wno-int-conversion -Wno-int-to-pointer-cast \
                    -Wno-pointer-sign -Wno-sign-compare \
                    -Wno-missing-field-initializers \
                    -Wno-unused-but-set-variable -Wno-strict-aliasing \
                    -Wno-maybe-uninitialized \
                    -DACE_NO_CONSOLE_MENU -DACE_NO_CONSOLE_APPWINDOW \
                    -DACE_NO_CONSOLE_COMPLETION \
                    -include ace_handler_types.h
# The BOOPSI sources are compiled against the ACE Intuition seam instead of the
# console handler seam, so they take the same warning relaxations but their own
# forced include.  See compat/aros-real/include/ace_boopsi_intern.h.
AROS_BOOPSI_CFLAGS := -Wno-implicit-function-declaration \
                      -Wno-int-conversion -Wno-int-to-pointer-cast \
                      -Wno-pointer-sign -Wno-sign-compare \
                      -Wno-missing-field-initializers \
                      -Wno-unused-but-set-variable -Wno-strict-aliasing \
                      -Wno-maybe-uninitialized \
                      -include ace_boopsi_intern.h
AROS_BOOPSI_INCLUDES := -I$(CURDIR)/compat/aros-real/include \
                        -I$(AROS_ROOT)/arch/all-pc/include \
                        -I$(AROS_ROOT)/arch/$(AROS_CPU_ARCH)/include \
                        -I$(AROS_ROOT)/compiler/arossupport/include \
                        -I$(AROS_ROOT)/compiler/include \
                        -I$(AROS_ALIB_DIR)
# The AmigaDOS commands: what a user types at the shell, and what SYS:C is a
# drawer of. C: is the loader's last resort, so a command reachable by name
AMIGA_COMMANDS := Echo CD Path PathPart Which Dir Peek Delete Protect Filenote Fault Ask Get Getenv Set Unset Alias Unalias Beep \
                  FailAt Why Prompt Clip Cut MakeDir MakeLink Join Eval Edit Ed Info Copy List Sort Search Touch EndCLI Assign Relabel Type Rename Stack Run Linux NewCLI \
                  If Else EndIf EndSkip Lab Quit Skip Execute Setenv Unsetenv Wait Status Break Tally Shutdown Reboot LhA say
# The host side: a launcher, the console, the shell the console starts, and
# the broker with its control tool. These are entry points into ACE rather
# than commands within it, and they are not in SYS:C.
HOST_BINS := ace-shell ace-user-shell ace-console ace-broker ace-brokerctl acepaste ace-fmm
INSTALL_BINS := $(AMIGA_COMMANDS) $(HOST_BINS)

all: $(BUILD)/Echo $(BUILD)/CD $(BUILD)/Path $(BUILD)/PathPart $(BUILD)/Which $(BUILD)/Dir $(BUILD)/Peek $(BUILD)/Delete $(BUILD)/Protect $(BUILD)/Filenote $(BUILD)/Fault $(BUILD)/Ask $(BUILD)/Get $(BUILD)/Getenv $(BUILD)/Set $(BUILD)/Unset $(BUILD)/Alias $(BUILD)/Unalias $(BUILD)/Beep $(BUILD)/FailAt $(BUILD)/Why $(BUILD)/Prompt $(BUILD)/Clip $(BUILD)/Cut $(BUILD)/MakeDir $(BUILD)/MakeLink $(BUILD)/Join $(BUILD)/Eval $(BUILD)/Edit $(BUILD)/Ed $(BUILD)/Info $(BUILD)/Copy $(BUILD)/List $(BUILD)/Sort $(BUILD)/Search $(BUILD)/Touch $(BUILD)/EndCLI $(BUILD)/Assign $(BUILD)/Relabel $(BUILD)/Type $(BUILD)/Rename $(BUILD)/Stack $(BUILD)/Run $(BUILD)/Linux $(BUILD)/LhA $(BUILD)/say $(BUILD)/ace-shell $(BUILD)/ace-user-shell $(BUILD)/ace-console $(BUILD)/NewCLI $(BUILD)/If $(BUILD)/Else $(BUILD)/EndIf $(BUILD)/EndSkip $(BUILD)/Lab $(BUILD)/Quit $(BUILD)/Skip $(BUILD)/Execute $(BUILD)/Setenv $(BUILD)/Unsetenv $(BUILD)/Wait $(BUILD)/Status $(BUILD)/Break $(BUILD)/Tally $(BUILD)/Shutdown $(BUILD)/Reboot $(BUILD)/ace-broker $(BUILD)/ace-fmm $(BUILD)/ace-brokerctl $(BUILD)/acepaste $(BUILD)/ace-amiga-posix.o $(BUILD)/exec_compat.o $(BUILD)/exec_compat_bindings.o $(BUILD)/aros-con-handler.o $(BUILD)/aros-con-support.o $(BUILD)/aros-exec-runtime.o $(BUILD)/aros-console-editor.o $(BUILD)/aros-boopsi-runtime.o $(AROS_BOOPSI_OBJS)

$(BUILD)/say: data/say/say | $(BUILD)
	$(INSTALL) -m 0755 $< $@

$(BUILD)/break-probe: tests/break_probe.c $(BUILD)/dos-runtime.o $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc $(filter-out %.h,$^) -o $@

$(BUILD)/lnx-pty-probe: tests/lnx_pty_probe.c | $(BUILD)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/foreground-spoof: tests/foreground_spoof.c $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) -pthread -Isrc $(filter-out %.h,$^) -o $@

$(BUILD)/broker-task-test: tests/broker_task_test.c $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) -pthread -Isrc $(filter-out %.h,$^) -o $@

$(BUILD)/broker-port-channel-test: tests/broker_port_channel_test.c $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) -pthread -Isrc $(filter-out %.h,$^) -o $@

$(BUILD)/broker-port-message-test: tests/broker_port_message_test.c $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) -pthread -Isrc $(filter-out %.h,$^) -o $@

# Both go through with_private_broker.sh; see the comment there for why a
# broker of their own is not optional.
test-broker-port-channel: $(BUILD)/broker-port-channel-test $(BUILD)/ace-broker
	sh tests/with_private_broker.sh $(BUILD)/broker-port-channel-test

test-broker-port-message: $(BUILD)/broker-port-message-test $(BUILD)/ace-broker
	sh tests/with_private_broker.sh $(BUILD)/broker-port-message-test

$(BUILD)/broker-port-abandon-test: tests/broker_port_abandon_test.c $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) -pthread -Isrc $(filter-out %.h,$^) -o $@

test-broker-port-abandon: $(BUILD)/broker-port-abandon-test $(BUILD)/ace-broker
	sh tests/with_private_broker.sh $(BUILD)/broker-port-abandon-test

$(BUILD)/easy-request-test: tests/easy_request_test.c $(BUILD)/ace-requestor.o \
                            $(BROKER_CLIENT_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -pthread -I$(COMPAT) -Isrc \
	    $(filter-out %.h,$^) -o $@

test-easy-request: $(BUILD)/easy-request-test $(BUILD)/ace-broker
	sh tests/with_private_broker.sh $(BUILD)/easy-request-test 2>&1 | \
	    grep -F -e 'EasyRequest: EasyRequest test' \
           -e 'Body one two' -e '[Retry|Cancel]'


.PHONY: break-signal-test
break-signal-test: $(BUILD)/break-probe $(BUILD)/ace-user-shell
	python3 tests/break_signal_test.py

$(BUILD)/terminal-translate-test: tests/terminal_translate_test.c src/terminal_translate.c src/terminal_translate.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(filter-out %.h,$^) -o $@

.PHONY: test-terminal-translate
test-terminal-translate: $(BUILD)/terminal-translate-test
	$(BUILD)/terminal-translate-test

.PHONY: test-lnx-pty
test-lnx-pty: $(BUILD)/Linux $(BUILD)/lnx-pty-probe $(BUILD)/ace-user-shell \
	$(BUILD)/ace-broker $(BUILD)/EndCLI $(BUILD)/foreground-spoof
	python3 tests/lnx_pty_test.py

# ET (Edified Tine) is a guest program, so its build is deliberately separate
# from the core command graph.  Install invokes it after ACE itself is built; this
# avoids two make processes racing to build ACE's shared broker objects.
TINE_DIR ?= $(CURDIR)/tools/tine
.PHONY: tine
tine:
	$(MAKE) -C "$(TINE_DIR)" ACE_DIR="$(CURDIR)" tine

$(BUILD):
	mkdir -p $@

lha-fetch: $(LHA_AROS_SOURCE_STAMP)

lha: $(BUILD)/LhA

$(LHA_AROS_ARCHIVE): | $(BUILD)
	$(CURL) --location --fail --silent --show-error --output "$@" "$(LHA_AROS_URL)"
	printf '%s  %s\n' "$(LHA_AROS_SHA256)" "$@" | $(SHA256SUM) --check --status -

$(LHA_AROS_SOURCE_STAMP): $(LHA_AROS_ARCHIVE) | $(BUILD)
	mkdir -p "$(LHA_AROS_DIR)"
	$(TAR) --extract --gzip --file "$<" --strip-components=1 --directory "$(LHA_AROS_DIR)"
	touch "$@"

$(BUILD)/native_dos.o: src/native_dos.c src/ace_crm_retry.h src/broker_protocol.h src/broker_client.h src/aros_dos_path.h src/aros_console_editor.h src/console_channel.h src/native_console_endpoint.h src/native_host.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/ace-amiga-posix.o: src/ace_amiga_posix.c src/ace_crm_retry.h src/ace_amiga_posix.h \
                            src/broker_client.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

# The fetched source path does not exist until the stamp recipe runs, so keep
# the stamp as the Make prerequisite and name the generated .c file explicitly
# in the recipe below.
$(BUILD)/lha-aros-%.o: $(LHA_AROS_SOURCE_STAMP) $(LHA_AROS_CONFIG) \
                       src/ace_amiga_posix.h | $(BUILD)
	$(CC) $(CFLAGS) $(LHA_AROS_CFLAGS) $(LHA_AROS_INCLUDES) \
	    -c $(LHA_AROS_DIR)/src/$*.c -o $@

$(BUILD)/LhA: $(LHA_AROS_OBJS) $(BUILD)/ace-amiga-posix.o \
             $(BUILD)/dos-runtime.o $(BUILD)/native_dos.o \
             $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
             $(BROKER_CLIENT_OBJS) $(AROS_DOSPAT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/ace-vim-runtime.o: src/ace_vim_runtime.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -c $< -o $@

$(BUILD)/native_command.o: src/native_command.c src/broker_protocol.h src/ace_shell_break.h src/aros_exec_runtime.h src/lnx_pty.h src/native_host.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/ace-launcher.o: src/ace_launcher.c src/ace_modes.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/ace-modes.o: src/ace_modes.c src/ace_modes.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(BUILD)/native_shcommand.o: src/native_shcommand.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -c $< -o $@

$(BUILD)/native_process.o: src/native_process.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/rexxsyslib.o: src/rexxsyslib.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/rexxsupport.o: src/rexxsupport.c | $(BUILD)
	$(CC) $(CFLAGS) -pthread -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/rexx-port-bridge.o: src/rexx_port_bridge.c src/broker_protocol.h \
                             src/aros_exec_runtime.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/ace-requestor.o: src/ace_requestor.c src/ace_requestor_protocol.h \
                          src/assign_compat.h src/broker_client.h \
                          src/broker_protocol.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/ace-requestor-gui.o: src/ace_requestor_gui.c \
                              src/ace_requestor_gui.h \
                              src/ace_requestor_protocol.h \
                              src/broker_client.h src/broker_protocol.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(GTK_CFLAGS) -Isrc -c $< -o $@

$(BUILD)/makedir.o: $(AROS_MAKEDIR_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Dmain=ace_command_entry_main -c $< -o $@

$(BUILD)/MakeLink.o: $(MAKELINK_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Dmain=ace_command_entry_main -c $< -o $@

$(BUILD)/Join.o: $(AROS_JOIN_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Dmain=ace_command_entry_main -c $< -o $@

# Edit has no AROS source to fetch: it is written here, to dos.library and
# exec.library alone, so the one file also builds with an Amiga or AROS
# compiler. On ACE it enters through the same seam as any command that keeps
# its own main() and calls ReadArgs() itself.
$(BUILD)/Edit.o: $(ACE_EDIT_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Dmain=ace_command_entry_main -c $< -o $@

# bison writes both files in one run, so they are a grouped target (&:).
# Listing only the .c leaves the .h with no rule at all, which a parallel
# build reports as "No rule to make target" the moment it schedules Eval.o
# before the .c has been made.  A serial build hid this by happening to make
# the .c first, and a tree carrying a pre-generated .h hid it completely.
$(BUILD)/evalParser.tab.c $(BUILD)/evalParser.tab.h &: $(AROS_EVAL_PARSER_SRC) | $(BUILD)
	bison --defines=$(BUILD)/evalParser.tab.h -o $(BUILD)/evalParser.tab.c $<

$(BUILD)/Eval.o: $(AROS_EVAL_SRC) $(BUILD)/evalParser.tab.c $(BUILD)/evalParser.tab.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -I$(BUILD) -include strings.h -include stdlib.h \
	    -Dmalloc=ace_eval_malloc -Dfree=ace_eval_free \
	    -Dmain=ace_command_entry_main -c $< -o $@

# Copy predates the AROS_SHn command-entry macros and enters through an
# AROS process-start function. Its fetched source stays unmodified: expose
# that function from this translation unit and call it from src/copy_entry.c.
$(BUILD)/Copy.o: $(AROS_COPY_SRC) compat/include/dos/dosextens.h | $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-variable \
	    -Wno-unused-but-set-variable -Wno-maybe-uninitialized \
	    -Wno-implicit-function-declaration -Wno-int-conversion \
	    -Wno-int-to-pointer-cast -Wno-sign-compare \
	    -I$(COMPAT) -I$(AROS_ROOT)/compiler/include -D__AROS__ -Dstatic= \
	    -DGetDeviceProc=ace_aros_GetDeviceProc \
	    -DFreeDeviceProc=ace_aros_FreeDeviceProc \
	    -include dos/dosextens.h -c $< -o $@

$(BUILD)/copy_entry.o: src/copy_entry.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -c $< -o $@

$(BUILD)/List.o: $(AROS_LIST_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -Wno-compare-distinct-pointer-types \
	    -I$(COMPAT) -I$(AROS_ROOT)/compiler/include \
	    -Dmain=ace_command_entry_main -c $< -o $@

$(BUILD)/Sort.o: $(AROS_SORT_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -Wno-compare-distinct-pointer-types -Wno-sign-compare \
	    -I$(COMPAT) -I$(AROS_ROOT)/compiler/include \
	    -Dmain=ace_command_entry_main -c $< -o $@

$(BUILD)/Search.o: $(AROS_SEARCH_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -Wno-compare-distinct-pointer-types -Wno-sign-compare \
	    -I$(COMPAT) -I$(AROS_ROOT)/compiler/include \
	    -Dmain=ace_command_entry_main -c $< -o $@

$(BUILD)/Touch.o: $(AROS_TOUCH_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Dmain=ace_command_entry_main -c $< -o $@

$(BUILD)/Wait.o: $(AROS_WAIT_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -I$(AROS_ROOT)/arch/all-pc/include \
	    -I$(AROS_ROOT)/arch/$(AROS_CPU_ARCH)/include \
	    -I$(AROS_ROOT)/compiler/include \
	    -DTICKS_PER_SECOND=50 -include dos/datetime.h \
	    -include aros_exec_runtime.h -Dmain=ace_command_entry_main -c $< -o $@

$(BUILD)/status.o: src/status.c src/broker_client.h src/broker_protocol.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/break.o: src/break.c src/broker_client.h src/broker_protocol.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/Beep.o: $(AROS_BEEP_SRC) src/assign_compat.h \
                 compat/include/intuition/intuitionbase.h | $(BUILD)
	$(CC) $(CFLAGS) -D__AROS__ -I$(COMPAT) \
	    -I$(AROS_ROOT)/compiler/include -include src/assign_compat.h \
	    -c $< -o $@

$(BUILD)/beep_entry.o: src/beep_entry.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -c $< -o $@

$(BUILD)/wayward_beep.o: src/wayward_beep.c src/wayward_beep.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/native_command_entry.o: src/native_command_entry.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -c $< -o $@

# Protect refuses to change a whole volume or device rather than an object
# inside one, and asks AROS's own arossupport routine which it is.
$(BUILD)/aros-arsupport-isdosentrya.o: $(AROS_ARSUPPORT_DIR)/isdosentrya.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -include proto/exec.h -include src/assign_compat.h \
	    -Wno-implicit-function-declaration -c $< -o $@

# Delete and Protect declare their arguments by calling ReadArgs() directly
# rather than through the AROS_SHn macros, so they take the generic host entry
# point in src/native_command_entry.c. Both drive the real AROS pattern
# matcher Dir already uses.
$(BUILD)/Delete.o: $(AROS_DELETE_SRC) compat/include/ace_dos_intern.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_DOSPAT_CFLAGS) -I$(COMPAT) \
	    -Dmain=ace_command_entry_main -c $< -o $@

$(BUILD)/Protect.o: $(AROS_PROTECT_SRC) compat/include/ace_dos_intern.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_DOSPAT_CFLAGS) -I$(COMPAT) \
	    -Dmain=ace_command_entry_main -c $< -o $@

# -D__AROS__ is what lets Filenote.c call IsDosEntryA(). Without it the file
# defines the call away to 0 for a non-AROS build, and Filenote would happily
# try to comment a whole volume.
$(BUILD)/Filenote.o: $(AROS_FILENOTE_SRC) compat/include/ace_dos_intern.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_DOSPAT_CFLAGS) -I$(COMPAT) -D__AROS__ \
	    -Dmain=ace_command_entry_main -c $< -o $@

$(BUILD)/endcli.o: $(AROS_ENDCLI_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

AROS_ASSIGN_CFLAGS := -Wno-implicit-function-declaration \
                      -Wno-int-conversion -Wno-int-to-pointer-cast \
                      -Wno-pointer-sign -Wno-sign-compare \
                      -Wno-missing-field-initializers -Wno-strict-aliasing \
                      -Wno-unused-but-set-variable
AROS_ASSIGN_INCLUDES := -I$(CURDIR)/compat/aros-real/include -I$(COMPAT) \
                        -I$(AROS_ROOT)/compiler/include -Isrc

AROS_DOSPATH_CFLAGS := -Wno-implicit-function-declaration \
                       -Wno-int-conversion -Wno-int-to-pointer-cast \
                       -Wno-pointer-sign -Wno-unused-variable \
                       -Wno-sign-compare -Wno-missing-field-initializers \
                       -Wno-unused-but-set-variable -Wno-strict-aliasing
AROS_DOSPATH_INCLUDES := -I$(COMPAT) -Isrc
DOS_RUNTIME_OBJ := $(BUILD)/dos-runtime.o


$(BUILD)/Assign.o: $(AROS_ASSIGN_SRC) src/assign_compat.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_ASSIGN_CFLAGS) -D__AROS__ $(AROS_ASSIGN_INCLUDES) \
	    -include src/assign_compat.h -c $< -o $@

$(BUILD)/assign_entry.o: src/assign_entry.c src/broker_client.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/assign_compat.o: src/assign_compat.c src/assign_compat.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/aros-dos-getdeviceproc.o: $(AROS_DOSPATH_DIR)/getdeviceproc.c compat/include/ace_dos_path_intern.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_DOSPATH_CFLAGS) $(AROS_DOSPATH_INCLUDES) \
	    -DGetDeviceProc=ace_aros_GetDeviceProc -include ace_dos_path_intern.h \
	    -c $< -o $@

$(BUILD)/aros-dos-freedeviceproc.o: $(AROS_DOSPATH_DIR)/freedeviceproc.c compat/include/ace_dos_path_intern.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_DOSPATH_CFLAGS) $(AROS_DOSPATH_INCLUDES) \
	    -DFreeDeviceProc=ace_aros_FreeDeviceProc -include ace_dos_path_intern.h \
	    -c $< -o $@

$(BUILD)/aros-dos-readargs.o: $(AROS_DOSPATH_DIR)/readargs.c compat/include/ace_dos_path_intern.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_DOSPATH_CFLAGS) $(AROS_DOSPATH_INCLUDES) \
	    -DReadArgs=ace_aros_ReadArgs -DFreeArgs=ace_aros_FreeArgs \
	    -DFindArg=ace_aros_FindArg -DStrToLong=ace_aros_StrToLong \
	    -include ace_dos_path_intern.h -c $< -o $@

$(BUILD)/aros-dos-readitem.o: $(AROS_DOSPATH_DIR)/readitem.c compat/include/ace_dos_path_intern.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_DOSPATH_CFLAGS) $(AROS_DOSPATH_INCLUDES) \
	    -include ace_dos_path_intern.h \
	    -c $< -o $@

$(BUILD)/aros-dos-freeargs.o: $(AROS_DOSPATH_DIR)/freeargs.c compat/include/ace_dos_path_intern.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_DOSPATH_CFLAGS) $(AROS_DOSPATH_INCLUDES) \
	    -DFreeArgs=ace_aros_FreeArgs -include ace_dos_path_intern.h \
	    -c $< -o $@

$(BUILD)/aros-dos-findarg.o: $(AROS_DOSPATH_DIR)/findarg.c compat/include/ace_dos_path_intern.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_DOSPATH_CFLAGS) $(AROS_DOSPATH_INCLUDES) \
	    -DFindArg=ace_aros_FindArg \
	    -include ace_dos_path_intern.h \
	    -c $< -o $@

$(BUILD)/aros-dos-strtolong.o: $(AROS_DOSPATH_DIR)/strtolong.c compat/include/ace_dos_path_intern.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_DOSPATH_CFLAGS) $(AROS_DOSPATH_INCLUDES) \
	    -DStrToLong=ace_aros_StrToLong -include ace_dos_path_intern.h \
	    -c $< -o $@

$(DOS_RUNTIME_OBJ): $(BUILD)/assign_compat.o \
                    $(BUILD)/aros-dos-getdeviceproc.o \
                    $(BUILD)/aros-dos-freedeviceproc.o \
                    $(BUILD)/aros-dos-readargs.o \
                    $(BUILD)/aros-dos-readitem.o \
                    $(BUILD)/aros-dos-freeargs.o \
                    $(BUILD)/aros-dos-findarg.o \
                    $(BUILD)/aros-dos-strtolong.o \
                    $(BUILD)/aros-console-editor.o \
                    $(BUILD)/aros-con-support.o \
                    $(BUILD)/aros-exec-runtime.o \
                    $(BUILD)/clipboard-bridge.o \
                    $(BUILD)/notify-compat.o \
                    $(BUILD)/clipboard-device.o \
                    $(BUILD)/iffparse-clipboard.o \
                    $(BUILD)/native-list-compat.o \
                    $(BUILD)/ace-vim-runtime.o \
                    $(BUILD)/console_channel.o \
                    $(BUILD)/console_device.o $(BUILD)/con_handler.o \
                    $(BUILD)/native_console_endpoint.o | $(BUILD)
	$(CC) $(CFLAGS) -r $(filter-out %.h,$^) -o $@

$(BUILD)/native-list-compat.o: src/native_list_compat.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -c $< -o $@

$(BUILD)/Assign: $(BUILD)/Assign.o $(BUILD)/assign_entry.o \
                 $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
                 $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
                 $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Relabel.o: $(AROS_RELABEL_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -include ace_dos_path_intern.h \
	    -Dmain=ace_command_entry_main -c $< -o $@

$(BUILD)/Relabel: $(BUILD)/Relabel.o $(BUILD)/native_command_entry.o \
                  $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
                  $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
                  $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Info.o: src/info.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -Dmain=ace_command_entry_main \
	    -c $< -o $@

$(BUILD)/Info: $(BUILD)/Info.o $(BUILD)/native_command_entry.o \
               $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
               $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
               $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Linux.o: $(INSTALL_LINUX_SRC) src/lnx_pty.h src/terminal_translate.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

# The xterm/Amiga console vocabulary, kept out of the PTY supervisor so it
# can be exercised without a terminal.
$(BUILD)/terminal_translate.o: src/terminal_translate.c src/terminal_translate.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

# The one place in ACE that decides an operation needs privilege.  Every
# command links it, and none of them contains that decision themselves.
$(BUILD)/ace-crm-retry.o: src/ace_crm_retry.c src/ace_crm_retry.h src/broker_client.h src/ace_modes.h src/ace_privilege_protocol.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(BUILD)/broker_client.o: src/broker_client.c src/broker_protocol.h src/broker_client.h src/ace_modes.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

# The FMM is the only ACE program that is meant to run as root, so it is
# also the only one that links none of the DOS runtime, none of the broker
# client, and nothing that knows what an Amiga path is.  Its whole dependency
# list is the protocol header, and that is a property worth being able to see
# at a glance in the build rules.
$(BUILD)/ace-fmm.o: src/ace_fmm.c src/ace_privilege_protocol.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(BUILD)/ace-fmm-volume.o: src/ace_fmm_volume.c src/ace_fmm_volume.h src/ace_privilege_protocol.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(BUILD)/ace-crm.o: src/ace_crm.c src/ace_crm.h src/ace_privilege_protocol.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(BUILD)/ace-fmm: $(BUILD)/ace-fmm.o $(BUILD)/ace-fmm-volume.o $(BUILD)/ace-crm.o
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

# The broker's end of that channel.  Only the broker links it: the shell and
# the commands do not get a privileged socket of their own.
$(BUILD)/ace-fmm-client.o: src/ace_fmm_client.c src/ace_fmm_client.h src/ace_privilege_protocol.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(BUILD)/fmm-crm-channel-probe: tests/fmm_crm_channel_probe.c $(BUILD)/ace-fmm-client.o
	$(CC) $(CFLAGS) -Isrc $(filter-out %.h,$^) -o $@

# SYS: is established from ACE_SYS_DIR, which CFLAGS now carries for every
# object.  An uninstalled build finds neither that path nor an ACE_SYS_DIR in
# its environment and falls back to its own directory, which for a build tree
# is where the commands are -- which is exactly what gives a build tree a
# different SYS:, and therefore a different broker, from an installed copy.
$(BUILD)/broker.o: src/broker.c src/broker_dictionary.h src/broker_protocol.h src/dos_devices.h src/clipboard_bridge.h src/ace_modes.h src/ace_fmm_client.h | $(BUILD)
	$(CC) $(CFLAGS) $(BLKID_CFLAGS) $(ZLIB_CFLAGS) -Isrc -c $< -o $@

$(BUILD)/dos-devices.o: src/dos_devices.c src/dos_devices.h src/ace_modes.h src/ace_fmm_client.h | $(BUILD)
	$(CC) $(CFLAGS) $(BLKID_CFLAGS) -Isrc -c $< -o $@

$(BUILD)/broker-identity.o: src/broker_identity.c src/broker_protocol.h src/ace_modes.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(BUILD)/brokerctl.o: src/brokerctl.c src/broker_protocol.h src/broker_client.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(BUILD)/amiga_console.o: src/amiga_console.c src/console_channel.h src/console_dispatch.h src/console_device_bridge.h src/ace_appmenu_wayland.h compat/include/libraries/iffparse.h compat/include/proto/iffparse.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(GTK_CFLAGS) $(GFX_CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/console_channel.o: src/console_channel.c src/console_channel.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(BUILD)/console_dispatch.o: src/console_dispatch.c src/console_dispatch.h | $(BUILD)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -Isrc -c $< -o $@

$(BUILD)/ace-appmenu-wayland.o: src/ace_appmenu_wayland.c src/ace_appmenu_wayland.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(GTK_CFLAGS) $(WAYLAND_CFLAGS) -Isrc -c $< -o $@

$(BUILD)/console_device_bridge.o: src/console_device_bridge.c src/console_device_bridge.h compat/aros-real/include/ace_graphics_intern.h | $(BUILD)
	$(CC) $(CFLAGS) $(GFX_CFLAGS) $(AROS_GRAPHICS_CFLAGS) $(AROS_GRAPHICS_INCLUDES) -Isrc -c $< -o $@

$(BUILD)/console_device.o: src/console_device.c | $(BUILD)
	$(CC) $(CFLAGS) -pthread -Isrc -c $< -o $@

$(BUILD)/con_handler.o: src/con_handler.c | $(BUILD)
	$(CC) $(CFLAGS) -pthread -Isrc -c $< -o $@

$(BUILD)/native_console_endpoint.o: src/native_console_endpoint.c \
                                    src/native_console_endpoint.h \
                                    src/con_handler.h src/console_device.h \
                                    src/console_channel.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread -Isrc -c $< -o $@

$(BUILD)/aros-con-handler.o: $(AROS_CON_HANDLER_SRC) compat/aros-real/include/ace_handler_types.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_REAL_CFLAGS) $(AROS_REAL_INCLUDES) -c $< -o $@

$(BUILD)/aros-con-support.o: $(AROS_CON_SUPPORT_SRC) compat/aros-real/include/ace_handler_types.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_REAL_CFLAGS) $(AROS_REAL_INCLUDES) -c $< -o $@

$(BUILD)/aros-con-completion.o: $(AROS_CON_COMPLETION_SRC) compat/aros-real/include/ace_handler_types.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_REAL_CFLAGS) $(AROS_REAL_INCLUDES) -c $< -o $@

$(BUILD)/aros-boopsi-%.o: $(AROS_BOOPSI_DIR)/%.c compat/aros-real/include/ace_boopsi_intern.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_BOOPSI_CFLAGS) $(AROS_BOOPSI_INCLUDES) -c $< -o $@

$(BUILD)/aros-alib-%.o: $(AROS_ALIB_DIR)/%.c compat/aros-real/include/ace_boopsi_intern.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_BOOPSI_CFLAGS) $(AROS_BOOPSI_INCLUDES) -c $< -o $@

$(BUILD)/aros-boopsi-runtime.o: src/aros_boopsi_runtime.c src/aros_boopsi_runtime.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(AROS_BOOPSI_CFLAGS) $(AROS_BOOPSI_INCLUDES) -c $< -o $@

$(BUILD)/boopsi-test.o: tests/boopsi_test.c src/aros_boopsi_runtime.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(AROS_BOOPSI_CFLAGS) $(AROS_BOOPSI_INCLUDES) -Isrc -c $< -o $@

$(BUILD)/boopsi-test: $(BUILD)/boopsi-test.o $(BUILD)/aros-boopsi-runtime.o $(AROS_BOOPSI_OBJS)
	$(CC) $(CFLAGS) -pthread $(filter-out %.h,$^) -o $@

$(BUILD)/aros-console-%.o: $(AROS_GRAPHICS_DIR)/%.c compat/aros-real/include/ace_graphics_intern.h compat/aros-real/include/proto/exec.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_GRAPHICS_CFLAGS) $(AROS_GRAPHICS_INCLUDES) -c $< -o $@

$(BUILD)/aros-arsupport-%.o: $(AROS_ARSUPPORT_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_GRAPHICS_CFLAGS) $(AROS_GRAPHICS_INCLUDES) -c $< -o $@

$(BUILD)/aros-graphics-runtime.o: src/aros_graphics_runtime.c src/aros_graphics_runtime.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(GFX_CFLAGS) $(AROS_GRAPHICS_CFLAGS) $(AROS_GRAPHICS_INCLUDES) -c $< -o $@

$(BUILD)/graphics-test.o: tests/graphics_test.c src/aros_graphics_runtime.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(GFX_CFLAGS) $(AROS_GRAPHICS_CFLAGS) $(AROS_GRAPHICS_INCLUDES) -Isrc -c $< -o $@

$(BUILD)/console-device-bridge-test.o: tests/console_device_bridge_test.c src/console_device_bridge.h src/aros_graphics_runtime.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(GFX_CFLAGS) $(AROS_GRAPHICS_CFLAGS) $(AROS_GRAPHICS_INCLUDES) -Isrc -c $< -o $@

$(BUILD)/graphics-test: $(BUILD)/graphics-test.o $(BUILD)/aros-graphics-runtime.o \
                        $(AROS_GRAPHICS_OBJS) $(AROS_ARSUPPORT_OBJS) \
                        $(AROS_BOOPSI_OBJS) $(BUILD)/aros-boopsi-runtime.o
	$(CC) $(CFLAGS) -pthread $(filter-out %.h,$^) $(GFX_LIBS) -o $@

$(BUILD)/console-device-bridge-test: $(BUILD)/console-device-bridge-test.o $(BUILD)/console_device_bridge.o \
                                     $(BUILD)/aros-graphics-runtime.o $(AROS_GRAPHICS_OBJS) \
                                     $(AROS_ARSUPPORT_OBJS) $(AROS_BOOPSI_OBJS) \
                                     $(BUILD)/aros-boopsi-runtime.o
	$(CC) $(CFLAGS) -pthread $(filter-out %.h,$^) $(GFX_LIBS) -o $@

$(BUILD)/console-channel-test.o: tests/console_channel_test.c src/console_channel.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(BUILD)/console-channel-test: $(BUILD)/console-channel-test.o $(BUILD)/console_channel.o
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/console-dispatch-test.o: tests/console_dispatch_test.c src/console_dispatch.h | $(BUILD)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -Isrc -c $< -o $@

$(BUILD)/console-dispatch-test: $(BUILD)/console-dispatch-test.o $(BUILD)/console_dispatch.o
	$(CC) $(CFLAGS) $(filter-out %.h,$^) $(GTK_LIBS) -o $@

$(BUILD)/aros-exec-runtime.o: src/aros_exec_runtime.c src/aros_exec_runtime.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(AROS_REAL_CFLAGS) $(AROS_REAL_INCLUDES) -c $< -o $@

$(BUILD)/clipboard-bridge.o: src/clipboard_bridge.c src/clipboard_bridge.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread -Isrc -c $< -o $@

$(BUILD)/notify-compat.o: src/notify_compat.c src/aros_exec_runtime.h \
                          src/clipboard_bridge.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/clipboard-device.o: src/clipboard_device.c src/clipboard_device.h src/clipboard_bridge.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/iffparse-clipboard.o: src/iffparse_clipboard.c src/aros_exec_runtime.h | $(BUILD)
	$(CC) $(CFLAGS) -pthread -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/aros-console-editor.o: src/aros_console_editor.c src/aros_console_editor.h $(AROS_CON_SUPPORT_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(AROS_REAL_CFLAGS) $(AROS_REAL_INCLUDES) -c $< -o $@

$(BUILD)/aros-console-editor-stubs.o: src/aros_console_editor_stubs.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -c $< -o $@

$(BUILD)/exec_compat.o: src/exec_compat.c | $(BUILD)
	$(CC) $(CFLAGS) -pthread -Isrc -c $< -o $@

$(BUILD)/exec_compat_bindings.o: src/exec_compat_bindings.c | $(BUILD)
	$(CC) $(CFLAGS) -pthread -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/console-device-test: tests/console_device_test.c $(BUILD)/console_device.o $(BUILD)/con_handler.o
	$(CC) $(CFLAGS) -pthread -Isrc $(filter-out %.h,$^) -o $@

$(BUILD)/aros-exec-runtime-test: tests/aros_exec_runtime_test.c \
                                 $(BUILD)/aros-exec-runtime.o \
                                 $(BUILD)/clipboard-device.o \
                                 $(BUILD)/clipboard-bridge.o
	$(CC) $(CFLAGS) -pthread -Isrc $(AROS_REAL_INCLUDES) $(filter-out %.h,$^) -o $@

$(BUILD)/aros-task-signal-test: tests/aros_task_signal_test.c \
                                 $(BUILD)/aros-exec-runtime.o \
                                 $(BUILD)/clipboard-device.o \
                                 $(BUILD)/clipboard-bridge.o
	$(CC) $(CFLAGS) -pthread -Isrc $(AROS_REAL_INCLUDES) $(filter-out %.h,$^) -o $@

$(BUILD)/iffparse-clipboard-test: tests/iffparse_clipboard_test.c \
                                  $(BUILD)/aros-exec-runtime.o \
                                  $(BUILD)/clipboard-device.o \
                                  $(BUILD)/clipboard-bridge.o \
                                  $(BUILD)/iffparse-clipboard.o
	$(CC) $(CFLAGS) -pthread -I$(COMPAT) -Isrc $(filter-out %.h,$^) -o $@

$(BUILD)/acepaste.o: src/acepaste.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/acepaste: $(BUILD)/acepaste.o $(BUILD)/aros-exec-runtime.o \
                   $(BUILD)/clipboard-device.o $(BUILD)/clipboard-bridge.o \
                   $(BUILD)/iffparse-clipboard.o
	$(CC) $(CFLAGS) -pthread $(filter-out %.h,$^) -o $@

$(BUILD)/native-input-test: tests/native_input_test.c $(DOS_RUNTIME_OBJ) \
                            $(BUILD)/native_dos.o $(BUILD)/native_command.o \
                            $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc $(filter-out %.h,$^) -o $@

$(BUILD)/native-console-handle-test: tests/native_console_handle_test.c $(DOS_RUNTIME_OBJ) \
                                     $(BUILD)/native_dos.o $(BUILD)/native_command.o \
                                     $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc $(filter-out %.h,$^) -o $@

$(BUILD)/aros-console-editor-test: tests/aros_console_editor_test.c $(BUILD)/aros-console-editor.o $(BUILD)/aros-console-editor-stubs.o $(BUILD)/aros-con-support.o $(BUILD)/aros-exec-runtime.o $(BUILD)/clipboard-device.o $(BUILD)/clipboard-bridge.o $(BUILD)/ace-vim-runtime.o \
                                   $(BUILD)/aros-boopsi-runtime.o $(AROS_BOOPSI_OBJS) \
                                   $(BUILD)/aros-graphics-runtime.o $(AROS_ARSUPPORT_OBJS)
	$(CC) $(CFLAGS) -pthread -Isrc $(AROS_REAL_CFLAGS) $(AROS_REAL_INCLUDES) $(filter-out %.h,$^) $(GFX_LIBS) -o $@

$(BUILD)/exec-compat-test: tests/exec_compat_test.c $(BUILD)/exec_compat.o $(BUILD)/exec_compat_bindings.o
	$(CC) $(CFLAGS) -pthread -DAMIGA_EXEC_COMPAT_ENABLED -I$(COMPAT) -Isrc $(filter-out %.h,$^) -o $@

$(BUILD)/aros-newcli.o: $(AROS_NEWCLI_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -Isrc -c $< -o $@

$(BUILD)/aros-shell-runtime.o: src/aros_shell_runtime.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -I$(AROS_ROOT)/workbench/c/Shell -Wno-sign-compare -Wno-implicit-function-declaration -c $< -o $@

$(BUILD)/aros-real-shell.o: $(AROS_ROOT)/workbench/c/Shell/Shell.c | $(BUILD)
	$(CC) $(CFLAGS) -Dmain=ace_aros_shell_main -I$(COMPAT) -Isrc -I$(AROS_ROOT)/workbench/c/Shell -Wno-sign-compare -Wno-implicit-function-declaration -c $< -o $@

$(BUILD)/ace-user-shell.o: src/ace_user_shell.c src/broker_client.h src/native_host.h src/ace_shell_break.h src/ace_modes.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/aros-shell-%.o: $(AROS_ROOT)/workbench/c/Shell/%.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -I$(AROS_ROOT)/workbench/c/Shell -Wno-sign-compare -Wno-implicit-function-declaration -c $< -o $@

# cliPrompt() is wrapped rather than patched.  ACE's own cliPrompt() prints
# the tally line and then calls this one, so the prompt is still drawn by the
# shell's code.  Same seam as -Dmain=ace_aros_shell_main above it.
$(BUILD)/aros-shell-cliPrompt.o: $(AROS_ROOT)/workbench/c/Shell/cliPrompt.c | $(BUILD)
	$(CC) $(CFLAGS) -DcliPrompt=ace_aros_cli_prompt -I$(COMPAT) -Isrc -I$(AROS_ROOT)/workbench/c/Shell -Wno-sign-compare -Wno-implicit-function-declaration -c $< -o $@

$(BUILD)/ace-shell-tally.o: src/ace_shell_tally.c src/broker_client.h src/broker_protocol.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/Echo.o: $(AROS_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

$(BUILD)/CD.o: $(AROS_CD_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) $(AROS_DOSPAT_CFLAGS) -c $< -o $@

$(BUILD)/PathPart.o: $(AROS_PATHPART_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

$(BUILD)/Peek.o: src/peek.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -Dmain=ace_command_entry_main -c $< -o $@

$(BUILD)/Path.o: src/path.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -Dmain=ace_command_entry_main -c $< -o $@

$(BUILD)/Which.o: $(AROS_WHICH_SRC) compat/include/ace_dos_path_intern.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_DOSPATH_CFLAGS) \
	    -DGetDeviceProc=ace_aros_GetDeviceProc \
	    -DFreeDeviceProc=ace_aros_FreeDeviceProc \
	    -include ace_dos_path_intern.h -Dmain=ace_command_entry_main \
	    -c $< -o $@

$(BUILD)/Type.o: $(AROS_TYPE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -Wno-sign-compare -I$(COMPAT) \
	    -Dmain=ace_command_entry_main -c $< -o $@

$(BUILD)/Rename.o: $(AROS_RENAME_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Dmain=ace_command_entry_main -c $< -o $@

$(BUILD)/Stack.o: $(AROS_STACK_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

$(BUILD)/run_entry.o: src/run_entry.c src/native_host.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/aros-dos-%.o: $(AROS_DOSPAT_DIR)/%.c compat/include/ace_dos_intern.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_DOSPAT_CFLAGS) -I$(COMPAT) -c $< -o $@

$(BUILD)/Dir.o: $(DIR_SRC) compat/include/ace_dos_intern.h | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_DOSPAT_CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

$(BUILD)/Fault.o: $(AROS_FAULT_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

$(BUILD)/Ask.o: $(AROS_ASK_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

# The conditionals need dos_commanderrors.h, which lives beside them and
# holds nothing but the two error codes they report.
$(BUILD)/If.o: $(AROS_IF_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) $(AROS_SHCOMMAND_INCLUDES) -c $< -o $@

$(BUILD)/Else.o: $(AROS_ELSE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) $(AROS_SHCOMMAND_INCLUDES) -c $< -o $@

$(BUILD)/EndIf.o: $(AROS_ENDIF_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) $(AROS_SHCOMMAND_INCLUDES) -c $< -o $@

$(BUILD)/EndSkip.o: $(AROS_ENDSKIP_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) $(AROS_SHCOMMAND_INCLUDES) -c $< -o $@

$(BUILD)/Lab.o: $(AROS_LAB_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) $(AROS_SHCOMMAND_INCLUDES) -c $< -o $@

$(BUILD)/Tally.o: src/tally.c src/broker_client.h src/broker_protocol.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -Isrc -c $< -o $@

$(BUILD)/system-halt.o: src/system_halt.c src/system_halt.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/Shutdown.o: src/shutdown_entry.c src/system_halt.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/Reboot.o: src/reboot_entry.c src/system_halt.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/Quit.o: $(ACE_QUIT_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc $(AROS_SHCOMMAND_CFLAGS) $(AROS_SHCOMMAND_INCLUDES) -c $< -o $@

$(BUILD)/Skip.o: $(AROS_SKIP_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) $(AROS_SHCOMMAND_INCLUDES) -c $< -o $@

$(BUILD)/Get.o: $(AROS_GET_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

$(BUILD)/Getenv.o: $(AROS_GETENV_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

$(BUILD)/Setenv.o: $(AROS_SETENV_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

$(BUILD)/Unsetenv.o: $(AROS_UNSETENV_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

$(BUILD)/Set.o: $(AROS_SET_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

$(BUILD)/Unset.o: $(AROS_UNSET_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

$(BUILD)/Alias.o: $(AROS_ALIAS_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

$(BUILD)/Unalias.o: $(AROS_UNALIAS_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

$(BUILD)/FailAt.o: $(AROS_FAILAT_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

$(BUILD)/Why.o: $(AROS_WHY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

$(BUILD)/Prompt.o: $(AROS_PROMPT_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@

$(BUILD)/MakeDir: $(BUILD)/makedir.o $(BUILD)/native_command_entry.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/MakeLink: $(BUILD)/MakeLink.o $(BUILD)/native_command_entry.o \
                   $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
                   $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
                   $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Join: $(BUILD)/Join.o $(BUILD)/native_command_entry.o \
              $(AROS_DOSPAT_OBJS) $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
              $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
              $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Eval: $(BUILD)/Eval.o $(BUILD)/native_command_entry.o \
              $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
              $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
              $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Edit: $(BUILD)/Edit.o $(BUILD)/native_command_entry.o \
              $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
              $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
              $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Copy: $(BUILD)/Copy.o $(BUILD)/copy_entry.o $(AROS_DOSPAT_OBJS) \
	             $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
	             $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
	             $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/List: $(BUILD)/List.o $(BUILD)/native_command_entry.o $(AROS_DOSPAT_OBJS) \
	             $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
	             $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
	             $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Sort: $(BUILD)/Sort.o $(BUILD)/native_command_entry.o \
	             $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
	             $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
	             $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Search: $(BUILD)/Search.o $(BUILD)/native_command_entry.o $(AROS_DOSPAT_OBJS) \
	             $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
	             $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
	             $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Touch: $(BUILD)/Touch.o $(BUILD)/native_command_entry.o \
	              $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
	              $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
	              $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Wait: $(BUILD)/Wait.o $(BUILD)/native_command_entry.o \
	             $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
	             $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
	             $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Status: $(BUILD)/status.o $(BUILD)/native_command_entry.o \
	               $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
	               $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
	               $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Break: $(BUILD)/break.o $(BUILD)/native_command_entry.o \
	              $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
	              $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
	              $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Beep: $(BUILD)/Beep.o $(BUILD)/beep_entry.o \
               $(BUILD)/wayward_beep.o
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/EndCLI: $(BUILD)/endcli.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Linux: $(BUILD)/Linux.o $(BUILD)/terminal_translate.o
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/ed_tine.o: $(ACE_ED_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/Ed: $(BUILD)/ed_tine.o
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Echo: $(BUILD)/Echo.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/CD: $(BUILD)/CD.o $(AROS_DOSPAT_OBJS) $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Path: $(BUILD)/Path.o $(BUILD)/native_command_entry.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Which: $(BUILD)/Which.o $(BUILD)/native_command_entry.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/PathPart: $(BUILD)/PathPart.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Peek: $(BUILD)/Peek.o $(BUILD)/native_command_entry.o \
              $(AROS_DOSPAT_OBJS) $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
              $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
              $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Type: $(BUILD)/Type.o $(BUILD)/native_command_entry.o $(AROS_DOSPAT_OBJS) $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Rename: $(BUILD)/Rename.o $(BUILD)/native_command_entry.o $(AROS_DOSPAT_OBJS) $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Stack: $(BUILD)/Stack.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Run: $(BUILD)/run_entry.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Dir: $(BUILD)/Dir.o $(AROS_DOSPAT_OBJS) $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Delete: $(BUILD)/Delete.o $(BUILD)/native_command_entry.o $(AROS_DOSPAT_OBJS) $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Protect: $(BUILD)/Protect.o $(BUILD)/native_command_entry.o $(BUILD)/aros-arsupport-isdosentrya.o $(AROS_DOSPAT_OBJS) $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Filenote: $(BUILD)/Filenote.o $(BUILD)/native_command_entry.o $(BUILD)/aros-arsupport-isdosentrya.o $(AROS_DOSPAT_OBJS) $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Fault: $(BUILD)/Fault.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Ask: $(BUILD)/Ask.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

# If and Else skip a block by reading the script themselves, through AROS's
# own ReadItem() and FindArg(), which the DOS runtime already carries.
$(BUILD)/If: $(BUILD)/If.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Else: $(BUILD)/Else.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/EndIf: $(BUILD)/EndIf.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/EndSkip: $(BUILD)/EndSkip.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Lab: $(BUILD)/Lab.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Tally: $(BUILD)/Tally.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/Shutdown: $(BUILD)/Shutdown.o $(BUILD)/system-halt.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/Reboot: $(BUILD)/Reboot.o $(BUILD)/system-halt.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/Quit: $(BUILD)/Quit.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Skip: $(BUILD)/Skip.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/execute_entry.o: src/execute_entry.c src/native_host.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc -c $< -o $@

$(BUILD)/Execute: $(BUILD)/execute_entry.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Get: $(BUILD)/Get.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Getenv: $(BUILD)/Getenv.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Setenv: $(BUILD)/Setenv.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Unsetenv: $(BUILD)/Unsetenv.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Set: $(BUILD)/Set.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Unset: $(BUILD)/Unset.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Alias: $(BUILD)/Alias.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Unalias: $(BUILD)/Unalias.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/FailAt: $(BUILD)/FailAt.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Why: $(BUILD)/Why.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/Prompt: $(BUILD)/Prompt.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

# The broker is the only ACE program that links the FMM client.  The
# shell and the commands do not get a privileged socket of their own: one
# semantic authority, one privilege ingress.
$(BUILD)/ace-broker: $(BUILD)/broker.o $(BUILD)/dos-devices.o \
                     $(BUILD)/broker-identity.o \
                     $(BUILD)/ace-modes.o \
                     $(BUILD)/ace-fmm-client.o \
                     $(BUILD)/clipboard-bridge.o
	$(CC) $(CFLAGS) $(filter-out %.h,$^) $(BLKID_LIBS) $(ZLIB_LIBS) -o $@

$(BUILD)/ace-brokerctl: $(BUILD)/brokerctl.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@


$(BUILD)/ace-console: $(BUILD)/amiga_console.o $(BUILD)/ace-requestor-gui.o $(BROKER_CLIENT_OBJS) $(BUILD)/console_channel.o $(BUILD)/console_dispatch.o $(BUILD)/console_spec.o $(BUILD)/ace-appmenu-wayland.o $(BUILD)/console_device_bridge.o $(BUILD)/aros-console-editor.o $(BUILD)/aros-console-editor-stubs.o $(BUILD)/aros-con-support.o $(BUILD)/aros-exec-runtime.o $(BUILD)/clipboard-device.o $(BUILD)/clipboard-bridge.o $(BUILD)/iffparse-clipboard.o $(BUILD)/ace-vim-runtime.o \
                      $(BUILD)/aros-boopsi-runtime.o $(AROS_BOOPSI_OBJS) \
                      $(BUILD)/aros-graphics-runtime.o $(AROS_GRAPHICS_OBJS) $(AROS_ARSUPPORT_OBJS)
	$(CC) $(CFLAGS) -pthread $(filter-out %.h,$^) $(GTK_LIBS) $(GFX_LIBS) $(WAYLAND_LIBS) -o $@

$(BUILD)/console_spec.o: src/console_spec.c src/console_spec.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

$(BUILD)/console-spec-test: tests/console_spec_test.c $(BUILD)/console_spec.o
	$(CC) $(CFLAGS) -Isrc $(filter-out %.h,$^) -o $@

# Where the last Vim build got its source. VIM_SRC names a tree outside this
# checkout that nothing here can guess, so a build that has been told once
# writes the answer down: install-vim builds Vim like every other install
# target builds what it installs, and has somewhere to read the path from
# when it was not passed again on the command line.
VIM_SRC_STAMP := $(BUILD)/vim-source

# Where to look when there is neither a VIM_SRC nor a stamp -- a first build in
# a fresh checkout. A candidate counts only if it holds src/proto/os_amiga.pro,
# which is the file the build script requires and refuses to generate, so a
# directory merely named "vim" is never mistaken for a Vim checkout.
VIM_SRC_SEARCH ?= $(THIRD_PARTY)/vim $(CURDIR)/../vim $(HOME)/vim \
                  $(HOME)/src/vim

# Vim remains an untouched external checkout. ACE supplies the Amiga backend
# build objects and runtime seams, while the target makes the exact source
# tree explicit and reproducible. The full DOS runtime is intentionally not a
# dependency: its cooked editor exports names that collide with Vim's editor.
$(BUILD)/vim: tools/build-vim-ace.sh \
              src/ace_vim_compat.c src/ace_vim_files.c \
              src/ace_vim_editor_stubs.c \
              src/ace_vim_pathdef.c src/native_dos.c \
              compat/vim/include/devices/conunit.h \
              $(BUILD)/ace-vim-runtime.o $(BROKER_CLIENT_OBJS) \
              $(BUILD)/native_process.o $(BUILD)/native_command.o \
              $(BUILD)/aros-exec-runtime.o \
              $(BUILD)/clipboard-device.o $(BUILD)/clipboard-bridge.o \
              $(BUILD)/assign_compat.o \
              $(BUILD)/console_channel.o \
              $(BUILD)/aros-dos-getdeviceproc.o \
              $(BUILD)/aros-dos-freedeviceproc.o \
              $(AROS_DOSPAT_OBJS) | $(BUILD)
	@vim_src="$(VIM_SRC)"; \
	if [ -z "$$vim_src" ] && [ -f "$(VIM_SRC_STAMP)" ]; then \
	    vim_src=`cat "$(VIM_SRC_STAMP)"`; \
	fi; \
	if [ -z "$$vim_src" ]; then \
	    for candidate in $(VIM_SRC_SEARCH); do \
	        if [ -f "$$candidate/src/proto/os_amiga.pro" ]; then \
	            vim_src=$$candidate; \
	            echo "vim: building from $$vim_src (set VIM_SRC to override)"; \
	            break; \
	        fi; \
	    done; \
	fi; \
	if [ -z "$$vim_src" ]; then \
	    echo "use: make vim VIM_SRC=/path/to/untouched/vim" >&2; \
	    exit 2; \
	fi; \
	echo VIM_SRC="$$vim_src" ACE_ROOT="$(CURDIR)" ACE_BUILD="$(BUILD)" CC="$(CC)" "$<"; \
	VIM_SRC="$$vim_src" ACE_ROOT="$(CURDIR)" ACE_BUILD="$(BUILD)" CC="$(CC)" "$<" && \
	printf '%s\n' "$$vim_src" > "$(VIM_SRC_STAMP)"

vim: $(BUILD)/vim

$(BUILD)/NewCLI: $(BUILD)/aros-newcli.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BUILD)/native_process.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/ace-shell: $(BUILD)/ace-launcher.o $(BUILD)/ace-modes.o
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

$(BUILD)/ace-user-shell: $(BUILD)/ace-user-shell.o $(BUILD)/aros-real-shell.o $(BUILD)/ace-shell-tally.o $(AROS_SHELL_OBJS) $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

# Regina built as its library rather than as the standalone interpreter.
#
# mmakefile.src's `contrib-regina-module` target: the same OFILES again, plus
# rexxsaa (RexxStart), client, mt_amigalib and isreginamsg. RexxMast links
# against this, not against the `rexx` program.
#
# The objects carry their own prefix because they are *not* the ones the
# standalone build produces: RXLIB and INCL_REXXSAA change what the shared
# sources compile to, so one set cannot serve both.
#
# regina_init.c is deliberately absent. It is AROS module glue -- it includes
# LC_LIBDEFS_FILE and hangs off genmodule's symbol sets to build a .library --
# and ACE has no module system. What it provided is in src/regina_library_init.c.
# mt_amigalib replaces mt_notmt here, which is upstream's choice: the library
# keeps per-task state and needs the threaded variant.
REGINA_LIB_NAMES := $(filter-out mt_notmt nosaa,$(REGINA_NAMES)) \
                    rexxsaa client mt_amigalib isreginamsg
REGINA_LIB_OBJS := $(addprefix $(BUILD)/regina-lib-,\
                     $(addsuffix .o,$(REGINA_LIB_NAMES)))
REGINA_LIB_CFLAGS := $(REGINA_CFLAGS) -DRXLIB -DINCL_REXXSAA -Dlint \
                     -include ace_regina_library.h

# The ACE side of the link shared by the Regina interpreter tests and
# RexxMast: the same DOS runtime every ACE command gets, plus
# rexxsyslib.o for the ARexx message surface Regina's amifuncs.c calls --
# CreateRexxMsg, IsRexxMsg, the argstrings.  aros-exec-runtime.o is not listed
# because dos-runtime.o already contains it; naming it again is a duplicate
# definition, not a second copy.
#
# This block sits here, below DOS_RUNTIME_OBJ, rather than up with the other
# Regina variables: Make expands a rule's prerequisites as it reads the rule,
# so $(BUILD)/rexx named from higher up the file would see DOS_RUNTIME_OBJ as
# empty and fail as a wall of undefined references from a file that is in fact
# linked.
REGINA_ACE_OBJS := $(BUILD)/rexxsyslib.o $(BUILD)/rexxsupport.o \
                  $(BUILD)/regina-arexx-import.o \
                  $(BUILD)/rexx-port-bridge.o \
                  $(BUILD)/ace-requestor.o \
                  $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
                  $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
                  $(BUILD)/native_process.o \
                  $(BROKER_CLIENT_OBJS) $(AROS_DOSPAT_OBJS)

# Regina is not part of `all`: it needs an external checkout that no target
# here can create.  `make regina` is the whole build, and says so when the
# checkout is not there rather than leaving Make to complain about a missing
# .c file.
ifeq ($(wildcard $(REGINA_SRC)/os_amiga.c),)
regina:
	@echo "regina: no Regina source at $(REGINA_SRC)" >&2
	@echo "  Expected it in third_party/regina, or in an aros-contrib" >&2
	@echo "  checkout; docs/regina-amiga-port.md has the clone command." >&2
	@echo "  Override with REGINA_SRC=... or AROS_CONTRIB_ROOT=..." >&2
	@exit 2
else
regina: $(BUILD)/rexx
endif

# Make's built-in rules regenerate yaccsrc.c from yaccsrc.y and lexsrc.c from
# lexsrc.l -- and they write the result *into the source directory*, replacing
# the generated files the checkout already ships and dirtying a tree that has
# to stay pristine.  Cancel both.  ACE has no yacc or lex sources of its own,
# so nothing else here wants them.
%.c: %.y
%.c: %.l

$(BUILD)/regina-%.o: $(REGINA_SRC)/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(REGINA_CFLAGS) $(REGINA_INCLUDES) -c $< -o $@

$(BUILD)/regina-lib-%.o: $(REGINA_SRC)/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(REGINA_LIB_CFLAGS) $(REGINA_INCLUDES) -c $< -o $@

# Pools and semaphores, shared by the BOOPSI runtime and Regina's library.
#
# aros-real first, for the pool and list prototypes in its proto/exec.h --
# the same ordering the rest of the Regina build uses. That puts AROS's own
# exec headers ahead of ACE's compat ones for this object, which is correct
# and also load-bearing: it is linked beside the imported BOOPSI and console
# sources, which are compiled the same way, and a structure they share has to
# have one layout. compat/include/exec/semaphores.h is what keeps the other
# side of that bargain -- see the note there before changing either.
$(BUILD)/aros-exec-memory.o: src/aros_exec_memory.c | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_REAL_INCLUDES) -I$(COMPAT) -Isrc \
	    -I$(CURDIR)/compat/regina/include \
	    -include ace_regina_library.h -c $< -o $@

$(BUILD)/regina-library-init.o: src/regina_library_init.c | $(BUILD)
	$(CC) $(CFLAGS) $(AROS_REAL_INCLUDES) -I$(COMPAT) -Isrc \
	    -I$(CURDIR)/compat/regina/include \
	    -include ace_regina_library.h -c $< -o $@

# The one object worth checking after the fact.  If the -U flags above are
# ever dropped or reordered away, this file compiles cleanly against the unix
# branch and nothing else complains -- the interpreter just stops being the
# Amiga port.  Assert the symbol the Amiga branch defines.
$(BUILD)/regina-mt_notmt.o: $(REGINA_SRC)/mt_notmt.c | $(BUILD)
	$(CC) $(CFLAGS) $(REGINA_CFLAGS) $(REGINA_INCLUDES) -c $< -o $@
	@nm $@ | grep -q '__regina_OS_Amiga' && ! nm $@ | grep -q '__regina_OS_Unx' || { \
	    echo "$@: built the unix port, not the Amiga one" >&2; \
	    echo "  (REGINA_CFLAGS must keep -Uunix -U__unix__ -U__unix)" >&2; \
	    rm -f $@; exit 2; }

# Regina's pointer-valued ARexx strings are inline binary values. Keep the
# upstream IMPORT object available for the other Amiga BIFs, but route the
# public IMPORT table entry to ACE's ABI-correct, allocation-aware shim.
$(BUILD)/regina-arxfuncs.o: $(REGINA_SRC)/arxfuncs.c | $(BUILD)
	$(CC) $(CFLAGS) $(REGINA_CFLAGS) \
	    -D__regina_arexx_import=ace_regina_arexx_import_legacy \
	    $(REGINA_INCLUDES) -c $< -o $@

$(BUILD)/regina-lib-arxfuncs.o: $(REGINA_SRC)/arxfuncs.c | $(BUILD)
	$(CC) $(CFLAGS) $(REGINA_LIB_CFLAGS) \
	    -D__regina_arexx_import=ace_regina_arexx_import_legacy \
	    $(REGINA_INCLUDES) -c $< -o $@

$(BUILD)/regina-arexx-import.o: src/regina_arexx_import.c | $(BUILD)
	$(CC) $(CFLAGS) $(REGINA_CFLAGS) $(REGINA_INCLUDES) -c $< -o $@

# ADDRESS COMMAND goes through SystemTags() -> launch_command(), which finds
# the shell beside the running executable.  A rexx run from anywhere else
# silently does nothing and returns 0, so the binary belongs in $(BUILD)
# alongside ace-user-shell, and is installed beside it too.
$(BUILD)/rexx: $(REGINA_OBJS) $(REGINA_ACE_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(filter-out %.h,$^) -o $@

$(BUILD)/rexxmast.o: src/rexxmast.c \
                      $(REGINA_SRC)/rexxmast/RexxMast.c | $(BUILD)
	$(CC) $(CFLAGS) $(REGINA_CFLAGS) $(REGINA_INCLUDES) -pthread \
	    -c $< -o $@

# Phase 1 RexxMast uses the library object set rather than the standalone
# interpreter's main. Its public REXX port is provided by rexx-port-bridge.o.
$(BUILD)/rexxmast: $(BUILD)/rexxmast.o $(REGINA_LIB_OBJS) \
                   $(BUILD)/regina-library-init.o \
                   $(BUILD)/aros-exec-memory.o $(REGINA_ACE_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(filter-out %.h,$^) -o $@

rexxmast: $(BUILD)/rexxmast

# The library object set is also exercised independently. It links exactly
# what RexxMast links, so a break in that set is a failing test here rather
# than a surprise there.
$(BUILD)/regina-library-test: tests/regina_library_test.c $(REGINA_LIB_OBJS) \
                             $(BUILD)/regina-library-init.o \
                             $(BUILD)/aros-exec-memory.o $(REGINA_ACE_OBJS) \
                             | $(BUILD)
	$(CC) $(CFLAGS) -pthread -D__AROS__ $(REGINA_INCLUDES) \
	    $(filter-out %.h,$^) -o $@

clean:
	$(RM) -r $(BUILD)

# Per-program cleans, one for each of the three optional programs.
#
# Each removes only what its own build produced, so cleaning one does not cost
# a rebuild of the other two or of ACE itself -- which is the whole reason to
# have these rather than just reaching for `clean`.
#
# None of them touch third_party/: that is source, not output. `make clean`
# remains the blunt instrument that removes $(BUILD) entirely.
#
# The recipes are quiet because they expand to every object by name -- four
# kilobytes of rm arguments apiece, which tells the reader nothing.

# Deliberately not $(BUILD)/ace-vim-runtime.o, even though the Vim build needs
# it: it is also linked into ace-console, dos-runtime.o and the console editor
# test, so removing it here would quietly force a rebuild of most of ACE. The
# same goes for every other $(BUILD)/*.o the vim target depends on. What is
# Vim's alone is the binary, the objects the build script compiles into
# vim-objects/, the runtime tree it copies in, and the stamp recording which
# source tree was used.
clean-vim:
	@echo "cleaning vim"
	@$(RM) -r $(BUILD)/vim $(BUILD)/vim-objects $(BUILD)/runtime \
	          $(VIM_SRC_STAMP)

clean-regina:
	@echo "cleaning regina"
	@$(RM) -r $(BUILD)/rexx $(BUILD)/rexxmast $(BUILD)/rexxmast.o \
	          $(BUILD)/rexxmast.o.d $(BUILD)/rexxmast-result-test \
	          $(BUILD)/rexxmast-func-test $(BUILD)/rexxmast-failure-test \
	          $(BUILD)/rexxmast-close-test $(BUILD)/rexxmast-resource-test \
	          $(BUILD)/rexxmast-private-test $(BUILD)/rexxmast-broadcast-test \
	          $(REGINA_OBJS) \
	          $(addsuffix .d,$(REGINA_OBJS))

# This one also throws away the fetched tarball and the tree unpacked from it,
# so the next `make lha` downloads and re-checksums rather than trusting what
# is already on disk. That is the point of a clean for a target whose source
# arrives over the network: `make clean-lha lha` is how you check the download
# and its SHA-256 still work, not just the compile.
clean-lha:
	@echo "cleaning lha, including the fetched tarball"
	@$(RM) -r $(BUILD)/LhA $(LHA_AROS_OBJS) $(addsuffix .d,$(LHA_AROS_OBJS)) \
	          $(LHA_AROS_ARCHIVE) $(LHA_AROS_DIR)

# The launcher names the binary it was installed beside, rather than looking
# ace-shell up on PATH. PATH order is not the desktop session's to promise,
# and when a second install exists the icon silently runs whichever directory
# comes first -- which is how an install can look complete and still start
# yesterday's build.
# Written by the install recipe rather than as a file target of its own,
# because its content depends on BINDIR -- a variable, not a file. As a target
# make would consider it up to date whenever data/ace.desktop.in had not
# changed, and happily install a launcher pointing at the prefix from the
# previous install.
install: all tine
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(RM) $(DESTDIR)$(BINDIR)/LNX
	$(INSTALL) -m 0755 $(addprefix $(BUILD)/,$(INSTALL_BINS)) $(DESTDIR)$(BINDIR)
	# Say is one command, with two private helpers. The broker starts the
	# voice-server helper itself; there are deliberately no systemd units.
	$(INSTALL) -m 0755 data/say/say-voice-server data/say/say-setup $(DESTDIR)$(BINDIR)
	# Piper and its initial voice models are part of the normal per-user ACE
	# installation. A staged or system-wide install cannot know which user's
	# home should own the models, so it installs the command set only.
	@if [ -z "$(DESTDIR)" ] && [ `id -u` -ne 0 ]; then \
	    "$(BINDIR)/say-setup"; \
	fi
	$(INSTALL) -m 0755 "$(TINE_DIR)/tine" $(DESTDIR)$(BINDIR)/tine
	$(INSTALL) -m 0755 broker-start broker-stop $(DESTDIR)$(BINDIR)
	# SYS: -- the boot volume, in the shape dos.library's boot assigns expect.
	# Only the drawers ACE actually fills are created: AddBootAssign() in
	# rom/dos/cliinit.c falls back to SYS: itself for a drawer that is not
	# there, so L:, LIBS:, DEVS: and FONTS: resolve to SYS: on a system with
	# nothing to put in them, which is what ACE is.
	$(INSTALL) -d $(DESTDIR)$(SYSDIR)/C $(DESTDIR)$(SYSDIR)/S \
	              $(DESTDIR)$(SYSDIR)/Prefs/Env-Archive
	$(RM) $(DESTDIR)$(SYSDIR)/C/LNX
	for command in $(AMIGA_COMMANDS); do \
	    ln -sf $(BINDIR)/$$command $(DESTDIR)$(SYSDIR)/C/$$command; \
	done
	$(INSTALL) -m 0644 data/Startup-Sequence $(DESTDIR)$(SYSDIR)/S/Startup-Sequence
	$(INSTALL) -m 0644 data/Shell-Startup $(DESTDIR)$(SYSDIR)/S/Shell-Startup
	$(INSTALL) -d $(DESTDIR)$(APPLICATIONSDIR) $(DESTDIR)$(ICONDIR) \
	              $(DESTDIR)$(POLKIT_ACTIONDIR)
	sed 's|@BINDIR@|$(BINDIR)|g' data/ace.desktop.in > $(BUILD)/ace.desktop
	$(INSTALL) -m 0644 $(BUILD)/ace.desktop $(DESTDIR)$(APPLICATIONSDIR)/ace.desktop
	$(INSTALL) -m 0644 assets/ace.png $(DESTDIR)$(ICONDIR)/ace.png
	sed 's|@BINDIR@|$(BINDIR)|g' data/org.ace.Ace.fmm.policy > \
	              $(BUILD)/org.ace.Ace.fmm.policy
	$(INSTALL) -m 0644 $(BUILD)/org.ace.Ace.fmm.policy \
	              $(DESTDIR)$(POLKIT_ACTIONDIR)/org.ace.Ace.fmm.policy
	# Vim, Regina and LhA, after the base install rather than as ordinary
	# prerequisites of it. Order matters: install-regina checks that
	# ace-user-shell is already beside the rexx it installs, and as a
	# prerequisite it could run first and warn about an absence it was
	# about to stop being true. Make gives no ordering among prerequisites,
	# so this is a sub-make -- the one place in this file that recurses,
	# and the reason is sequencing rather than modularity.
	#
	# Command-line and environment variables reach the sub-make on their
	# own, so DESTDIR and PREFIX do not need repeating here.
	$(MAKE) install-vim install-regina install-lha
	@if [ -z "$(DESTDIR)" ]; then \
	    found=`command -v ace-shell 2>/dev/null || true`; \
	    if [ -n "$$found" ] && [ "$$found" != "$(BINDIR)/ace-shell" ]; then \
	        echo; \
	        echo "warning: installed $(BINDIR)/ace-shell, but PATH finds $$found first."; \
	        echo "         Typing ace-shell runs that older install, not this one."; \
	        echo "         Remove it, or put $(BINDIR) earlier on PATH."; \
		fi; \
	    if command -v gtk-update-icon-cache >/dev/null 2>&1; then \
	        gtk-update-icon-cache -f -t $(DATADIR)/icons/hicolor || true; \
	    fi; \
	    if command -v update-desktop-database >/dev/null 2>&1; then \
	        update-desktop-database $(APPLICATIONSDIR) || true; \
	    fi; \
	fi

# The three optional programs -- Vim, Regina and LhA -- are built and
# installed the same way, by three pairs of targets:
#
#   make vim     make regina     make lha
#   make install-vim  make install-regina  make install-lha
#
# Each install target depends on its build target, so "make install-regina" on
# its own builds first when the binary is missing or out of date, exactly as
# "make install" builds "all". Each installs into $(BINDIR) beside
# ace-user-shell -- which is not a tidiness preference but a requirement, since
# every ACE program finds its companions beside its own executable -- and then
# symlinks the command into SYS:C so typing its name resolves through C:.
#
# Vim and Regina build from source under third_party/; LhA fetches a release
# tarball into $(BUILD), because its upstream is a tarball rather than a tree
# ACE tracks. LhA is also part of AMIGA_COMMANDS and so is covered by the
# ordinary "make install" as well; install-lha exists to install just it,
# without rebuilding and reinstalling everything else.

install-vim: vim
	@test -f "$(BUILD)/runtime/defaults.vim" || \
		(echo "install-vim: $(BUILD)/runtime is missing Vim's runtime files" >&2; exit 2)
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(BINDIR)/runtime
	$(INSTALL) -m 0755 $(BUILD)/vim $(DESTDIR)$(BINDIR)/vim
	cp -a $(BUILD)/runtime/. $(DESTDIR)$(BINDIR)/runtime/
	# In SYS:C like any other command, so typing "vim" finds it through C:.
	$(INSTALL) -d $(DESTDIR)$(SYSDIR)/C
	ln -sf $(BINDIR)/vim $(DESTDIR)$(SYSDIR)/C/vim

# rexx must land in $(BINDIR) rather than only in SYS:C, and the symlink must
# point there rather than being a copy: ADDRESS COMMAND goes through
# SystemTags() -> launch_command(), which looks for ace-user-shell beside the
# running executable. A rexx that resolved to somewhere else would run, and
# report success, while silently doing nothing.
install-regina: regina rexxmast $(BUILD)/ace-user-shell \
                $(BUILD)/ace-broker $(BUILD)/ace-brokerctl
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 0755 $(BUILD)/rexx $(DESTDIR)$(BINDIR)/rexx
	$(INSTALL) -m 0755 $(BUILD)/rexxmast $(DESTDIR)$(BINDIR)/rexxmast
	# Regina's ADDRESS COMMAND and ARexx support routines launch this shell
	# beside rexx, so the standalone Regina install needs the matching build.
	$(INSTALL) -m 0755 $(BUILD)/ace-user-shell $(DESTDIR)$(BINDIR)/ace-user-shell
	# RexxMast discovers the broker beside its own executable. Install the
	# matching protocol build here so install-regina works independently of a
	# previously installed ACE base set.
	$(INSTALL) -m 0755 $(BUILD)/ace-broker $(DESTDIR)$(BINDIR)/ace-broker
	$(INSTALL) -m 0755 $(BUILD)/ace-brokerctl $(DESTDIR)$(BINDIR)/ace-brokerctl
	$(INSTALL) -d $(DESTDIR)$(SYSDIR)/C
	ln -sf $(BINDIR)/rexx $(DESTDIR)$(SYSDIR)/C/rexx
	ln -sf $(BINDIR)/rexxmast $(DESTDIR)$(SYSDIR)/C/rexxmast
	# RX is what an Amiga user types, so it is here from the start. It is
	# an alias for the interpreter. RexxMast is now available as a separate
	# user-started service; RX still invokes the local interpreter here, while
	# ADDRESS REXX explicitly exercises the public RexxMast port.
	ln -sf $(BINDIR)/rexx $(DESTDIR)$(SYSDIR)/C/RX
	@if [ -z "$(DESTDIR)" ] && [ ! -x "$(BINDIR)/ace-user-shell" ]; then \
	    echo; \
	    echo "warning: installed $(BINDIR)/rexx, but ace-user-shell is not"; \
	    echo "         beside it. ADDRESS COMMAND will do nothing and still"; \
	    echo "         report success. Run \"make install\" too."; \
	fi

install-lha: lha
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 0755 $(BUILD)/LhA $(DESTDIR)$(BINDIR)/LhA
	$(INSTALL) -d $(DESTDIR)$(SYSDIR)/C
	ln -sf $(BINDIR)/LhA $(DESTDIR)$(SYSDIR)/C/LhA

test-console-device: $(BUILD)/console-device-test
	$(BUILD)/console-device-test

test-console-channel: $(BUILD)/console-channel-test
	$(BUILD)/console-channel-test

test-console-dispatch: $(BUILD)/console-dispatch-test
	$(BUILD)/console-dispatch-test

test-console-spec: $(BUILD)/console-spec-test
	$(BUILD)/console-spec-test

# The clipboard half of this test writes an IFF stream and reads it back.
# Left to itself the clipboard device bridges to the host clipboard, so the
# read returns whatever the user last copied, wrapped as IFF, and the test
# fails on any machine with a desktop session and a non-empty clipboard.
# Isolate it: no host bridging, and a private spool directory.
$(BUILD)/create-new-proc-test: tests/create_new_proc_test.c \
                              $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
                              $(BUILD)/native_command.o \
                              $(BUILD)/native_shcommand.o \
                              $(BUILD)/native_process.o $(BROKER_CLIENT_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -pthread -I$(COMPAT) -Isrc $(filter-out %.h,$^) -o $@

# CreateNewProc() runs its entry point on a host thread, so this one is worth
# running under the sanitisers when the handshake is changed:
#   make CC=gcc CCACHE_PREFIX= CFLAGS='-g -fsanitize=thread' test-create-new-proc
test-create-new-proc: $(BUILD)/create-new-proc-test
	$(BUILD)/create-new-proc-test

# The whole path, through the Amiga API rather than the broker's: CreatePort,
# FindPort, PutMsg, WaitPort, GetMsg, ReplyMsg, across two processes.
$(BUILD)/rexx-port-test: tests/rexx_port_test.c $(BUILD)/rexxsyslib.o \
                        $(BUILD)/rexx-port-bridge.o \
                        $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
                        $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
                        $(BUILD)/native_process.o $(BROKER_CLIENT_OBJS) \
                        $(AROS_DOSPAT_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(AROS_REAL_INCLUDES) -I$(COMPAT) -Isrc \
	    -Wno-int-conversion $(filter-out %.h,$^) -o $@

test-rexx-port: $(BUILD)/rexx-port-test $(BUILD)/ace-broker
	sh tests/with_private_broker.sh $(BUILD)/rexx-port-test

test-rexxmast: $(BUILD)/rexxmast $(BUILD)/rexx $(BUILD)/sendrexxmsg \
               $(BUILD)/rexxmast-result-test $(BUILD)/rexxmast-func-test \
               $(BUILD)/rexxmast-failure-test $(BUILD)/rexxmast-resource-test \
               $(BUILD)/rexxmast-private-test $(BUILD)/rexxmast-broadcast-test \
               $(BUILD)/rexxmast-close-test \
               $(BUILD)/ace-broker $(BUILD)/ace-brokerctl
	sh tests/with_private_broker.sh sh tests/rexxmast_test.sh

# sendrexxmsg.c and listen4msg.c from the AROS tree, built unmodified. They
# are the acceptance test for the whole port mechanism: if ACE implements the
# contract, these compile, link and run with nothing done to them.
#
# Built with Regina's include order for the reason ace_regina_compat.h gives:
# the aros-real tree's own thin proto/dos.h and proto/alib.h win the lookup
# and hide Write(), CreatePort() and SystemTags().
AREXX_DEMO_INCLUDES := -I$(CURDIR)/compat/aros-real/include -I$(COMPAT) \
                       -I$(CURDIR)/compat/regina/include -Isrc \
                       -I$(AROS_ROOT)/arch/all-pc/include \
                       -I$(AROS_ROOT)/arch/$(AROS_CPU_ARCH)/include \
                       -I$(AROS_ROOT)/compiler/arossupport/include \
                       -I$(AROS_ROOT)/compiler/include
AREXX_DEMO_CFLAGS := -w -Wno-implicit-function-declaration \
                     -Wno-int-conversion -Wno-incompatible-pointer-types \
                     -include ace_regina_compat.h
AREXX_DEMO_OBJS = $(BUILD)/rexxsyslib.o $(BUILD)/rexx-port-bridge.o \
                  $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o \
                  $(BUILD)/native_command.o $(BUILD)/native_shcommand.o \
                  $(BUILD)/native_process.o $(BROKER_CLIENT_OBJS) \
                  $(AROS_DOSPAT_OBJS)

$(BUILD)/sendrexxmsg: $(REGINA_SRC)/rexxmast/sendrexxmsg.c $(AREXX_DEMO_OBJS) \
                      | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(AREXX_DEMO_CFLAGS) $(AREXX_DEMO_INCLUDES) \
	    $(filter-out %.h,$^) -o $@

$(BUILD)/rexxmast-result-test: tests/rexxmast_result_test.c $(AREXX_DEMO_OBJS) \
                              | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(AREXX_DEMO_CFLAGS) $(AREXX_DEMO_INCLUDES) \
	    $(filter-out %.h,$^) -o $@

$(BUILD)/rexxmast-func-test: tests/rexxmast_func_test.c $(AREXX_DEMO_OBJS) \
                             | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(AREXX_DEMO_CFLAGS) $(AREXX_DEMO_INCLUDES) \
	    $(filter-out %.h,$^) -o $@

$(BUILD)/rexxmast-failure-test: tests/rexxmast_failure_test.c $(AREXX_DEMO_OBJS) \
                                | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(AREXX_DEMO_CFLAGS) $(AREXX_DEMO_INCLUDES) \
	    $(filter-out %.h,$^) -o $@

$(BUILD)/rexxmast-resource-test: tests/rexxmast_resource_test.c $(AREXX_DEMO_OBJS) \
                                 | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(AREXX_DEMO_CFLAGS) $(AREXX_DEMO_INCLUDES) \
	    $(filter-out %.h,$^) -o $@

$(BUILD)/rexxmast-private-test: tests/rexxmast_private_test.c $(AREXX_DEMO_OBJS) \
                                | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(AREXX_DEMO_CFLAGS) $(AREXX_DEMO_INCLUDES) \
	    $(filter-out %.h,$^) -o $@

$(BUILD)/rexxmast-broadcast-test: tests/rexxmast_broadcast_test.c $(AREXX_DEMO_OBJS) \
                                  | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(AREXX_DEMO_CFLAGS) $(AREXX_DEMO_INCLUDES) \
	    $(filter-out %.h,$^) -o $@

$(BUILD)/rexxmast-close-test: tests/rexxmast_close_test.c $(AREXX_DEMO_OBJS) \
                              | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(AREXX_DEMO_CFLAGS) $(AREXX_DEMO_INCLUDES) \
	    $(filter-out %.h,$^) -o $@

$(BUILD)/listen4msg: $(REGINA_SRC)/rexxmast/listen4msg.c $(AREXX_DEMO_OBJS) \
                     | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(AREXX_DEMO_CFLAGS) $(AREXX_DEMO_INCLUDES) \
	    $(filter-out %.h,$^) -o $@

# The counterpart each demo needs, because the two do not pair with each
# other: sendrexxmsg sends to REXX, listen4msg serves TEST.
$(BUILD)/arexx-demo-peer: tests/arexx_demo_peer.c $(AREXX_DEMO_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -pthread $(AREXX_DEMO_CFLAGS) $(AREXX_DEMO_INCLUDES) \
	    $(filter-out %.h,$^) -o $@

test-arexx-demos: $(BUILD)/sendrexxmsg $(BUILD)/listen4msg \
                  $(BUILD)/arexx-demo-peer $(BUILD)/ace-broker \
                  $(BUILD)/ace-brokerctl
	sh tests/rexx_port_test.sh

test-aros-exec-runtime: $(BUILD)/aros-exec-runtime-test
	ACE_CLIPBOARD_DISABLE_HOST=1 \
	    ACE_CLIPBOARD_DIR="$$(mktemp -d)" $(BUILD)/aros-exec-runtime-test

test-iffparse-clipboard: $(BUILD)/iffparse-clipboard-test
	$(BUILD)/iffparse-clipboard-test

test-acepaste: $(BUILD)/acepaste
	set -eu; clip_dir=$$(mktemp -d); host_file=$$(mktemp); \
	trap 'rm -rf "$$clip_dir" "$$host_file"' EXIT; \
	printf 'from host' >"$$host_file"; \
	ACE_CLIPBOARD_DIR="$$clip_dir" ACE_CLIPBOARD_HOST_FILE="$$host_file" \
		$(BUILD)/acepaste --get >"$$clip_dir/get-before"; \
	test "$$(cat "$$clip_dir/get-before")" = 'from host'; \
	printf 'primary text' | ACE_CLIPBOARD_DIR="$$clip_dir" \
		ACE_CLIPBOARD_HOST_FILE="$$host_file" $(BUILD)/acepaste --set; \
	test "$$(cat "$$host_file")" = 'primary text'; \
	test -f "$$clip_dir/clip0"; \
	printf 'alternate text' | ACE_CLIPBOARD_DIR="$$clip_dir" \
		ACE_CLIPBOARD_HOST_FILE="$$host_file" $(BUILD)/acepaste --unit 7 --set; \
	test -f "$$clip_dir/clip7"; \
	ACE_CLIPBOARD_DIR="$$clip_dir" ACE_CLIPBOARD_HOST_FILE="$$host_file" \
		$(BUILD)/acepaste --unit 7 --get >"$$clip_dir/get-seven"; \
	test "$$(cat "$$clip_dir/get-seven")" = 'alternate text'; \
	count=0; for unit in $$(seq 0 255); do \
		test -f "$$clip_dir/clip$$unit" && count=$$((count + 1)); \
	done; test "$$count" -eq 2

test-clipboard-client: $(BUILD)/Clip $(BUILD)/Assign $(BUILD)/acepaste
	sh tests/clipboard_client_test.sh

test-aros-console-editor: $(BUILD)/aros-console-editor-test
	$(BUILD)/aros-console-editor-test

test-native-input: $(BUILD)/native-input-test
	$(BUILD)/native-input-test

test-native-console-handle: $(BUILD)/native-console-handle-test
	$(BUILD)/native-console-handle-test

test-exec-compat: $(BUILD)/exec-compat-test
	$(BUILD)/exec-compat-test

test-boopsi: $(BUILD)/boopsi-test
	$(BUILD)/boopsi-test

test-graphics: $(BUILD)/graphics-test
	$(BUILD)/graphics-test

test-console-device-bridge: $(BUILD)/console-device-bridge-test
	$(BUILD)/console-device-bridge-test

# The comment harness is a test binary rather than a command: it checks the
# metadata path independently of List's command-level formatting.
$(BUILD)/dos-comment-test: tests/dos_comment_test.c $(DOS_RUNTIME_OBJ) \
                          $(BUILD)/native_dos.o $(BUILD)/native_command.o \
                          $(BUILD)/native_shcommand.o \
                          $(BROKER_CLIENT_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) -Isrc $(filter-out %.h,$^) -o $@

test-system-assigns: all
	sh tests/system_assigns_test.sh

test-fmm-crm-channel: all $(BUILD)/fmm-crm-channel-probe
	sh tests/fmm_crm_channel_test.sh

test-filesystem-translation: all $(BUILD)/dos-comment-test
	sh tests/filesystem_translation_test.sh

test-dir-break: all $(BUILD)/dos-comment-test
	dir_name=$$($(BUILD)/ace-brokerctl name "$(CURDIR)"); \
	$(BUILD)/dos-comment-test break-exnext "$$dir_name"

test-peek: all
	sh tests/peek_test.sh

test-lha: all
	sh tests/lha_test.sh

test-file-commands: all
	sh tests/file_commands_test.sh

test-relabel: all
	sh tests/relabel_test.sh

test-info: all
	sh tests/info_test.sh

test-edit: all
	sh tests/edit_test.sh

test-dir-sort: all
	sh tests/dir_sort_test.sh

test-dir-exall-scale: all
	sh tests/dir_exall_scale_test.sh

test-dir-softlink: all
	sh tests/dir_softlink_test.sh

test-brokerctl-assign: all
	sh tests/brokerctl_assign_test.sh

test-modes: all
	sh tests/mode_test.sh

test-device-view: all
	sh tests/device_view_test.sh

test-fmm-crm-broker: all
	sh tests/fmm_crm_broker_test.sh

test-privileged-file: all
	sh tests/privileged_file_test.sh

test-escalation-contract: all
	sh tests/escalation_contract_test.sh

# Navigation over a randomly generated tree.  Takes a seed: a failure prints
# the one it used, and ACE_NAV_SEED=<n> replays exactly that tree.
test-navigation: all
	sh tests/navigation_matrix_test.sh

test-deleted-cwd: all
	sh tests/deleted_cwd_test.sh

test-tally: all
	sh tests/tally_test.sh

test-halt: all
	sh tests/halt_test.sh

test-dir-volume-root: all
	sh tests/dir_volume_root_test.sh

test-device-node: all
	sh tests/device_node_test.sh

test-assign-missing-target: all
	sh tests/assign_missing_target_test.sh

test-prompt-newline: all
	sh tests/prompt_newline_test.sh

test-shell-return-code: all
	sh tests/shell_return_code_test.sh

test-shell-redirection: all
	sh tests/shell_redirection_test.sh

test-regina-library: $(BUILD)/regina-library-test
	$(BUILD)/regina-library-test

test-regina-arexx: $(BUILD)/rexx $(BUILD)/rexxmast $(BUILD)/ace-broker
	sh tests/with_private_broker.sh sh tests/regina_arexx_test.sh

test-tine: all tine
	sh tests/tine_test.sh
	python3 tests/tine_console_query_test.py
	python3 tests/tine_screen_trace_test.py

test-say-broker: $(BUILD)/ace-broker $(BUILD)/ace-brokerctl
	sh tests/say_broker_test.sh

.PHONY: all clean clean-vim clean-regina clean-lha install tine lha lha-fetch regina rexxmast test-broker-port-channel test-broker-port-message test-broker-port-abandon test-rexx-port test-rexxmast test-arexx-demos test-regina-arexx install-vim install-regina install-lha vim test-console-device test-console-channel test-console-dispatch test-console-spec test-console-device-bridge test-filesystem-translation test-fmm-crm-channel test-dir-break test-peek test-lha test-file-commands test-relabel test-info test-edit test-dir-sort test-dir-exall-scale test-dir-softlink test-brokerctl-assign test-modes test-device-view test-escalation-contract test-navigation test-deleted-cwd test-tally test-halt test-dir-volume-root test-device-node test-assign-missing-target test-tine test-system-assigns test-aros-exec-runtime test-create-new-proc test-iffparse-clipboard test-acepaste test-clipboard-client test-aros-console-editor test-native-input test-native-console-handle test-exec-compat test-boopsi test-graphics test-prompt-newline test-shell-return-code test-shell-redirection test-lnx-pty test-terminal-translate test-say-broker
AROS_CLIP_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Clip.c
$(BUILD)/Clip.o: $(AROS_CLIP_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -Wno-sign-compare -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@
$(BUILD)/Clip: $(BUILD)/Clip.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

AROS_CUT_SRC := $(AROS_ROOT)/workbench/c/shellcommands/Cut.c
$(BUILD)/Cut.o: $(AROS_CUT_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -I$(COMPAT) $(AROS_SHCOMMAND_CFLAGS) -c $< -o $@
$(BUILD)/Cut: $(BUILD)/Cut.o $(DOS_RUNTIME_OBJ) $(BUILD)/native_dos.o $(BUILD)/native_command.o $(BUILD)/native_shcommand.o $(BROKER_CLIENT_OBJS)
	$(CC) $(CFLAGS) $(filter-out %.h,$^) -o $@

# Generated by -MMD above, one per object.  -MP adds a phony target for every
# header so that deleting or renaming one does not wedge the build with a
# "No rule to make target" error against a stale dependency file.
-include $(wildcard $(BUILD)/*.d)
