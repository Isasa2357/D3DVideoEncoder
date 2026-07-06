# D3DVideoEncoder

`D3DVideoEncoder` は、Direct3D 上のテクスチャを動画ファイルへ書き出すための C++17 ライブラリです。

現在の実装では、**D3D11 と D3D12 のエンコーダを明確に分離**しています。

- `D3D11VideoEncoder`: 現在の正式実装。`ID3D11Texture2D` を Media Foundation backend で H.264 / HEVC MP4 へ書き出します。
- `D3D12VideoEncoder`: `NvencD3D12` / native `D3D12 Video Encode` backend 用APIです。NVENC有効時は、制約付きで `NvencD3D12` backend を使用できます。
- `D3DVideoEncoder`: 移行用の互換ラッパです。新規コードでは `D3D11VideoEncoder` / `D3D12VideoEncoder` を直接使ってください。

> 以前検討していた `D3D12Resource -> D3D11 shared texture -> Media Foundation` 経路は、D3D12/D3D11共有リソース互換性の問題が大きいため削除しました。D3D12 + Media Foundation backend は将来実装扱いです。

---

## 現在の対応状況

| 項目 | 状態 |
|---|---:|
| D3D11 `ID3D11Texture2D` 入力 | 対応 |
| D3D11 `NV12` 入力 | 対応 |
| D3D11 `P010` 入力 | HEVC向けに対応 |
| D3D11 `BGRA8` / `RGBA8` / `RGBA16F` 入力 | D3D11Processingで `NV12` / `P010` へ変換 |
| Media Foundation backend | D3D11専用で対応 |
| H.264 MP4 | 対応 |
| HEVC MP4 | 環境依存で対応。`QueryMediaFoundationCapabilities()` で事前確認 |
| async write | D3D11/D3D12ともに対応。D3D11実動画テスト、D3D12 queueテスト、NVENC D3D12 async smoke、native D3D12 Video Encode async smokeで確認 |
| D3D12入力API | D3D11版から分離済み |
| D3D12 + Media Foundation | 未対応。将来実装 |
| NVENC D3D11 | `D3DVIDEOENCODER_ENABLE_NVENC=ON` 時に対応。`.h264` / `.h265` elementary stream と `.mp4` / `.mkv` mux出力に対応。resource/register pool化済み |
| NVENC D3D12 | `D3DVIDEOENCODER_ENABLE_NVENC=ON` 時に対応。NV12/P010直接入力、またはD3D12ProcessingによるRGB→NV12/P010変換。`.h264` / `.h265` / `.mp4` / `.mkv` 出力に対応。async write と resource/register pool化済み |
| native D3D12 Video Encode | `D3DVIDEOENCODER_ENABLE_D3D12_VIDEO_ENCODE=ON` 時に対応。H.264/NV12直接入力、D3D12ProcessingによるRGBA8/BGRA8/RGBA16F→NV12変換、`.h264` / `.mp4` / `.mkv` 出力、async writeに対応。HEVC/P010、B-frameは未対応 |
| AV1 | NVENC SDK/GPU依存。初期実装ではH.264/HEVCを主対象 |

---

## 依存関係

このライブラリは、Direct3Dの定型処理を自作Helperへ寄せる方針です。

- D3D11Helper: <https://github.com/Isasa2357/D3D11Helper>
- D3D12Helper: <https://github.com/Isasa2357/D3D12Helper>

CMakeでは `FetchContent` により、未指定ならGitHubから自動取得します。

---

## 必要環境

- Windows 10 / 11
- Visual Studio 2019以降
- CMake 3.20以降
- Direct3D 11対応GPU
- Media Foundation H.264 encoder MFT
- D3D11Processingを使う場合は `dxcompiler.dll` / `dxil.dll`
- NVENC backendを使う場合は NVIDIA GPU / NVIDIA display driver / NVIDIA Video Codec SDK の `nvEncodeAPI.h`
- `NvencD3D12` でRGB入力を使う場合は D3D12Processing 用の `dxcompiler.dll` / `dxil.dll`

---

## ビルド

CMDプロンプトで、リポジトリルートから実行します。

