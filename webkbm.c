#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>

#include "qrcodegen.h"
#include "webapp_data.h"

#define WEBKBM_PORT      9000

#define WM_TRAYICON     (WM_USER + 1)
#define ID_TRAY_EXIT     1001
#define ID_TRAY_SHOWQR   1002

#define MOUSE_BTN_LEFT   0
#define MOUSE_BTN_RIGHT  1
#define MOUSE_BTN_MIDDLE 2

// ------------------------------------------------------------------
// Globals
// ------------------------------------------------------------------

static NOTIFYICONDATAA nid;
static HWND             hwndMsg = NULL;
static HWND             hwndQr  = NULL;
static HANDLE           hServerThread = NULL;
static volatile int     running = 1;
static volatile LONG    clientCount = 0;
static char             serverUrl[160];

static FILE            *logFile;
static CRITICAL_SECTION logLock;

// ------------------------------------------------------------------
// Logging (thread-safe)
// ------------------------------------------------------------------

static void logInit(void) {
    logFile = fopen("webkbm.log", "w");
    if (!logFile) logFile = stdout;
    InitializeCriticalSection(&logLock);
}

static void logMsg(const char *fmt, ...) {
    EnterCriticalSection(&logLock);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(logFile, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logFile, fmt, ap);
    va_end(ap);
    fputc('\n', logFile);
    fflush(logFile);
    LeaveCriticalSection(&logLock);
}

// ------------------------------------------------------------------
// LAN IP discovery
// ------------------------------------------------------------------

static int getLanIp(char *out, size_t outLen) {
    ULONG bufLen = 16 * 1024;
    IP_ADAPTER_ADDRESSES *adapters = (IP_ADAPTER_ADDRESSES *)malloc(bufLen);
    if (!adapters) return -1;

    DWORD r = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        NULL, adapters, &bufLen);
    if (r == ERROR_BUFFER_OVERFLOW) {
        free(adapters);
        adapters = (IP_ADAPTER_ADDRESSES *)malloc(bufLen);
        if (!adapters) return -1;
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 NULL, adapters, &bufLen);
    }
    if (r != NO_ERROR) { free(adapters); return -1; }

    int found = 0;
    for (IP_ADAPTER_ADDRESSES *a = adapters; a && !found; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
            SOCKADDR_IN *sa = (SOCKADDR_IN *)u->Address.lpSockaddr;
            if (sa->sin_family != AF_INET) continue;
            char tmp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sa->sin_addr, tmp, sizeof(tmp));
            if (strncmp(tmp, "169.254.", 8) == 0) continue;
            strncpy(out, tmp, outLen);
            out[outLen - 1] = 0;
            found = 1;
            break;
        }
    }
    free(adapters);
    return found ? 0 : -1;
}

// ------------------------------------------------------------------
// Mouse injection
// ------------------------------------------------------------------

static void injectMove(float dx, float dy) {
    INPUT inp = {0};
    inp.type = INPUT_MOUSE;
    inp.mi.dx = (LONG)dx;
    inp.mi.dy = (LONG)dy;
    inp.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &inp, sizeof(INPUT));
}

static void injectButton(int button, int down) {
    INPUT inp = {0};
    inp.type = INPUT_MOUSE;
    switch (button) {
    case MOUSE_BTN_LEFT:
        inp.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
    case MOUSE_BTN_RIGHT:
        inp.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
    case MOUSE_BTN_MIDDLE:
        inp.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    default: return;
    }
    SendInput(1, &inp, sizeof(INPUT));
}

static void injectScroll(float dx, float dy) {
    if (dy != 0.0f) {
        INPUT inp = {0};
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = MOUSEEVENTF_WHEEL;
        inp.mi.mouseData = (DWORD)(int)(dy * WHEEL_DELTA);
        SendInput(1, &inp, sizeof(INPUT));
    }
    if (dx != 0.0f) {
        INPUT inp = {0};
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        inp.mi.mouseData = (DWORD)(int)(dx * WHEEL_DELTA);
        SendInput(1, &inp, sizeof(INPUT));
    }
}

// ------------------------------------------------------------------
// Keyboard injection
// ------------------------------------------------------------------

