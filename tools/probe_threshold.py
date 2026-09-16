import ctypes
from ctypes import wintypes

gdi32 = ctypes.WinDLL('gdi32', use_last_error=True)


class GLYPHMETRICS(ctypes.Structure):
    _fields_ = [("gmBlackBoxX", wintypes.UINT),
                ("gmBlackBoxY", wintypes.UINT),
                ("gmptGlyphOrigin", wintypes.POINT),
                ("gmCellIncX", ctypes.c_short),
                ("gmCellIncY", ctypes.c_short)]


class FIXED(ctypes.Structure):
    _fields_ = [("fract", ctypes.c_ushort), ("value", ctypes.c_short)]


class MAT2(ctypes.Structure):
    _fields_ = [("eM11", FIXED), ("eM12", FIXED), ("eM21", FIXED), ("eM22", FIXED)]


gdi32.GetGlyphOutlineW.restype = wintypes.DWORD
gdi32.GetGlyphOutlineW.argtypes = [wintypes.HDC, wintypes.UINT, wintypes.UINT,
                                   ctypes.POINTER(GLYPHMETRICS), wintypes.DWORD,
                                   ctypes.c_void_p, ctypes.POINTER(MAT2)]

PATH = r"C:\Windows\Fonts\SourceHanSansCN-Regular.otf"
gdi32.AddFontResourceExW(PATH, 0x10, None)
hdc = gdi32.CreateCompatibleDC(None)

for px in (49, 100):
    hf = gdi32.CreateFontW(-px, 0, 0, 0, 400, 0, 0, 0, 1, 4, 0, 4, 0, "Source Han Sans CN Regular")
    old = gdi32.SelectObject(hdc, hf)
    for ch in "\u674e\u4e2d\u65e5H0":
        mat = MAT2(FIXED(0, 1), FIXED(0, 0), FIXED(0, 0), FIXED(0, 1))
        gm = GLYPHMETRICS()
        size = gdi32.GetGlyphOutlineW(hdc, ord(ch), 5, ctypes.byref(gm), 0, None, ctypes.byref(mat))
        buf = ctypes.create_string_buffer(size)
        gdi32.GetGlyphOutlineW(hdc, ord(ch), 5, ctypes.byref(gm), size, buf, ctypes.byref(mat))
        raw = bytes(buf)
        w, h = gm.gmBlackBoxX, gm.gmBlackBoxY
        pitch = (w + 3) & ~3
        vals = [raw[y * pitch + x] for y in range(h) for x in range(w)]
        peak = max(vals)
        old_rule = sum(1 for v in vals if v > 32)
        new_rule = sum(1 for v in vals if v * 2 > peak)
        print("raster %3d  U+%04X  box=%dx%d peak=%2d   old(>32)=%-5d new(peak/2)=%-5d (%.0f%% of box)" % (
            px, ord(ch), w, h, peak, old_rule, new_rule, 100.0 * new_rule / (w * h)))
    gdi32.SelectObject(hdc, old)
    gdi32.DeleteObject(hf)

gdi32.DeleteDC(hdc)
