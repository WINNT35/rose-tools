# rose-tools

An open source re-implementation of open-vm-tools for ReactOS and Windows NT.

## What is rose-tools?

Rose Tools provides VMware guest services for ReactOS and Windows NT virtual
machines, built on the open-vm-tools protocol stack. Version 0.1 establishes
the core architecture and plugin infrastructure.

## Components

- **checkvm.exe** — detects whether the system is running inside a VMware
  virtual machine and reports the tools version
- **vmrosd.exe** — the Rose Tools daemon; loads plugins and manages the RPC
  channel between the guest and VMware host

## Building

**Requirements:**
- `i686-w64-mingw32-gcc` (MinGW-w64, win32 threading model)
- GNU Make

**Build:**
bash
make clean && make


Produces `checkvm.exe` and `vmrosd.exe` in the project root.

## Building the User Documentation

Note: The documentation (rosehelp.chm) must be built separately and requires 
Windows or Wine. It is not essential for building the project. The build
process will be improved in a future release.

**Requirements:**
- Microsoft HTML Help Compiler (`hhc.exe`)

**Build:**
```bash
cd help/chm
hhc.exe rosehelp.hhp
```

Produces `rosehelp.chm` in `help/chm/`. A pre-compiled copy is included
with binary releases.

## License

Rose Tools as a whole is licensed under the GNU General Public License v2.0
or later. Individual components may carry different licenses:

- `lib/` — GNU Lesser General Public License v2.1 only
- `services/` — GNU General Public License v2.0 or later

See `lib/COPYING`, `services/COPYING`, and `LICENSE` for full license texts.

## Documentation

A compiled help file (`rosehelp.chm`) is included with binary releases.
Source files for the documentation are located in `help/chm/`.

## Contributing

See the Contributing section of the Rose Tools Documentation or `help/chm/contributing.htm`.

## Acknowledgements

Rose Tools is derived from [open-vm-tools](https://github.com/vmware/open-vm-tools),
the open source implementation of VMware Tools developed by VMware.

## Repository
Source code and releases available at:
https://github.com/WINNT35/rose-tools
