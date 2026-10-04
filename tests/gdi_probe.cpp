// SPDX-License-Identifier: GPL-3.0-or-later
// ===========================================================================
//  gdi_probe.cpp - 隔离测试：GDI 到底能不能画出制表符
//  分别试「无 dx 数组」「有 dx 数组」「ETO_OPAQUE+CLIPPED+dx」「SimSun」
//  输出一张 BMP 供肉眼比对。
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static const int W = 980;
static const int H = 260;

int main() {
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(hdc, bmp);

    RECT full = { 0, 0, W, H };
    HBRUSH bg = CreateSolidBrush(RGB(0x10, 0x12, 0x16));
    FillRect(hdc, &full, bg);
    DeleteObject(bg);

    LOGFONTW lf = {};
    lf.lfHeight = -20;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    lstrcpynW(lf.lfFaceName, L"Consolas", LF_FACESIZE);
    HFONT fontConsolas = CreateFontIndirectW(&lf);

    lstrcpynW(lf.lfFaceName, L"SimSun", LF_FACESIZE);
    HFONT fontSimSun = CreateFontIndirectW(&lf);

    const wchar_t* line = L"A┌─┬─┐ B├─┼─┤ C└─┴─┘ D╔═╦═╗ E╚═╩═╝ F★☆●○■□ G";
    int len = (int)wcslen(line);
    const int cellW = 11;

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(240, 240, 240));

    // 1) Consolas，无 dx
    SelectObject(hdc, fontConsolas);
    TextOutW(hdc, 10, 8, L"1) Consolas  no-dx", 18);
    TextOutW(hdc, 10, 30, line, len);

    // 2) Consolas，有 dx
    std::vector<INT> dx((size_t)len, cellW);
    TextOutW(hdc, 10, 68, L"2) Consolas  with-dx", 20);
    ExtTextOutW(hdc, 10, 90, 0, nullptr, line, (UINT)len, dx.data());

    // 3) Consolas，ETO_OPAQUE|ETO_CLIPPED + dx
    TextOutW(hdc, 10, 128, L"3) Consolas  opaque+clipped+dx", 30);
    RECT r = { 10, 150, 10 + len * cellW, 176 };
    ExtTextOutW(hdc, 10, 150, ETO_OPAQUE | ETO_CLIPPED, &r, line, (UINT)len, dx.data());

    // 4) SimSun，有 dx
    SelectObject(hdc, fontSimSun);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(240, 240, 240));
    TextOutW(hdc, 10, 188, L"4) SimSun    with-dx", 20);
    ExtTextOutW(hdc, 10, 210, 0, nullptr, line, (UINT)len, dx.data());

    GdiFlush();

    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = bmi.bmiHeader;
    ih.biHeight = H;
    ih.biSizeImage = (DWORD)(W * H * 4);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;

    FILE* f = nullptr;
    // 输出到当前工作目录，别把路径写死
    if (fopen_s(&f, "gdi_probe.bmp", "wb") == 0 && f) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        fwrite(bits, 1, ih.biSizeImage, f);
        fclose(f);
        printf("saved gdi_probe.bmp\n");
    } else {
        printf("cannot write gdi_probe.bmp\n");
    }

    SelectObject(hdc, old);
    DeleteObject(bmp);
    DeleteObject(fontConsolas);
    DeleteObject(fontSimSun);
    DeleteDC(hdc);
    return 0;
}
