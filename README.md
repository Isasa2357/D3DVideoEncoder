# D3DVideoEncoder

`D3DVideoEncoder` は、Direct3D 上のテクスチャを H.264 / HEVC 動画へ書き出すための C++17 ライブラリです。

現在の設計では、**D3D11 と D3D12 のエンコーダを明確に分離**しています。

- `D3D11VideoEncoder`: D3D11 `ID3D11Texture2D` 入力を Media Foundation または NVENC でエンコードします。
- `D3D12VideoEncoder`: D3D12 `ID3D12Resource` 入力を `NvencD3D12` または native `D3D12 Video Encode` backend でエンコードします。
- `D3DVideoEncoder`: 移行用の互換ラッパです。新規コードでは `D3D11VideoEncoder` / `D3D12VideoEncoder` を直接使ってください。

`D3D12Resource -> D3D11 shared texture -> Media Foundation` 経路は、D3D12/D3D11 共有リソース互換性の問題が大きいため、現在は正式経路から外しています。D3D12 では `NvencD3D12` または native `D3D12VideoEncode` を使う方針です。

---

## 現在の対応状況

| 項目 | 状態 |
|---|---|
| D3D11 `ID3D11Texture2D` 入力 | 対応 |
| D3D11 `NV12` 入力 | 対応 |
| D3D11 `P010` 入力 | HEVC向けに対応 |
| D3D11 `BGRA8` / `RGBA8` / `RGBA16F` 入力 | D3D11Processingで `NV12` / `P010` へ変換 |
| Media Foundation backend | D3D11専用で対応 |
| D3D12入力API | D3D11版から分離済み |
| D3D12 + Media Foundation | 未対応。将来再検討 |
| NVENC D3D11 | `D3DVIDEOENCODER_ENABLE_NVENC=ON` 時に対応 |
| NVENC D3D12 | `D3DVIDEOENCODER_ENABLE_NVENC=ON` 時に対応 |
| native D3D12 Video Encode | `D3DVIDEOENCODER_ENABLE_D3D12_VIDEO_ENCODE=ON` 時に v0.1 実用範囲として対応 |
| async write | D3D11 / D3D12 / NVENC / native D3D12 Video Encode で対応 |
| AV1 | NVENC capability query は対応。実運用・muxは future work |

---

## native D3D12 Video Encode v0.1 の対応範囲

native `D3D12VideoEncode` backend は、Windows / GPU / driver の D3D12 Video Encode 対応に依存します。使用前に `QueryD3D12VideoEncodeSupport()` または capability dump sample で確認してください。

```text
対応済み:
  - H.264 / NV12
  - H.264 / RGBA8, BGRA8, RGBA16F -> D3D12Processing -> NV12
  - H.264 .h264 / .264 elementary stream
  - H.264 .mp4 / .mkv minimal mux
  - HEVC / NV12
  - HEVC / P010
  - HEVC .h265 / .265 elementary stream
  - HEVC .mp4 / .mkv minimal mux
  - direct NV12/P010 input
  - direct NV12/P010 crop/resize
  - RGB-like input crop/resize
  - asyncMode=true
  - capability query
  - smoke test群
```

```text
意図的に未対応:
  - B-frame 実エンコード
  - AV1 native D3D12 Video Encode
  - D3D12 + Media Foundation backend
  - full-featured MP4/MKV muxer
  - driverがVPS/SPS/PPSを出さない場合のparameter-set injection
```

`bFrameCount > 0` は現在の backend では明確な `D3DVideoEncoderError` として拒否します。`D3D12VideoEncodeFrameScheduler` は将来の B-frame 実装に向けた **experimental/internal planning layer** です。現時点では backend 本体には接続していません。

---

## 依存関係

- D3D11Helper: <https://github.com/Isasa2357/D3D11Helper>
- D3D12Helper: <https://github.com/Isasa2357/D3D12Helper>

CMakeでは `FetchContent` により、未指定なら GitHub から自動取得します。

必要環境です。

```text
- Windows 10 / 11
- Visual Studio 2019以降
- CMake 3.20以降
- Direct3D 11 / 12 対応GPU
- Media Foundation H.264 encoder MFT
- D3D11Processing / D3D12Processing を使う場合は dxcompiler.dll / dxil.dll
- NVENC backendを使う場合は NVIDIA GPU / NVIDIA display driver / NVIDIA Video Codec SDK
- native D3D12 Video Encode を使う場合は GPU/driver 側の D3D12 Video Encode support
```

---

## ビルド

基本ビルドです。

```bat
rmdir /s /q out

cmake -S . -B out/build/default ^
  -DD3DVIDEOENCODER_BUILD_SAMPLE=ON ^
  -DD3DVIDEOENCODER_BUILD_TESTS=ON

cmake --build out/build/default --config Release
ctest --test-dir out/build/default -C Release --output-on-failure
```

D3D12 native backend と NVENC も有効にする場合です。

