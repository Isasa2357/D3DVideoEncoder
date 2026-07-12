# D3DVideoEncoder D3DHelper migration report

## 1. Result

- status: acceptance pass; release readiness conditional
- baseline commit: `104c07b4876b698df19347d2532e116d759b6a95`
- final local worktree: `main`, uncommitted migration changes present
- Structural migration: complete
- Build validation: complete
- Native D3D12 hardware acceptance: pass
- NVENC D3D11 hardware acceptance: pass
- NVENC D3D12 hardware acceptance: pass
- Native D3D12 long-duration test: pass
- NVENC D3D11 long-duration test: pass
- NVENC D3D12 long-duration test: pass
- Output decode validation: pass for the above backends
- Public API compatibility: maintained
- Release readiness: conditional
- GitHub changes: none
- original video files modified: no

## 2. Selected dependencies

| Repository | Tag | Commit SHA | Pinning method |
|---|---|---|---|
| D3D11Helper | `v1.13.0` | `8711cd297f8af527c2d4701f9f522e54f29ad5b8` | `FetchContent` SHA pin; local directory override retained |
| D3D12Helper | `v1.13.0` | `98e97d7b5dfa02063152549b05211d7408edc524` | `FetchContent` SHA pin; local directory override retained |

Local validation used the trusted `../extern/D3D11Helper-1.13.0` and
`../extern/D3D12Helper-1.13.0` source trees. Neither Helper source tree was modified.

## 3. Changed files

| File | Summary | Reason |
|---|---|---|
| `CMakeLists.txt` | Helper SHA pins, migrated sources, test/runtime deployment | Reproducible dependencies and complete build graph |
| `src/D3D11VideoEncoderImpl.*` | D3D11 validation and multithread protection | Preserve thread safety with current Helper ownership rules |
| `src/input/D3D11VideoInputAdapter.cpp` | `D3D11ResourceView`, validation and copy migration | Remove old Helper API assumptions |
| `src/backend/d3d12video/D3D12VideoEncodeBackend.*` | Resource, Queue, SyncPoint, Barrier, readback, capability and GOP work | Current D3D12Helper API and native encode correctness |
| `src/backend/d3d12video/D3D12VideoEncodeCapabilities.cpp` | Stable capability query storage | Remove dangling output pointers and initialize writable outputs |
| `src/backend/d3d12video/D3D12VideoEncodeDiagnostic.hpp` | Internal capability diagnostics | Debug/Release query equivalence without public API changes |
| `src/backend/d3d12video/D3D12VideoEncodeBitstreamWriter.*` | H.264 parameter-set integration and 64-bit MP4 duration | Decodable Annex B and long-duration MP4 |
| `src/backend/d3d12video/D3D12VideoEncodeH264ParameterSets.*` | Configuration-driven SPS/PPS generator | Driver output does not guarantee parameter sets |
| `src/backend/nvenc/NvencCommon.*` | Preset initialization, EOS/drain and strategy selection | SDK 13.1 session contract and correct shutdown |
| `src/backend/nvenc/NvencD3D11EncoderBackend.cpp` | D3D11 strategy integration | Preserve accepted system-memory output path |
| `src/backend/nvenc/NvencD3D12EncoderBackend.cpp` | current ResourceView/state and D3D12 output strategy integration | D3D12Helper and NVENC D3D12 correctness |
| `src/backend/nvenc/NvencD3D12OutputStrategy.*` | Registered READBACK output ring and fence lifecycle | SDK 13.1 D3D12 output contract |
| `src/backend/nvenc/NvencLifecyclePolicy.hpp` | Internal EOS/drain policy | Prevent lock after all sent frames were received |
| `src/backend/mux/NvencOutputMuxer.cpp` | MP4 version 1 duration use | Durations beyond 32-bit movie timescale |
| `src/backend/mux/NvencMp4DurationBoxes.hpp` | Internal 64-bit duration box helpers | Testable `mvhd`/`tkhd`/`mdhd` version 1 encoding |
| `sample/03_*`, `sample/04_*` | CMake/runtime deployment and unified capability diagnostics | Current Helper/runtime behavior |
| `test/CMakeLists.txt`, `test/test_*.cpp` | Capability, hardware, lifecycle, GOP, slot and duration regressions | Preserve migration invariants and acceptance cases |
| `tools/make_test_videos.*` | Deterministic generated test assets and manifest | Repeatable tests without modifying source videos |
| `README.md`, this report | Validation matrix and final audit | Final documentation |

