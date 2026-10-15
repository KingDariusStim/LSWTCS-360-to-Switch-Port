// Host side of the network layer (lsw_net.inc in kernel_stubs.cpp is the guest side): plain sockets and the
// console's user account, behind a small C API so kernel_stubs.cpp never includes winsock / BSD / libnx headers
// (its win32 shim on the Switch and <windows.h> on the PC both clash with them).
// Addresses and ports cross this API in host byte order. Errors are returned as WSA codes (10035 = WOULDBLOCK ...),
// which is what the guest's WSAGetLastError expects. Every socket is non-blocking on the host; the guest side
// emulates blocking calls without holding the GIL.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET lsw_sock_t;
#  define LSW_BAD_SOCK INVALID_SOCKET
static int sock_err() { return WSAGetLastError(); }
static int sock_close(lsw_sock_t s) { return closesocket(s); }
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/ioctl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <poll.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <errno.h>
#  ifdef __SWITCH__
#    include <switch.h>
#  endif
typedef int lsw_sock_t;
#  define LSW_BAD_SOCK (-1)
// errno -> WSA code (the guest only knows Winsock numbers).
static int sock_err() {
    switch (errno) {
        case EWOULDBLOCK:  return 10035;
        case EINPROGRESS:  return 10036;
        case EALREADY:     return 10037;
        case ENOTSOCK:     return 10038;
        case EMSGSIZE:     return 10040;
        case ENOPROTOOPT:  return 10042;
        case EPROTONOSUPPORT: return 10043;
        case EAFNOSUPPORT: return 10047;
        case EADDRINUSE:   return 10048;
        case EADDRNOTAVAIL: return 10049;
        case ENETDOWN:     return 10050;
        case ENETUNREACH:  return 10051;
        case ECONNABORTED: return 10053;
        case ECONNRESET:   return 10054;
        case ENOBUFS:      return 10055;
        case EISCONN:      return 10056;
        case ENOTCONN:     return 10057;
        case ETIMEDOUT:    return 10060;
        case ECONNREFUSED: return 10061;
        case EHOSTUNREACH: return 10065;
        case EINVAL:       return 10022;
        case EACCES:       return 10013;
        case EFAULT:       return 10014;
        default:           return 10050;   // WSAENETDOWN
    }
}
static int sock_close(lsw_sock_t s) { return close(s); }
#endif

static thread_local int t_err = 0;
static int fail() { t_err = sock_err(); return -1; }
static sockaddr_in make_sin(uint32_t ip, uint16_t port) {
    sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(ip);
    return a;
}

extern "C" {

int lswnet_last_error(void) { return t_err; }

// Once per process. Switch: the BSD service (libnx socketInitializeDefault); PC: Winsock 2.2.
int lswnet_host_init(void) {
    static std::once_flag once; static int ok = 0;
    std::call_once(once, [] {
#ifdef _WIN32
        WSADATA d; ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
#elif defined(__SWITCH__)
        Result rc = socketInitializeDefault();
        ok = R_SUCCEEDED(rc);
        if (!ok) printf("[NET] socketInitializeDefault failed rc=0x%X\n", (unsigned)rc);
        else nifmInitialize(NifmServiceType_User);   // gethostid fallback in lswnet_local_ip
#else
        ok = 1;
#endif
        printf("[NET] host sockets %s\n", ok ? "ready" : "UNAVAILABLE"); fflush(stdout);
    });
    return ok;
}

// type 1 = stream (TCP), 2 = datagram (UDP). Returns a host socket id >= 0 or -1.
long long lswnet_socket(int type) {
    lsw_sock_t s = socket(AF_INET, type == 1 ? SOCK_STREAM : SOCK_DGRAM, type == 1 ? IPPROTO_TCP : IPPROTO_UDP);
    if (s == LSW_BAD_SOCK) return fail();
#ifdef _WIN32
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    return (long long)s;
}
int lswnet_close(long long s) { return sock_close((lsw_sock_t)s) == 0 ? 0 : fail(); }

int lswnet_bind(long long s, uint32_t ip, uint16_t port) {
    sockaddr_in a = make_sin(ip, port);
    return bind((lsw_sock_t)s, (sockaddr*)&a, sizeof a) == 0 ? 0 : fail();
}
int lswnet_connect(long long s, uint32_t ip, uint16_t port) {
    sockaddr_in a = make_sin(ip, port);
    return connect((lsw_sock_t)s, (sockaddr*)&a, sizeof a) == 0 ? 0 : fail();
}
int lswnet_listen(long long s, int backlog) { return listen((lsw_sock_t)s, backlog) == 0 ? 0 : fail(); }
long long lswnet_accept(long long s, uint32_t* ip, uint16_t* port) {
    sockaddr_in a; socklen_t n = sizeof a; memset(&a, 0, sizeof a);
    lsw_sock_t c = accept((lsw_sock_t)s, (sockaddr*)&a, &n);
    if (c == LSW_BAD_SOCK) return fail();
#ifdef _WIN32
    u_long nb = 1; ioctlsocket(c, FIONBIO, &nb);
#else
    fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) | O_NONBLOCK);
