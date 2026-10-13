#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Everything that depends on the exact GG2Game.exe build lives here.
// Verified build: PE TimeDateStamp 0x57FC3FE2, SizeOfImage 0x352D000
// (SHA-256 5B28CFC6719426725317C5B8C7646C8F7B81A10504CE06E08E7AE9733FCEBFF1).
namespace ggdpvr::game {

namespace rva {
// Render resources (E1b)
constexpr uint32_t FRenderResource_Dtor = 0x409230;     // FRenderResource::~FRenderResource
constexpr uint32_t FRenderResource_InitResource = 0x40B380;
constexpr uint32_t FRenderResource_ReleaseResource = 0x40B430;
constexpr uint32_t GIsCriticalError = 0x3128C9C;        // UBOOL
constexpr uint32_t GIsRHIInitialized = 0x325B9F4;       // UBOOL
constexpr uint32_t VFData_Dtor = 0x96F6E0;              // struct { TIndirectArray x4 @0/10/20/30; sub @40 }
constexpr uint32_t SkelMeshLOD_Dtor = 0x972990;         // per-LOD element (0x154 bytes)
constexpr uint32_t SkelMeshObject_Dtor = 0x97A890;      // owner (FDeferredCleanupInterface, vtbl 0x251A0C0)
constexpr uint32_t SkelMeshObject_VTable = 0x251A0C0;
// Engine
constexpr uint32_t GEngine = 0x3265CA0;
constexpr uint32_t GWorld = 0x326A018;                 // UWorld* (loaded by AActor::execTrace)
constexpr uint32_t UWorld_SingleLineCheck = 0x6B9150;  // (FCheckResult&, AActor* src, const FVector& end, const FVector& start,
                                                       //  DWORD flags, const FVector& extent, void*) -> UBOOL (0 = hit)
constexpr uint32_t ProcessEvent_VSlot = 0x220;          // byte offset in UObject vtable (index 68)
constexpr uint32_t FindFunctionChecked = 0x220EE0;
constexpr uint32_t CalcSceneView = 0x7F58F0;
constexpr uint32_t GameViewportClient_Draw = 0x7FA710;
}  // namespace rva

bool Init();                    // resolves base, validates the build; false => do not hook anything
uintptr_t Base();
inline uintptr_t Addr(uint32_t r) { return Base() + r; }
uint32_t ToRva(uintptr_t a);    // 0 if outside the module
bool InImage(uintptr_t a);
bool InRData(uintptr_t a);      // .rdata (vtables)
bool InText(uintptr_t a);
void DataRange(uintptr_t& beg, uintptr_t& end);  // .data section
// Checks that the code at `rva` starts with `expected` (guards every hook site).
bool VerifyBytes(uint32_t rva, const uint8_t* expected, size_t n);

// SEH-protected memory reads.
bool SafeRead(const void* p, void* out, size_t n);
template <typename T> bool Read(uintptr_t p, T& out) { return SafeRead(reinterpret_cast<const void*>(p), &out, sizeof(T)); }

// "rva1 rva2 ..." for frames inside GG2Game.exe (others as module+off)
std::string StackRvas(int skip, int max);

std::wstring ModDir();          // ...\Binaries\Win64\GGDPVR
// The process's main thread (= UE3 game thread): the earliest-created thread of this process.
// Do not use the DllMain thread: under injectors (RenderDoc) static-import DllMains run elsewhere.
uint32_t MainThreadId();
}  // namespace ggdpvr::game
