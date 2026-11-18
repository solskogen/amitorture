# Third-party licenses

The bench kernels come from benchwork (<https://github.com/codewiz/benchwork>),
whose own code is 0BSD (see [../LICENSE.benchwork](../LICENSE.benchwork)).
The code under `third_party/` keeps its own license, in the `LICENSE` file of
each directory:

* zlib (zlib License)
* FreeType's smooth rasterizer `ftgrays.c` (FreeType License, FTL)
* Dhrystone 2.1 (Reinhold Weicker's original terms) via xSysInfo
  (BSD-2-Clause)
* The NFS3 kernels in `nfs3/` (0BSD)
* OpenSSL's AES, GCM, ChaCha20 and Poly1305 code as AmiSSL ships it, in
  `amissl/` (Apache-2.0)
* The glyph outlines in `../glyphs.c` are derived from DejaVu Sans
  (Bitstream Vera license).

Portions of this software are copyright (C) 1996-2022 The FreeType Project
(www.freetype.org). All rights reserved.