#endif
    if (ip) *ip = ntohl(a.sin_addr.s_addr);
    if (port) *port = ntohs(a.sin_port);
    return (long long)c;
}
// has_to = 0: send() on a connected socket.
int lswnet_sendto(long long s, const void* buf, int len, uint32_t ip, uint16_t port, int has_to) {
    int r;
    if (has_to) { sockaddr_in a = make_sin(ip, port); r = (int)sendto((lsw_sock_t)s, (const char*)buf, len, 0, (sockaddr*)&a, sizeof a); }
    else r = (int)send((lsw_sock_t)s, (const char*)buf, len, 0);
    return r >= 0 ? r : fail();
}
int lswnet_recvfrom(long long s, void* buf, int len, uint32_t* ip, uint16_t* port) {
    sockaddr_in a; socklen_t n = sizeof a; memset(&a, 0, sizeof a);
    int r = (int)recvfrom((lsw_sock_t)s, (char*)buf, len, 0, (sockaddr*)&a, &n);
    if (r < 0) {
#ifdef _WIN32
        // A UDP socket reports the ICMP "port unreachable" of an earlier send as WSAECONNRESET on the next
        // recvfrom; the 360 stack does not, and a peer that has not bound its port yet is normal while joining.
        if (WSAGetLastError() == WSAECONNRESET) { t_err = 10035; return -1; }
#endif
        return fail();
    }
    if (ip) *ip = ntohl(a.sin_addr.s_addr);
    if (port) *port = ntohs(a.sin_port);
    return r;
}
int lswnet_shutdown(long long s, int how) { return shutdown((lsw_sock_t)s, how) == 0 ? 0 : fail(); }

// opt: 1 = SO_BROADCAST, 2 = SO_REUSEADDR, 3 = SO_RCVBUF, 4 = SO_SNDBUF, 5 = TCP_NODELAY.
int lswnet_setopt(long long s, int opt, int value) {
    int level = SOL_SOCKET, name;
    switch (opt) {
        case 1: name = SO_BROADCAST; break;
        case 2: name = SO_REUSEADDR; break;
        case 3: name = SO_RCVBUF; break;
        case 4: name = SO_SNDBUF; break;
        case 5: level = IPPROTO_TCP; name = TCP_NODELAY; break;
        default: t_err = 10042; return -1;
    }
    return setsockopt((lsw_sock_t)s, level, name, (const char*)&value, sizeof value) == 0 ? 0 : fail();
}
int lswnet_pending(long long s) {
#ifdef _WIN32
    u_long n = 0; if (ioctlsocket((lsw_sock_t)s, FIONREAD, &n) != 0) return fail();
#else
    int n = 0; if (ioctl((lsw_sock_t)s, FIONREAD, &n) != 0) return fail();
#endif
    return (int)n;
}
int lswnet_sockname(long long s, uint32_t* ip, uint16_t* port, int peer) {
    sockaddr_in a; socklen_t n = sizeof a; memset(&a, 0, sizeof a);
    int r = peer ? getpeername((lsw_sock_t)s, (sockaddr*)&a, &n) : getsockname((lsw_sock_t)s, (sockaddr*)&a, &n);
    if (r != 0) return fail();
    *ip = ntohl(a.sin_addr.s_addr); *port = ntohs(a.sin_port);
    return 0;
}

// ev/rev bits: 1 = readable, 2 = writable, 4 = error/exception. Returns the number of ready sockets.
int lswnet_poll(int n, const long long* socks, const short* ev, short* rev, int timeout_ms) {
    if (n <= 0) return 0;
    if (n > 256) n = 256;
#ifdef _WIN32
    WSAPOLLFD p[256];
#else
    pollfd p[256];
#endif
    for (int i = 0; i < n; ++i) {
        p[i].fd = (lsw_sock_t)socks[i]; p[i].revents = 0; p[i].events = 0;
        if (ev[i] & 1) p[i].events |= POLLIN;
        if (ev[i] & 2) p[i].events |= POLLOUT;
    }
#ifdef _WIN32
    int r = WSAPoll(p, (ULONG)n, timeout_ms);
#else
    int r = poll(p, (nfds_t)n, timeout_ms);
#endif
    if (r < 0) return fail();
    for (int i = 0; i < n; ++i) {
        short o = 0;
        if (p[i].revents & (POLLIN | POLLHUP)) o |= 1;
        if (p[i].revents & POLLOUT) o |= 2;
        if (p[i].revents & (POLLERR | POLLNVAL)) o |= 4;
        rev[i] = o;
    }
    return r;
}

