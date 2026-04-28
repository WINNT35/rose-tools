# rose-tools top-level Makefile
# Target:   Windows NT 5.1 (XP) 32-bit
# Compiler: i686-w64-mingw32-gcc (MinGW-w64)

CC      = i686-w64-mingw32-gcc
CFLAGS  = -std=gnu89 \
          -m32 \
          -Wall \
          -Wextra \
          -DWINVER=0x0501 \
          -D_WIN32_WINNT=0x0501 \
          -D_WIN32 \
          -DWIN32

# Include paths - core headers
INCLUDES = -I lib/include \
           -I lib/backdoor

# Core lib sources (copied from open-vm-tools, do not modify)
# Note: str.c excluded due to bsd_output.h dependency (generated file,
# not present in source tree). See lib/string/str_rose.c.
# Note: rpcChannel.c excluded - see lib/rpcChannel/rpcChannel_rose.c.
LIB_SRCS = lib/backdoor/backdoor.c              \
           lib/backdoor/backdoorGcc32.c         \
           lib/message/message.c                \
           lib/rpcOut/rpcout.c                  \
           lib/vmCheck/vmcheck.c                \
           lib/stubs/stub-debug.c               \
           lib/string/str_rose.c                \
           lib/rpcChannel/rpcChannel_rose.c

LIB_OBJS = $(LIB_SRCS:.c=.o)

# Build targets
.PHONY: all clean checkvm vmrosd

all: checkvm vmrosd

# Compile core objects
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# checkvm.exe
checkvm: $(LIB_OBJS) services/checkvm/checkvm.o
	$(CC) $(CFLAGS) -o checkvm.exe $^

services/checkvm/checkvm.o: services/checkvm/checkvm.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# vmrosd.exe
VMROSD_SRCS = services/vmrosd/vmrosd.c                      \
              services/vmrosd/mainLoop.c                     \
              services/vmrosd/pluginMgr.c                    \
              services/vmrosd/toolsRpc.c                     \
              services/plugins/guestInfo/guestInfo.c

VMROSD_OBJS = $(VMROSD_SRCS:.c=.o)

vmrosd: $(LIB_OBJS) $(VMROSD_OBJS)
	$(CC) $(CFLAGS) -o vmrosd.exe $^ -lws2_32 -liphlpapi

$(VMROSD_OBJS): %.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(LIB_OBJS) \
	      services/checkvm/checkvm.o checkvm.exe \
	      $(VMROSD_OBJS) vmrosd.exe

