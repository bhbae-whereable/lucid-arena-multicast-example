# Plan

## Goals
- Add a new example at Examples/Arena/Cpp_Multicast_Save/Cpp_Multicast_Save.cpp.
- Base multicast flow on Cpp_Multicast and keep its comments where possible.
- Reuse SaveApi usage and comments from Cpp_Save for per-frame image saving.
- No CLI options; behavior is fixed in code.
- Master: save first 10 frames only, then continue streaming until ESC is pressed.
- Listener: save first 10 frames, then stop and exit.
- Save path: {executable dir}/imgs/{program start time}/{timestampNs}-{frameId}.png.

## References to reuse comments
- Examples/Arena/Cpp_Multicast/Cpp_Multicast.cpp (multicast setup and stream loop comments)
- Examples/Arena/Cpp_Save/Cpp_Save.cpp (save flow comments and SaveApi usage)
- include/Save/ImageWriter.h (file name pattern behavior)

## Implementation Outline
1) Create Cpp_Multicast_Save.cpp by copying Cpp_Multicast structure and header.
   - Add `#include "SaveApi.h"` and required headers for filesystem/time.
   - Keep existing comment blocks from Cpp_Multicast where the logic is reused.
2) Build an output directory helper.
   - Determine executable directory (Linux: resolve /proc/self/exe).
   - Create `imgs/<run_timestamp>` once at startup (run_timestamp is program start time).
   - Keep timestamp format ASCII (e.g. YYYYMMDD_HHMMSS).
3) Add save helper based on Cpp_Save.
   - Convert to PIXEL_FORMAT (BGR8) before saving.
   - Use Save::ImageParams and Save::ImageWriter (reuse Cpp_Save comments).
   - For each saved frame, set file name pattern to the full path `{timestampNs}-{frameId}.png` and save via `writer << pConverted->GetData()`.
4) Update AcquireImages loop for master/listener behavior.
   - Detect access mode via DeviceAccessStatus.
   - Master: configure acquisition mode + stream settings (same as Cpp_Multicast).
   - Both: StartStream, GetImage loop, track imageCount and unreceivedImageCount (keep original comments).
   - When printing frame info, append the saved filename (e.g. `std::cout << " (frame ID " << frameId << "; timestamp (ns): " << timestampNs << ") - saved: " << filename;`).
   - Save only first 10 received frames; after 10, continue to requeue without saving.
   - Listener: exit loop immediately after saving 10 frames.
5) Add ESC key handling for master and listener.
   - Use non-blocking stdin (termios + fcntl) to detect ESC (27) without stopping acquisition.
   - Listener: exit after saving 10 frames or if ESC is pressed before that.
   - Restore terminal settings on exit (RAII helper or explicit cleanup).
6) Cleanup and restore.
   - Requeue buffers on every successful GetImage.
   - StopStream after loop; restore AcquisitionMode for master.
   - Keep existing exception handling and teardown from Cpp_Multicast.

## Staged Implementation and Testing
1) Baseline multicast copy
   - Create `Cpp_Multicast_Save.cpp` by copying `Cpp_Multicast.cpp` and updating the example name.
   - Test: build/run to confirm multicast behavior matches original (no saving yet).
2) Output directory helper
   - Add executable-dir resolution + run timestamp + `imgs/<run_timestamp>` creation.
   - Test: run once and verify the output directory is created.
3) Save helper wiring (single frame)
   - Add SaveApi-based conversion and writer logic; save 1 frame only.
   - Test: verify a single PNG is written and buffer is requeued.
4) Full save loop behavior
   - Save first 10 frames, print filename in the log, then follow master/listener exit rules.
   - Test: listener exits after 10 saves; master continues streaming after 10 saves.
5) ESC handling for master and listener
   - Add non-blocking ESC detection and terminal cleanup.
   - Test: press ESC during streaming to exit early; confirm listener exits on ESC or after 10 saves, and master exits only on ESC.

## Build
- Example-local: `cd Examples/Arena/Cpp_Multicast_Save && make`
- From Arena root: `cd Examples/Arena && make Cpp_Multicast_Save`
- Output binary is produced in the example folder and copied to the OutputDirectory path from `Examples/Arena/common.mk`.

## Required Files (Platform Notes)
- Linux build (makefile/common.mk flow):
  - `Cpp_Multicast_Save.cpp`: example source.
  - `makefile`: local build target definition.
  - `stdafx.h`: required because the source includes it; Linux uses it as a normal header.
  - `stdafx.cpp`, `resource.h`, `targetver.h`: not required for Linux; can be omitted.
- Windows/Visual Studio-style builds:
  - `stdafx.h` + `stdafx.cpp`: used for precompiled headers (PCH).
  - `targetver.h`: Windows SDK version selection header.
  - `resource.h`: optional unless you add Windows resources.
  - Recommendation: copy `stdafx.cpp`, `targetver.h`, and `resource.h` from `Examples/Arena/Cpp_Multicast` if you need MSVC builds.

## Checks
- Saved files appear under imgs/<run_timestamp>/ with names based on timestampNs.
- Master keeps streaming after 10 saves until ESC.
- Listener exits after 10 saves.
