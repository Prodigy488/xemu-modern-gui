//
// xemu User Interface - Desktop shortcuts for games
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "game-shortcut.hh"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objidl.h>

static std::wstring Widen(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) {
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    }
    return w;
}

// GUIDs spelled out so no extra import library is needed
static const CLSID kClsidShellLink = {
    0x00021401, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 }
};
static const IID kIidShellLinkW = {
    0x000214F9, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 }
};
static const IID kIidPersistFile = {
    0x0000010B, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 }
};
static const GUID kFolderDesktop = {
    0xB4BFCC3A, 0xDB2C, 0x424C, { 0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41 }
};

bool GameShortcutsSupported(void)
{
    return true;
}

bool CreateGameDesktopShortcut(const std::string &title,
                               const std::string &image_path,
                               std::string &error)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n == MAX_PATH) {
        error = "Could not find xemu.exe";
        return false;
    }
    std::wstring exe_dir(exe);
    exe_dir = exe_dir.substr(0, exe_dir.find_last_of(L"\\/"));

    PWSTR desktop = NULL;
    if (FAILED(SHGetKnownFolderPath(kFolderDesktop, 0, NULL, &desktop))) {
        error = "Could not find the desktop folder";
        return false;
    }
    std::wstring name = Widen(title);
    for (auto &c : name) {
        if (wcschr(L"\\/:*?\"<>|", c) || c < 32) {
            c = L'_';
        }
    }
    if (name.empty()) {
        name = L"xemu game";
    }
    std::wstring link_path = std::wstring(desktop) + L"\\" + name + L".lnk";
    CoTaskMemFree(desktop);

    std::wstring args = L"-dvd_path \"" + Widen(image_path) + L"\"";

    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    if (SUCCEEDED(CoCreateInstance(kClsidShellLink, NULL, CLSCTX_INPROC_SERVER,
                                   kIidShellLinkW, (void **)&link))) {
        link->SetPath(exe);
        link->SetArguments(args.c_str());
        link->SetWorkingDirectory(exe_dir.c_str());
        link->SetIconLocation(exe, 0);
        link->SetDescription((L"Play " + name + L" in xemu").c_str());
        if (SUCCEEDED(link->QueryInterface(kIidPersistFile, (void **)&file))) {
            ok = SUCCEEDED(file->Save(link_path.c_str(), TRUE));
            file->Release();
        }
        link->Release();
    }
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    if (!ok) {
        error = "Could not create the shortcut";
    }
    return ok;
}

#else

bool GameShortcutsSupported(void)
{
    return false;
}

bool CreateGameDesktopShortcut(const std::string &, const std::string &,
                               std::string &error)
{
    error = "Desktop shortcuts are only supported on Windows";
    return false;
}

#endif
