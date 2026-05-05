# rose-tools top-level Makefile
# Target:   Windows NT 5.1 (XP) 32-bit
# Compiler: i686-w64-mingw32-gcc (MinGW-w64)
#
# Targets:
#   make all         -- build checkvm.exe + vmrosd.exe + all plugin DLLs
#   make test        -- build all unit and integration tests
#   make testconfig  -- unit test (run on Windows or via Wine)
#   make testrpc     -- integration test (run inside VMware guest)
#   make clean       -- remove all build artifacts

CC      = i686-w64-mingw32-gcc
CFLAGS  = -std=gnu89 \
          -m32 \
          -Wall \
          -Wextra \
          -DWINVER=0x0501 \
          -D_WIN32_WINNT=0x0501 \
          -D_WIN32 \
          -DWIN32 \
          -D__USE_MINGW_ANSI_STDIO=1

INCLUDES = -I lib/include \
           -I lib/backdoor

# ---------------------------------------------------------------------------
# Core library
#
# Notes:
#   str.c        -- excluded: bsd_output.h dependency (generated, not in tree).
#                   See lib/string/str_rose.c.
#   rpcChannel.c -- excluded: replaced by lib/rpcChannel/rpcChannel_rose.c.
#   rpcin.c      -- included with ROSE-TOOLS shim block (non-GLib path).
# ---------------------------------------------------------------------------
LIB_SRCS = lib/backdoor/backdoor.c              \
           lib/backdoor/backdoorGcc32.c         \
           lib/message/message.c                \
           lib/rpcOut/rpcout.c                  \
           lib/rpcIn/rpcin.c                    \
           lib/vmCheck/vmcheck.c                \
           lib/stubs/stub-debug.c               \
           lib/string/str_rose.c                \
           lib/rpcChannel/rpcChannel_rose.c     \
           lib/config/roseConfig.c

LIB_OBJS = $(LIB_SRCS:.c=.o)

.PHONY: all test clean checkvm vmrosd testconfig testrpc guestinfo timesync resolutionset

all: checkvm vmrosd guestinfo timesync resolutionset
test: testconfig testrpc testpluginmgr

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ---------------------------------------------------------------------------
# checkvm.exe
# ---------------------------------------------------------------------------
checkvm: $(LIB_OBJS) services/checkvm/checkvm.o
	$(CC) $(CFLAGS) -static-libgcc -o checkvm.exe $^

services/checkvm/checkvm.o: services/checkvm/checkvm.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ---------------------------------------------------------------------------
# vmrosd.exe
# ---------------------------------------------------------------------------
VMROSD_SRCS = services/vmrosd/vmrosd.c                \
              services/vmrosd/serviceMain.c            \
              services/vmrosd/mainLoop.c               \
              services/vmrosd/pluginMgr.c              \
              services/vmrosd/toolsRpc.c

VMROSD_OBJS = $(VMROSD_SRCS:.c=.o)

vmrosd: $(LIB_OBJS) $(VMROSD_OBJS)
	$(CC) $(CFLAGS) -static-libgcc -o vmrosd.exe $^ -lws2_32 -liphlpapi

$(VMROSD_OBJS): %.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ---------------------------------------------------------------------------
# testconfig.exe  --  unit test (run on Windows or via Wine)
# ---------------------------------------------------------------------------
tests/unit/testConfig/test_config.o: tests/unit/testConfig/test_config.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

testconfig: lib/config/roseConfig.o                   \
            lib/stubs/stub-debug.o                    \
            lib/string/str_rose.o                     \
            tests/unit/testConfig/test_config.o
	$(CC) $(CFLAGS) -o testconfig.exe $^

# ---------------------------------------------------------------------------
# testrpc.exe  --  integration test (run inside VMware guest only)
# ---------------------------------------------------------------------------
tests/integration/testRpc/test_rpc.o: tests/integration/testRpc/test_rpc.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

testrpc: $(LIB_OBJS) tests/integration/testRpc/test_rpc.o
	$(CC) $(CFLAGS) -o testrpc.exe $^ -lws2_32 -liphlpapi

