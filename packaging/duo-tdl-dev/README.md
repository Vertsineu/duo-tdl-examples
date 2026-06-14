# duo-tdl-dev packaging

This directory contains the Debian package metadata for Duo TDL development
library packages.

- `package.conf` defines package defaults and architecture-specific source
  library directories.
- `DEBIAN/control.in` is rendered by the build script with the current version and
  installed size.
- Maintainer scripts in `DEBIAN/` are copied into the package root with executable
  permissions.

Load the build environment, then build the package from the repository root:

```sh
source envsetup.sh
scripts/build-package.sh
```

The script uses the product, chip, and architecture selected by `envsetup.sh`.
The package name is generated as `duo-tdl-dev-<product>`, for example
`duo-tdl-dev-duo`, `duo-tdl-dev-duo256`, or `duo-tdl-dev-duos`.
System libraries are packaged from `libs/system`. `libini.so` and
`libwiringx.so` are installed to the Debian multiarch library directory, such as
`/usr/lib/riscv64-linux-gnu`; the other system libraries are installed to
`/mnt/system/lib`.

TDL libraries are packaged from `libs/tdl`. `libcvi_rtsp.so` and `libopencv*`
are installed to `/mnt/system/usr/lib`; the remaining `libcvi*`/`libcv*`
libraries are installed to `/mnt/system/lib`.

Generated package roots and `.deb` files are written under `dist/`.
