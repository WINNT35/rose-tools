# Rose Tools — Glossary

This document explains terms and concepts that appear throughout the
Rose Tools codebase.

---

## The VMware Backdoor

The backdoor is how the guest (your VM) talks to the VMware host without
going through the network. It uses a special x86 I/O port (`0x5658`,
the ASCII value of "VX") that VMware intercepts at the hypervisor level.
When the guest executes an `IN` instruction on this port with specific
values in the CPU registers, VMware reads those registers and responds
by writing values back into them.

This is deliberately invisible to the guest OS: it looks like a normal
I/O port instruction, but VMware silently handles it before the OS ever
sees it.

The backdoor protocol uses CPU registers to pass information in both
directions: it is not a function call, it is a register convention.
Before executing the backdoor instruction you load values into specific
registers; after it executes you read values out of those same registers.
For GETVERSION specifically: ECX carries the command in, EBX carries
BDOOR_MAGIC (`0x564D5868`, the ASCII value of "VMXh") out if VMware
responded, and EAX carries the version number out.

The pre-checks in the example below matter: before the call, whatever
garbage happens to be in EBX or ECX from previous code could look like
a valid VMware response by coincidence. The code deliberately sets EBX
to `~BDOOR_MAGIC` (the bitwise inverse: guaranteed not to be BDOOR_MAGIC)
and ECX high half to `0xFFFF` (not a valid VMX type) so that if those
values are unchanged after the call, you know VMware did not respond
rather than getting a lucky coincidence from leftover register state.

```c
/* From vmcheck.c - detecting whether we are inside a VMware VM */
Backdoor_proto bp;

ASSERT(version);
ASSERT(type);

/* Make sure EBX does not contain BDOOR_MAGIC */
bp.in.size = (size_t)~BDOOR_MAGIC;
/* Make sure ECX does not contain any known VMX type */
bp.in.cx.halfs.high = 0xFFFF;
bp.in.cx.halfs.low = BDOOR_CMD_GETVERSION;
Backdoor(&bp);

if (bp.out.ax.word == 0xFFFFFFFF) {
   /*
    * No backdoor device there. This code is not executing in a VMware
    * virtual machine. --hpreg
    */
   return FALSE;
}
if (bp.out.bx.word != BDOOR_MAGIC) {
   return FALSE;
}
/* bp.out.ax.word now contains the VMware version */
```

The backdoor is the foundation everything else in Rose Tools is built on.
RpcOut, RpcIn, and all the higher-level channels ultimately talk through
it.

---

## RPCI / RpcOut (Outbound RPC)

RPCI stands for Remote Procedure Call Interface. It is the protocol
Rose Tools uses to send messages *from the guest to the host*. In the
codebase this is handled by `rpcout.c` (imported from open-vm-tools) and
wrapped by `rpcChannel_rose.c`.

A typical outbound message looks like this:

```c
/* From guestInfo.c - sending the guest IP to the host */
/* Primary IP - send as info-set guestinfo.ip */
GetPrimaryIp(ip, sizeof ip);
if (ip[0] != '\0' &&                  /* got a valid IP */
    (g_cachedIp == NULL ||            /* first time sending */
     strcmp(g_cachedIp, ip) != 0)) {  /* IP changed since last send */
   char buf[128];
   char *reply   = NULL;
   size_t repLen = 0;
   _snprintf(buf, sizeof buf, "info-set guestinfo.ip %s", ip);
   buf[sizeof buf - 1] = '\0';        /* ensure null termination */
   if (RpcChannel_Send(ctx->rpc, buf, strlen(buf),
                       &reply, &repLen)) {  /* send to host */
      if (reply != NULL && reply[0] == '\0') {  /* empty reply = success */
         free(g_cachedIp);
         g_cachedIp = _strdup(ip);    /* cache the new IP */
      }
      /* non-empty reply = host-side error; silently retry next tick */
   }
   /* send failure: silently retry next tick.
    * if the channel is persistently broken, ROSE_SIG_RESET will fire
    * and the channel will be reset at a higher level. */
   free(reply);                       /* always free the reply */
}
```

The host receives this, acts on it, and sends back a reply string.
A reply starting with `"1 "` means success. A reply starting with `"0 "`
means failure, with an error message following.

**RpcChannel** is the Rose Tools wrapper around RpcOut. It manages the
channel lifecycle (open, send, close) and is what plugins use via
`ctx->rpc`.

---

## TCLO / RpcIn (Inbound RPC)

TCLO stands for Tools Command Line Output. While RPCI is guest→host,
TCLO is host→guest.

The guest polls the host periodically by sending an empty message on the
TCLO channel. If the host has a command queued (e.g. `Resolution_Set
1920 1080`), it sends it back in the reply. The guest dispatches the
command to the registered handler and sends back a response.

In Rose Tools this is handled by `rpcin.c` (imported from open-vm-tools)
and driven by `RoseToolsRpc_Poll()` in the main loop. `RoseToolsRpc_Poll()`
is called on every main loop tick and asks the host "do you have anything
for me?" Without it, no handler would ever fire regardless of
registration.