The worktree also contains the approved task and migration specification documents. No
public file under `include/` was changed.

## 4. Adopted D3DHelper APIs

| API | Used in | Replaced code |
|---|---|---|
| `D3D11ResourceView` | D3D11 input validation/copy | Old owning/wrapper assumptions around borrowed resources |
| `D3D12ResourceView` | NVENC D3D12 and native processing input | Temporary owning wrappers and implicit state assumptions |
| detailed D3D12 Resource creation | native buffers/textures | Repeated raw committed-resource boilerplate where Helper can express the contract |
| `D3D12Queue` / `D3D12QueueSyncPoint` | video-to-direct handoff and completion | Manual generic queue/fence bookkeeping |
| `D3D12BarrierBatch` | native video/direct transitions | Repeated generic `D3D12_RESOURCE_BARRIER` construction |
| typed command-list creation / allocator context | native video/direct lists | Generic list creation/reset boilerplate |
| `D3D12ReadbackBuffer::MapRead` | metadata and bitstream readback | Raw readback range map/unmap handling |

## 5. Intentionally retained raw APIs

| API/category | File | Reason |
|---|---|---|
| `ID3D12VideoDevice3::CreateVideoEncoder/CreateVideoEncoderHeap` | native backend | Video Encode domain API; not generic Helper boilerplate |
| `ID3D12VideoEncodeCommandList::EncodeFrame/ResolveEncoderOutputMetadata` | native backend | Video Encode submission semantics |
| codec/profile/GOP/picture-control/reference descriptors | native backend | Codec policy and DPB ownership remain D3DVideoEncoder responsibilities |
| NVENC open/query/init/register/map/encode/lock/unlock/EOS APIs | NVENC backend | NVIDIA session and codec contract |
| D3D11 `nvEncCreateBitstreamBuffer` and async event APIs | NVENC D3D11 | Accepted system-memory output strategy |
| D3D12 READBACK resource, fence and persistent event | NVENC D3D12 output strategy | Exact SDK output-resource/fence contract; no Helper extension is appropriate |
| persistent processing fence wait | native/NVENC D3D12 processing | Avoid `CpuWaitPoint` implementations that allocate a Win32 event per frame |
| Media Foundation sink writer APIs | Media Foundation backend | Backend-specific encoding contract |

## 6. Synchronization proof

- processing completion: existing Direct Queue submission/signal/wait order is retained. The
  processing wait uses a backend-lifetime persistent wait object, so no per-frame event is created.
- video -> direct: Video Queue signals one `D3D12QueueSyncPoint`; Direct Queue performs
  `GpuWaitPoint` before bitstream/metadata copy.
- direct copy -> CPU map: Direct Queue copy is submitted and its completion point is CPU-waited
  before `MapRead`. `MapRead` objects remain alive through metadata parse/writer consumption.
- allocator reuse: each allocator is reset only after the fence value for its previous GPU work
  has completed.
- NVENC D3D12: each fixed output slot carries a monotonically increasing output fence value and
  transitions `Free -> Prepared -> Submitted -> FenceCompleted -> Locked -> Free`.
- count proof: the migration adds no D3D12 Queue submission, Queue signal/wait, GPU copy, or
  processing dispatch. The required NVENC output-fence wait does not allocate per frame.

## 7. Ownership / lifetime proof

- Public write calls remain borrowed-resource APIs; async job payloads retain owning `ComPtr`s.
- `ResourceView` is used only as an immediate borrowed validation/dispatch view and is not stored
  in async jobs or backend members.
