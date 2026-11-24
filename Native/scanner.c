/* 屏幕二维码扫描器 - 原生 Win32 单文件版
 * 依赖：quirc（公共领域）、Win32 API。无 Python、无 WebView。
 */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quirc/lib_quirc.h"

#define HOTKEY_ID      1
#define TRAY_ID        2
#define WM_RESULT      (WM_APP + 1)
#define WM_ADDRESULT   (WM_APP + 2)
#define WM_DONE        (WM_APP + 3)

typedef struct {
    wchar_t *text;
    int      is_url;
} Result;

typedef struct {
    Result  *items;
    int      count;
    wchar_t *error;
} ScanResult;

static HWND g_hwnd, g_status, g_list, g_delay;
static HICON g_icon_big, g_icon_small;
static NOTIFYICONDATAW g_nid;
static volatile int g_busy = 0;
static wchar_t g_appdir[MAX_PATH];

/* ---------- 小工具 ---------- */
static void beep(int kind)
{
    if (kind == 0)      Beep(700, 70);    /* 开始 */
    else if (kind == 1) Beep(1100, 80);   /* 成功 */
    else                Beep(300, 130);   /* 失败 */
}

static void log_result(ScanResult *r)
{
    FILE *f = fopen("scan_log.txt", "ab");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d  %s  %d\n",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
            r->items ? "OK" : (r->error ? "FAIL" : "NONE"), r->count);
    for (int i = 0; i < r->count; i++) {
        char buf[1024] = {0};
        WideCharToMultiByte(CP_UTF8, 0, r->items[i].text, -1, buf, sizeof(buf) - 1, NULL, NULL);
        fprintf(f, "    [%d] %.900s\n", i + 1, buf);
    }
    fclose(f);
}

static int is_url(const wchar_t *s)
{
    return !wcsncmp(s, L"http://", 7) || !wcsncmp(s, L"https://", 8) ||
           !wcsncmp(s, L"mailto:", 7) || !wcsncmp(s, L"workbuddy://", 12);
}

static void set_clipboard(const wchar_t *text)
{
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        wchar_t *p = (wchar_t *)GlobalLock(h);
        if (p) { memcpy(p, text, bytes); GlobalUnlock(h); }
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

static wchar_t *payload_to_wide(const unsigned char *p, int len)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)p, len, NULL, 0);
    UINT cp = CP_UTF8;
    if (n <= 0) { cp = 936; n = MultiByteToWideChar(cp, 0, (const char *)p, len, NULL, 0); }
    if (n <= 0) return NULL;
    wchar_t *out = (wchar_t *)malloc((size_t)(n + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    MultiByteToWideChar(cp, 0, (const char *)p, len, out, n);
    out[n] = 0;
    return out;
}

/* ---------- 截屏 ---------- */
static int grab_gray(unsigned char **out_gray, int *out_w, int *out_h, wchar_t **err)
{
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) { *err = _wcsdup(L"取不到屏幕尺寸"); return 0; }

    HDC dc = GetDC(NULL);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, vw, vh);
    HGDIOBJ old = SelectObject(mem, bmp);
    if (!BitBlt(mem, 0, 0, vw, vh, dc, vx, vy, SRCCOPY)) {
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, dc);
        *err = _wcsdup(L"截屏失败"); return 0;
    }

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = vw;
    bi.bmiHeader.biHeight = -vh;          /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    unsigned char *pix = (unsigned char *)malloc((size_t)vw * vh * 4);
    if (!pix || !GetDIBits(mem, bmp, 0, vh, pix, &bi, DIB_RGB_COLORS)) {
        free(pix); SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, dc);
        *err = _wcsdup(L"读取屏幕像素失败"); return 0;
    }
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, dc);

    unsigned char *gray = (unsigned char *)malloc((size_t)vw * vh);
    if (!gray) { free(pix); *err = _wcsdup(L"内存不足"); return 0; }
    for (int i = 0, j = 0; i < vw * vh; i++, j += 4)
        gray[i] = (unsigned char)((pix[j] * 77 + pix[j + 1] * 150 + pix[j + 2] * 29) >> 8);
    free(pix);

    *out_gray = gray; *out_w = vw; *out_h = vh;
    return 1;
}