```bat
rmdir /s /q out

cmake -S . -B out/build/default ^
  -DD3DVIDEOENCODER_BUILD_SAMPLE=ON ^
  -DD3DVIDEOENCODER_BUILD_TESTS=ON ^
  -DD3DVIDEOENCODER_ENABLE_NVENC=ON ^
  -DD3DVIDEOENCODER_ENABLE_D3D12_VIDEO_ENCODE=ON ^
  -DD3DVIDEOENCODER_NVENC_INCLUDE_DIR="C:/Work/library/Video_Codec_SDK_13.1.15/Interface" ^
  -DD3DVIDEOENCODER_DXC_RUNTIME_DIR="C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"

cmake --build out/build/default --config Release
ctest --test-dir out/build/default -C Release --output-on-failure
```

ローカルの D3DHelper を使う場合です。

```bat
rmdir /s /q out

cmake -S . -B out/build/default ^
  -DD3DVIDEOENCODER_BUILD_SAMPLE=ON ^
  -DD3DVIDEOENCODER_BUILD_TESTS=ON ^
  -DD3DVIDEOENCODER_D3D11HELPER_DIR="C:/Work/libs/D3D11Helper" ^
  -DD3DVIDEOENCODER_D3D12HELPER_DIR="C:/Work/libs/D3D12Helper" ^
  -DD3DVIDEOENCODER_DXC_RUNTIME_DIR="C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"

cmake --build out/build/default --config Release
ctest --test-dir out/build/default -C Release --output-on-failure
```

---

## native D3D12 Video Encode capability query

```cpp
const auto h264 = D3DVideoEncoderLib::D3D12VideoEncoder::QueryD3D12VideoEncodeSupport(
    core.get(),
    D3DVideoEncoderLib::VideoCodec::H264,
    D3DVideoEncoderLib::VideoPixelFormat::NV12,
    1920,
    1080);

if (!h264.supported) {
    // h264.message に非対応理由が入る
}
```

まとめて確認する場合です。

```cpp
const auto caps = D3DVideoEncoderLib::D3D12VideoEncoder::QueryD3D12VideoEncodeCapabilities(
    core.get(), 1920, 1080);

if (caps.supportsH264Nv12()) { /* H.264/NV12 */ }
if (caps.supportsHevcNv12()) { /* HEVC/NV12 */ }
if (caps.supportsHevcP010()) { /* HEVC/P010 */ }
```

---

## D3D12VideoEncoder: H.264 RGB -> MP4

```cpp
D3D12VideoEncoderDesc desc;
desc.outputPath = L"output_h264.mp4";
desc.width = 1280;
desc.height = 720;
desc.frameRateNum = 60;
desc.frameRateDen = 1;
desc.backend = D3DVideoEncoderBackendType::D3D12VideoEncode;
desc.codec = VideoCodec::H264;
desc.internalFormat = VideoPixelFormat::NV12;
desc.bitrate = 12'000'000;
desc.gopLength = 60;
desc.bFrameCount = 0;
desc.input.core = core.get();
desc.input.inputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
desc.input.allowFormatConversion = true;
desc.input.processingShaderDirectory = L"shaders/D3D12Processing";
desc.input.restoreStateAfterEncode = true;

D3D12VideoEncoder encoder(desc);
encoder.write(rgbaTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
encoder.close();
```

入力が `RGBA8` / `BGRA8` / `RGBA16F` の場合、内部で D3D12Processing により `NV12` へ変換してから native D3D12 Video Encode へ渡します。

---

## D3D12VideoEncoder: HEVC P010 -> MP4

```cpp
D3D12VideoEncoderDesc desc;
desc.outputPath = L"output_hevc_p010.mp4";
desc.width = 1280;
desc.height = 720;
desc.frameRateNum = 60;
desc.frameRateDen = 1;
desc.backend = D3DVideoEncoderBackendType::D3D12VideoEncode;
desc.codec = VideoCodec::HEVC;
desc.internalFormat = VideoPixelFormat::P010;
desc.bitrate = 16'000'000;
desc.gopLength = 60;
desc.bFrameCount = 0;
desc.input.core = core.get();
desc.input.inputFormat = DXGI_FORMAT_P010;
desc.input.restoreStateAfterEncode = true;

D3D12VideoEncoder encoder(desc);
encoder.write(p010Texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
encoder.close();
```

HEVC MP4/MKV mux は bitstream 内の VPS/SPS/PPS から `hvcC` / CodecPrivate を構築します。driver が VPS/SPS/PPS を出さない場合は `.h265` 出力を使うか、将来の parameter-set injection 実装が必要です。

---

## crop / resize

D3D12入力サイズとエンコード出力サイズを分けられます。

```cpp
D3D12VideoEncoderDesc desc;
desc.width = 1280;
desc.height = 720;
desc.input.sourceWidth = 1920;
desc.input.sourceHeight = 1080;
desc.input.sourceRect = { 320, 180, 1280, 720 };
desc.input.resizeFilter = VideoProcessingFilter::Linear;
desc.input.preferFusedResize = true;
```

