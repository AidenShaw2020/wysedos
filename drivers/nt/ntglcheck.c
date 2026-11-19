/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Guest-only render and ICD selection check through the system OPENGL32. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>

static const char class_name[] = "WyseNtGlCheck";

typedef enum {
    COLOR_CHECK, DEPTH_CHECK, TEXTURE_CHECK, SCISSOR_CHECK, RESIZE_CHECK,
    SINGLE_CHECK, MIP_CHECK, LIGHTMAP_CHECK, INVALID_CHECK
} CheckMode;

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, lstrlenA(text), &written, 0);
}

static void report_icd_escape(HDC dc)
{
    enum { OPENGL_GETINFO = 0x1101 };
    struct {
        DWORD interface_version, driver_version;
        WCHAR name[256];
    } info = {0};
    char name[256];

    if (ExtEscape(dc, OPENGL_GETINFO, 0, 0, sizeof info, (char *)&info) <= 0) {
        say("Display ICD escape: unavailable\r\n");
        return;
    }
    info.name[255] = 0;
    if (WideCharToMultiByte(CP_ACP, 0, info.name, -1, name,
                            sizeof name, 0, 0)) {
        say("Display ICD escape: ");
        say(name);
        say("\r\n");
    }
}

static HWND create_window(void)
{
    WNDCLASSA cls = {0};
    cls.style = CS_OWNDC;
    cls.lpfnWndProc = DefWindowProcA;
    cls.hInstance = GetModuleHandleA(0);
    cls.lpszClassName = class_name;
    if (!RegisterClassA(&cls)) return 0;
    return CreateWindowA(class_name, "OpenGL render check", WS_OVERLAPPEDWINDOW,
                         16, 16, 228, 244, 0, 0, cls.hInstance, 0);
}

static void draw_triangle(float z)
{
    glBegin(GL_TRIANGLES);
    glTexCoord2f(0.25f, 0.25f);
    glVertex3f(-0.8f, -0.8f, z);
    glVertex3f(0.8f, -0.8f, z);
    glVertex3f(0.0f, 0.8f, z);
    glEnd();
}

static BOOL check_single_buffer(void)
{
    GLboolean doubled = GL_TRUE;
    GLint draw_buffer = 0, read_buffer = 0;
    glGetBooleanv(GL_DOUBLEBUFFER, &doubled);
    glGetIntegerv(GL_DRAW_BUFFER, &draw_buffer);
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    if (doubled || draw_buffer != GL_FRONT || read_buffer != GL_FRONT ||
        glGetError() != GL_NO_ERROR)
        return FALSE;
    glDrawBuffer(GL_BACK);
    if (glGetError() != GL_INVALID_OPERATION) return FALSE;
    glReadBuffer(GL_BACK);
    return glGetError() == GL_INVALID_OPERATION;
}

static BOOL check_mip_pixels(HDC dc)
{
    static GLubyte texels[64 * 64 * 4];
    static const GLint filters[] = {GL_NEAREST, GL_NEAREST_MIPMAP_NEAREST,
                                    GL_LINEAR_MIPMAP_LINEAR};
    GLubyte sample[3] = {0};
    GLuint texture = 0;
    unsigned edge, level, i, pass;
    BOOL ok = TRUE;
    glGenTextures(1, &texture);
    if (!texture) return FALSE;
    glBindTexture(GL_TEXTURE_2D, texture);
    for (edge = 64, level = 0; edge; edge /= 2, ++level) {
        for (i = 0; i < edge * edge; ++i) {
            texels[4 * i] = level == 0 ? 255 : 0;
            texels[4 * i + 1] = level == 0 ? 0 : 255;
            texels[4 * i + 2] = 0;
            texels[4 * i + 3] = 255;
        }
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, edge, edge, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, texels);
    }
    glViewport(0, 0, 128, 128);
    glEnable(GL_TEXTURE_2D);
    glColor3ub(255, 255, 255);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    for (pass = 0; pass < sizeof filters / sizeof filters[0]; ++pass) {
        unsigned channel = pass ? 1 : 0;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filters[pass]);
        glClearColor(0, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        glTexCoord2f(0, 0);
        glVertex2f(-0.8f, -0.8f);
        glTexCoord2f(32, 0);
        glVertex2f(0.8f, -0.8f);
        glTexCoord2f(16, 32);
        glVertex2f(0, 0.8f);
        glEnd();
        glReadPixels(64, 64, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, sample);
        if (glGetError() != GL_NO_ERROR || sample[channel] < 240 ||
            sample[1 - channel] > 15 || sample[2] > 15) {
            static const char hex[] = "0123456789ABCDEF";
            char report[] = "Mip test pass 0, RGB 00 00 00\r\n";
            report[14] = (char)('0' + pass);
            for (i = 0; i < 3; ++i) {
                report[21 + 3 * i] = hex[sample[i] >> 4];
                report[22 + 3 * i] = hex[sample[i] & 15];
            }
            say(report);
            ok = FALSE;
            break;
        }
    }
    glDeleteTextures(1, &texture);
    return ok && SwapBuffers(dc);
}

