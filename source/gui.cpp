// MTGA Ultrawide Patcher v3.0.0 - native Windows user interface.
// The engine is shared with the command-line entry point; no managed runtime.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include "engine.h"

namespace {
constexpr wchar_t CLASS_NAME[] = L"MTGAUltrawidePatcherWindow3";
constexpr wchar_t TITLE[] = L"MTGA Ultrawide Patcher";
constexpr UINT RESULT_MESSAGE = WM_APP + 1;
constexpr UINT INITIAL_CHECK_MESSAGE = WM_APP + 2;
enum ControlId { Folder = 101, Browse, Check, Install, Restore };

struct WindowState {
    HWND window = nullptr;
    HWND title = nullptr, subtitle = nullptr, folderLabel = nullptr;
    HWND folder = nullptr, browse = nullptr, check = nullptr;
    HWND install = nullptr, restore = nullptr, status = nullptr;
    HWND details = nullptr, reminder = nullptr, footer = nullptr;
    HFONT normalFont = nullptr, titleFont = nullptr, statusFont = nullptr;
    UINT dpi = 96;
    bool busy = false;
    bool initializing = true;
    bool canInstall = false, canRestore = false;
};

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!length) return L"Unable to display the result text.";
    std::wstring value(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), value.data(), length);
    return value;
}

std::wstring editText(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

std::wstring windowsLines(const std::string& text) {
    std::wstring out;
    const auto decoded = wide(text);
    out.reserve(decoded.size() + 32);
    for (size_t i = 0; i < decoded.size(); ++i) {
        if (decoded[i] == L'\n' && (i == 0 || decoded[i - 1] != L'\r'))
            out += L'\r';
        out += decoded[i];
    }
    return out;
}

UINT windowDpi(HWND window) {
    using GetDpi = UINT(WINAPI*)(HWND);
    const auto function = reinterpret_cast<GetDpi>(GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (function) return function(window);
    HDC dc = GetDC(window);
    const UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY)) : 96;
    if (dc) ReleaseDC(window, dc);
    return dpi ? dpi : 96;
}

int scale(const WindowState& s, int value) {
    return MulDiv(value, static_cast<int>(s.dpi), 96);
}

void fonts(WindowState& s) {
    HFONT oldNormal = s.normalFont, oldTitle = s.titleFont, oldStatus = s.statusFont;
    const auto make = [&](int points, int weight) {
        return CreateFontW(-MulDiv(points, static_cast<int>(s.dpi), 72), 0,
            0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };
    s.normalFont = make(10, FW_NORMAL);
    s.titleFont = make(17, FW_SEMIBOLD);
    s.statusFont = make(11, FW_SEMIBOLD);
    HWND controls[] = {s.title, s.subtitle, s.folderLabel, s.folder, s.browse,
        s.check, s.install, s.restore, s.status, s.details, s.reminder, s.footer};
    for (const auto control : controls)
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(s.normalFont), TRUE);
    SendMessageW(s.title, WM_SETFONT, reinterpret_cast<WPARAM>(s.titleFont), TRUE);
    SendMessageW(s.status, WM_SETFONT, reinterpret_cast<WPARAM>(s.statusFont), TRUE);
    if (oldNormal) DeleteObject(oldNormal);
    if (oldTitle) DeleteObject(oldTitle);
    if (oldStatus) DeleteObject(oldStatus);
}

void layout(WindowState& s) {
    RECT area{};
    GetClientRect(s.window, &area);
    const int width = MulDiv(area.right, 96, static_cast<int>(s.dpi));
    const int height = MulDiv(area.bottom, 96, static_cast<int>(s.dpi));
    auto place = [&](HWND control, int x, int y, int w, int h) {
        MoveWindow(control, scale(s, x), scale(s, y), scale(s, std::max(1, w)),
            scale(s, std::max(1, h)), TRUE);
    };
    place(s.title, 22, 17, width - 44, 33);
    place(s.subtitle, 23, 55, width - 46, 24);
    place(s.folderLabel, 23, 93, width - 46, 21);
    place(s.folder, 23, 118, width - 159, 29);
    place(s.browse, width - 126, 117, 103, 31);
    place(s.check, 23, 163, 102, 33);
    place(s.install, 136, 163, 139, 33);
    place(s.restore, 286, 163, 150, 33);
    place(s.status, 23, 213, width - 46, 28);
    place(s.details, 23, 248, width - 46, height - 322);
    place(s.reminder, 23, height - 60, width - 46, 23);
    place(s.footer, 23, height - 33, width - 46, 22);
}

