#include "objects.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "../core/game.h"
#include "../core/log.h"

namespace ggdpvr::ue3 {
namespace {
using game::Read;
Layout g;
int g_nameIndexShift = 1;

struct TArr {
    uintptr_t data;
    int32_t num, max;
};
bool ReadArr(uintptr_t at, TArr& a) { return Read(at, a) && a.num >= 0 && a.max >= a.num; }
uintptr_t Ptr(uintptr_t at) {
    uintptr_t v = 0;
    Read(at, v);
    return v;
}
int32_t I32(uintptr_t at) {
    int32_t v = 0;
    Read(at, v);
    return v;
}

// Reads a NUL-terminated ANSI or UTF-16 string, converted to UTF-8.
std::string ReadStr(uintptr_t p, bool wide, size_t maxChars = 256) {
    std::string out;
    if (!wide) {
        for (size_t i = 0; i < maxChars; ++i) {
            char c = 0;
            if (!Read(p + i, c) || !c) break;
            out.push_back(c);
        }
        return out;
    }
    std::wstring w;
    for (size_t i = 0; i < maxChars; ++i) {
        wchar_t c = 0;
        if (!Read(p + 2 * i, c) || !c) break;
        w.push_back(c);
    }
    if (w.empty()) return out;
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    out.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool StrAt(uintptr_t p, const char* s, bool wide) {
    for (size_t i = 0;; ++i) {
        if (wide) {
            wchar_t c = 0;
            if (!Read(p + 2 * i, c)) return false;
            if (c != wchar_t(s[i])) return false;
        } else {
            char c = 0;
            if (!Read(p + i, c)) return false;
            if (c != s[i]) return false;
        }
        if (!s[i]) return true;
    }
}

std::mutex g_nameMu;
std::vector<std::string> g_nameCache;
std::vector<bool> g_nameCached;

std::string EntryString(uintptr_t e) {
    bool wide = g.nameWideDefault;
    if (g.nameIndexField >= 0 && g_nameIndexShift == 1) wide = (I32(e + g.nameIndexField) & 1) != 0;
    return ReadStr(e + g.nameStr, wide);
}
}  // namespace

const Layout& L() { return g; }

int32_t NumNames() {
    TArr a{};
    return ReadArr(g.gnames, a) ? a.num : 0;
}

std::string Name(int32_t index, int32_t number) {
    TArr a{};
    if (!g.gnames || !ReadArr(g.gnames, a) || index < 0 || index >= a.num) return "<badname>";
    std::string s;
    {
        std::lock_guard l(g_nameMu);
        if (size_t(index) >= g_nameCache.size()) g_nameCache.resize(a.num), g_nameCached.resize(a.num);
        if (g_nameCached[index]) s = g_nameCache[index];
    }
    if (s.empty()) {
        uintptr_t e = Ptr(a.data + 8ull * index);
        s = e ? EntryString(e) : "<null>";
        std::lock_guard l(g_nameMu);
        g_nameCache[index] = s, g_nameCached[index] = true;
    }
    if (number > 0) s += "_" + std::to_string(number - 1);
    return s;
}

int32_t NumObjects() {
    TArr a{};
    return ReadArr(g.gobjects, a) ? a.num : 0;
}
uintptr_t ObjectAt(int32_t i) {
    TArr a{};
    if (!ReadArr(g.gobjects, a) || i < 0 || i >= a.num) return 0;
    return Ptr(a.data + 8ull * i);
}
bool IsObject(uintptr_t p) {
    if (!p || !g.gobjects || (p & 3)) return false;
    int32_t i = 0;
    if (!Read(p + g.objIndex, i)) return false;
    return ObjectAt(i) == p;
}

std::string ObjName(uintptr_t o) { return Name(I32(o + g.objName), I32(o + g.objName + 4)); }
uintptr_t ClassOf(uintptr_t o) { return Ptr(o + g.objClass); }
uintptr_t OuterOf(uintptr_t o) { return Ptr(o + g.objOuter); }
uintptr_t SuperOf(uintptr_t s) { return g.structSuper ? Ptr(s + g.structSuper) : 0; }

std::string PathName(uintptr_t o) {
    std::string s = ObjName(o);
    for (uintptr_t x = OuterOf(o), n = 0; x && n < 16; x = OuterOf(x), ++n) s = ObjName(x) + "." + s;
    return s;
}
std::string FullName(uintptr_t o) { return (IsObject(ClassOf(o)) ? ObjName(ClassOf(o)) : "?") + " " + PathName(o); }

bool IsA(uintptr_t obj, const char* className) {
    for (uintptr_t c = ClassOf(obj), n = 0; c && n < 64; c = SuperOf(c), ++n)
        if (ObjName(c) == className) return true;
    return false;
}

uintptr_t FindObject(const char* path, const char* className) {
    const char* dot = strrchr(path, '.');
    const std::string leaf = dot ? dot + 1 : path;
    for (int32_t i = 0, n = NumObjects(); i < n; ++i) {
        uintptr_t o = ObjectAt(i);
        if (!o || ObjName(o) != leaf) continue;
        if (className && ObjName(ClassOf(o)) != className) continue;
        if (PathName(o) == path) return o;
    }
    return 0;
}

std::string DescribeIfObject(uintptr_t p) { return IsObject(p) ? FullName(p) : std::string(); }

// ---------------------------------------------------------------------------------------------
bool BootstrapObjects() {
    uintptr_t db, de;
    game::DataRange(db, de);
    // 1) GNames: TArray<FNameEntry*> whose [0] is "None" and [1] is "ByteProperty".
    // Known addresses for this executable (TimeDateStamp 0x57FC3FE2) first; the full .data scan is the fallback
    // (it can take ~40 s).
    constexpr uint32_t kKnownGNames = 0x31D5A80, kKnownGObjects = 0x3257DD0;
    const uintptr_t knownN = game::Addr(kKnownGNames), knownO = game::Addr(kKnownGObjects);
    static int s_calls = 0;
    const int maxPass = ++s_calls > 40 ? 2 : 1;  // full scan only after ~20 s of retries at the known addresses
    for (int pass = 0; pass < maxPass && !g.gnames; ++pass)
    for (uintptr_t a = pass ? db : knownN; pass ? (a + 16 <= de && !g.gnames) : (a == knownN); a += 8) {
        TArr t{};
        if (!ReadArr(a, t) || t.num < 1000 || t.num > 5000000 || !t.data || game::InImage(t.data)) continue;
        uintptr_t e0 = Ptr(t.data), e1 = Ptr(t.data + 8);
        if (!e0 || !e1) continue;
        for (uint32_t off = 0; off < 0x40 && !g.gnames; off += 4)
            for (bool wide : {false, true})
                if (StrAt(e0 + off, "None", wide) && StrAt(e1 + off, "ByteProperty", wide)) {
                    g.gnames = a, g.nameStr = off, g.nameWideDefault = wide;
                    break;
                }
    }
    if (!g.gnames) return false;
    {
        TArr t{};
        ReadArr(g.gnames, t);
        for (int shift : {1, 0})
            for (uint32_t f = 0; f + 4 <= g.nameStr && g.nameIndexField < 0; f += 4) {
                int good = 0, bad = 0;
                for (int k = 0; k < 300 && k < t.num; ++k) {
                    uintptr_t e = Ptr(t.data + 8ull * k);
                    if (!e) continue;
                    (((I32(e + f) >> shift) == k) ? good : bad)++;
                }
                if (good > 200 && !bad) g.nameIndexField = int32_t(f), g_nameIndexShift = shift;
            }
        LOG("GNames @rva 0x%X num=%d strOff=0x%X wideDefault=%d indexField=%d shift=%d", game::ToRva(g.gnames), t.num,
            g.nameStr, g.nameWideDefault, g.nameIndexField, g_nameIndexShift);
    }
    // 2) GObjects: TArray<UObject*> where each object stores its own index.
    static int s_attempts = 0;
    int bestGood = -1, bestBad = 0, bestForeign = 0, bestNum = 0;
    uintptr_t bestAddr = 0;
    for (int pass = 0; pass < maxPass && !g.gobjects; ++pass)
    for (uintptr_t a = pass ? db : knownO; pass ? (a + 16 <= de && !g.gobjects) : (a == knownO); a += 8) {
        TArr t{};
        if (!ReadArr(a, t) || t.num < 20000 || t.num > 5000000 || !t.data || game::InImage(t.data)) continue;
        for (uint32_t off = 8; off < 0x80; off += 4) {
            int good = 0, bad = 0, foreignVt = 0;
            for (int i = 0; i < 400; ++i) {
                uintptr_t o = Ptr(t.data + 8ull * i), vt = 0;
                if (!o) continue;
                if (!Read(o, vt)) {
                    ++bad;
                    continue;
                }
                if (!game::InRData(vt)) {  // e.g. a vtable patched/proxied by another module: not decisive
                    ++foreignVt;
                    continue;
                }
                (I32(o + off) == i ? good : bad)++;
            }
            if (good > bestGood) bestGood = good, bestBad = bad, bestForeign = foreignVt, bestAddr = a, bestNum = t.num;
            if (good > 300 && bad <= 2) {
                if (bad || foreignVt) LOG("GObjects scan: tolerated %d mismatches, %d foreign vtables", bad, foreignVt);
                g.gobjects = a, g.objIndex = off;
                LOG("GObjects @rva 0x%X num=%d objIndexOffset=0x%X", game::ToRva(a), t.num, off);
                break;
            }
        }
    }
    if (!g.gobjects) {
        if (++s_attempts % 10 == 1)
            LOG("GObjects not found yet (attempt %d): best candidate rva 0x%X num=%d good=%d bad=%d foreignVtables=%d",
                s_attempts, bestAddr ? game::ToRva(bestAddr) : 0, bestNum, bestGood, bestBad, bestForeign);
        return false;
    }

    // 3) UObject header: Class, Name, Outer offsets.
    const int32_t n = NumObjects();
    std::vector<uintptr_t> sample;
    for (int32_t i = 0; i < n && sample.size() < 3000; i += std::max(1, n / 3000))
        if (uintptr_t o = ObjectAt(i)) sample.push_back(o);
    uint32_t bestClass = 0;
    size_t bestClassScore = 0;
    for (uint32_t off = 8; off < 0x90; off += 4) {
        if (off == g.objIndex) continue;
        size_t score = 0;
        for (uintptr_t o : sample) {
            uintptr_t c = Ptr(o + off);
            if (!IsObject(c)) continue;
            uintptr_t cc = Ptr(c + off);  // class of the class == UClass'Class'
            if (IsObject(cc) && Ptr(cc + off) == cc) ++score;
        }
        if (score > bestClassScore) bestClassScore = score, bestClass = off;
    }
    g.objClass = bestClass;
    uintptr_t classClass = Ptr(Ptr(sample[0] + g.objClass) + g.objClass);
    for (uint32_t off = 8; off < 0x90 && !g.objName; off += 4) {
        if (off == g.objIndex || off == g.objClass || off + 4 == g.objClass) continue;
        int32_t idx = I32(classClass + off), num = I32(classClass + off + 4);
        if (idx < 0 || idx >= NumNames() || num < 0 || num > 1000) continue;
        if (Name(idx) != "Class") continue;
        size_t valid = 0;
        for (uintptr_t o : sample) {
            int32_t ix = I32(o + off);
            valid += (ix >= 0 && ix < NumNames());
        }
        if (valid == sample.size()) g.objName = off;
    }
    for (uint32_t off = 8; off < 0x90 && !g.objOuter; off += 4) {
        if (off == g.objIndex || off == g.objClass || off == g.objName || off == g.objName + 4) continue;
        uintptr_t outer = Ptr(classClass + off);
        if (!IsObject(outer) || Name(I32(outer + g.objName)) != "Core") continue;
        size_t valid = 0;
        for (uintptr_t o : sample) {
            uintptr_t p = Ptr(o + off);
            valid += (!p || IsObject(p));
        }
        if (valid * 100 >= sample.size() * 99) g.objOuter = off;
    }
    LOG("UObject header: Index=0x%X Outer=0x%X Name=0x%X Class=0x%X (class score %zu/%zu)", g.objIndex, g.objOuter,
        g.objName, g.objClass, bestClassScore, sample.size());
    return g.objClass && g.objName && g.objOuter;
}

// ---------------------------------------------------------------------------------------------
namespace {
std::vector<uintptr_t> ObjectsWithOuter(uintptr_t outer) {
    std::vector<uintptr_t> v;
    for (int32_t i = 0, n = NumObjects(); i < n; ++i)
        if (uintptr_t o = ObjectAt(i); o && OuterOf(o) == outer) v.push_back(o);
    return v;
}
uintptr_t Child(uintptr_t outer, const char* name) {
    for (uintptr_t o : ObjectsWithOuter(outer))
        if (ObjName(o) == name) return o;
    return 0;
}
bool IsPow2(uint32_t v) { return v && !(v & (v - 1)); }
}  // namespace

bool BootstrapReflection() {
    uintptr_t camera = FindObject("Engine.Camera", "Class"), actor = FindObject("Engine.Actor", "Class");
    uintptr_t objCls = FindObject("Core.Object", "Class");
    if (!camera || !actor || !objCls) {
        LOG("reflection: missing anchor classes camera=%p actor=%p object=%p", (void*)camera, (void*)actor,
            (void*)objCls);
        return false;
    }
    const auto camFields = ObjectsWithOuter(camera);
    // Children / Next: the field chain must enumerate the objects outered to Camera.
    size_t best = 0;
    for (uint32_t ch = 0x40; ch < 0x140; ch += 4) {
        uintptr_t first = Ptr(camera + ch);
        if (!IsObject(first) || OuterOf(first) != camera) continue;
        for (uint32_t nx = 0x40; nx < 0x100; nx += 4) {
            std::unordered_set<uintptr_t> seen;
            bool ok = true;
            for (uintptr_t cur = first; cur && seen.size() < 20000; cur = Ptr(cur + nx)) {
                if (!IsObject(cur) || OuterOf(cur) != camera || !seen.insert(cur).second) {
                    ok = false;
                    break;
                }
            }
            if (ok && seen.size() > best) best = seen.size(), g.structChildren = ch, g.fieldNext = nx;
        }
    }
    LOG("reflection: Children=0x%X Next=0x%X (chain %zu / %zu objects outered to Camera) => sizeof(UObject)=0x%X",
        g.structChildren, g.fieldNext, best, camFields.size(), g.fieldNext);
    if (!best) return false;
    for (uint32_t off = g.fieldNext; off < g.structChildren; off += 4)
        if (Ptr(camera + off) == actor) {
            g.structSuper = off;
            break;
        }
    // UProperty::Offset calibrated against Core.Object's own header properties.
    uintptr_t pName = Child(objCls, "Name"), pClass = Child(objCls, "Class"), pOuter = Child(objCls, "Outer");
    uintptr_t pFov = Child(camera, "DefaultFOV");
    if (!pName || !pClass || !pOuter || !pFov) {
        LOG("reflection: missing calibration properties");
        return false;
    }
    for (uint32_t off = g.fieldNext + 8; off < g.fieldNext + 0x90 && !g.propOffset; off += 4)
        if (uint32_t(I32(pName + off)) == g.objName && uint32_t(I32(pClass + off)) == g.objClass &&
            uint32_t(I32(pOuter + off)) == g.objOuter)
            g.propOffset = off;
    for (uint32_t off = g.fieldNext + 8; off < g.propOffset; off += 4)
        if (I32(pName + off) == 8 && I32(pClass + off) == 8 && I32(pFov + off) == 4 && I32(pName + off - 4) == 1 &&
            I32(pFov + off - 4) == 1) {
            g.propElementSize = off, g.propArrayDim = off - 4;
            break;
        }
    // Type-specific fields.
    std::vector<uintptr_t> bools, objs, structs;
    for (uintptr_t f : camFields) {
        std::string cn = ObjName(ClassOf(f));
        if (cn == "BoolProperty") bools.push_back(f);
        if (cn == "ObjectProperty") objs.push_back(f);
        if (cn == "StructProperty") structs.push_back(f);
    }
    for (uint32_t off = g.propOffset + 4; off < g.propOffset + 0x80 && !g.boolBitMask && bools.size() > 1; off += 4) {
        bool ok = true;
        for (uintptr_t b : bools) ok &= IsPow2(uint32_t(I32(b + off)));
        if (ok) g.boolBitMask = off;
    }
    for (uint32_t off = g.propOffset + 4; off < g.propOffset + 0x80 && !g.objPropClass && !objs.empty(); off += 4) {
        bool ok = true;
        for (uintptr_t o : objs) ok &= IsObject(Ptr(o + off)) && ObjName(ClassOf(Ptr(o + off))) == "Class";
        if (ok) g.objPropClass = off;
    }
    for (uint32_t off = g.propOffset + 4; off < g.propOffset + 0x80 && !g.structPropStruct && !structs.empty();
         off += 4) {
        bool ok = true;
        for (uintptr_t s : structs) ok &= IsObject(Ptr(s + off)) && ObjName(ClassOf(Ptr(s + off))) == "ScriptStruct";
        if (ok) g.structPropStruct = off;
    }
    LOG("reflection: SuperField=0x%X ArrayDim=0x%X ElementSize=0x%X Offset=0x%X BoolBitMask=0x%X "
        "ObjectProperty.PropertyClass=0x%X StructProperty.Struct=0x%X",
        g.structSuper, g.propArrayDim, g.propElementSize, g.propOffset, g.boolBitMask, g.objPropClass,
        g.structPropStruct);
    g.reflectionReady = g.structSuper && g.propOffset && g.propElementSize;
    return g.reflectionReady;
}

// ---------------------------------------------------------------------------------------------
std::vector<PropInfo> Properties(uintptr_t strct) {
    std::vector<PropInfo> out;
    if (!g.reflectionReady) return out;
    for (uintptr_t s = strct, depth = 0; s && depth < 64; s = SuperOf(s), ++depth) {
        std::unordered_set<uintptr_t> seen;
        for (uintptr_t f = Ptr(s + g.structChildren); f && seen.insert(f).second; f = Ptr(f + g.fieldNext)) {
            std::string cn = ObjName(ClassOf(f));
            if (cn.size() < 8 || cn.compare(cn.size() - 8, 8, "Property") != 0) continue;
            PropInfo p;
            p.prop = f, p.name = ObjName(f), p.owner = ObjName(s);
            p.type = cn.substr(0, cn.size() - 8);
            p.offset = uint32_t(I32(f + g.propOffset));
            p.elementSize = uint32_t(I32(f + g.propElementSize));
            p.arrayDim = uint32_t(I32(f + g.propArrayDim));
            if (p.type == "Bool" && g.boolBitMask) p.bitMask = uint32_t(I32(f + g.boolBitMask));
            if ((p.type == "Object" || p.type == "Class") && g.objPropClass) {
                uintptr_t c = Ptr(f + g.objPropClass);
                if (IsObject(c)) p.type += "<" + ObjName(c) + ">";
            }
            if (p.type == "Struct" && g.structPropStruct) {
                uintptr_t c = Ptr(f + g.structPropStruct);
                if (IsObject(c)) p.type += "<" + ObjName(c) + ">";
            }
            out.push_back(p);
        }
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) {
        return a.offset != b.offset ? a.offset < b.offset : a.bitMask < b.bitMask;
    });
    return out;
}

bool PropertyAt(uintptr_t cls, uint32_t off, PropInfo& out) {
    for (auto& p : Properties(cls))
        if (off >= p.offset && off < p.offset + p.elementSize * std::max(1u, p.arrayDim)) {
            out = p;
            return true;
        }
    return false;
}

bool FindProperty(uintptr_t cls, const char* name, PropInfo& out) {
    for (auto& p : Properties(cls))
        if (p.name == name) {
            out = p;
            return true;
        }
    return false;
}

void DumpReflection(const std::wstring& path) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"w");
    if (!f) return;
    fprintf(f, "# GGDPVR runtime reflection dump (UObject size 0x%X; offsets are byte offsets in instances)\n",
            g.fieldNext);
    std::vector<std::pair<std::string, uintptr_t>> structs;
    for (int32_t i = 0, n = NumObjects(); i < n; ++i) {
        uintptr_t o = ObjectAt(i);
        if (!o) continue;
        std::string cn = ObjName(ClassOf(o));
        if (cn != "Class" && cn != "ScriptStruct") continue;
        std::string objPath = PathName(o);
        std::string pkg = objPath.substr(0, objPath.find('.'));
        if (pkg != "Core" && pkg != "Engine" && pkg != "GameFramework" && pkg != "GFxUI" && pkg != "GG2Game") continue;
        structs.emplace_back(cn + " " + objPath, o);
    }
    std::sort(structs.begin(), structs.end());
    size_t props = 0;
    for (auto& [label, o] : structs) {
        uintptr_t sup = SuperOf(o);
        fprintf(f, "\n%s%s%s\n", label.c_str(), sup ? " : " : "", sup ? PathName(sup).c_str() : "");
        for (auto& p : Properties(o)) {
            if (p.owner != ObjName(o)) continue;  // own properties only (supers are listed separately)
            ++props;
            fprintf(f, "  +0x%04X %-28s %s", p.offset, p.type.c_str(), p.name.c_str());
            if (p.arrayDim > 1) fprintf(f, "[%u]", p.arrayDim);
            if (p.bitMask) fprintf(f, " (bit 0x%X)", p.bitMask);
            fprintf(f, "  size=0x%X\n", p.elementSize);
        }
    }
    fclose(f);
    LOG("reflection dump: %zu classes/structs, %zu properties -> %ls", structs.size(), props, path.c_str());
}
}  // namespace ggdpvr::ue3