/* A two-pass lightmap multiplies the destination by the texture RGB.
 * Use unequal channels to catch channel order and low-intensity errors.
 */
static BOOL check_lightmap_pixels(HDC dc)
{
    static const GLubyte texel[][4] = {{64, 96, 128, 255}, {128, 64, 96, 255}};
    static const int expected[][3] = {{32, 48, 64}, {64, 32, 48}};
    GLubyte sample[3] = {0};
    GLuint texture = 0;
    unsigned channel, pass;
    BOOL match = TRUE;
    glViewport(0, 0, 128, 128);
    glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glGenTextures(1, &texture);
    if (!texture) return FALSE;
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, texel[0]);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    for (pass = 0; pass < 2 && match; ++pass) {
        if (pass != 0) {
            glClear(GL_COLOR_BUFFER_BIT);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA,
                            GL_UNSIGNED_BYTE, texel[pass]);
        }
        draw_triangle(0.0f);
        glReadPixels(64, 64, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, sample);
        for (channel = 0; channel < 3; ++channel) {
            const int delta = sample[channel] - expected[pass][channel];
            if (delta < -9 || delta > 9) match = FALSE;
        }
    }
    glDeleteTextures(1, &texture);
    return match && glGetError() == GL_NO_ERROR && SwapBuffers(dc);
}

static BOOL check_pixels(HDC dc, CheckMode mode)
{
    static const GLubyte texels[] = {
        0, 255, 0, 255,   255, 0, 0, 255,
        255, 0, 0, 255,   255, 0, 0, 255
    };
    GLubyte inside[3] = {0}, outside[3] = {0};
    GLuint texture = 0;
    unsigned channel = mode == TEXTURE_CHECK ? 1 : 0;
    glViewport(0, 0, 128, 128);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    if (mode == DEPTH_CHECK) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    } else {
        glClear(GL_COLOR_BUFFER_BIT);
    }
    if (mode == TEXTURE_CHECK) {
        glGenTextures(1, &texture);
        if (!texture) return FALSE;
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, texels);
        glEnable(GL_TEXTURE_2D);
        glColor3ub(255, 255, 255);
    } else {
        glColor3ub(255, 0, 0);
    }
    if (mode == SCISSOR_CHECK) {
        glScissor(56, 56, 16, 16);
        glEnable(GL_SCISSOR_TEST);
    }
    draw_triangle(-0.5f);
    if (mode == DEPTH_CHECK) {
        glColor3ub(0, 255, 0);
        draw_triangle(0.5f);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(mode == SINGLE_CHECK ? GL_FRONT : GL_BACK);
    glReadPixels(64, 64, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, inside);
    if (mode == SCISSOR_CHECK) {
        glDisable(GL_SCISSOR_TEST);
        glReadPixels(46, 60, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, outside);
    } else {
        glReadPixels(3, 3, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, outside);
    }
    if (texture) glDeleteTextures(1, &texture);
    if (glGetError() != GL_NO_ERROR || inside[channel] < 240 || inside[1 - channel] > 15 ||
        inside[2] > 15 || outside[0] > 15 || outside[1] > 15 || outside[2] < 240)
        return FALSE;
    return mode == SINGLE_CHECK || SwapBuffers(dc);
}

static int find_single_format(HDC dc, BYTE color_bits)
{
    PIXELFORMATDESCRIPTOR format;
    int index, count = DescribePixelFormat(dc, 1, 0, 0);
    const DWORD required = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    for (index = 1; index <= count; ++index) {
        if (DescribePixelFormat(dc, index, sizeof format, &format) &&
            (format.dwFlags & (required | PFD_DOUBLEBUFFER | PFD_GENERIC_FORMAT)) == required &&
            format.iPixelType == PFD_TYPE_RGBA && format.cColorBits == color_bits &&
            format.cDepthBits >= 16)
            return index;
    }
    return 0;
}

