# Vendored libwebp (decode + demux only)

This is a trimmed-down copy of Google's libwebp (https://github.com/webmproject/libwebp),
containing only the decoder and animation-demuxer source files needed to decode a WebP
image (including extracting a single frame from an animated WebP) on-device. Encoder,
muxer, and CPU-specific SIMD source files (SSE2/SSE41/NEON/MIPS/MSA - none apply to the
ESP32's Xtensa core) were intentionally excluded to keep flash usage down; only the
portable C fallback implementations are included.

License: BSD-style, see COPYING. See PATENTS for the accompanying patent grant.

Vendored from upstream commit: see `git -C /path/to/libwebp log -1` at vendoring time
(webmproject/libwebp, default branch, cloned during this change).

Do not hand-edit files under src/ - if you need a newer libwebp, re-vendor using the same
file list documented in this repo's firmware widget change history rather than patching
these copies directly.
