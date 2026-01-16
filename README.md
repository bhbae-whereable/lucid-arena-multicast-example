# Cpp_Multicast_Save

Multicast example based on Arena SDK samples. Work in progress.

## Requirements
- Arena SDK installed (headers, libs, and examples tree).
- This folder is expected to live under `ArenaSDK_Linux_ARM64/Examples/Arena/Cpp_Multicast_Save` so the makefile can include `../common.mk`.

## Build
From this folder:
```
make
```

## Run
```
./Cpp_Multicast_Save
```

## Notes
- If you clone this repo outside the SDK tree, update the include/lib paths in `makefile` or adjust the folder location.
- Runtime outputs (images, binaries, objects) are ignored via `.gitignore`.

