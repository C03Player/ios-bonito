# bonito userland GPU stack (Metal on the Adreno 615)

iOS draws through Metal. The kernel side (IOAcceleratorES + a user client, `xnu/patches/0156`, boot-args `sweetgpu=6 sweetmtl=2`) gives userland the
GPU; this directory builds the userland that uses it, on a macOS CI runner (`.github/workflows/userland.yml`, artifact `userland-gpu`):

    Metal (QuartzCore, SpringBoard, apps) -> SweetMetal2.bundle (device + frontend) -> metal2vulkan bridge (Rust: AIR -> SPIR-V, Metal API on Vulkan)
      -> Mesa Turnip (Vulkan for Adreno, built for iOS with a "darwin" kernel backend) -> the kernel user client (NativeAdreno ABI v1)

Everything here except `setup-sources.sh` is copied from ios-flame (MIT, Copyright (c) 2026 dipa-studio / ios-flame contributors) @ 745c4341, which got
the home screen rendering on the Pixel 4 with the same kernel ABI:

- `phone/metal2/` `SweetMetal2.mm`, `NativeBridgeMetalFrontend.{h,mm}` (UniXNU's native Metal frontend, patched for iOS 16), `bridge/` (UniXNU's
  bridge `NativeMetalBridge.rs` and `NativeMetalObjects.*`, MIT, plus patches 0001-0006 from UniXNU and 0100+ from ios-flame for metal2vulkan)
- `phone/turnip/` `NativeAdrenoABI.h` (UniXNU), smoke tests `vktest`/`v2test`, one Mesa patch
- `tools/build-turnip-ios.sh`, `tools/build-m2v-bridge.sh`

Fetched at build time, never committed (`setup-sources.sh`): UniXNU 31506f7e (patch list + Mesa pin; BSD-2-Clause-Patent), Mesa 85fda3b3
(MIT, gitlab.freedesktop.org) + UniXNU's `patches/mesa`, metal2vulkan 720ef1ce (steelbrain, GPL-2.0-or-later OR LGPL-3.0-or-later).
The Adreno 615 is identified by the kernel (chip id 0x06010500, GMEM 512 KB); Mesa/Turnip support it (a6xx gen1).
