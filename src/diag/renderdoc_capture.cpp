#include "renderdoc_capture.h"

#include <windows.h>
#include <renderdoc_app.h>

#include "../core/log.h"

namespace ggdpvr::diag::renderdoc {
namespace {
RENDERDOC_API_1_6_0* Api() {
    static RENDERDOC_API_1_6_0* api = nullptr;
    static bool tried = false;
    if (tried) return api;
    HMODULE mod = GetModuleHandleW(L"renderdoc.dll");
    if (!mod) return nullptr;  // not launched under RenderDoc (retry next time)
    tried = true;
    auto get = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
    if (get && get(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&api)) == 1) {
        std::wstring dir = log::Directory() + L"\\captures";
        CreateDirectoryW(dir.c_str(), nullptr);
        char path[MAX_PATH]{};
        WideCharToMultiByte(CP_UTF8, 0, (dir + L"\\gg2").c_str(), -1, path, MAX_PATH, nullptr, nullptr);
        api->SetCaptureFilePathTemplate(path);
        api->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);  // keep screenshots clean
        LOG("[renderdoc] API 1.6.0 attached; captures -> %s*.rdc", path);
    } else {
        api = nullptr;
        LOG("[renderdoc] renderdoc.dll present but RENDERDOC_GetAPI failed");
    }
    return api;
}
}  // namespace

bool Available() { return Api() != nullptr; }

bool TriggerCapture(unsigned frames) {
    auto* api = Api();
    if (!api) {
        LOG("[renderdoc] capture requested but RenderDoc is not loaded");
        return false;
    }
    if (frames <= 1) api->TriggerCapture();
    else api->TriggerMultiFrameCapture(frames);
    LOG("[renderdoc] capture triggered (%u frame(s))", frames);
    return true;
}

std::string LastCapturePath() {
    auto* api = Api();
    if (!api) return {};
    uint32_t n = api->GetNumCaptures();
    if (!n) return {};
    char path[1024]{};
    uint32_t len = sizeof path;
    uint64_t ts = 0;
    api->GetCapture(n - 1, path, &len, &ts);
    return path;
}
}  // namespace ggdpvr::diag::renderdoc