static WORD vkFromName(const char *name) {
    if (!strcmp(name, "Escape"))     return VK_ESCAPE;
    if (!strcmp(name, "Tab"))        return VK_TAB;
    if (!strcmp(name, "Enter"))      return VK_RETURN;
    if (!strcmp(name, "Backspace"))  return VK_BACK;
    if (!strcmp(name, "ArrowUp"))    return VK_UP;
    if (!strcmp(name, "ArrowDown"))  return VK_DOWN;
    if (!strcmp(name, "ArrowLeft"))  return VK_LEFT;
    if (!strcmp(name, "ArrowRight")) return VK_RIGHT;
    if (!strcmp(name, "Meta"))       return VK_LWIN;
    if (!strcmp(name, "Control"))    return VK_CONTROL;
    if (!strcmp(name, "Alt"))        return VK_MENU;
    if (!strcmp(name, "Shift"))      return VK_SHIFT;
    if (!strcmp(name, "Space"))      return VK_SPACE;
    if (!strcmp(name, "Home"))       return VK_HOME;
    if (!strcmp(name, "End"))        return VK_END;
    if (!strcmp(name, "PageUp"))     return VK_PRIOR;
    if (!strcmp(name, "PageDown"))   return VK_NEXT;
    if (!strcmp(name, "Delete"))     return VK_DELETE;
    if (!strcmp(name, "Insert"))     return VK_INSERT;
    return 0;
}

static void injectVk(WORD vk, int down) {
    INPUT inp = {0};
    inp.type = INPUT_KEYBOARD;
    inp.ki.wVk = vk;
    inp.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &inp, sizeof(INPUT));
}

// Parse and execute a chord spec like "Escape", "Meta", "Meta+r", "Control+Shift+Escape".
static void injectChord(const char *spec) {
    char buf[128];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    char *parts[8];
    int   n = 0;
    char *p = buf;
    while (*p && n < 8) {
        parts[n++] = p;
        char *plus = strchr(p, '+');
        if (!plus) break;
        *plus = 0;
        p = plus + 1;
    }
    if (n == 0) return;

    WORD modVk[8]; int modCount = 0;
    for (int i = 0; i < n - 1; i++) {
        WORD v = vkFromName(parts[i]);
        if (v) modVk[modCount++] = v;
    }

    const char *last = parts[n - 1];
    WORD lastVk = vkFromName(last);
    WORD targetVk = 0;
    BYTE extraShift = 0;

    if (lastVk) {
        targetVk = lastVk;
    } else if (last[0] != 0 && last[1] == 0) {
        // Single ASCII character: derive VK via VkKeyScan.
        SHORT s = VkKeyScanA(last[0]);
        if (s == -1) return;
        targetVk = (WORD)LOBYTE(s);
        // High byte: shift state. Bit 0 = Shift required by layout.
        if ((HIBYTE(s) & 1) != 0 && modCount < 8) {
            // Only add an implicit Shift if not already armed.
            int hasShift = 0;
            for (int i = 0; i < modCount; i++) if (modVk[i] == VK_SHIFT) { hasShift = 1; break; }
            if (!hasShift) extraShift = 1;
        }
    } else {
        return; // unrecognized
    }

    for (int i = 0; i < modCount; i++) injectVk(modVk[i], 1);
    if (extraShift) injectVk(VK_SHIFT, 1);
    injectVk(targetVk, 1);
    injectVk(targetVk, 0);
    if (extraShift) injectVk(VK_SHIFT, 0);
    for (int i = modCount - 1; i >= 0; i--) injectVk(modVk[i], 0);
}

static void typeUtf8(const char *utf8, size_t len) {
    if (len == 0) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, NULL, 0);
    if (wlen <= 0) return;
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * wlen);
    if (!w) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, w, wlen);

    INPUT *inputs = (INPUT *)malloc(sizeof(INPUT) * wlen * 2);
    if (!inputs) { free(w); return; }

    int ic = 0;
    for (int i = 0; i < wlen; i++) {
        memset(&inputs[ic], 0, sizeof(INPUT));
        inputs[ic].type = INPUT_KEYBOARD;
        inputs[ic].ki.wScan = w[i];
        inputs[ic].ki.dwFlags = KEYEVENTF_UNICODE;
        ic++;
        memset(&inputs[ic], 0, sizeof(INPUT));
        inputs[ic].type = INPUT_KEYBOARD;
        inputs[ic].ki.wScan = w[i];
        inputs[ic].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        ic++;
    }
    SendInput(ic, inputs, sizeof(INPUT));
    free(inputs);
    free(w);
}