# ---------------------------------------------------------------------------
# guestInfo.dll  --  guestInfo plugin (dynamic)
# ---------------------------------------------------------------------------
GUESTINFO_SRCS = services/plugins/guestInfo/guestInfo.c

guestinfo: $(GUESTINFO_SRCS) $(LIB_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -shared \
	    -static-libgcc \
	    -o guestInfo.dll $(GUESTINFO_SRCS) \
	    $(LIB_OBJS) -lws2_32 -liphlpapi

# ---------------------------------------------------------------------------
# timeSync.dll  --  time synchronization plugin (dynamic)
# ---------------------------------------------------------------------------
TIMESYNC_SRCS = services/plugins/timeSync/timeSync.c \
                services/plugins/timeSync/timeSyncWin.c

timesync: $(TIMESYNC_SRCS) $(LIB_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -shared \
	    -static-libgcc \
	    -o timeSync.dll $(TIMESYNC_SRCS) \
	    $(LIB_OBJS) -lws2_32

# ---------------------------------------------------------------------------
# resolutionSet.dll  --  auto-resolution plugin (dynamic)
# ---------------------------------------------------------------------------
RESOLUTIONSET_SRCS = services/plugins/resolutionSet/resolutionSet.c

resolutionset: $(RESOLUTIONSET_SRCS) $(LIB_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -shared \
	    -static-libgcc \
	    -o resolutionSet.dll $(RESOLUTIONSET_SRCS) \
	    $(LIB_OBJS) -lws2_32
# Stub DLLs are built alongside the exe and expected in the same directory.
# ---------------------------------------------------------------------------
PLUGINMGR_STUB_SRCS = tests/unit/testPluginMgr/stub_valid.c     \
                      tests/unit/testPluginMgr/stub_null.c      \
                      tests/unit/testPluginMgr/stub_badinit.c   \
                      tests/unit/testPluginMgr/stub_noexport.c

stub_valid.dll: tests/unit/testPluginMgr/stub_valid.c
	$(CC) $(CFLAGS) $(INCLUDES) -shared -o $@ $<

stub_null.dll: tests/unit/testPluginMgr/stub_null.c
	$(CC) $(CFLAGS) $(INCLUDES) -shared -o $@ $<

stub_badinit.dll: tests/unit/testPluginMgr/stub_badinit.c
	$(CC) $(CFLAGS) $(INCLUDES) -shared -o $@ $<

stub_noexport.dll: tests/unit/testPluginMgr/stub_noexport.c
	$(CC) $(CFLAGS) $(INCLUDES) -shared -o $@ $<

tests/unit/testPluginMgr/test_pluginmgr.o: tests/unit/testPluginMgr/test_pluginmgr.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

testpluginmgr: stub_valid.dll stub_null.dll stub_badinit.dll stub_noexport.dll \
               lib/config/roseConfig.o                                          \
               lib/stubs/stub-debug.o                                           \
               lib/string/str_rose.o                                            \
               services/vmrosd/pluginMgr.o                                     \
               tests/unit/testPluginMgr/test_pluginmgr.o
	$(CC) $(CFLAGS) -o testpluginmgr.exe                \
	    lib/config/roseConfig.o                         \
	    lib/stubs/stub-debug.o                          \
	    lib/string/str_rose.o                           \
	    services/vmrosd/pluginMgr.o                     \
	    tests/unit/testPluginMgr/test_pluginmgr.o
clean:
	rm -f $(LIB_OBJS)                                           \
	      services/checkvm/checkvm.o checkvm.exe                \
	      $(VMROSD_OBJS) vmrosd.exe                             \
	      tests/integration/testRpc/test_rpc.o testrpc.exe      \
	      tests/unit/testConfig/test_config.o testconfig.exe    \
	      tests/unit/testPluginMgr/test_pluginmgr.o             \
	      testpluginmgr.exe                                     \
	      stub_valid.dll stub_null.dll stub_badinit.dll stub_noexport.dll \
	      guestInfo.dll timeSync.dll resolutionSet.dll
