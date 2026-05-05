# rose-tools
An open source re-implementation of open-vm-tools for ReactOS and Windows NT.

## What is rose-tools?
Rose Tools provides VMware guest services for ReactOS and Windows NT virtual
machines, built on the open-vm-tools protocol stack. Version 0.2 significantly
expands the v0.1 core with a dynamic plugin architecture, Windows service
integration, configuration file support, and two new plugins.

## Components
- **checkvm.exe** — detects whether the system is running inside a VMware
  virtual machine and reports the tools version
- **vmrosd.exe** — the Rose Tools daemon; loads plugins dynamically and manages
  the RPC channel between the guest and VMware host
- **guestInfo.dll** — collects and reports guest system information to the
  VMware host
- **timeSync.dll** — synchronizes the guest system clock with the VMware host
- **resolutionSet.dll** — allows the VMware window to resize the guest display
  (disabled by default, see documentation)

## Building
**Requirements:**
- `i686-w64-mingw32-gcc` (MinGW-w64, win32 threading model)
- GNU Make

**Build:**
```bash
make clean && make
```
Produces `checkvm.exe`, `vmrosd.exe`, `guestInfo.dll`, `timeSync.dll`, and
`resolutionSet.dll`.

**Tests:**
```bash
make test
```
Produces `testconfig.exe`, `testrpc.exe`, and `testpluginmgr.exe`. Tests can
be run on Wine or a Windows machine.

## Building the User Documentation
Note: The documentation (rosehelp.chm) must be built separately and requires
Windows or Wine. It is not essential for building the project.

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
or later. The license structure is nuanced — individual files may carry
different licenses depending on their origin. Always refer to the license
header of the individual source file or the `COPYING` file in the relevant
directory as the authoritative source. The general breakdown is:

- `lib/` — GNU Lesser General Public License v2.1 only
- `services/` — GNU General Public License v2.0 or later; files sufficiently
  derivative of open-vm-tools carry dual attribution and are licensed under
  LGPL v2.1 only instead
- `tests/` — GNU General Public License v2.0 or later
- `help/` — MIT License

See the License page in the documentation for the full breakdown.

## Documentation
A compiled help file (`rosehelp.chm`) is included with binary releases.
Source files are located in `help/chm/`. Developer documentation is located
in `docs/`.

## Contributing
See `docs/` for developer documentation including the glossary and architecture
notes. See `help/chm/contributing.htm` or the Contributing section of the
compiled help file for contribution guidelines.

## Acknowledgements
Rose Tools is derived from [open-vm-tools](https://github.com/vmware/open-vm-tools),
the open source implementation of VMware Tools developed by VMware.

The [OSDev Wiki article on VMware Tools](https://wiki.osdev.org/VMware_tools)
was a useful reference for the guest protocol implementation on setResolution.dll

## Repository
Source code and releases available at:
https://github.com/WINNT35/rose-tools
