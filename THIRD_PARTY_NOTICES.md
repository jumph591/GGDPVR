# Third-party notices

GGDPVR includes, or is built with, the following third-party software. Each keeps its own license; the full license
texts are in the folders listed.

| Component | Used for | License | Where |
|---|---|---|---|
| [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) 1.1.63 (Khronos Group) | the OpenXR loader (linked statically) and headers | Apache License 2.0 (some files under other permissive licenses, listed in its `LICENSES/`) | `third_party/OpenXR-SDK/LICENSE`, `third_party/OpenXR-SDK/LICENSES/` |
| [MinHook](https://github.com/TsudaKageyu/minhook) (Tsuda Kageyu) | function hooking | BSD 2-Clause | `third_party/minhook/LICENSE.txt` |
| [RenderDoc](https://renderdoc.org) in-application API header (Baldur Karlsson) | optional frame captures for developers (only used when the game is started under RenderDoc) | MIT | header of `third_party/renderdoc/renderdoc_app.h` |

The released binaries also link the Microsoft C/C++ runtime statically (Visual Studio 2022), which is redistributable
under the Visual Studio license terms.

GGDPVR contains no files from Gal\*Gun: Double Peace. "Gal\*Gun" and "Gal\*Gun: Double Peace" are trademarks of their
respective owners; the GGDPVR logo is fan art based on the game's logo.
