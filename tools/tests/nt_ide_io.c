/* SPDX-License-Identifier: GPL-3.0-or-later
 * NT5 guest integration probe. Never run against a physical disk handle.
 * Creates a new, fixed 4 MiB file; refuses any pre-existing file. Uses
 * unbuffered write-through I/O, closes/reopens, and checks every DWORD.
 * Passing does not prove every IDE command/timing sequence is correct.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define BLOCK_BYTES 65536ul
#define BLOCK_COUNT 64ul
static const char test_path[] = "C:\\NTIOTEST.BIN";

static void text(const char *message)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, lstrlenA(message), &written, NULL);
}

static void number(const char *name, DWORD value)
{
    static const char digits[] = "0123456789abcdef";
    char output[11];
    unsigned i;
    for (i = 0; i < 8; ++i) {
        output[i] = digits[(value >> ((7 - i) * 4)) & 15];
    }
    output[8] = '\r'; output[9] = '\n'; output[10] = 0;
    text(name); text(output);
}

static DWORD pattern(DWORD block, DWORD word)
{
    DWORD value = (block + 1) * 0x9e3779b9ul ^ word * 0x85ebca6bul;
    value ^= value >> 16;
    return value;
}

static void fail(HANDLE file, const char *operation)
{
    DWORD error = GetLastError();
    text("FAIL: "); text(operation); text("\r\n");
    number("Win32 error=", error);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    text("Any created test file is retained for inspection.\r\n");
    ExitProcess(1);
}

void mainCRTStartup(void)
{
    DWORD *buffer;
    DWORD block, word, transferred, start, written_at, finished_at;
    HANDLE file;
    buffer = VirtualAlloc(NULL, BLOCK_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!buffer) fail(INVALID_HANDLE_VALUE, "VirtualAlloc");
    file = CreateFileA(test_path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
    if (file == INVALID_HANDLE_VALUE) fail(file, "Create new C:\\NTIOTEST.BIN (never overwrite)");
    text("Writing 4 MiB, unbuffered and write-through...\r\n");
    start = GetTickCount();
    for (block = 0; block < BLOCK_COUNT; ++block) {
        for (word = 0; word < BLOCK_BYTES / 4; ++word) buffer[word] = pattern(block, word);
        if (!WriteFile(file, buffer, BLOCK_BYTES, &transferred, NULL) || transferred != BLOCK_BYTES)
            fail(file, "WriteFile");
    }
    if (!FlushFileBuffers(file)) fail(file, "FlushFileBuffers");
    if (!CloseHandle(file)) fail(INVALID_HANDLE_VALUE, "Close after write");
    written_at = GetTickCount();
    file = CreateFileA(test_path, GENERIC_READ, 0, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING, NULL);
    if (file == INVALID_HANDLE_VALUE) fail(file, "Reopen for unbuffered read");
    if (GetFileSize(file, NULL) != BLOCK_BYTES * BLOCK_COUNT) fail(file, "Unexpected file size");
    text("Reopened; reading and checking every DWORD...\r\n");
    for (block = 0; block < BLOCK_COUNT; ++block) {
        if (!ReadFile(file, buffer, BLOCK_BYTES, &transferred, NULL) || transferred != BLOCK_BYTES)
            fail(file, "ReadFile");
        for (word = 0; word < BLOCK_BYTES / 4; ++word) {
            DWORD expected = pattern(block, word);
            if (buffer[word] != expected) {
                number("Mismatch block=", block); number("word=", word);
                number("expected=", expected); number("actual=", buffer[word]);
                CloseHandle(file);
                text("FAIL: data mismatch; test file retained\r\n");
                ExitProcess(1);
            }
        }
    }
    if (!CloseHandle(file)) fail(INVALID_HANDLE_VALUE, "Close after read");
    finished_at = GetTickCount();
    if (!DeleteFileA(test_path)) fail(INVALID_HANDLE_VALUE, "Remove own verified test file");
    number("write+flush ms (hex)=", written_at - start);
    number("read+verify ms (hex)=", finished_at - written_at);
    number("bytes verified (hex)=", BLOCK_BYTES * BLOCK_COUNT);
    text("PASS: unbuffered write/flush/reopen/read comparison; own test file removed\r\n");
    ExitProcess(0);
}