void enableControls(WindowState& s) {
    EnableWindow(s.folder, !s.busy);
    EnableWindow(s.browse, !s.busy);
    EnableWindow(s.check, !s.busy);
    EnableWindow(s.install, !s.busy && s.canInstall);
    EnableWindow(s.restore, !s.busy && s.canRestore);
}

void showResult(WindowState& s, const mtga::Report& result) {
    s.busy = false;
    s.canInstall = result.can_install;
    s.canRestore = result.can_restore;
    SetWindowTextW(s.status, wide(result.headline).c_str());
    SetWindowTextW(s.details, windowsLines(result.detail).c_str());
    SendMessageW(s.details, EM_SETSEL, 0, 0);
    SendMessageW(s.details, EM_SCROLLCARET, 0, 0);
    enableControls(s);
}

void beginAction(WindowState& s, mtga::Action action) {
    if (s.busy) return;
    const std::wstring selected = editText(s.folder);
    if (selected.empty()) {
        SetWindowTextW(s.status, L"Select your Arena installation folder.");
        SetFocus(s.folder);
        return;
    }
    s.busy = true;
    enableControls(s);
    SetWindowTextW(s.status, action == mtga::Action::Status ? L"Checking Core.dll..." :
        action == mtga::Action::Install ? L"Installing and verifying patch..." :
        L"Restoring and verifying original...");
    SetWindowTextW(s.details, L"Please wait. You can close this window when the operation finishes.");
    const HWND target = s.window;
    try {
        std::thread([target, selected, action] {
            auto result = std::make_unique<mtga::Report>();
            try {
                *result = mtga::execute(action, std::filesystem::path(selected));
            } catch (const std::exception& ex) {
                result->success = false;
                result->headline = "Operation could not be completed";
                result->detail = ex.what();
            } catch (...) {
                result->success = false;
                result->headline = "Operation could not be completed";
                result->detail = "An unexpected error occurred. Check the installation folder and try again.";
            }
            // Ownership transfers to the UI only if posting succeeds.
            if (PostMessageW(target, RESULT_MESSAGE, 0, reinterpret_cast<LPARAM>(result.get())))
                result.release();
        }).detach();
    } catch (const std::exception& ex) {
        mtga::Report result{};
        result.headline = "Unable to start operation";
        result.detail = ex.what();
        showResult(s, result);
    }
}

void browseFolder(WindowState& s) {
    IFileDialog* dialog = nullptr;
    const HRESULT created = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(created)) {
        MessageBoxW(s.window, L"The folder picker could not be opened. Type or paste the Arena installation folder instead.",
            TITLE, MB_OK | MB_ICONINFORMATION);
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Select the Arena installation folder");
    IShellItem* current = nullptr;
    const auto path = editText(s.folder);
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&current)))) {
        dialog->SetFolder(current);
        current->Release();
    }
    if (SUCCEEDED(dialog->Show(s.window))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR selected = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected))) {
                SetWindowTextW(s.folder, selected);
                CoTaskMemFree(selected);
                beginAction(s, mtga::Action::Status);
            }
            item->Release();
        }
    }
    dialog->Release();
}