// ------------------------------------------------------------------
// Base64 + SHA-1 (BCrypt)
// ------------------------------------------------------------------

static void base64Encode(const unsigned char *in, size_t inLen, char *out) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    char  *p = out;
    while (i + 3 <= inLen) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        *p++ = tbl[(v >> 18) & 63];
        *p++ = tbl[(v >> 12) & 63];
        *p++ = tbl[(v >> 6) & 63];
        *p++ = tbl[v & 63];
        i += 3;
    }
    if (i < inLen) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < inLen) v |= (uint32_t)in[i + 1] << 8;
        *p++ = tbl[(v >> 18) & 63];
        *p++ = tbl[(v >> 12) & 63];
        *p++ = (i + 1 < inLen) ? tbl[(v >> 6) & 63] : '=';
        *p++ = '=';
    }
    *p = 0;
}

static int sha1Bytes(const unsigned char *in, size_t inLen, unsigned char out[20]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0) != 0) return -1;
    BCRYPT_HASH_HANDLE h = NULL;
    int r = -1;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        if (BCryptHashData(h, (PUCHAR)in, (ULONG)inLen, 0) == 0 &&
            BCryptFinishHash(h, out, 20, 0) == 0) {
            r = 0;
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return r;
}

// ------------------------------------------------------------------
// Socket helpers
// ------------------------------------------------------------------

static int recvExact(SOCKET s, char *buf, int n) {
    int total = 0;
    while (total < n) {
        int r = recv(s, buf + total, n - total, 0);
        if (r <= 0) return -1;
        total += r;
    }
    return 0;
}

static int recvLine(SOCKET s, char *buf, int max) {
    int n = 0;
    while (n < max - 1) {
        char c;
        int r = recv(s, &c, 1, 0);
        if (r <= 0) return -1;
        buf[n++] = c;
        if (n >= 2 && buf[n - 2] == '\r' && buf[n - 1] == '\n') {
            buf[n] = 0;
            return n;
        }
    }
    buf[max - 1] = 0;
    return -1;
}

static int sendAll(SOCKET s, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int r = send(s, buf + sent, len - sent, 0);
        if (r <= 0) return -1;
        sent += r;
    }
    return 0;
}

// ------------------------------------------------------------------
// HTTP static serving
// ------------------------------------------------------------------

static void serveStatic(SOCKET s, const char *path) {
    const webapp_route_t *match = NULL;
    const char *lookup = path;
    if (strcmp(lookup, "/") == 0) lookup = "/index.html";
    for (int i = 0; i < webapp_routes_count; i++) {
        if (strcmp(webapp_routes[i].path, lookup) == 0) {
            match = &webapp_routes[i];
            break;
        }
    }
    char hdr[512];
    if (match) {
        int hl = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n\r\n",
            match->mime, match->len);
        sendAll(s, hdr, hl);
        sendAll(s, (const char *)match->data, (int)match->len);
    } else {
        const char *body = "404 not found";
        int hl = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n%s",
            (int)strlen(body), body);
        sendAll(s, hdr, hl);
    }
}

// ------------------------------------------------------------------
// WebSocket framing (RFC 6455)
// ------------------------------------------------------------------

static int doWsHandshake(SOCKET s, const char *clientKey) {
    static const char magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char concat[256];
    int n = snprintf(concat, sizeof(concat), "%s%s", clientKey, magic);
    if (n <= 0 || n >= (int)sizeof(concat)) return -1;
    unsigned char digest[20];
    if (sha1Bytes((unsigned char *)concat, (size_t)n, digest) != 0) return -1;
    char accept[40];
    base64Encode(digest, 20, accept);
    char hdr[512];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        accept);
    return sendAll(s, hdr, hl);
}

