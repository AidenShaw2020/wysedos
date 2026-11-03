# Generic USB storage packages

See [the provenance and licensing notice](../../../LICENSES/nusb.txt).
The SE collection is by Maximus Decim; the FE adaptation credits Q@PassingBy.
No blanket redistribution licence was found in the supplied readme/INF.
Download availability and attribution do not establish such permission.
No open-source licence or source-code availability is claimed for NUSB.
The same notice is shipped as `C:\DRIVERS\NUSB-LICENSE.TXT`.

These are preserved upstream packages, not wyseDOS binaries. The installer
builder verifies their SHA-256 hashes and extracts both FE and SE packages
into separate directories under the new guest disk's `C:\DRIVERS`.

| Package | Source | SHA-256 |
|---|---|---|
| NUSB 3.20 FE English | external-reference-removed | `3579a694d67ca33e2659770fd99dff42aecc8b24f206224f2e92a1e5dede907a` |
| NUSB 3.6 SE English | external-reference-removed | `42b13ce440cc6528600a7b085226050d855c50199ecde6cbd6f31cbf83a69621` |
| Generic Win95 OSR2.1 | external-reference-removed | `c10130c9a8f0f320a0522b63bae8b360e2b97a645ca591892d6b71892e581dbf` |

Install only the package matching the installed Windows version. NUSB replaces
system components as well as storage drivers; its English components can
change parts of a localized Windows installation. These third-party packages
include Microsoft/SanDisk files and retain their original licensing terms;
the project's GPL does not relicense them.

Read/write and virtual reconnect were tested with the full FE package on
Windows 98 FE 4.10.1998. SE and Win95 are included for those operating systems,
without claiming hardware testing on them. Physical hotplug qualification is
recorded separately in the test report; do not infer it from enumeration.