HWND control(WindowState& s, DWORD extended, const wchar_t* type,
    const wchar_t* text, DWORD styles, int id = 0) {
    return CreateWindowExW(extended, type, text, WS_CHILD | WS_VISIBLE | styles,
        0, 0, 0, 0, s.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<WindowState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wparam, lparam);
    auto& s = *state;
    switch (message) {
    case WM_CREATE:
        s.dpi = windowDpi(window);
        s.title = control(s, 0, L"STATIC", TITLE, SS_LEFT);
        s.subtitle = control(s, 0, L"STATIC", L"Keep ultrawide fullscreen through scene transitions.", SS_LEFT);
        s.folderLabel = control(s, 0, L"STATIC", L"Arena installation &folder", SS_LEFT);
        s.folder = control(s, WS_EX_CLIENTEDGE, L"EDIT",
            L"C:\\Program Files\\Wizards of the Coast\\MTGA", ES_AUTOHSCROLL | WS_TABSTOP, Folder);
        SendMessageW(s.folder, EM_SETLIMITTEXT, 32760, 0);
        s.browse = control(s, 0, L"BUTTON", L"&Browse...", WS_TABSTOP | BS_PUSHBUTTON, Browse);
        s.check = control(s, 0, L"BUTTON", L"&Check", WS_TABSTOP | BS_DEFPUSHBUTTON, Check);
        s.install = control(s, 0, L"BUTTON", L"&Install patch", WS_TABSTOP | BS_PUSHBUTTON, Install);
        s.restore = control(s, 0, L"BUTTON", L"&Restore original", WS_TABSTOP | BS_PUSHBUTTON, Restore);
        s.status = control(s, 0, L"STATIC", L"Ready to check your installation", SS_LEFT | SS_ENDELLIPSIS);
        s.details = control(s, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP);
        SendMessageW(s.details, EM_SETLIMITTEXT, 1024 * 1024, 0);
        s.reminder = control(s, 0, L"STATIC", L"Close Arena and its launcher before installing or restoring.", SS_LEFT);
        s.footer = control(s, 0, L"STATIC", L"v3.0.0   \x2022   Program Files changes require administrator access.", SS_LEFT);
        fonts(s);
        layout(s);
        enableControls(s);
        s.initializing = false;
        PostMessageW(window, INITIAL_CHECK_MESSAGE, 0, 0);
        return 0;
    case INITIAL_CHECK_MESSAGE:
        beginAction(s, mtga::Action::Status);
        return 0;
    case RESULT_MESSAGE: {
        std::unique_ptr<mtga::Report> result(reinterpret_cast<mtga::Report*>(lparam));
        showResult(s, *result);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == Folder && HIWORD(wparam) == EN_CHANGE && !s.initializing && !s.busy) {
            s.canInstall = s.canRestore = false;
            enableControls(s);
            SetWindowTextW(s.status, L"Check this folder before making changes.");
            SetWindowTextW(s.details, L"");
            return 0;
        }
        if (s.busy || HIWORD(wparam) != BN_CLICKED) return 0;
        switch (LOWORD(wparam)) {
        case Browse: browseFolder(s); return 0;
        case Check: beginAction(s, mtga::Action::Status); return 0;
        case Install: if (s.canInstall) beginAction(s, mtga::Action::Install); return 0;
        case Restore: if (s.canRestore) beginAction(s, mtga::Action::Restore); return 0;
        // IsDialogMessage maps Enter to IDOK and Escape to IDCANCEL.
        case IDOK: beginAction(s, mtga::Action::Status); return 0;
        case IDCANCEL: SendMessageW(window, WM_CLOSE, 0, 0); return 0;
        }
        break;
    case WM_SIZE:
        if (s.title && wparam != SIZE_MINIMIZED) layout(s);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize.x = scale(s, 650);
        limits->ptMinTrackSize.y = scale(s, 455);
        return 0;
    }
    case WM_DPICHANGED: {
        s.dpi = HIWORD(wparam);
        fonts(s);
        const auto* bounds = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, bounds->left, bounds->top,
            bounds->right - bounds->left, bounds->bottom - bounds->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        layout(s);
        return 0;
    }
    case WM_CLOSE:
        if (s.busy) {
            MessageBoxW(window, L"Please wait for the current operation to finish before closing.",
                TITLE, MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

int run_gui(HINSTANCE instance, int showCommand) {
    // This also works when the caller omits a DPI-awareness manifest.
    using SetAwareness = BOOL(WINAPI*)(HANDLE);
    const auto awareness = reinterpret_cast<SetAwareness>(GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (awareness) awareness(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
    else SetProcessDPIAware();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW definition{};
    definition.cbSize = sizeof(definition);
    definition.hInstance = instance;
    definition.lpfnWndProc = windowProcedure;
    definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    definition.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    definition.hIconSm = definition.hIcon;
    definition.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    definition.lpszClassName = CLASS_NAME;
    if (!RegisterClassExW(&definition)) {
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    WindowState state;
    const DWORD style = WS_OVERLAPPEDWINDOW;
    const DWORD extended = WS_EX_CONTROLPARENT;
    HWND window = CreateWindowExW(extended, CLASS_NAME, TITLE, style,
        CW_USEDEFAULT, CW_USEDEFAULT, 760, 540, nullptr, nullptr, instance, &state);
    if (!window) {
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    // Keep the same usable client size on higher-density displays.
    RECT bounds{0, 0, scale(state, 720), scale(state, 460)};
    using AdjustDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    const auto adjust = reinterpret_cast<AdjustDpi>(GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
    if (adjust) adjust(&bounds, style, FALSE, extended, state.dpi);
    else AdjustWindowRectEx(&bounds, style, FALSE, extended);
    SetWindowPos(window, nullptr, 0, 0, bounds.right - bounds.left,
        bounds.bottom - bounds.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(window, showCommand);
    UpdateWindow(window);
    MSG message{};
    int result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (state.normalFont) DeleteObject(state.normalFont);
    if (state.titleFont) DeleteObject(state.titleFont);
    if (state.statusFont) DeleteObject(state.statusFont);
    UnregisterClassW(CLASS_NAME, instance);
    if (SUCCEEDED(com)) CoUninitialize();
    return result < 0 ? 1 : static_cast<int>(message.wParam);
}