- Native bitstream, metadata, reconstruction, processing and readback resources are backend-owned.
- Native picture-control/reference pointer targets are stored in frame-local backing structures
  that outlive command recording and execution completion.
- NVENC D3D12 output resources, registration handles, descriptors, fences and event are owned by
  a session-lifetime fixed ring. The vector is resized once and descriptor pointers remain stable.
- Locked/mapped/registered state is explicit; normal close and failure unwind prevent double
  unlock, unmap, unregister and destroy. Close-twice tests pass.

## 8. Resource State proof

- Cross-queue producer resources are handed off through `COMMON`; direct upload uses
  `COMMON -> COPY_DEST -> COMMON` before `write(COMMON)`.
- Native input uses `COMMON -> VIDEO_ENCODE_READ -> COMMON` when restore is requested.
- Native bitstream and metadata use `COMMON -> VIDEO_ENCODE_WRITE/READ -> COMMON` on Video Queue,
  then `COMMON -> COPY_SOURCE -> COMMON` on Direct Queue.
- Readback destinations remain `COPY_DEST`; they are mapped only after Direct Queue completion.
- Reconstruction resources have authoritative per-resource state members and alternate current
  and reference resources; a frame never uses the same resource for both roles.
- State members are updated only after the corresponding barrier has been recorded successfully.
- Native direct, processing, async and long tests completed without new Debug Layer state errors,
  device removal or deadlock.

## 9. Build / CTest

| Configuration | Result | Notes |
|---|---|---|
| Debug configure / `ALL_BUILD` | pass | all enabled, short build directory |
| Release configure / `ALL_BUILD` | pass | all enabled, short build directory |
| Debug focused regressions | pass | lifecycle, D3D12 output slot, MP4 duration, GOP-local and capability tests |
| Release focused regressions | pass | same focused set |
| Debug full CTest | 25/27 | two known Media Foundation failures |
| Release full CTest | 25/27 | two known Media Foundation failures |

The failing tests remain enabled and were not converted to skip/success.

## 10. Backend tests

| Backend | Build | Short encode | Async | MP4 | Long test | Status |
|---|---|---|---|---|---|---|
| Native D3D12 | pass | pass | pass | pass | pass | validated |
| NVENC D3D11 | pass | pass | pass | pass | pass | validated |
| NVENC D3D12 | pass | pass | pass | pass | pass | validated |
| D3D11 Media Foundation | pass | fail | unverified | fail/unverified | not run | known issues |

Hardware acceptance used NVIDIA GeForce RTX 5070 Ti. No fixed adapter LUID is recorded or
required: each process obtains `ID3D12Device::GetAdapterLuid()` and validates that current value
with `IDXGIFactory4::EnumAdapterByLuid()` and adapter identity.

## 11. Test assets

- FFmpeg: `C:\Program Files (x86)\ffmpeg-8.0.1-essentials_build\bin\ffmpeg.exe`
- FFprobe: `C:\Program Files (x86)\ffmpeg-8.0.1-essentials_build\bin\ffprobe.exe`
- manifest: `../video/generated/logs/generated_assets_manifest.json`
- short NV12 raw: 103,680,000 bytes, matching 300 frames at 640x360 NV12
- long normalized source: 19,799 frames at 30 fps
- all generated raw/output/log files are under `../video/generated` and outside Git tracking
- original `../video/long_source.mp4` and `../video/short_source.mp4` were not modified

## 12. Output equivalence

- codec/container: H.264 Annex B and MP4 validated for Native D3D12 and both NVENC backends
- dimensions: 640x360 validated by FFprobe
- frame count: short 300/300; long 19,799/19,799
- timestamps/duration: monotonic short timestamps; accepted long duration 659.966007 seconds
- decode integrity: FFmpeg null decode exit 0 with no accepted-backend decode warnings
- framemd5: native direct 30-frame acceptance produced 30 entries
- PSNR: not executed; no bit-exact or objective quality threshold was part of final acceptance
- SSIM: not executed; no bit-exact or objective quality threshold was part of final acceptance

## 13. Performance

