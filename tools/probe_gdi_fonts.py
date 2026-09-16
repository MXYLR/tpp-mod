import ctypes, glob, os
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


GGO_GRAY8_BITMAP = 5
FR_PRIVATE = 0x10
GDI_ERROR = 0xFFFFFFFF
ANTIALIASED_QUALITY = 4

gdi32.GetGlyphOutlineW.restype = wintypes.DWORD
gdi32.GetGlyphOutlineW.argtypes = [wintypes.HDC, wintypes.UINT, wintypes.UINT,
                                   ctypes.POINTER(GLYPHMETRICS), wintypes.DWORD,
                                   ctypes.c_void_p, ctypes.POINTER(MAT2)]

PX = 49
hdc = gdi32.CreateCompatibleDC(None)

FONTS = [
    (r"C:\Windows\Fonts\SourceHanSansCN-Regular.otf", "Source Han Sans CN Regular", "李中H"),
    (r"C:\Windows\Fonts\msyh.ttc", "Microsoft YaHei", "李中H"),
    (r"C:\Windows\Fonts\simsun.ttc", "SimSun", "李中H"),
    (r"C:\Windows\Fonts\msgothic.ttc", "MS Gothic", "李中H"),
]

for path, family, chars in FONTS:
    if not os.path.exists(path):
        print("missing:", path)
        continue
    added = gdi32.AddFontResourceExW(path, FR_PRIVATE, None)
    hf = gdi32.CreateFontW(-PX, 0, 0, 0, 400, 0, 0, 0, 1, 4, 0, ANTIALIASED_QUALITY, 0, family)
    old = gdi32.SelectObject(hdc, hf)
    print("=== %s (added=%d) %s" % (path, added, family))
    for ch in chars:
        mat = MAT2(FIXED(0, 1), FIXED(0, 0), FIXED(0, 0), FIXED(0, 1))
        gm = GLYPHMETRICS()
        size = gdi32.GetGlyphOutlineW(hdc, ord(ch), GGO_GRAY8_BITMAP, ctypes.byref(gm), 0, None, ctypes.byref(mat))
        if size in (0, GDI_ERROR):
            print("  %s size=%s" % (ch, "ERR" if size == GDI_ERROR else size))
            continue
        buf = ctypes.create_string_buffer(size)
        gdi32.GetGlyphOutlineW(hdc, ord(ch), GGO_GRAY8_BITMAP, ctypes.byref(gm), size, buf, ctypes.byref(mat))
        raw = bytes(buf)
        w, h = gm.gmBlackBoxX, gm.gmBlackBoxY
        pitch = (w + 3) & ~3
        vals = [raw[y * pitch + x] for y in range(h) for x in range(w)]
        over32 = sum(1 for v in vals if v > 32)
        over8 = sum(1 for v in vals if v > 8)
        print("  %s box=%dx%d adv=%d origin=%d/%d max=%d  >32:%d  >8:%d" % (
            ch, w, h, gm.gmCellIncX, gm.gmptGlyphOrigin.x, gm.gmptGlyphOrigin.y,
            max(vals), over32, over8))
        if ch == "中":
            ramp = " .:-=+*#%@"
            for y in range(0, h, 2):
                print("      " + "".join(ramp[min(9, raw[y * pitch + x] // 7)] for x in range(w)))
    gdi32.SelectObject(hdc, old)
    gdi32.DeleteObject(hf)
    gdi32.RemoveFontResourceExW(path, FR_PRIVATE, None)

gdi32.DeleteDC(hdc)
