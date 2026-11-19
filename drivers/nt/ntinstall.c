/* SPDX-License-Identifier: GPL-3.0-or-later
 * NT5 guest installer for the project's exact PCI subsystem ID.
 * Never load this program or its driver on the development host.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef BOOL (WINAPI *UpdateDriver)(HWND, LPCSTR, LPCSTR, DWORD, PBOOL);

static void text(const char *message)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, lstrlenA(message), &written, NULL);
}

static void error(DWORD value)
{
    char number[11];
    unsigned i;
    static const char digits[] = "0123456789abcdef";
    for (i = 0; i < 8; ++i) {
        number[i] = digits[(value >> ((7 - i) * 4)) & 15];
    }
    number[8] = '\r'; number[9] = '\n'; number[10] = 0;
    text("Win32 error (hex)=");
    text(number);
}

void mainCRTStartup(void)
{
    OSVERSIONINFOA version;
    char path[MAX_PATH], library_path[MAX_PATH];
    char *argument = GetCommandLineA();
    DWORD length, last_separator, i, failure;
    HMODULE library;
    UpdateDriver update;
    BOOL reboot = FALSE, installed;
    LPCSTR inf_name, hardware_id;

    if (*argument == '"') {
        ++argument;
        while (*argument && *argument != '"') { ++argument; }
        if (*argument) { ++argument; }
    } else {
        while (*argument && *argument != ' ' && *argument != '\t') { ++argument; }
    }
    while (*argument == ' ' || *argument == '\t') { ++argument; }
    if (lstrcmpA(argument, "--install") == 0) {
        inf_name = "WGPUNT.INF";
        hardware_id = "PCI\\VEN_8086&DEV_22B0&SUBSYS_57471234";
    } else if (lstrcmpA(argument, "--display") == 0) {
        inf_name = "WGPUDSP.INF";
        hardware_id = "PCI\\VEN_1234&DEV_1111";
    } else {
        text("Usage: ntinstall --install | --display\r\nInstalls the adjacent WGPU or framebuffer INF on its matching NT5 guest device.\r\n");
        ExitProcess(2);
    }
    version.dwOSVersionInfoSize = sizeof(version);
    if (!GetVersionExA(&version) || version.dwPlatformId != VER_PLATFORM_WIN32_NT ||
        version.dwMajorVersion != 5 || version.dwMinorVersion > 1) {
        text("Refused: this installer targets Windows 2000/XP guests only.\r\n");
        ExitProcess(2);
    }
    length = GetModuleFileNameA(NULL, path, sizeof(path));
    last_separator = 0;
    if (!length || length >= sizeof(path)) { ExitProcess(2); }
    for (i = 0; i < length; ++i) {
        if (path[i] == '\\') { last_separator = i + 1; }
    }
    if (!last_separator || last_separator + lstrlenA(inf_name) + 1 > sizeof(path)) {
        ExitProcess(2);
    }
    lstrcpyA(path + last_separator, inf_name);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        error(GetLastError()); ExitProcess(1);
    }
    length = GetSystemDirectoryA(library_path, sizeof(library_path));
    if (!length || length + sizeof("\\newdev.dll") > sizeof(library_path)) {
        ExitProcess(2);
    }
    lstrcpyA(library_path + length, "\\newdev.dll");
    library = LoadLibraryA(library_path);
    if (!library) { error(GetLastError()); ExitProcess(1); }
    update = (UpdateDriver)(void *)GetProcAddress(library, "UpdateDriverForPlugAndPlayDevicesA");
    if (!update) { error(GetLastError()); FreeLibrary(library); ExitProcess(1); }
    text("PnP install: "); text(hardware_id); text("\r\nINF: ");
    text(path); text("\r\n");
    /* Flags zero: preserve normal matching, ranking and signature prompts.
     * No forced binding, arbitrary service creation or automatic reboot. */
    installed = update(NULL, hardware_id, path, 0, &reboot);
    failure = installed ? 0 : GetLastError();
    FreeLibrary(library);
    if (!installed) { error(failure); ExitProcess(1); }
    text(reboot ? "Installed; Windows restart required.\r\n" : "Installed; no restart requested.\r\n");
    ExitProcess(reboot ? 3 : 0);
}