// Returns opcode (0..15) on success, -1 on error. *outLen receives payload length (must fit in outMax).
static int wsReadFrame(SOCKET s, unsigned char *out, size_t outMax, size_t *outLen) {
    unsigned char hdr[2];
    if (recvExact(s, (char *)hdr, 2) != 0) return -1;
    int      opcode    = hdr[0] & 0x0F;
    int      masked    = hdr[1] & 0x80;
    uint64_t payloadLen = hdr[1] & 0x7F;
    if (payloadLen == 126) {
        unsigned char ext[2];
        if (recvExact(s, (char *)ext, 2) != 0) return -1;
        payloadLen = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (payloadLen == 127) {
        unsigned char ext[8];
        if (recvExact(s, (char *)ext, 8) != 0) return -1;
        payloadLen = 0;
        for (int i = 0; i < 8; i++) payloadLen = (payloadLen << 8) | ext[i];
    }
    if (payloadLen > outMax) return -1;
    unsigned char mask[4] = {0};
    if (masked && recvExact(s, (char *)mask, 4) != 0) return -1;
    if (payloadLen > 0) {
        if (recvExact(s, (char *)out, (int)payloadLen) != 0) return -1;
        if (masked) {
            for (uint64_t i = 0; i < payloadLen; i++) out[i] ^= mask[i & 3];
        }
    }
    *outLen = (size_t)payloadLen;
    return opcode;
}

static int wsSendFrame(SOCKET s, int opcode, const unsigned char *data, size_t len) {
    unsigned char hdr[10];
    int hl = 0;
    hdr[0] = (unsigned char)(0x80 | (opcode & 0x0F));
    if (len < 126) {
        hdr[1] = (unsigned char)len;
        hl = 2;
    } else if (len <= 0xFFFF) {
        hdr[1] = 126;
        hdr[2] = (unsigned char)((len >> 8) & 0xFF);
        hdr[3] = (unsigned char)(len & 0xFF);
        hl = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++) hdr[2 + i] = (unsigned char)((len >> ((7 - i) * 8)) & 0xFF);
        hl = 10;
    }
    if (sendAll(s, (char *)hdr, hl) != 0) return -1;
    if (len > 0 && sendAll(s, (const char *)data, (int)len) != 0) return -1;
    return 0;
}

// ------------------------------------------------------------------
// Message dispatch
// ------------------------------------------------------------------

static void handleWsMessage(const char *msg, size_t len) {
    if (len < 1) return;
    char op = msg[0];

    switch (op) {
    case 'M': {  // mouse move: "M<dx> <dy>"
        char buf[64];
        size_t cl = (len - 1 < sizeof(buf) - 1) ? len - 1 : sizeof(buf) - 1;
        memcpy(buf, msg + 1, cl);
        buf[cl] = 0;
        float dx = 0, dy = 0;
        if (sscanf(buf, "%f %f", &dx, &dy) == 2) injectMove(dx, dy);
        break;
    }
    case 'S': {  // scroll: "S<dx> <dy>"
        char buf[64];
        size_t cl = (len - 1 < sizeof(buf) - 1) ? len - 1 : sizeof(buf) - 1;
        memcpy(buf, msg + 1, cl);
        buf[cl] = 0;
        float dx = 0, dy = 0;
        if (sscanf(buf, "%f %f", &dx, &dy) == 2) injectScroll(dx, dy);
        break;
    }
    case 'D':
    case 'U':
    case 'C': {  // button down/up/click
        if (len < 2) break;
        int btn = -1;
        switch (msg[1]) {
        case 'l': btn = MOUSE_BTN_LEFT; break;
        case 'r': btn = MOUSE_BTN_RIGHT; break;
        case 'm': btn = MOUSE_BTN_MIDDLE; break;
        }
        if (btn < 0) break;
        if (op == 'C') { injectButton(btn, 1); injectButton(btn, 0); }
        else           { injectButton(btn, op == 'D'); }
        break;
    }
    case 'T':  // type unicode text
        typeUtf8(msg + 1, len - 1);
        break;
    case 'K': {  // special key / chord
        char buf[128];
        size_t cl = (len - 1 < sizeof(buf) - 1) ? len - 1 : sizeof(buf) - 1;
        memcpy(buf, msg + 1, cl);
        buf[cl] = 0;
        injectChord(buf);
        break;
    }
    default:
        break;
    }
}