```bat
rmdir /s /q out

cmake -S . -B out/build/default ^
  -DD3DVIDEOENCODER_BUILD_SAMPLE=ON ^
  -DD3DVIDEOENCODER_BUILD_TESTS=ON

cmake --build out/build/default --config Release
ctest --test-dir out/build/default -C Release --output-on-failure
```

DXC runtime が自動検出されない場合は、明示指定します。

```bat
rmdir /s /q out

cmake -S . -B out/build/default ^
  -DD3DVIDEOENCODER_BUILD_SAMPLE=ON ^
  -DD3DVIDEOENCODER_BUILD_TESTS=ON ^
  -DD3DVIDEOENCODER_DXC_RUNTIME_DIR="C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"

cmake --build out/build/default --config Release
ctest --test-dir out/build/default -C Release --output-on-failure
```

configure時に以下のように表示されれば、DXC runtimeを検出できています。

```text
D3DVideoEncoder: found dxcompiler.dll: ...
D3DVideoEncoder: found dxil.dll: ...
```

---

## ローカルのD3DHelperを使う場合

GitHubから取得せず、手元の `D3D11Helper` / `D3D12Helper` を使う場合は以下のように指定します。

```bat
rmdir /s /q out

cmake -S . -B out/build/default ^
  -DD3DVIDEOENCODER_BUILD_SAMPLE=ON ^
  -DD3DVIDEOENCODER_BUILD_TESTS=ON ^
  -DD3DVIDEOENCODER_D3D11HELPER_DIR="C:/Work/libs/D3D11Helper" ^
  -DD3DVIDEOENCODER_D3D12HELPER_DIR="C:/Work/libs/D3D12Helper"

cmake --build out/build/default --config Release
ctest --test-dir out/build/default -C Release --output-on-failure
```

---

## D3D11VideoEncoder の使用例

```cpp
#include <D3DVideoEncoder/D3D11VideoEncoder.hpp>

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <D3D11Helper/D3D11Framework/D3D11Framework.hpp>

using namespace D3DVideoEncoderLib;
using namespace D3D11CoreLib;

int main() {
    auto core = D3D11Core::CreateShared();

    constexpr uint32_t width = 1280;
    constexpr uint32_t height = 720;

    auto texture = CreateTexture2D(
        *core,
        width,
        height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        D3D11_BIND_SHADER_RESOURCE,
        D3D11_USAGE_DEFAULT,
        0);

    D3D11VideoEncoderDesc desc;
    desc.outputPath = L"output.mp4";
    desc.width = width;
    desc.height = height;
    desc.frameRateNum = 60;
    desc.frameRateDen = 1;
    desc.backend = D3DVideoEncoderBackendType::MediaFoundation;
    desc.codec = VideoCodec::H264;
    desc.internalFormat = VideoPixelFormat::NV12;
    desc.bitrate = 40'000'000;
    desc.input.core = core.get();
    desc.input.inputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.input.processingShaderDirectory = L"shaders/D3D11Processing";

    D3D11VideoEncoder encoder(desc);

    // texture を更新したあと、毎フレーム呼ぶ
    encoder.write(texture.AsTexture2D());

    encoder.close();
}
```

---

## Media Foundation capability query

HEVC encoder や P010(Main10) 入力は、Windows環境やインストール済みMedia Foundation MFTに依存します。
そのため、HEVC/P010を使う前に capability query で確認できます。

```cpp
const auto caps = D3DVideoEncoderLib::D3D11VideoEncoder::QueryMediaFoundationCapabilities();

if (caps.supportsHevcP010()) {
    // desc.codec = VideoCodec::HEVC;
    // desc.internalFormat = VideoPixelFormat::P010;
} else {
    // H.264/NV12 などへfallback
}
```

個別の組み合わせだけ確認することもできます。

```cpp
const auto hevcP010 = D3DVideoEncoderLib::D3D11VideoEncoder::QueryMediaFoundationSupport(
    D3DVideoEncoderLib::VideoCodec::HEVC,
    D3DVideoEncoderLib::VideoPixelFormat::P010);

if (hevcP010.supported) {
    // HEVC/P010 のMedia Foundation encoder MFTが見つかった
}
```

---

## NVENC backend を有効にする場合

NVENC backendはデフォルトではビルドしません。NVIDIA Video Codec SDK の `Interface/nvEncodeAPI.h` を指定して有効化します。

