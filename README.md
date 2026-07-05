# D3DVideoEncoder

`D3DVideoEncoder` は、Direct3D 上のテクスチャを動画ファイルへ書き出すための C++17 ライブラリです。

現在の実装では、**D3D11 と D3D12 のエンコーダを明確に分離**しています。

- `D3D11VideoEncoder`: 現在の正式実装。`ID3D11Texture2D` を Media Foundation backend で H.264 / HEVC MP4 へ書き出します。
- `D3D12VideoEncoder`: 将来の `NvencD3D12` / native `D3D12 Video Encode` backend 用APIです。現時点では動画出力backendは未実装です。
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
| async write | 対応。実動画テスト追加済み |
| D3D12入力API | 分離済み。backend未実装 |
| D3D12 + Media Foundation | 未対応。将来実装 |
| NVENC D3D11 | 未実装 |
| NVENC D3D12 | 未実装 |
| native D3D12 Video Encode | 未実装 |
| AV1 | 未実装。capability query上は確認項目のみ |

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

## D3D12VideoEncoder について

`D3D12VideoEncoder` はD3D11版から完全に分離されています。

現時点では、D3D12入力をD3D11共有リソースへ変換してMedia Foundationに渡す経路は削除しています。そのため、以下は未対応です。

```text
ID3D12Resource
  -> D3D12Processing
  -> D3D11 shared texture
  -> Media Foundation
```

今後のD3D12側の本命backendは以下です。

```text
ID3D12Resource
  -> D3D12Processing
  -> NV12 / P010
  -> NvencD3D12
  -> H.264 / HEVC / AV1
```

または、

```text
ID3D12Resource
  -> D3D12Processing
  -> NV12 / P010
  -> native D3D12 Video Encode
  -> H.264 / HEVC / AV1
```

現時点で `D3D12VideoEncoder` を作成すると、backend未実装を明示する `D3DVideoEncoderError` を投げます。

---

## テスト

現在のテストは、共通ユーティリティ、D3D12 backend の未実装状態、D3D11 実エンコード経路を確認します。

```text
TimestampGenerator
Types
DescValidation
HResult
EncodeJobQueue
D3D12UnsupportedBackend
D3D11EncodeSmoke
D3D11EncodeAsyncSmoke
D3D11MediaFoundationCapabilities
```

`D3D11EncodeSmoke` は、D3D11Helperで作成した `BGRA8` textureを `D3D11VideoEncoder` に渡し、Media Foundation backendで H.264 MP4 を生成します。その後、生成されたMP4を Media Foundation Source Reader で読み返し、動画streamの幅・高さ・sample数を検証します。

`D3D11EncodeAsyncSmoke` は同じ実動画出力を `asyncMode=true` / `queueDepth=2` で実行し、worker thread 経由の書き込みと `close()` 時のflushを検証します。

`D3D11MediaFoundationCapabilities` は、H.264/NV12、HEVC/NV12、HEVC/P010、AV1/NV12、AV1/P010 のMedia Foundation encoder MFTを列挙します。H.264/NV12は必須として検証し、HEVC/P010は環境依存のため対応可否を表示します。

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

1. D3D11 surface pool / async queue の細部強化
2. HEVC/P010 実エンコードテストの条件付き追加
3. NVENC D3D11 backend
4. NvencD3D12 backend
5. native D3D12 Video Encode backend
6. D3D12 + Media Foundation backend の再検討


## Recent stability improvements

The D3D11 Media Foundation backend now reports failures with encoder context such as codec, input format, frame size, frame rate, bitrate, hardware-transform settings, and the Media Foundation call that failed.  It also checks Media Foundation encoder capabilities before constructing the sink writer so unsupported HEVC/P010 environments fail with a clear message.

The D3D11 encode surface pool now tracks active surfaces with per-pool generations, waits for all outstanding surfaces during flush/close, releases surfaces if input preparation fails, and returns dropped `DropOldest` queue jobs to the encoder so their surfaces are released correctly.