/* ---------- 解码 ---------- */
static int collect_codes(struct quirc *q, Result **items, int *count)
{
    int n = quirc_count(q);
    for (int i = 0; i < n; i++) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(q, i, &code);
        if (quirc_decode(&code, &data) != QUIRC_SUCCESS) {
            quirc_flip(&code);
            if (quirc_decode(&code, &data) != QUIRC_SUCCESS) continue;
        }
        wchar_t *text = payload_to_wide(data.payload, data.payload_len);
        if (!text || !*text) { free(text); continue; }
        int dup = 0;
        for (int k = 0; k < *count; k++) if (!wcscmp((*items)[k].text, text)) { dup = 1; break; }
        if (dup) { free(text); continue; }
        Result *tmp = (Result *)realloc(*items, (size_t)(*count + 1) * sizeof(Result));
        if (!tmp) { free(text); break; }
        *items = tmp;
        (*items)[*count].text = text;
        (*items)[*count].is_url = is_url(text);
        (*count)++;
    }
    return n;
}

static ScanResult *decode_gray(unsigned char *gray, int w, int h)
{
    ScanResult *r = (ScanResult *)calloc(1, sizeof(ScanResult));
    if (!r) return NULL;
    struct quirc *q = quirc_new();
    if (!q || quirc_resize(q, w, h) != 0) {
        if (q) quirc_destroy(q);
        r->error = _wcsdup(L"解码器初始化失败");
        return r;
    }
    unsigned char *buf = quirc_begin(q, NULL, NULL);
    memcpy(buf, gray, (size_t)w * h);
    quirc_end(q);
    collect_codes(q, &r->items, &r->count);

    /* 反色重试：不用 Quirc 不支持反色，只能取反像素再跑一遍 */
    if (r->count == 0) {
        for (int i = 0; i < w * h; i++) gray[i] = (unsigned char)(255 - gray[i]);
        buf = quirc_begin(q, NULL, NULL);
        memcpy(buf, gray, (size_t)w * h);
        quirc_end(q);
        collect_codes(q, &r->items, &r->count);
    }
    quirc_destroy(q);
    return r;
}

/* ---------- 工作线程 ---------- */
typedef struct { int delay; } ScanArgs;

static DWORD WINAPI scan_worker(LPVOID param)
{
    ScanArgs *a = (ScanArgs *)param;
    int delay = a->delay;
    free(a);

    if (delay > 0) Sleep(delay * 1000);

    wchar_t *err = NULL;
    unsigned char *gray = NULL;
    int w = 0, h = 0;
    ScanResult *r = NULL;

    if (grab_gray(&gray, &w, &h, &err))
        r = decode_gray(gray, w, h);
    free(gray);
    if (err) {
        r = (ScanResult *)calloc(1, sizeof(ScanResult));
        if (r) r->error = err; else free(err);
    }
    if (!r) {
        r = (ScanResult *)calloc(1, sizeof(ScanResult));
        if (r) r->error = _wcsdup(L"未知错误");
    }
    log_result(r);
    PostMessage(g_hwnd, WM_RESULT, 0, (LPARAM)r);
    return 0;
}

static void start_scan(int delay)
{
    if (g_busy) return;
    g_busy = 1;
    beep(0);
    ShowWindow(g_hwnd, SW_HIDE);
    SendMessage(g_status, WM_SETTEXT, 0, (LPARAM)L"倒计时中，窗口已隐藏…");
    ScanArgs *a = (ScanArgs *)malloc(sizeof(ScanArgs));
    a->delay = delay;
    HANDLE th = CreateThread(NULL, 0, scan_worker, a, 0, NULL);
    if (th) CloseHandle(th);
    else { g_busy = 0; ShowWindow(g_hwnd, SW_SHOW); }
}

/* DIB（系统截图工具/微信截图复制出来的格式）直接转灰度并解码 */
static ScanResult *decode_clip_dib(void *dib)
{
    BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *)dib;
    ScanResult *bad = NULL;
    if (!bi || bi->biSize < sizeof(BITMAPINFOHEADER) || bi->biWidth <= 0 || bi->biHeight == 0) {
        bad = (ScanResult *)calloc(1, sizeof(ScanResult));
        if (bad) bad->error = _wcsdup(L"剪贴板图片格式不支持");
        return bad;
    }
    int w = bi->biWidth, h = abs((int)bi->biHeight);
    int topdown = bi->biHeight < 0;
    int bpp = bi->biBitCount;
    if (bpp != 24 && bpp != 32) {
        bad = (ScanResult *)calloc(1, sizeof(ScanResult));
        if (bad) bad->error = _wcsdup(L"只支持 24/32 位截图");
        return bad;
    }
    int stride = ((w * bpp + 31) / 32) * 4;
    unsigned char *bits = (unsigned char *)dib + bi->biSize;
    unsigned char *gray = (unsigned char *)malloc((size_t)w * h);
    if (!gray) {
        bad = (ScanResult *)calloc(1, sizeof(ScanResult));
        if (bad) bad->error = _wcsdup(L"内存不足");
        return bad;
    }
    for (int y = 0; y < h; y++) {
        int sy = topdown ? y : (h - 1 - y);
        unsigned char *row = bits + (size_t)sy * stride;
        for (int x = 0; x < w; x++) {
            unsigned char *p = row + (size_t)x * (bpp / 8);
            gray[(size_t)y * w + x] = (unsigned char)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        }
    }
    ScanResult *r = decode_gray(gray, w, h);
    free(gray);
    return r;
}

