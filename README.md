# Cpp_Multicast_Save

Multicast acquisition example based on Arena SDK samples. Saves the first 10 frames to disk using an async save worker.

## Behavior
- Master (ReadWrite): enables multicast, streams until ESC; saves first 10 frames.
- Listener (ReadOnly): does not change device settings; exits after 10 saves or ESC.
- Output path: `{exe_dir}/imgs/{run_timestamp}/{timestampNs}-{frameId}.png`.
- Buffers are requeued immediately after copying to reduce drops.
- Multicast group join/leave is performed in code (no `ip addr add ... autojoin`).

## Requirements
- Arena SDK installed (headers, libs, and examples tree).
- This folder is expected to live under `ArenaSDK_Linux_ARM64/Examples/Arena/Cpp_Multicast_Save` so the makefile can include `../common.mk`.
- Linux environment (uses `/proc/self/exe` and `termios` for ESC handling).

## Getting Started
1. Download Arena SDK from the LUCID website.
2. Extract the SDK (example path: `~/Downloads/ArenaSDK_Linux_ARM64`).
3. Move into the examples folder:
   ```
   cd ~/Downloads/ArenaSDK_Linux_ARM64/Examples/Arena
   ```
4. Clone this repo into `Cpp_Multicast_Save`:
   ```
   git clone <repo-url> Cpp_Multicast_Save
   cd Cpp_Multicast_Save
   ```
   The default `origin` remote is sufficient.

## Build
From this folder:
```
make
```

## Run
```
./Cpp_Multicast_Save eno1
```

## Notes
- Press ESC to stop; requires a TTY.
- Pass the interface name (e.g. `eno1`) as the first argument.
- If you clone this repo outside the SDK tree, update the include/lib paths in `makefile` or adjust the folder location.
- Runtime outputs (images, binaries, objects) are ignored via `.gitignore`.