| Metric | Baseline | Migrated | Difference |
|---|---:|---:|---:|
| D3D12 Queue submissions per frame | existing path | existing path | 0 |
| Queue signals per frame | existing path | existing path | 0 |
| Queue waits per frame | existing path | existing path | 0 |
| GPU copies per frame | existing path | existing path | 0 |
| Processing dispatches per converted frame | existing path | existing path | 0 |
| Per-frame Resource/Fence/Event allocation | 0 required | 0 | 0 |
| NVENC D3D12 output ring | unavailable/invalid old path | 4 slots in tested configuration | initialization-time fixed storage |

Handle plateau tests showed no iteration-proportional growth. NVENC D3D12 output slots are freed
after completion and do not grow with duration.

## 14. Long-duration tests

| Backend | Result | Memory | GPU memory | Frames | Notes |
|---|---|---|---|---:|---|
| Native D3D12 | pass | max 744.2 MB | max 1,753 MB | 19,799 | H.264 MP4, 659.966007 s, decode exit 0 |
| NVENC D3D11 | pass | start 20.4 MB, max 1,102.8 MB | 1,689/1,697/1,677 MB | 19,799 | H.264 MP4, decode exit 0, close success |
| NVENC D3D12 | pass | 23.7/950.2/136.8 MB start/max/end | 1,631/1,681/1,637 MB | 19,799 | H.264 MP4, decode exit 0, close success |

No long test was repeated during final documentation/audit.

## 15. Warnings / blockers / unverified items

### D3D11 Media Foundation

- Debug `D3D11EncodeSmoke`: finalize hang.
- Release `D3D11EncodeSmoke`: `0x887A0005`.
- Debug/Release `D3D11HevcP010Smoke`: `0xc00d6d76`.
- These are pre-migration known runtime issues, separate from the accepted Helper migration.

### MP4 muxer memory scalability

- The muxer retains samples until close and constructs final `mdat` data at finalization.
- At about 660 seconds, maximum working set was approximately 0.95-1.1 GB for NVENC long tests.
- Memory is released at/after close and the behavior is not an NVENC output-slot leak.
- Long-duration or continuous recording memory use therefore grows with duration.
- A streaming MP4 muxer is explicitly deferred to a separate phase.

Release readiness remains conditional on these known issues. Media Foundation tests remain active.

## 16. Definition of Done checklist

### Dependency

- [x] Latest stable D3D11Helper/D3D12Helper tags and SHAs recorded.
- [x] Remote dependencies pinned to immutable commit SHAs.
- [x] Local directory override retained.

### API / behavior

- [x] Public API unchanged.
- [x] Backend, codec and container scope unchanged.
- [x] Timestamp, duration, async behavior, ownership and public error policy maintained.

### Migration

- [x] D3D11 ResourceView, validation and copy migration complete.
- [x] D3D12 ResourceView and explicit-state validation migration complete.
- [x] Native Queue, SyncPoint, Barrier, typed command list and Readback migration complete.
- [x] Persistent processing CPU wait prevents per-frame event allocation.
- [x] Native H.264 GOP-local numbering and parameter-set output validated.
- [x] Native/NVENC MP4 version 1 duration handling validated.
- [x] NVENC preset initialization, EOS drain and D3D12 registered-output strategy validated.
- [x] D3D11 multithread protection retained.

### Test / process

- [x] Deterministic test asset generator and manifest present.
- [x] Debug/Release build and focused regressions pass.
- [x] Accepted hardware backends pass short, async, MP4, decode and long tests.
- [x] Debug Layer introduced no new accepted-backend errors.
- [x] Performance, memory, retained raw APIs and known failures documented.
- [x] Generated assets are outside Git; original videos are unchanged.
- [x] Helper repositories and GitHub were not modified.
- [ ] PSNR/SSIM were not executed; final acceptance used decode, dimensions, frames, duration,
  timestamps and framemd5 criteria instead.

Final classification: the requested migration and three hardware backend acceptances are complete;
repository-wide release readiness is conditional because of the separately documented Media
Foundation failures and MP4 muxer memory scalability risk.