static int run(HWND window, HDC dc, CheckMode mode)
{
    PIXELFORMATDESCRIPTOR desired = {0}, actual = {0};
    HGLRC context;
    const GLubyte *renderer;
    int format;
    BOOL rendered, generic;

    desired.nSize = sizeof desired;
    desired.nVersion = 1;
    desired.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    if (mode != SINGLE_CHECK) desired.dwFlags |= PFD_DOUBLEBUFFER;
    desired.iPixelType = PFD_TYPE_RGBA;
    desired.cColorBits = (BYTE)GetDeviceCaps(dc, BITSPIXEL);
    desired.cDepthBits = 16;
    report_icd_escape(dc);
    format = mode == SINGLE_CHECK ? find_single_format(dc, desired.cColorBits) :
                                   ChoosePixelFormat(dc, &desired);
    if (!format || !DescribePixelFormat(dc, format, sizeof actual, &actual) ||
        (actual.dwFlags & desired.dwFlags) != desired.dwFlags ||
        !SetPixelFormat(dc, format, &actual)) {
        say("FAIL: requested OpenGL pixel format\r\n");
        return 1;
    }
    context = wglCreateContext(dc);
    if (!context || !wglMakeCurrent(dc, context)) {
        if (context) wglDeleteContext(context);
        say("FAIL: OpenGL context\r\n");
        return 1;
    }
    generic = (actual.dwFlags & PFD_GENERIC_FORMAT) != 0;
    renderer = glGetString(GL_RENDERER);
    say("Renderer: ");
    say(renderer ? (const char *)renderer : "unknown");
    say("\r\n");
    rendered = renderer && (mode != SINGLE_CHECK || check_single_buffer()) &&
               (mode == MIP_CHECK ? check_mip_pixels(dc) :
                mode == LIGHTMAP_CHECK ? check_lightmap_pixels(dc) : check_pixels(dc, mode));
    if (rendered && mode == RESIZE_CHECK) {
        RECT before, after;
        rendered = GetClientRect(window, &before) &&
                   SetWindowPos(window, 0, 0, 0, 301, 251,
                                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) &&
                   GetClientRect(window, &after) &&
                   before.right != after.right && before.bottom != after.bottom &&
                   check_pixels(dc, COLOR_CHECK);
    }
    wglMakeCurrent(0, 0);
    wglDeleteContext(context);
    if (!rendered) {
        say("FAIL: expected triangle or background color, or SwapBuffers\r\n");
        return 1;
    }
    if (mode == DEPTH_CHECK) say("PASS: nearer triangle occludes farther triangle\r\n");
    if (mode == TEXTURE_CHECK) say("PASS: triangle sampled the green texel\r\n");
    if (mode == LIGHTMAP_CHECK) say("PASS: lightmap RGB multiplied the framebuffer color before and after the texture update\r\n");
    if (mode == MIP_CHECK) say("PASS: base level red, both mip filters selected green lower levels\r\n");
    if (mode == SCISSOR_CHECK) say("PASS: scissor excluded an interior triangle pixel\r\n");
    if (mode == RESIZE_CHECK) say("PASS: same context rendered after window resize\r\n");
    if (mode == SINGLE_CHECK) {
        say("PASS: single-buffered defaults, back-buffer rejection and front pixels\r\n");
        return 0;
    }
    if (generic) {
        say("PASS: pixels and SwapBuffers, generic pixel format\r\n");
        return 2;
    }
    say("PASS: pixels and SwapBuffers, ICD pixel format\r\n");
    return 0;
}

static CheckMode check_mode(void)
{
    const char *command = GetCommandLineA();
    char option[12];
    unsigned length = 0;
    if (*command == '"') {
        do { ++command; } while (*command && *command != '"');
        if (*command == '"') ++command;
    } else {
        while (*command && *command != ' ' && *command != '\t') ++command;
    }
    while (*command == ' ' || *command == '\t') ++command;
    if (!*command) return COLOR_CHECK;
    while (*command && *command != ' ' && *command != '\t') {
        if (length + 1 >= sizeof option) return INVALID_CHECK;
        option[length++] = *command++;
    }
    option[length] = '\0';
    while (*command == ' ' || *command == '\t') ++command;
    if (*command) return INVALID_CHECK;
    if (lstrcmpA(option, "--depth") == 0) return DEPTH_CHECK;
    if (lstrcmpA(option, "--texture") == 0) return TEXTURE_CHECK;
    if (lstrcmpA(option, "--mip") == 0) return MIP_CHECK;
    if (lstrcmpA(option, "--lightmap") == 0) return LIGHTMAP_CHECK;
    if (lstrcmpA(option, "--scissor") == 0) return SCISSOR_CHECK;
    if (lstrcmpA(option, "--resize") == 0) return RESIZE_CHECK;
    if (lstrcmpA(option, "--single") == 0) return SINGLE_CHECK;
    return INVALID_CHECK;
}

void __stdcall GlCheckEntry(void)
{
    CheckMode mode = check_mode();
    HWND window;
    HDC dc;
    int result = 1;
    if (mode == INVALID_CHECK) {
        say("Usage: NTGLCHK.EXE [--depth|--texture|--mip|--lightmap|--scissor|--resize|--single]\r\n");
        ExitProcess(1);
    }
    window = create_window();
    if (window) {
        ShowWindow(window, SW_SHOWNORMAL);
        UpdateWindow(window);
        dc = GetDC(window);
        if (dc) {
            result = run(window, dc, mode);
            ReleaseDC(window, dc);
        }
        DestroyWindow(window);
    } else {
        say("FAIL: OpenGL check window creation\r\n");
    }
    ExitProcess((UINT)result);
}