Note that in C89 the handler function must be defined before the
registration call since C89 does not allow forward references to static
functions without a prototype. In resolutionSet.c the handler is defined
first, then `ResolutionSet_Init` registers it further down the file.

```c
/* From resolutionSet.c - registering a TCLO handler */
if (ctx->rpcIn != NULL) {
   RpcIn_RegisterCallback(ctx->rpcIn, RESOLUTION_SET_CMD,
                          ResolutionSetTcloHandler, ctx);
}
```

**RpcIn** is the inbound channel handle, stored as `ctx->rpcIn`.

Here is what a TCLO handler actually looks like:

```c
/* From resolutionSet.c - a TCLO handler.
 * The host sends "Resolution_Set <width> <height>", rpcin.c dispatches
 * it here. result/resultLen carry the reply back to the host. */
static unsigned int
ResolutionSetTcloHandler(char const **result,    /* reply string out */
                         size_t      *resultLen, /* reply length out */
                         const char  *name,      /* command name, unused here */
                         const char  *args,      /* "<width> <height>" from host */
                         size_t       argsSize,  /* length of args, unused here */
                         void        *clientData)/* registered client data, unused here */
{
   unsigned int width  = 0;
   unsigned int height = 0;
   int          ok;

   (void)name;        /* suppress unused parameter warnings */
   (void)argsSize;
   (void)clientData;

   if (args == NULL || argsSize == 0) {  /* host sent no arguments */
      return RpcIn_SetRetVals(result, resultLen,
                              "Invalid arguments", FALSE); /* FALSE = failure */
   }

   if (sscanf(args, "%u %u", &width, &height) != 2) { /* parse width and height */
      fprintf(stderr, "resolutionSet: failed to parse args: '%s'\n", args);
      return RpcIn_SetRetVals(result, resultLen,
                              "Invalid arguments", FALSE);
   }

   ok = ResolutionSet(width, height);  /* apply the resolution change */
   return RpcIn_SetRetVals(result, resultLen,
                           ok ? "" : "Resolution_Set failed", /* empty string = success */
                           ok ? TRUE : FALSE);
}
```

---

## RoseAppCtx

The application context. Passed to every plugin on load and to every
callback. Contains everything a plugin needs to interact with the rest
of the system:

```c
typedef struct RoseAppCtx {
    RoseCoreAPI         version;      /* API version */
    const char         *name;         /* service name ("toolbox") */
    int                 isVMware;     /* running in a VM? */
    int                 running;      /* main loop keep-alive flag */
    int                 errorCode;    /* non-zero = exit with error */
    RoseConfig         *config;       /* parsed tools.conf */
    RoseSignalRegistry *signals;      /* signal dispatch table */
    RpcChannel         *rpc;          /* outbound channel (guest->host) */
    struct RpcIn       *rpcIn;        /* inbound channel (host->guest) */
} RoseAppCtx;
```

Plugins must not store a pointer to the context beyond the lifetime of
the plugin itself; the context is owned by vmrosd.

---

## RosePluginData

The struct a plugin returns from its `RoseOnLoad` entry point. Tells
vmrosd everything it needs to know about the plugin:

```c
typedef struct RosePluginData {
    const char  *name;              /* plugin name, e.g. "timeSync" */
    RoseAppReg   regs[32];          /* TCLO handlers, signal subscriptions */
    int          regCount;
    int  (*init)(RoseAppCtx *ctx);  /* called after RPC channel is up */
    int  (*tick)(RoseAppCtx *ctx);  /* called periodically */
    void (*shutdown)(RoseAppCtx *ctx);
    DWORD        tickIntervalMs;    /* how often tick() is called */
    void        *_private;          /* plugin-managed private data */
} RosePluginData;
```

If `RoseOnLoad` returns NULL, the plugin is skipped. If `init()` returns
zero, the plugin is unloaded.

---

## RoseOnLoad

The single exported entry point every plugin DLL must provide. This is
what `pluginMgr.c` looks for via `GetProcAddress` when loading a DLL.

```c
/* Every plugin must export exactly this function */
ROSE_MODULE_EXPORT RosePluginData *RoseOnLoad(RoseAppCtx *ctx);
```

This is Rose Tools' equivalent of `ToolsOnLoad` in open-vm-tools.

---

## Signals (RoseSignalRegistry)

Signals are the internal event system used to notify plugins of things
happening in vmrosd: reset, shutdown, capabilities registration, etc.
They replace GObject signals from open-vm-tools. Signals are triggered
internally by vmrosd, usually in response to something the host did;  a
reset command, a capabilities request, or a shutdown instruction. The host
never sees signals directly; they are vmrosd's way of broadcasting host
events to all interested plugins at once.

A plugin subscribes to a signal during `RoseOnLoad`:

```c
RoseRegisterSignal(ctx->signals, ROSE_SIG_RESET,
                   MyPlugin_OnReset, pluginData);
```

vmrosd emits signals at appropriate points:

```c
RoseEmitSignal(ctx->signals, ROSE_SIG_RESET, ctx);
```

Common signals:

| Signal name | When it fires |
|---|---|
| `ROSE_SIG_RESET` | VMware reset / resume from suspend |
| `ROSE_SIG_CAPABILITIES` | Host requesting capability registration |
| `ROSE_SIG_SHUTDOWN` | vmrosd is shutting down |
| `ROSE_SIG_SET_OPTION` | Host sent a Set_Option command |
| `ROSE_SIG_CONF_RELOAD` | tools.conf was reloaded |

---

## The "toolbox" Channel Name

When vmrosd registers itself with the VMware host it uses the service
name `"toolbox"`, not `"vmrosd"` or `"rose-tools"`. This is intentional
and important.

`"toolbox"` is the legacy name used by older VMware Tools and is accepted
by all known VMware versions. Newer versions of open-vm-tools switched to
`"vmtoolsd"`, but VMware maintains backwards compatibility so `"toolbox"`
still works on newer VMware versions as well. If `"vmtoolsd"` were used
instead, capability registration would silently fail on older VMware
versions and the guest would appear to have no tools capabilities at all.

```c
/* From rose_plugin.h */
#define ROSE_GUEST_SERVICE  "toolbox"   /* do not change this */
```

Rose Tools uses `"toolbox"` deliberately to maximise compatibility across
all supported VMware versions.

*Note: compatibility across all VMware versions has not been fully
verified. If you encounter capability registration issues on a specific
version, this is the first thing to check.*

---

## Capabilities_Register

A TCLO command sent by the VMware host to the guest after a reset. The
host is asking: "what can you do?" The guest responds by sending a series
of `tools.capability.*` RPC messages back to the host.

```c
/* Example capability advertisement from resolutionSet */
RpcChannel_Send(ctx->rpc,
    "tools.capability.resolution_server 1",
    strlen("tools.capability.resolution_server 1"),
    &reply, &replyLen);
```

If a capability is not advertised, the host will not send the
corresponding commands. For example, if `resolution_server` is not
advertised, the host will never send `Resolution_Set`.

---

## ROSE_SIG_RESET

The most important signal in Rose Tools. Fired when:
- vmrosd first starts up and establishes the RPC channel
- The VM resumes from suspend
- VMware resets the tools channel for any reason

Plugins that need to re-register capabilities or re-sync state (like
timeSync correcting the clock after a suspend) hook this signal.

```c
RoseRegisterSignal(ctx->signals, ROSE_SIG_RESET,
                   TimeSync_OnReset, data);
```

---

## GLib Replacements

open-vm-tools was written with GLib as a core dependency. Rose Tools
removes GLib entirely. Here is the mapping:

| GLib | Rose Tools replacement |
|---|---|
| `GMainLoop` | Win32 event loop managed by vmrosd |
| `GArray` | Fixed-size static arrays (`ROSE_MAX_REGISTRATIONS`) |
| `GKeyFile` | `RoseConfig` - INI parser in `roseConfig.c` |
| `GObject signals` | `RoseSignalRegistry` in `rose_plugin.h` |
| `gboolean` | `int` |
| `gchar` | `char` |
| `gpointer` | `void *` |
| `g_malloc` / `g_free` | `malloc` / `free` |

The fixed-size arrays mean there are hard limits (e.g. 32 registrations
per plugin, 64 signal handlers system-wide). These limits are defined in
`rose_plugin.h` and can be raised if needed.

---

## RpcIn_OldCallback

A renamed typedef. In open-vm-tools there are two different callback
signatures named `RpcIn_Callback`: one in `rpcin.c` (old-style,
unsigned int return) and one in `guestrpc.h` (new-style, gboolean
return). They are different types used in different dispatch paths.

Rose Tools renames the old-style one to `RpcIn_OldCallback` to avoid
the collision:

```c
/* Old-style: registered via RpcIn_RegisterCallback */
typedef unsigned int (*RpcIn_OldCallback)(char const **result,
                                          size_t      *resultLen,
                                          const char  *name,
                                          const char  *args,
                                          size_t       argsSize,
                                          void        *clientData);
```

If you see a TCLO handler function with this signature, it is an
old-style handler registered directly with RpcIn.

---

## pluginMgr

The plugin manager (`pluginMgr.c`). Responsible for:
1. Scanning the plugin directory for `*.dll` files
2. Sorting them alphabetically (mirrors upstream GDir sort order)
3. Calling `GetProcAddress` to find `RoseOnLoad` in each DLL
4. Calling `RoseOnLoad` and storing the returned `RosePluginData`
5. Deferring `FreeLibrary` until after RPC shutdown (important: see below)

The deferred `FreeLibrary` is significant. In v0.1 DLLs were unloaded
inside `RosePluginMgr_Shutdown`, before the TCLO channel was closed.
This left signal handlers pointing into freed memory, causing a crash or
stale channel on next launch. The fix splits shutdown into two phases:
`RosePluginMgr_Shutdown` (unregister callbacks) and `RosePluginMgr_Free`
(FreeLibrary), with Free called only after `RoseToolsRpc_Shutdown`.

---

*Work in progress. Add missing concepts via GitHub issues or pull requests.*