/* 剪贴板图片识别：先 CF_DIB（截图工具），再 CF_BITMAP */
static void scan_clipboard(void)
{
    if (g_busy) return;
    if (!IsClipboardFormatAvailable(CF_DIB) && !IsClipboardFormatAvailable(CF_BITMAP)) {
        MessageBoxW(g_hwnd, L"剪贴板里没有图片", L"二维码扫描器", MB_ICONWARNING);
        return;
    }
    ScanResult *r = NULL;
    if (OpenClipboard(g_hwnd)) {
        void *dib = GetClipboardData(CF_DIB);
        if (dib) r = decode_clip_dib(dib);
        CloseClipboard();
    }
    if (r) {
        g_busy = 1;
        beep(0);
        log_result(r);
        SendMessage(g_hwnd, WM_RESULT, 0, (LPARAM)r);
        return;
    }

    if (!IsClipboardFormatAvailable(CF_BITMAP)) return;
    if (!OpenClipboard(g_hwnd)) return;
    HBITMAP bmp = (HBITMAP)GetClipboardData(CF_BITMAP);
    if (!bmp) { CloseClipboard(); return; }
    BITMAP bm;
    GetObjectW(bmp, sizeof(bm), &bm);
    int w = bm.bmWidth, h = bm.bmHeight;
    HDC dc = GetDC(NULL), mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, bmp);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    unsigned char *pix = (unsigned char *)malloc((size_t)w * h * 4);
    if (pix) GetDIBits(mem, bmp, 0, h, pix, &bi, DIB_RGB_COLORS);
    SelectObject(mem, old); DeleteDC(mem); ReleaseDC(NULL, dc);
    CloseClipboard();
    if (!pix) return;
    unsigned char *gray = (unsigned char *)malloc((size_t)w * h);
    for (int i = 0, j = 0; i < w * h; i++, j += 4)
        gray[i] = (unsigned char)((pix[j] * 77 + pix[j + 1] * 150 + pix[j + 2] * 29) >> 8);
    free(pix);
    g_busy = 1;
    beep(0);
    r = decode_gray(gray, w, h);
    free(gray);
    log_result(r);
    SendMessage(g_hwnd, WM_RESULT, 0, (LPARAM)r);
}

