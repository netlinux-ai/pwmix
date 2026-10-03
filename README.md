# pwmix

A mixer-style console for PipeWire. One C++20 binary, drawn with Cairo and
Pango on a plain X11 window (no Qt or GTK), talking to PipeWire directly.

- Apps, devices and links shown as strips in Sources / Middle / Sinks columns
- Per-node volume and mute
- Application streams matched to the desktop window that owns them
  (process ancestry, X11 EWMH, MPRIS)

Inspired by [qpwgraph](https://github.com/rncbc/qpwgraph). Status: early
prototype.

## Build

    cmake -S . -B build && cmake --build build
    ./build/pwmix

Build dependencies: cmake, pkg-config, libpipewire-0.3-dev, libcairo2-dev,
libpango1.0-dev, libx11-dev, libsystemd-dev.

## Debian package

    ./packaging/build-deb.sh

builds `pwmix_<version>_amd64.deb` into `scratch/deb/`.

## License

GPL-2.0-or-later. See `LICENSE`.
