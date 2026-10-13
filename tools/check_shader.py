"""Compile the UI layer's embedded HLSL (ui_layer.cpp kShader) with the system d3dcompiler_47 to catch errors before a
build reaches the game (a failed compile disables the whole composite at runtime).
Usage: python check_shader.py [path/to/ui_layer.cpp]"""
import ctypes, os, re, sys

src_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'vr', 'ui_layer.cpp')
cpp = open(src_path, encoding='utf-8').read()
a = cpp.index('const char kShader[] = R"(') + len('const char kShader[] = R"(')
b = cpp.index(')";', a)
hlsl = ''.join(cpp[a:b].split(')" R"('))
entries = re.findall(r'cc\("(\w+)", "(\w+)"', cpp)

dc = ctypes.WinDLL('d3dcompiler_47.dll')
D3DCompile = dc.D3DCompile
D3DCompile.restype = ctypes.c_long
D3DCompile.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p,
                       ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint,
                       ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]


def blob_text(blob):
    vt = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])(blob)
    size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])(blob)
    return ctypes.string_at(ptr, size).decode('utf-8', 'replace')


data = hlsl.encode('utf-8')
ok = True
for entry, target in entries:
    code, err = ctypes.c_void_p(), ctypes.c_void_p()
    hr = D3DCompile(data, len(data), b'ggdpvr_ui', None, None, entry.encode(), target.encode(), 0, 0,
                    ctypes.byref(code), ctypes.byref(err))
    if hr < 0:
        ok = False
        print(f'{entry} ({target}): FAILED\n{blob_text(err) if err.value else hex(hr & 0xFFFFFFFF)}')
    else:
        print(f'{entry} ({target}): ok')
sys.exit(0 if ok else 1)