現在は RGB-like 入力、および direct `NV12` / `P010` 入力の crop/resize に対応しています。direct YUV420 crop/resize は、既存 D3D12Processing を使い、内部的に `YUV420 -> RGBA8 scratch -> NV12/P010 encode scratch` の2段処理で実行します。

---

## Variable duration / explicit timestamps

固定fpsの通常 `write()` に加えて、明示timestampとdurationを指定できます。

```cpp
encoder.write(resource, currentState, timestamp100ns, duration100ns);
```

`timestamp100ns` は単調増加、`duration100ns` は正の値である必要があります。MP4/MKV muxでは sample duration に反映されます。`.h264` / `.h265` elementary streamではコンテナがないため duration 情報は保持されません。

---

## Samples

`D3DVIDEOENCODER_BUILD_SAMPLE=ON` で以下をビルドします。

```text
sample/01_encode_d3d11_texture
  D3D11 BGRA8 texture -> D3D11VideoEncoder -> H.264 MP4

sample/02_d3d12_video_encode_h264_rgb_mp4
  D3D12 RGBA8 texture -> D3D12Processing -> NV12 -> native D3D12 Video Encode -> H.264 MP4

sample/03_d3d12_video_encode_hevc_p010_mp4
  D3D12 P010 texture -> native D3D12 Video Encode -> HEVC MP4

sample/04_d3d12_video_encode_capability_dump
  D3D12 Video Encode と NVENC の capability を表示
```

D3D12 sample は `D3DVIDEOENCODER_ENABLE_D3D12_VIDEO_ENCODE=ON` のときだけ追加されます。

実行例です。

```bat
out\build\default\sample\02_d3d12_video_encode_h264_rgb_mp4\Release\D3DVideoEncoder_02_d3d12_video_encode_h264_rgb_mp4.exe output.mp4 "C:\Work\libs\D3D12Helper\shaders\D3D12Processing"

out\build\default\sample\03_d3d12_video_encode_hevc_p010_mp4\Release\D3DVideoEncoder_03_d3d12_video_encode_hevc_p010_mp4.exe output_hevc.mp4

out\build\default\sample\04_d3d12_video_encode_capability_dump\Release\D3DVideoEncoder_04_d3d12_video_encode_capability_dump.exe 1920 1080
```

---

## Tests

通常テストです。

```text
TimestampGenerator
Types
DescValidation
HResult
EncodeJobQueue
D3D12EncodeJobQueue
D3D12UnsupportedBackend
D3D11EncodeSmoke
D3D11EncodeAsyncSmoke
D3D11MediaFoundationCapabilities
D3D11HevcP010Smoke
```

NVENC有効時の追加テストです。

```text
NvencCapabilities
NvencD3D11H264Mp4Smoke
NvencD3D12RgbaH264Mp4Smoke
```

native D3D12 Video Encode有効時の追加テストです。

```text
D3D12VideoEncodeCapabilities
D3D12VideoEncodeH264Nv12Smoke
D3D12VideoEncodeRgbaH264Mp4Smoke
D3D12VideoEncodeH264Nv12AsyncSmoke
D3D12VideoEncodeNv12CropResizeSmoke
D3D12VideoEncodeHevcP010Smoke
D3D12VideoEncodeHevcP010Mp4Smoke
D3D12VideoEncodeFrameScheduler
```

対応GPU/driverがない場合、native D3D12 Video Encode 系の実エンコードテストは skip 扱いで正常終了する設計です。

---

## ディレクトリ構成

```text
include/D3DVideoEncoder/
  D3D11VideoEncoder.hpp
  D3D12VideoEncoder.hpp
  D3DVideoEncoder.hpp
  D3DVideoEncoderDesc.hpp
  D3DVideoEncoderTypes.hpp
  D3DVideoEncoderError.hpp

src/
  D3D11VideoEncoder.cpp
  D3D11VideoEncoderImpl.cpp
  D3D12VideoEncoder.cpp
  D3D12VideoEncoderImpl.cpp
  backend/
    d3d12video/
    mux/
    nvenc/
  input/
  surface/
  async/
  util/

sample/
  01_encode_d3d11_texture/
  02_d3d12_video_encode_h264_rgb_mp4/
  03_d3d12_video_encode_hevc_p010_mp4/
  04_d3d12_video_encode_capability_dump/

test/
```

---

## 今後の実装候補

v0.1としては、native D3D12 Video Encode の I/P-frame backend は一度完成扱いにします。次の候補は以下です。

```text
v0.2以降:
  - B-frame 実エンコード
  - AV1 native D3D12 Video Encode
  - AV1 MP4/MKV mux
  - driver別 parameter-set injection
  - D3D12 + Media Foundation backend の再検討
  - readback / mux / async queue の高度なring化
```

B-frame scheduler はすでに internal planning layer として追加済みですが、backend本体へは未接続です。

---

## GitHubへpushする前に

```bat
git status
git add .
git commit -m "Finalize native D3D12 video encode v0.1 docs and samples"
git push
```

`out/` や生成された `.mp4` / `.mkv` / `.h264` / `.h265` は `.gitignore` で除外される想定です。