// The address other machines reach us at: LSWTCS_NET_IP if set, else the interface the default route uses
// (a UDP connect sends nothing), else (Switch) nifm's current address.
uint32_t lswnet_local_ip(void) {
    if (const char* e = getenv("LSWTCS_NET_IP")) {
        in_addr a; if (inet_pton(AF_INET, e, &a) == 1) return ntohl(a.s_addr);
    }
    uint32_t ip = 0;
    lsw_sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != LSW_BAD_SOCK) {
        sockaddr_in a = make_sin(0x0A000001u, 9);   // 10.0.0.1: any routable address works
        if (connect(s, (sockaddr*)&a, sizeof a) == 0) {
            sockaddr_in me; socklen_t n = sizeof me;
            if (getsockname(s, (sockaddr*)&me, &n) == 0) ip = ntohl(me.sin_addr.s_addr);
        }
        sock_close(s);
    }
#ifdef __SWITCH__
    if (!ip) { u32 v = 0; if (R_SUCCEEDED(nifmGetCurrentIpAddress(&v))) ip = ntohl(v); }
#endif
    return ip;
}

// Directed broadcast of our subnet (192.168.0.255 for 192.168.0.x/24). The Switch's BSD service does not reliably
// pass 255.255.255.255 on to the Wi-Fi. Switch: nifm's mask; PC: /24 (the usual home network).
uint32_t lswnet_subnet_broadcast(uint32_t ip) {
    uint32_t mask = 0xFFFFFF00u;
#ifdef __SWITCH__
    u32 a = 0, m = 0, g = 0, d1 = 0, d2 = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&a, &m, &g, &d1, &d2)) && m) mask = ntohl(m);
#endif
    return (ip & mask) | ~mask;
}

int lswnet_parse_ip(const char* s, uint32_t* ip) {
    in_addr a; if (inet_pton(AF_INET, s, &a) != 1) return 0;
    *ip = ntohl(a.s_addr); return 1;
}

// The player's profile: nickname (UTF-8) and a 16-byte account id. Returns 1 when it came from the console's
// account system (Switch), 0 for the fallback (PC: LSWTCS_GAMERTAG or "Player", id from the name).
// Switch order (HANDOFF_2026-10-09_netcoop.md §7): the user the system picker chose at launch (the forwarder's
// NACP has StartupUserAccount = Required), the only user on the console, then the last user that opened an app.
// No picker applet here: this runs from a guest thread mid-game.
int lswnet_profile(char* name, int cap, uint8_t uid[16]) {
    memset(uid, 0, 16);
    if (cap > 0) name[0] = 0;
#ifdef __SWITCH__
    if (R_SUCCEEDED(accountInitialize(AccountServiceType_Application))) {
        AccountUid u = {};
        bool got = R_SUCCEEDED(accountGetPreselectedUser(&u)) && accountUidIsValid(&u);
        const char* how = "preselected";
        if (!got) { got = R_SUCCEEDED(accountTrySelectUserWithoutInteraction(&u, false)) && accountUidIsValid(&u); how = "only user"; }
        if (!got) { got = R_SUCCEEDED(accountGetLastOpenedUser(&u)) && accountUidIsValid(&u); how = "last opened"; }
        int ok = 0;
        if (got) {
            AccountProfile prof;
            if (R_SUCCEEDED(accountGetProfile(&prof, u))) {
                AccountProfileBase pb; AccountUserData ud;
                if (R_SUCCEEDED(accountProfileGet(&prof, &ud, &pb))) {
                    snprintf(name, cap, "%s", pb.nickname);
                    memcpy(uid, &u, 16);
                    ok = 1;
                }
                accountProfileClose(&prof);
            }
        }
        accountExit();
        printf("[NET] Switch account: %s (%s)\n", ok ? name : "none", ok ? how : "no user found"); fflush(stdout);
        if (ok) return 1;
    }
#endif
    const char* e = getenv("LSWTCS_GAMERTAG");
    snprintf(name, cap, "%s", (e && *e) ? e : "Player");
    // FNV-1a of the name and the computer name as the fallback id: stable per PC, distinct between two PCs even
    // when both use the default name.
    uint64_t h = 1469598103934665603ull;
    for (const char* p = name; *p; ++p) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
    const char* host = getenv("COMPUTERNAME"); if (!host) host = getenv("HOSTNAME");
    if (host) for (const char* p = host; *p; ++p) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
    memcpy(uid, &h, 8);
    return 0;
}

}  // extern "C"