/* ---------- 窗口 ---------- */
static void show_results(ScanResult *r)
{
    g_busy = 0;
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    SendMessage(g_list, LB_RESETCONTENT, 0, 0);

    if (r->error) {
        beep(2);
        SendMessage(g_status, WM_SETTEXT, 0, (LPARAM)r->error);
        free(r->error);
        free(r);
        return;
    }
    if (r->count == 0) {
        beep(2);
        SendMessage(g_status, WM_SETTEXT, 0, (LPARAM)L"没找到二维码。把码放大、转正，或用剪贴板识别。");
        free(r);
        return;
    }
    beep(1);
    for (int i = 0; i < r->count; i++)
        SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)r->items[i].text);
    wchar_t msg[256];
    swprintf(msg, 256, L"识别到 %d 个二维码，第一个已复制到剪贴板。", r->count);
    SendMessage(g_status, WM_SETTEXT, 0, (LPARAM)msg);
    set_clipboard(r->items[0].text);
    free(r);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = h;
        g_status = CreateWindowExW(0, L"Static", L"就绪。按 Ctrl+Alt+Q 全屏截码。",
            WS_CHILD | WS_VISIBLE, 12, 10, 700, 24, h, NULL, NULL, NULL);
        g_delay = CreateWindowExW(0, L"ComboBox", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 12, 40, 120, 100, h, (HMENU)100, NULL, NULL);
        SendMessageW(g_delay, CB_ADDSTRING, 0, (LPARAM)L"2 秒后截屏");
        SendMessageW(g_delay, CB_ADDSTRING, 0, (LPARAM)L"3 秒后截屏");
        SendMessageW(g_delay, CB_ADDSTRING, 0, (LPARAM)L"5 秒后截屏");
        SendMessageW(g_delay, CB_SETCURSEL, 1, 0);
        CreateWindowExW(0, L"Button", L"截屏识别",
            WS_CHILD | WS_VISIBLE, 145, 40, 100, 25, h, (HMENU)101, NULL, NULL);
        CreateWindowExW(0, L"Button", L"从剪贴板识别",
            WS_CHILD | WS_VISIBLE, 255, 40, 130, 25, h, (HMENU)102, NULL, NULL);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"ListBox", NULL,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
            12, 76, 700, 330, h, (HMENU)103, NULL, NULL);
        CreateWindowExW(0, L"Button", L"复制选中",
            WS_CHILD | WS_VISIBLE, 12, 416, 100, 26, h, (HMENU)104, NULL, NULL);
        CreateWindowExW(0, L"Button", L"打开链接",
            WS_CHILD | WS_VISIBLE, 122, 416, 100, 26, h, (HMENU)105, NULL, NULL);
        CreateWindowExW(0, L"Button", L"退出",
            WS_CHILD | WS_VISIBLE, 610, 416, 100, 26, h, (HMENU)106, NULL, NULL);

        NONCLIENTMETRICSW ncm; ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(g_list, WM_SETFONT, (WPARAM)font, TRUE);

        RegisterHotKey(h, HOTKEY_ID, MOD_CONTROL | MOD_ALT, 'Q');

        wchar_t iconpath[MAX_PATH];
        swprintf(iconpath, MAX_PATH, L"%s\\app.ico", g_appdir);
        g_icon_big = (HICON)LoadImageW(NULL, iconpath, IMAGE_ICON, 48, 48, LR_LOADFROMFILE);
        g_icon_small = (HICON)LoadImageW(NULL, iconpath, IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
        if (g_icon_big) SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)g_icon_big);
        if (g_icon_small) SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)g_icon_small);

        memset(&g_nid, 0, sizeof(g_nid));
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = h;
        g_nid.uID = TRAY_ID;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_ADDRESULT;
        g_nid.hIcon = g_icon_small;
        wcscpy(g_nid.szTip, L"二维码扫描器 Ctrl+Alt+Q");
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }
    case WM_HOTKEY:
        if (wp == HOTKEY_ID) {
            int idx = (int)SendMessageW(g_delay, CB_GETCURSEL, 0, 0);
            int delay = (idx == 0) ? 2 : (idx == 2) ? 5 : 3;
            start_scan(delay);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 101: {
            int idx = (int)SendMessageW(g_delay, CB_GETCURSEL, 0, 0);
            start_scan((idx == 0) ? 2 : (idx == 2) ? 5 : 3);
            break;
        }
        case 102: scan_clipboard(); break;
        case 104: {
            int i = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
            if (i != LB_ERR) {
                int n = (int)SendMessageW(g_list, LB_GETTEXTLEN, i, 0);
                wchar_t *buf = (wchar_t *)malloc((size_t)(n + 1) * sizeof(wchar_t));
                SendMessageW(g_list, LB_GETTEXT, i, (LPARAM)buf);
                set_clipboard(buf);
                free(buf);
            }
            break;
        }
        case 105: {
            int i = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
            if (i != LB_ERR) {
                int n = (int)SendMessageW(g_list, LB_GETTEXTLEN, i, 0);
                wchar_t *buf = (wchar_t *)malloc((size_t)(n + 1) * sizeof(wchar_t));
                SendMessageW(g_list, LB_GETTEXT, i, (LPARAM)buf);
                if (is_url(buf)) ShellExecuteW(NULL, L"open", buf, NULL, NULL, SW_SHOWNORMAL);
                else MessageBoxW(h, L"选中的不是链接", L"二维码扫描器", MB_ICONINFORMATION);
                free(buf);
            }
            break;
        }
        case 106: PostMessageW(h, WM_CLOSE, 0, 0); break;
        }
        return 0;

    case WM_ADDRESULT:  /* 托盘图标消息 */
        if (lp == WM_LBUTTONDBLCLK || lp == WM_LBUTTONUP) {
            ShowWindow(h, SW_SHOW);
            SetForegroundWindow(h);
        }
        return 0;

    case WM_RESULT:
        show_results((ScanResult *)lp);
        return 0;

    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;

    case WM_DESTROY:
        UnregisterHotKey(h, HOTKEY_ID);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_icon_big) DestroyIcon(g_icon_big);
        if (g_icon_small) DestroyIcon(g_icon_small);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE prev, PWSTR cmd, int show)
{
    GetModuleFileNameW(NULL, g_appdir, MAX_PATH);
    wchar_t *slash = wcsrchr(g_appdir, L'\\');
    if (slash) *slash = 0;

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst;
    wc.lpszClassName = L"QRScannerNative";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND h = CreateWindowExW(0, L"QRScannerNative", L"二维码扫描器",
        WS_OVERLAPPEDWINDOW, 120, 120, 740, 490, NULL, NULL, hinst, NULL);
    if (!h) { MessageBoxW(NULL, L"创建窗口失败", L"二维码扫描器", MB_ICONERROR); return 1; }
    ShowWindow(h, show);
    UpdateWindow(h);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