```bat
rmdir /s /q out

cmake -S . -B out/build/default ^
  -DD3DVIDEOENCODER_BUILD_SAMPLE=ON ^
  -DD3DVIDEOENCODER_BUILD_TESTS=ON ^
  -DD3DVIDEOENCODER_ENABLE_NVENC=ON ^
  -DD3DVIDEOENCODER_NVENC_INCLUDE_DIR="C:/SDK/Video_Codec_SDK/Interface" ^
  -DD3DVIDEOENCODER_DXC_RUNTIME_DIR="C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"

cmake --build out/build/default --config Release
ctest --test-dir out/build/default -C Release --output-on-failure
```

または、`NV_CODEC_SDK_ROOT` 環境変数をVideo Codec SDKのルートへ向けることもできます。

```bat
set "NV_CODEC_SDK_ROOT=C:/SDK/Video_Codec_SDK"
```

現在のNVENC backendは、NVENCから得た圧縮bitstreamを内部muxerへ渡します。出力拡張子に応じて以下を選びます。

```text
.h264 / .h265 : elementary stream
.mp4          : minimal MP4 mux
.mkv          : minimal Matroska mux
```

MP4/MKV muxはH.264/HEVCを対象にします。AV1 muxは今後の実装対象です。

---

## NvencD3D11 の使用例

```cpp
D3D11VideoEncoderDesc desc;
desc.outputPath = L"output.mp4";
desc.width = width;
desc.height = height;
desc.frameRateNum = 60;
desc.frameRateDen = 1;
desc.backend = D3DVideoEncoderBackendType::NvencD3D11;
desc.codec = VideoCodec::H264;
desc.internalFormat = VideoPixelFormat::NV12;
desc.bitrate = 40'000'000;
desc.input.core = core.get();
desc.input.inputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
desc.input.processingShaderDirectory = L"shaders/D3D11Processing";

D3D11VideoEncoder encoder(desc);
encoder.write(texture);
encoder.close();
```

`NvencD3D11` では、入力が `BGRA8` / `RGBA8` / `RGBA16F` の場合、既存の `D3D11Processing` で `NV12` / `P010` に変換してからNVENCへ渡します。

---

## NvencD3D12 の使用例

`NvencD3D12` は、D3D11版とは完全に分離されたD3D12-native backendです。
入力がすでに `NV12` / `P010` の場合はそのままNVENCへ渡し、`RGBA8` / `BGRA8` / `RGBA16F` の場合は D3D12Processing で `NV12` / `P010` に変換してからNVENCへ渡します。

```cpp
D3D12VideoEncoderDesc desc;
desc.outputPath = L"output.mp4";
desc.width = width;
desc.height = height;
desc.frameRateNum = 60;
desc.frameRateDen = 1;
desc.backend = D3DVideoEncoderBackendType::NvencD3D12;
desc.codec = VideoCodec::H264;
desc.internalFormat = VideoPixelFormat::NV12;
desc.bitrate = 40'000'000;
desc.input.core = core.get();
desc.input.inputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
desc.input.processingShaderDirectory = L"shaders/D3D12Processing";

D3D12VideoEncoder encoder(desc);
encoder.write(texture.Get(), currentState);
encoder.close();
```

現在の制約です。

```text
- 出力は .h264 / .h265 の elementary stream、または .mp4 / .mkv mux
- asyncMode=true の場合、D3D12 resourceは内部queueが ComPtr で保持し、worker threadでencodeする
- D3D12Processingを使う場合、内部でDirectQueueへ変換コマンドを投入し、NVENC投入前にfence待ちする
- 入力がNV12/P010直接入力の場合も、write() に渡された currentState から内部で COMMON へ遷移できる
- 入力がRGB系の場合、write()へ渡された currentState から変換し、変換後scratchをCOMMONでNVENCへ渡す
- NVENC resource registration はセッション内でpool化し、同じresourceを毎フレームregister/unregisterしない
```

---

## NvencD3D12 crop / resize / state management

D3D12入力サイズとエンコード出力サイズを分けられます。未指定時は従来通り `desc.width x desc.height` の全体入力として扱います。

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