// ------------------------------------------------------------------
// Per-connection thread
// ------------------------------------------------------------------

static DWORD WINAPI clientThread(LPVOID param) {
    SOCKET s = (SOCKET)(uintptr_t)param;
    char line[2048];

    if (recvLine(s, line, sizeof(line)) <= 0) { closesocket(s); return 0; }

    char method[16] = {0};
    char path[512]  = {0};
    if (sscanf(line, "%15s %511s", method, path) != 2) { closesocket(s); return 0; }

    int  wantUpgrade = 0;
    char wsKey[128]  = {0};

    for (;;) {
        int n = recvLine(s, line, sizeof(line));
        if (n <= 0) { closesocket(s); return 0; }
        if (n == 2 && line[0] == '\r' && line[1] == '\n') break;
        if (n >= 2) line[n - 2] = 0;

        if (_strnicmp(line, "Upgrade:", 8) == 0) {
            if (strstr(line, "websocket") || strstr(line, "WebSocket")) wantUpgrade |= 1;
        } else if (_strnicmp(line, "Sec-WebSocket-Key:", 18) == 0) {
            const char *v = line + 18;
            while (*v == ' ' || *v == '\t') v++;
            strncpy(wsKey, v, sizeof(wsKey) - 1);
            wantUpgrade |= 2;
        }
    }

    if (strcmp(path, "/ws") == 0 && wantUpgrade == 3) {
        if (doWsHandshake(s, wsKey) != 0) { closesocket(s); return 0; }
        InterlockedIncrement(&clientCount);
        logMsg("WS connected (clients=%ld)", clientCount);

        unsigned char payload[8192];
        size_t plen;
        while (running) {
            int op = wsReadFrame(s, payload, sizeof(payload), &plen);
            if (op < 0) break;
            if (op == 0x8) {            // close
                wsSendFrame(s, 0x8, NULL, 0);
                break;
            } else if (op == 0x9) {     // ping -> pong
                wsSendFrame(s, 0xA, payload, plen);
            } else if (op == 0x1) {     // text
                handleWsMessage((const char *)payload, plen);
            }
            // continuation, binary, pong: ignored
        }

        InterlockedDecrement(&clientCount);
        logMsg("WS disconnected (clients=%ld)", clientCount);
    } else if (strcmp(method, "GET") == 0) {
        serveStatic(s, path);
    } else {
        const char *body = "405 method not allowed";
        char hdr[256];
        int hl = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
            (int)strlen(body), body);
        sendAll(s, hdr, hl);
    }

    closesocket(s);
    return 0;
}

// ------------------------------------------------------------------
// Server thread
// ------------------------------------------------------------------

static DWORD WINAPI serverThread(LPVOID param) {
    (void)param;
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        logMsg("socket() failed: %d", WSAGetLastError());
        return 1;
    }
    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(WEBKBM_PORT);

    if (bind(listenSock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        logMsg("bind() failed: %d", WSAGetLastError());
        closesocket(listenSock);
        return 1;
    }
    if (listen(listenSock, 8) == SOCKET_ERROR) {
        logMsg("listen() failed: %d", WSAGetLastError());
        closesocket(listenSock);
        return 1;
    }
    logMsg("Listening on port %d", WEBKBM_PORT);

    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listenSock, &fds);
        struct timeval tv = {1, 0};
        int sel = select(0, &fds, NULL, NULL, &tv);
        if (sel > 0) {
            SOCKET c = accept(listenSock, NULL, NULL);
            if (c != INVALID_SOCKET) {
                int nd = 1;
                setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&nd, sizeof(nd));

                // TCP keepalive so we notice dead phones (locked screen,
                // backgrounded tab, suspended Wi-Fi) within ~30s instead of
                // Windows' default ~2 hours.
                struct tcp_keepalive ka;
                ka.onoff             = 1;
                ka.keepalivetime     = 15000;  // first probe after 15s idle
                ka.keepaliveinterval = 5000;   // 5s between probes
                DWORD outBytes = 0;
                WSAIoctl(c, SIO_KEEPALIVE_VALS, &ka, sizeof(ka),
                         NULL, 0, &outBytes, NULL, NULL);

                HANDLE t = CreateThread(NULL, 0, clientThread, (LPVOID)(uintptr_t)c, 0, NULL);
                if (t) CloseHandle(t);
                else closesocket(c);
            }
        }
    }

    closesocket(listenSock);
    return 0;
}

