#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Minimal UE3 runtime reflection for GG2Game (engine build 10897, x64, 4-byte struct packing).
// No object/property offsets are hard-coded: everything is derived at runtime from invariants
// and logged, so the result doubles as verification.
namespace ggdpvr::ue3 {

struct Layout {
    uintptr_t gnames = 0, gobjects = 0;        // addresses of the TArray headers in .data
    uint32_t nameStr = 0;                      // offset of the string in FNameEntry
    int32_t nameIndexField = -1;               // offset of Index field (bit0 = unicode) or -1
    bool nameWideDefault = false;
    uint32_t objIndex = 0, objOuter = 0, objName = 0, objClass = 0;
    uint32_t fieldNext = 0;                    // == sizeof(UObject)
    uint32_t structSuper = 0, structChildren = 0;
    uint32_t propArrayDim = 0, propElementSize = 0, propOffset = 0;
    uint32_t boolBitMask = 0, objPropClass = 0, structPropStruct = 0;
    bool reflectionReady = false;
};

bool BootstrapObjects();      // GNames + GObjects + UObject header offsets
bool BootstrapReflection();   // UField/UStruct/UProperty offsets
const Layout& L();

int32_t NumNames();
std::string Name(int32_t index, int32_t number = 0);
int32_t NumObjects();
uintptr_t ObjectAt(int32_t i);
bool IsObject(uintptr_t p);

std::string ObjName(uintptr_t obj);        // "Name" (+ _N)
std::string PathName(uintptr_t obj);       // "Outer.Outer.Name"
std::string FullName(uintptr_t obj);       // "ClassName Outer.Name"
uintptr_t ClassOf(uintptr_t obj);
uintptr_t OuterOf(uintptr_t obj);
uintptr_t SuperOf(uintptr_t strct);        // needs reflection
bool IsA(uintptr_t obj, const char* className);
// Finds by path, e.g. "Engine.Camera" (optionally restricted to a class name like "Class").
uintptr_t FindObject(const char* path, const char* className = nullptr);
std::string DescribeIfObject(uintptr_t p);  // "" if p is not a live UObject

struct PropInfo {
    uintptr_t prop = 0;
    std::string name, type, owner;  // owner = declaring struct
    uint32_t offset = 0, elementSize = 0, arrayDim = 0, bitMask = 0;
};
// All properties of a struct/class including supers, sorted by offset.
std::vector<PropInfo> Properties(uintptr_t strct);
// Property covering byte offset `off` in instances of `cls` (searches supers).
bool PropertyAt(uintptr_t cls, uint32_t off, PropInfo& out);
bool FindProperty(uintptr_t cls, const char* name, PropInfo& out);

void DumpReflection(const std::wstring& path);
}  // namespace ggdpvr::ue3