この場合は `1920x1080` の入力テクスチャから `(320,180)-(1600,900)` を切り出し、必要に応じて `1280x720` へresizeしてから `NV12/P010` へ変換します。

直接 `NV12/P010` を渡す場合も、呼び出し側が必ず `COMMON` に遷移しておく必要はなくなりました。`write(resource, currentState)` に現在stateを渡すと、encoder側で `COMMON` へ遷移し、既定ではencode後に元stateへ戻します。復帰が不要な場合は以下を指定できます。

```cpp
desc.input.restoreStateAfterEncode = false;
```

制約として、現時点のcrop/resizeはRGB系入力向けです。直接 `NV12/P010` 入力に対するcrop/resizeは未対応です。

---

## Variable duration / explicit timestamps

固定fpsの通常 `write()` に加えて、明示timestampとdurationを指定できます。
可変fps、欠落フレームを含む入力、外部clockに同期した録画ではこの overload を使います。

```cpp
// D3D11
encoder.write(texture, timestamp100ns, duration100ns);

// D3D12
encoder.write(resource, currentState, timestamp100ns, duration100ns);
```

`timestamp100ns` は従来通り単調増加が必須です。`duration100ns` は正の値である必要があります。
Media Foundation backendでは `IMFSample::SetSampleDuration()` に渡され、NVENC backendではMP4/MKV muxのsample durationへ反映されます。`.h264` / `.h265` elementary stream出力ではコンテナがないためduration情報は保持されません。

---

## D3D12VideoEncoder について

`D3D12VideoEncoder` はD3D11版から完全に分離されています。

現時点では、D3D12入力をD3D11共有リソースへ変換してMedia Foundationに渡す経路は削除しています。そのため、以下は未対応です。

```text
ID3D12Resource
  -> D3D12Processing
  -> D3D11 shared texture
  -> Media Foundation
```

D3D12側の現時点の本命backendは以下です。

```text
ID3D12Resource
  -> D3D12Processing
  -> NV12 / P010
  -> NvencD3D12
  -> H.264 / HEVC / AV1
```

native `D3D12VideoEncode` backendは、H.264/NV12の実エンコード経路に対応しています。入力がすでに `NV12` の場合は直接encodeし、`RGBA8` / `BGRA8` / `RGBA16F` の場合は D3D12Processing で `NV12` へ変換してからencodeします。`asyncMode=true` の場合は既存の D3D12 async queue が `ID3D12Resource` を `ComPtr` で保持し、worker threadで native D3D12 Video Encode backend へ投入します。

```bat
cmake -S . -B out/build/default ^
  -DD3DVIDEOENCODER_ENABLE_D3D12_VIDEO_ENCODE=ON
```

現在の制約です。

```text
- codec は H.264 のみ
- internalFormat は NV12 のみ
- 出力は .h264 / .264 / .mp4 / .mkv
- RGB入力は RGBA8 / BGRA8 / RGBA16F から D3D12Processing で NV12 へ変換
- HEVC/P010、AV1、B-frame は未対応
- direct NV12/P010 入力に対する crop/resize は未対応
```

`D3DVIDEOENCODER_ENABLE_NVENC=OFF` の場合、`NvencD3D12` は未有効backendとして `D3DVideoEncoderError` を投げます。`MediaFoundation` はD3D12では引き続き未対応です。

---

## テスト