// ------------------------------------------------------------------
// QR window
// ------------------------------------------------------------------

static uint8_t qrCodeBytes[qrcodegen_BUFFER_LEN_MAX];
static int     qrValid = 0;

static LRESULT CALLBACK qrWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HBRUSH wh = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(dc, &rc, wh);
        DeleteObject(wh);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0, 0, 0));

        HFONT font = CreateFontA(20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                  ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT oldFont = (HFONT)SelectObject(dc, font);
        RECT txtRc = { 0, 12, rc.right, 44 };
        DrawTextA(dc, serverUrl, -1, &txtRc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, oldFont);
        DeleteObject(font);

        if (qrValid) {
            int qrSize = qrcodegen_getSize(qrCodeBytes);
            int yStart = 56;
            int avail  = (rc.bottom - yStart) - 16;
            int availW = rc.right - 32;
            int side   = (avail < availW) ? avail : availW;
            int modulePx = side / qrSize;
            if (modulePx < 1) modulePx = 1;
            int qrPx = modulePx * qrSize;
            int xStart = (rc.right - qrPx) / 2;
            HBRUSH bk = CreateSolidBrush(RGB(0, 0, 0));
            for (int y = 0; y < qrSize; y++) {
                for (int x = 0; x < qrSize; x++) {
                    if (qrcodegen_getModule(qrCodeBytes, x, y)) {
                        RECT m = { xStart + x * modulePx, yStart + y * modulePx,
                                   xStart + (x + 1) * modulePx, yStart + (y + 1) * modulePx };
                        FillRect(dc, &m, bk);
                    }
                }
            }
            DeleteObject(bk);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        hwndQr = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void showQrWindow(void) {
    if (hwndQr) {
        ShowWindow(hwndQr, SW_SHOW);
        SetForegroundWindow(hwndQr);
        return;
    }

    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    qrValid = qrcodegen_encodeText(serverUrl, tmp, qrCodeBytes,
                                   qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                   qrcodegen_Mask_AUTO, true) ? 1 : 0;

    static int registered = 0;
    if (!registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc   = qrWndProc;
        wc.hInstance     = GetModuleHandle(NULL);
        wc.lpszClassName = "WebkbmQrClass";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        RegisterClassA(&wc);
        registered = 1;
    }

    int W = 420, H = 500;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;
    hwndQr = CreateWindowExA(0, "WebkbmQrClass", "webkbm - scan to connect",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             sx, sy, W, H, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (hwndQr) {
        ShowWindow(hwndQr, SW_SHOW);
        SetForegroundWindow(hwndQr);
    }
}

// ------------------------------------------------------------------
// Tray icon
// ------------------------------------------------------------------

static HICON createTrayIcon(void) {
    HDC screenDC = GetDC(NULL);
    HDC memDC    = CreateCompatibleDC(screenDC);
    HBITMAP hbmColor = CreateCompatibleBitmap(screenDC, 16, 16);
    HBITMAP hbmMask  = CreateBitmap(16, 16, 1, 1, NULL);
    SelectObject(memDC, hbmColor);

    HBRUSH bg = CreateSolidBrush(RGB(16, 20, 24));
    RECT rc = {0, 0, 16, 16};
    FillRect(memDC, &rc, bg);
    DeleteObject(bg);

    // Tiny stylized "kbd": three rows of dots in accent color
    HBRUSH fg = CreateSolidBrush(RGB(108, 182, 255));
    int dots[3][4] = {
        {2, 4, 4, 6},
        {6, 4, 8, 6},
        {10, 4, 12, 6}
    };
    for (int i = 0; i < 3; i++) {
        RECT r = { dots[i][0], dots[i][1], dots[i][2], dots[i][3] };
        FillRect(memDC, &r, fg);
        RECT r2 = { dots[i][0], dots[i][1] + 4, dots[i][2], dots[i][3] + 4 };
        FillRect(memDC, &r2, fg);
    }
    RECT bar = { 4, 12, 12, 14 };
    FillRect(memDC, &bar, fg);
    DeleteObject(fg);

    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);

    ICONINFO ii = {0};
    ii.fIcon    = TRUE;
    ii.hbmColor = hbmColor;
    ii.hbmMask  = hbmMask;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmMask);
    return icon;
}

static void showTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING | MF_DISABLED, 0, serverUrl);
    char status[64];
    if (clientCount > 0) snprintf(status, sizeof(status), "%ld client(s) connected", clientCount);
    else                 snprintf(status, sizeof(status), "no clients connected");
    AppendMenuA(menu, MF_STRING | MF_DISABLED, 0, status);
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, ID_TRAY_SHOWQR, "Show QR code\tscan with phone");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, ID_TRAY_EXIT, "Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

