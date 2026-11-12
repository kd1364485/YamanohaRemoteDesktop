# Third-party notices

This repository's own code is under the MIT License (see [LICENSE](LICENSE)).
The following third-party material keeps its own license.

## Included in this repository

- `native/third_party/nvcodec/nvEncodeAPI.h`
  Copyright (c) 2010-2023 NVIDIA Corporation. MIT License (see the notice at the top of that file).

## Downloaded at build time (not included)

CMake `FetchContent` downloads these when you build `native/`. They are used under their own licenses.

- libdatachannel
- MbedTLS

Dependencies of the Tauri app (`src-tauri/`) and the signaling server (`signaling-server/`) are installed
with `cargo` / `npm`, and each package carries its own license.
