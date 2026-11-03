# Bret Johnson DOS USB drivers

Unmodified upstream archive: https://bretjohnson.us/programs/usbdos.zip

SHA-256: `45937257ac508443b8c30d31e9b492e50ee50ccc9250e7f8d831fd4aa3cf46ba`.

Copyright Bret E. Johnson, with Richard Bonner credited in the documentation.
This is copyrighted freeware under its own terms, not the project GPL.
Read the complete COPYRIGHTED FREEWARE section in USBINTRO.DOC inside the
archive. It describes free use and redistribution restrictions, including
charging and distribution of related/derivative programs.

The installer extracts the entire archive, including its documentation,
unchanged into C:\DRIVERS\USBDOS. USBON.BAT loads USBUHCIL plus USBDRIVE with
LowMemory:Yes; USBOFF.BAT unloads them before starting Windows USB drivers.
Local unpacked copies used for hardware testing are ignored by Git.