現在のテストは、共通ユーティリティ、D3D12 backend の未実装状態、D3D11 実エンコード経路を確認します。

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
D3D12VideoEncodeCapabilities
D3D12VideoEncodeH264Nv12Smoke
D3D12VideoEncodeRgbaH264Mp4Smoke
D3D12VideoEncodeH264Nv12AsyncSmoke
```

`D3D11EncodeSmoke` は、D3D11Helperで作成した `BGRA8` textureを `D3D11VideoEncoder` に渡し、Media Foundation backendで H.264 MP4 を生成します。その後、生成されたMP4を Media Foundation Source Reader で読み返し、動画streamの幅・高さ・sample数を検証します。

`D3D11EncodeAsyncSmoke` は同じ実動画出力を `asyncMode=true` / `queueDepth=2` で実行し、worker thread 経由の書き込みと `close()` 時のflushを検証します。`D3D12EncodeJobQueue` はD3D12 async queueのDropOldest/DropNewestとdrain動作を検証します。

`D3D11MediaFoundationCapabilities` は、H.264/NV12、HEVC/NV12、HEVC/P010、AV1/NV12、AV1/P010 のMedia Foundation encoder MFTを列挙します。H.264/NV12は必須として検証し、HEVC/P010は環境依存のため対応可否を表示します。

### Optional NVENC / HEVC tests

`D3DVIDEOENCODER_ENABLE_NVENC=ON` の場合、以下のNVENCテストも追加されます。NVENC runtimeや対応GPUがない場合はskipします。

```text
NvencCapabilities
NvencD3D11H264Mp4Smoke
NvencD3D12RgbaH264Mp4Smoke
```

`NvencD3D12RgbaH264Mp4Smoke` は `asyncMode=true` で実行し、D3D12ProcessingによるRGBA→NV12変換、D3D12 worker queue、NVENC resource/register pool、MP4 muxを同時に通します。

`D3D12VideoEncodeRgbaH264Mp4Smoke` は native D3D12 Video Encode backendで `RGBA8 -> D3D12Processing -> NV12 -> H.264 -> MP4` を通します。`D3D12VideoEncodeH264Nv12AsyncSmoke` は `asyncMode=true` / `queueDepth=2` で direct NV12 input を worker thread 経由でH.264 MP4へ出力します。native D3D12 Video Encode非対応環境ではskipします.

また、通常テストに `D3D11HevcP010Smoke` を追加しています。これはMedia Foundation capability queryでHEVC/P010 encoderが見つかる環境だけ実エンコードを行い、未対応環境ではskipします。


これらの実エンコードテストは、実際にD3D11 GPU処理、D3D11Processingによる `BGRA8 -> NV12` 変換、Media Foundation Sink Writer、Source Reader readback を通るため、`dxcompiler.dll` / `dxil.dll` と Media Foundation H.264 encoder が必要です。

---

## ディレクトリ構成

```text
include/D3DVideoEncoder/
  D3D11VideoEncoder.hpp
  D3D12VideoEncoder.hpp
  D3DVideoEncoder.hpp            # migration wrapper
  D3DVideoEncoderDesc.hpp
  D3DVideoEncoderTypes.hpp
  D3DVideoEncoderError.hpp

src/
  D3D11VideoEncoder.cpp
  D3D11VideoEncoderImpl.cpp
  D3D12VideoEncoder.cpp
  D3D12VideoEncoderImpl.cpp
  D3DVideoEncoder.cpp            # migration wrapper
  backend/
  input/
  surface/
  async/
  util/

sample/
  01_encode_d3d11_texture/

test/
```

---

## GitHubへpushする前に

`.gitignore` は、CMake/MSBuild出力、`out/`、FetchContentの `_deps/`、生成動画、Visual Studio中間ファイルを除外します。

テスト後でも、通常は以下でpushできます。

```bat
git status
git add .
git commit -m "Initial D3D11 video encoder implementation"
git push
```

`out/` や生成された `.mp4` はコミット対象から除外されます。

---

## 今後の実装予定

優先順位は以下です。

1. native D3D12 Video Encode backend の HEVC/P010 対応
2. native D3D12 Video Encode backend のB-frame / 高度なGOP対応
3. D3D12 + Media Foundation backend の再検討
4. AV1 mux / AV1詳細tuning


## Recent stability improvements

The D3D11 Media Foundation backend now reports failures with encoder context such as codec, input format, frame size, frame rate, bitrate, hardware-transform settings, and the Media Foundation call that failed.  It also checks Media Foundation encoder capabilities before constructing the sink writer so unsupported HEVC/P010 environments fail with a clear message.

The D3D11 encode surface pool now tracks active surfaces with per-pool generations, waits for all outstanding surfaces during flush/close, releases surfaces if input preparation fails, and returns dropped `DropOldest` queue jobs to the encoder so their surfaces are released correctly. D3D12VideoEncoder now supports async write with a ComPtr-backed D3D12 job queue, and NVENC DirectX resource registration is pooled for the lifetime of the encoder session. Variable-duration `write(..., timestamp100ns, duration100ns)` overloads are available for D3D11, D3D12, and the compatibility wrapper.
