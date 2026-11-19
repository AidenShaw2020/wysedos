# WINVBE and the NT5 graphics package

The 32-bit NT display miniport and XDDM driver derive from
[WINVBE](https://github.com/ages2001/WINVBE/tree/25ee72d97cc6129b79329b249cea337ae88a4e25)
at revision `25ee72d97cc6129b79329b249cea337ae88a4e25`. WINVBE is
distributed under GNU GPL version 3. Preserve the upstream authors'
copyright and licence notices. The wyseDOS changes add the WGPU
transport, framebuffer operations and a hardware Direct3D HAL.

The OpenGL ICD adapts the Velocity9x OpenGL front end to this HAL.
It uses the [Velocity9x source and notices](velocity9x.txt), with the
NT5 adapter and patches under `drivers/nt/opengl/`. The WDM transport
and NT installation helper are part of wyseDOS.

The shared driver CD and the driver ZIP contain `NT5/DISPSRC.ZIP`.
It contains the pinned WINVBE sources, corresponding driver sources,
build files, the Velocity9x source archive, and the applicable notices.
The complete GNU GPL version 3 text is provided in
[`LICENSE`](../LICENSE) and the release's licence ZIP. Microsoft Windows,
its system libraries, and the tested game files are separate works and
are not included.
