# d3d12_stereo_sync sample

This sample opens two Media Foundation cameras through the D3D12 backend, pairs
frames by timestamp, previews the left/right concatenated image, and optionally
records the concatenated stream as H.264/MP4.

## Usage

```bat
out\build\default\Debug\d3d12_stereo_sync.exe ^
  <leftIndex> <rightIndex> <width> <height> <fpsNum> <fpsDen> <subtype> <shaderDir> [pairCount] [outputMp4|-] [bitrate]
```

Example:

```bat
out\build\default\Debug\d3d12_stereo_sync.exe 0 1 1920 1080 60 1 NV12 out\build\default\_deps\d3d12helper-src\shaders\D3D12Processing 300 stereo_sync_output.mp4 50000000
```

`outputMp4` can be `-` to disable recording.

## Preview colour handling

The preview path reads the final D3D12 RGBA8 resources back to a CPU BGRA8
canvas.  The MP4 writer receives that same canvas as Media Foundation RGB32.
The preview window renders the BGRA8 canvas through GDI using explicit BGRA bit
masks and HALFTONE scaling.  This avoids colour aliasing or channel ambiguity in
some 32-bit `BI_RGB` / `StretchDIBits` paths when a large stereo image is
scaled down for preview.