static LRESULT CALLBACK msgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) showTrayMenu(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TRAY_EXIT:
            running = 0;
            Shell_NotifyIconA(NIM_DELETE, &nid);
            PostQuitMessage(0);
            break;
        case ID_TRAY_SHOWQR:
            showQrWindow();
            break;
        }
        return 0;
    case WM_DESTROY:
        running = 0;
        Shell_NotifyIconA(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------
// Elevation
// ------------------------------------------------------------------

static int isAdmin(void) {
    BOOL elevated = FALSE;
    HANDLE token = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION te;
        DWORD sz;
        if (GetTokenInformation(token, TokenElevation, &te, sizeof(te), &sz)) {
            elevated = te.TokenIsElevated;
        }
        CloseHandle(token);
    }
    return elevated ? 1 : 0;
}

// Relaunch self via UAC. Returns 0 if a child was started (caller should exit),
// -1 if the user declined or the relaunch failed (caller may continue degraded).
static int relaunchAsAdmin(void) {
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return -1;

    char dir[MAX_PATH];
    strncpy(dir, exe, MAX_PATH);
    dir[MAX_PATH - 1] = 0;
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = 0;

    SHELLEXECUTEINFOA sei = {0};
    sei.cbSize     = sizeof(sei);
    sei.lpVerb     = "runas";
    sei.lpFile     = exe;
    sei.lpDirectory = dir;
    sei.nShow      = SW_SHOWNORMAL;
    if (!ShellExecuteExA(&sei)) return -1;
    return 0;
}

// ------------------------------------------------------------------
// Entry point
// ------------------------------------------------------------------

int main(void) {
    // Without admin we can't SendInput into elevated foreground windows
    // (UIPI blocks lower-integrity input). Self-elevate via UAC.
    if (!isAdmin()) {
        if (relaunchAsAdmin() == 0) return 0;
        // User declined UAC or relaunch failed — continue degraded.
    }

    logInit();
    logMsg("webkbm starting (elevated=%d)", isAdmin());

    char ip[64] = "127.0.0.1";
    getLanIp(ip, sizeof(ip));
    snprintf(serverUrl, sizeof(serverUrl), "http://%s:%d/", ip, WEBKBM_PORT);
    logMsg("URL: %s", serverUrl);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        logMsg("WSAStartup failed");
        return 1;
    }

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = msgWndProc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.lpszClassName = "WebkbmMsgClass";
    RegisterClassA(&wc);
    hwndMsg = CreateWindowExA(0, "WebkbmMsgClass", "webkbm", 0,
                              0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);

    memset(&nid, 0, sizeof(nid));
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwndMsg;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon            = createTrayIcon();
    strncpy(nid.szTip, "webkbm", sizeof(nid.szTip) - 1);
    Shell_NotifyIconA(NIM_ADD, &nid);

    hServerThread = CreateThread(NULL, 0, serverThread, NULL, 0, NULL);
    logMsg("System tray active");

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    running = 0;
    if (hServerThread) {
        WaitForSingleObject(hServerThread, 3000);
        CloseHandle(hServerThread);
    }
    if (nid.hIcon) DestroyIcon(nid.hIcon);
    WSACleanup();
    DeleteCriticalSection(&logLock);
    if (logFile && logFile != stdout) fclose(logFile);
    return 0;
}

#else
#include <stdio.h>
int main(void) {
    printf("webkbm - Windows only\n");
    return 1;
}
#endif
