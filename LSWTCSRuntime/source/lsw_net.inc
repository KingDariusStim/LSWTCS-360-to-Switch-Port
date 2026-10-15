// lsw_net.inc — network multiplayer and identity, included at the end of kernel_stubs.cpp (it uses kobj_*, GilYield,
// g_phys_alloc, xam_ovl_return and dbg_ram from there). Host sockets: lsw_net_host.cpp.
//
// Plan and evidence: HANDOFF_2026-10-09_netcoop.md; what this file does: HANDOFF_2026-10-14_netplay.md.
// Structure layouts: Xenia Canary netplay fork (AdrianCassar, netplay_canary_experimental: xsession.h, xnet.h,
// xam_net.cc), reference only. Never contacts Microsoft services.
//
// Identity (user decision 2026-10-09, re-stated 2026-10-14): the console's user account stands in for the Xbox Live
// account. Switch profile nickname = gamertag, Switch account id -> XUID; PC: LSWTCS_GAMERTAG.
//   LSWTCS_SWITCHID=0  old offline profile ("Player", XUID 0xB13EBABEBABEBABE)
// Network (LSWTCS_NET):
//   0 (default)  old stubs: every NetDll_* call succeeds and does nothing; calls are logged ([NET0]) so the online
//                menu's call sequence can be read from trace.log
//   1            online: user 0 is signed in to "Live" (the Switch account), privileges granted, real UDP/TCP
//                sockets (VDP = UDP), XNADDR = this machine's IP, and Xbox Live sessions become LAN/direct-IP
//                sessions found through a small discovery protocol on UDP LSWTCS_NET_DISCPORT (default 3076):
//                broadcast on the LAN plus unicast to every address in LSWTCS_NET_PEERS ("ip,ip,...") for play over
//                the internet (port-forward the game's ports + the discovery port on the host, or use a VPN).
//   LSWTCS_NET_IP=a.b.c.d   advertise/bind this address (two instances on one PC: 127.0.0.1 and 127.0.0.2 with
//                           LSWTCS_NET_PEERS pointing at each other)
//   LSWTCS_NET_XNSTATUS=hex XNetGetTitleXnAddr status in online mode (default 0xE6 = ETHERNET|STATIC|GATEWAY|DNS|
//                           ONLINE); boot keeps the old answer when LSWTCS_NET=0
//   LSWTCS_NET_SEARCHMS=n   how long a session search listens for hosts (default 400 ms)
//   LSWTCS_NETLOG=1         log every socket call

extern "C" {
int lswnet_host_init(void);
int lswnet_last_error(void);
long long lswnet_socket(int type);
int lswnet_close(long long s);
int lswnet_bind(long long s, uint32_t ip, uint16_t port);
int lswnet_connect(long long s, uint32_t ip, uint16_t port);
int lswnet_listen(long long s, int backlog);
long long lswnet_accept(long long s, uint32_t* ip, uint16_t* port);
int lswnet_sendto(long long s, const void* buf, int len, uint32_t ip, uint16_t port, int has_to);
int lswnet_recvfrom(long long s, void* buf, int len, uint32_t* ip, uint16_t* port);
int lswnet_shutdown(long long s, int how);
int lswnet_setopt(long long s, int opt, int value);
int lswnet_pending(long long s);
int lswnet_sockname(long long s, uint32_t* ip, uint16_t* port, int peer);
int lswnet_poll(int n, const long long* socks, const short* ev, short* rev, int timeout_ms);
uint32_t lswnet_local_ip(void);
int lswnet_parse_ip(const char* s, uint32_t* ip);
uint32_t lswnet_subnet_broadcast(uint32_t ip);
int lswnet_profile(char* name, int cap, uint8_t uid[16]);
}

// ── config ─────────────────────────────────────────────────────────────────────────────────────────────────────────
static int lsw_net_mode() { static const int v = [] { const char* e = getenv("LSWTCS_NET"); return e ? atoi(e) : 0; }(); return v; }
static bool lsw_net_on() { return lsw_net_mode() >= 1; }
static bool lsw_netlog() { static const bool v = getenv("LSWTCS_NETLOG") != nullptr; return v; }
static uint16_t lsw_net_discport() {
    static const uint16_t v = [] { const char* e = getenv("LSWTCS_NET_DISCPORT"); int p = e ? atoi(e) : 0; return (uint16_t)(p > 0 && p < 65536 ? p : 3076); }();
    return v;
}

// Session / discovery events: trace.log and stdout.log (play builds run without LSWTCS_TRACE).
#define NETEV(...) do { dbg_ram(__VA_ARGS__); printf(__VA_ARGS__); fflush(stdout); } while (0)

// ── guest memory (host-thread safe: no PPC_FUNC `base` needed) ───────────────────────────────────────────────────
static inline bool ngp(uint32_t p) { return p >= 0x80000000u && p < 0xF0000000u; }
static inline uint32_t nr32(uint32_t a) { uint32_t v; memcpy(&v, lsw_host(a), 4); return __builtin_bswap32(v); }
static inline uint16_t nr16(uint32_t a) { uint16_t v; memcpy(&v, lsw_host(a), 2); return __builtin_bswap16(v); }
static inline uint64_t nr64(uint32_t a) { uint64_t v; memcpy(&v, lsw_host(a), 8); return __builtin_bswap64(v); }
static inline void nw32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(lsw_host(a), &v, 4); }
static inline void nw16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(lsw_host(a), &v, 2); }
static inline void nw64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(lsw_host(a), &v, 8); }
static inline void nw8(uint32_t a, uint8_t v) { *lsw_host(a) = v; }
static inline void nrd(uint32_t a, void* dst, uint32_t n) { memcpy(dst, lsw_host(a), n); }
static inline void nwr(uint32_t a, const void* src, uint32_t n) { memcpy(lsw_host(a), src, n); }
static inline void nzero(uint32_t a, uint32_t n) { memset(lsw_host(a), 0, n); }
static inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint64_t net_now_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static uint64_t net_rand64() {
    static std::mutex m; static uint64_t s = 0;
    std::lock_guard<std::mutex> lk(m);
    if (!s) s = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count() ^ 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)&s;
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;   // xorshift64
    return s;
}
static void net_ip_str(uint32_t ip, char* out, size_t n) { snprintf(out, n, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); }

// ── identity ────────────────────────────────────────────────────────────────────────────────────────────────────────
static const uint64_t kLswOfflineXuid = 0xB13EBABEBABEBABEull;
struct LswIdentity { char gamertag[16]; uint64_t xuid; bool from_account; };
// Gamertags are <= 15 characters and the game font is the 360's: keep ASCII, fold Latin-1 accents, '?' for the rest.
static void net_gamertag_from_utf8(const char* s, char* out, int cap) {
    static const char kLatin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";   // U+00C0..U+00FF
    int n = 0;
    const unsigned char* p = (const unsigned char*)s;
    while (*p && n < cap - 1) {
        uint32_t cp; int len;
        if (*p < 0x80) { cp = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
        else { cp = *p & 0x07; len = 4; }
        for (int i = 1; i < len; ++i) { if ((p[i] & 0xC0) != 0x80) { len = i; break; } cp = (cp << 6) | (p[i] & 0x3F); }
        p += len;
        char c = '?';
        if (cp >= 0x20 && cp < 0x7F) c = (char)cp;
        else if (cp >= 0xC0 && cp <= 0xFF) c = kLatin1[cp - 0xC0];
        out[n++] = c;
    }
    out[n] = 0;
    if (!n) snprintf(out, cap, "Player");
}
static const LswIdentity& lsw_identity() {
    static const LswIdentity id = [] {
        LswIdentity r = {};
        const char* e = getenv("LSWTCS_SWITCHID");
        if (e && e[0] == '0') {
            snprintf(r.gamertag, sizeof r.gamertag, "Player"); r.xuid = kLswOfflineXuid;
        } else {
            char name[64]; uint8_t uid[16];
            r.from_account = lswnet_profile(name, sizeof name, uid) != 0;
            net_gamertag_from_utf8(name, r.gamertag, sizeof r.gamertag);
            // Online-shaped XUID (0x0009 prefix), stable per console account (HANDOFF_2026-10-09_netcoop.md §7).
            r.xuid = 0x0009000000000000ull | (XXH3_64bits(uid, 16) & 0x0000FFFFFFFFFFFFull);
        }
        NETEV("[NET] identity: gamertag '%s' xuid %016llX (%s)\n", r.gamertag, (unsigned long long)r.xuid,
                r.from_account ? "console account" : "fallback");
        return r;
    }();
    return id;
}
// Used by XamContentGetCreator: a save's creator must stay the signed-in user's XUID.
static uint64_t lsw_net_user_xuid(uint32_t user) { return user == 0 ? lsw_identity().xuid : 0; }

// ── addresses ───────────────────────────────────────────────────────────────────────────────────────────────────────
static uint32_t net_my_ip() {
    static uint32_t ip = 0; static bool done = false;
    if (!done) { done = true; lswnet_host_init(); ip = lswnet_local_ip(); if (!ip) ip = 0x7F000001u;
        char s[20]; net_ip_str(ip, s, sizeof s); NETEV("[NET] local address %s\n", s); }
    return ip;
}
static uint32_t net_bind_ip() { return getenv("LSWTCS_NET_IP") ? net_my_ip() : 0u; }
static uint64_t net_machine_id(uint32_t ip) { return 0xFA00000000000000ull | ip; }
// XNADDR (0x24, guest layout) for a machine, derived from its IP alone so every peer computes the same one:
// ina, inaOnline, wPortOnline, abEnet (02:4C:ip, locally administered), SGADDR{ina, spi, xbox_id, platform 1}.
static void net_make_xnaddr(uint32_t ip, uint8_t xn[36]) {
    memset(xn, 0, 36);
    put_be32(xn + 0, ip);
    put_be32(xn + 4, ip);
    xn[8] = 0x03; xn[9] = 0xE8;                                   // wPortOnline 1000 (XNetXnAddrToMachineId wants != 0)
    xn[10] = 0x02; xn[11] = 0x4C; put_be32(xn + 12, ip);          // abEnet
    put_be32(xn + 16, ip);                                        // abOnline.ina
    put_be32(xn + 20, ip ^ 0x5A5A5A5Au);                          // security parameter index
    uint64_t mid = net_machine_id(ip);
    for (int i = 0; i < 8; ++i) xn[24 + i] = (uint8_t)(mid >> (56 - 8 * i));
    xn[32] = 1;                                                   // platform: Xbox 360
}

// ── sessions, peers, discovery state ──────────────────────────────────────────────────────────────────────────────
struct NetMember { uint64_t xuid; uint32_t user_index, flags; };
struct NetSession {
    uint32_t handle = 0, obj = 0;
    bool created = false, host = false, deleted = false;
    uint32_t flags = 0, pub_max = 0, priv_max = 0, pub_open = 0, priv_open = 0, state = 0;   // state: 0 lobby, 2 in game
    uint64_t nonce = 0;
    uint8_t info[60] = {};   // XSESSION_INFO: XNKID, host XNADDR, XNKEY
    std::vector<NetMember> members;
};
struct NetFound {             // a host seen through discovery
    uint32_t ip = 0;
    uint8_t info[60] = {};
    char gamertag[16] = {};
    uint64_t xuid = 0;
    uint32_t flags = 0, pub_max = 0, priv_max = 0, pub_open = 0, priv_open = 0, state = 0;
    std::vector<uint8_t> ctx, props, qos;   // contexts {id,value}*; props {id u32, len u16, bytes}*
    uint64_t seen_ms = 0;
};
struct NetSock {
    long long h = -1;
    bool stream = false, vdp = false, guest_nb = false;
    uint32_t rcv_timeout_ms = 0;
    uint32_t ev_handle = 0, ev_mask = 0;
};
// An overlapped receive in flight. Like Winsock, the WSABUF descriptors are copied at call time (the caller's array
// is often on its stack) and the bytes / flags out-pointers are not written at completion (the result is in the
// overlapped; writing them later corrupted the stack frames of whatever ran then: host crash at the first join,
// Switch 2026-10-15). from / fromlen must stay valid until completion, as in Winsock.
struct NetPendingRecv { uint32_t sock; uint32_t buf_len[4], buf_ptr[4], nbufs, from_p, fromlen_p, ovl; };
struct NetQosJob { uint32_t qos, count, event; uint64_t due_ms; };

static std::mutex g_net_mtx;
static std::unordered_map<uint32_t, NetSock> g_net_socks;
static uint32_t g_net_next_sock = 0x100;
static std::unordered_map<uint32_t, NetSession> g_net_sessions;   // by handle
static uint32_t g_net_next_session = 0x5E550001u;
static std::map<uint64_t, NetFound> g_net_found;                  // by XNKID (as BE u64)
static std::map<uint64_t, std::vector<uint8_t>> g_net_qos_listen; // QoS reply data by XNKID
static std::map<uint32_t, uint32_t> g_net_ctx;                    // our XGIUserSetContext values
static std::map<uint32_t, std::vector<uint8_t>> g_net_props;      // our XGIUserSetPropertyEx values
static std::vector<NetPendingRecv> g_net_pending;
static std::vector<NetQosJob> g_net_qos_jobs;
static std::vector<uint32_t> g_net_peers;                         // LSWTCS_NET_PEERS
static long long g_net_disc = -1;                                 // discovery socket
static uint64_t g_net_joined_xnkid = 0;                           // session we joined (client side)
static thread_local uint32_t t_net_err = 0;

static uint64_t net_xnkid(const uint8_t* info) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | info[i]; return v; }
static void net_set_err(uint32_t e) { t_net_err = e; }

// Session from an X_KSESSION guest object (obj->handle) — the XGI messages pass the object, not the handle.
static NetSession* net_session_by_obj_locked(uint32_t obj) {
    if (!ngp(obj)) return nullptr;
    auto it = g_net_sessions.find(nr32(obj));
    return it == g_net_sessions.end() ? nullptr : &it->second;
}

// ── discovery wire format (big-endian) ────────────────────────────────────────────────────────────────────────────
// "LSWN" ver 1, type 1 = query, 2 = host beacon. Both carry the sender's XNADDR, gamertag and XUID; a beacon adds
// the session (XSESSION_INFO, flags, slots, state), the host's contexts / properties and its QoS reply data.
static const uint8_t kNetMagic[4] = {'L', 'S', 'W', 'N'};
struct NetWire {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
    void u32(uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back((uint8_t)(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 7; i >= 0; --i) b.push_back((uint8_t)(v >> (8 * i))); }
    void raw(const void* p, size_t n) { const uint8_t* q = (const uint8_t*)p; b.insert(b.end(), q, q + n); }
};
struct NetRead {
    const uint8_t* p; size_t n, o = 0; bool bad = false;
    bool need(size_t k) { if (o + k > n) { bad = true; return false; } return true; }
    uint8_t u8() { return need(1) ? p[o++] : 0; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = (uint16_t)((p[o] << 8) | p[o + 1]); o += 2; return v; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = be32(p + o); o += 4; return v; }
    uint64_t u64() { uint64_t hi = u32(); return (hi << 32) | u32(); }
    void raw(void* d, size_t k) { if (need(k)) { memcpy(d, p + o, k); o += k; } }
};
static void net_wire_header(NetWire& w, uint8_t type) {
    w.raw(kNetMagic, 4); w.u8(1); w.u8(type); w.u16(0);
    uint8_t xn[36]; net_make_xnaddr(net_my_ip(), xn); w.raw(xn, 36);
    const LswIdentity& id = lsw_identity(); w.raw(id.gamertag, 16); w.u64(id.xuid);
}
static void net_send_disc(const NetWire& w, uint32_t ip, uint16_t port) {
    if (g_net_disc < 0) return;
    if (lswnet_sendto(g_net_disc, w.b.data(), (int)w.b.size(), ip, port, 1) < 0) {
        static int n = 0; if (n++ < 8) { char a[20]; net_ip_str(ip, a, sizeof a); NETEV("[NET] discovery send to %s failed, err %d\n", a, lswnet_last_error()); }
    }
}
static void net_send_disc_all(const NetWire& w) {
    net_send_disc(w, lswnet_subnet_broadcast(net_my_ip()), lsw_net_discport());   // directed: the Switch needs it
    net_send_disc(w, 0xFFFFFFFFu, lsw_net_discport());
    for (uint32_t ip : g_net_peers) net_send_disc(w, ip, lsw_net_discport());
}
// Session hosted by this machine and advertised: created with HOST and with peer-network / matchmaking use, not
// deleted, slots open, and not started with join-in-progress disabled (0x400).
static bool net_advertised(const NetSession& s) {
    return s.created && s.host && !s.deleted && (s.flags & 0x01) && (s.flags & 0x28) &&
           (s.pub_open + s.priv_open) > 0 && !(s.state == 2 && (s.flags & 0x400));
}
static bool net_build_beacon_locked(NetWire& w) {
    for (auto& kv : g_net_sessions) {
        const NetSession& s = kv.second;
        if (!net_advertised(s)) continue;
        net_wire_header(w, 2);
        w.raw(s.info, 60);
        w.u32(s.flags); w.u32(s.pub_max); w.u32(s.priv_max); w.u32(s.pub_open); w.u32(s.priv_open); w.u32(s.state);
        w.u16((uint16_t)g_net_ctx.size());
        for (auto& c : g_net_ctx) { w.u32(c.first); w.u32(c.second); }
        // Our properties plus the host's gamertag as XPROPERTY_GAMER_HOSTNAME (0x40008109, UTF-16BE with NUL).
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> props(g_net_props.begin(), g_net_props.end());
        if (!g_net_props.count(0x40008109u)) {
            std::vector<uint8_t> u16s; for (const char* c = lsw_identity().gamertag; *c; ++c) { u16s.push_back(0); u16s.push_back((uint8_t)*c); }
            u16s.push_back(0); u16s.push_back(0);
            props.emplace_back(0x40008109u, u16s);
        }
        w.u16((uint16_t)props.size());
        for (auto& p : props) { w.u32(p.first); w.u16((uint16_t)p.second.size()); w.raw(p.second.data(), p.second.size()); }
        auto q = g_net_qos_listen.find(net_xnkid(s.info));
        uint16_t ql = q == g_net_qos_listen.end() ? 0 : (uint16_t)q->second.size();
        w.u16(ql); if (ql) w.raw(q->second.data(), ql);
        return true;
    }
    return false;
}
static void net_handle_disc(const uint8_t* p, int n, uint32_t from_ip, uint16_t from_port) {
    NetRead r{p, (size_t)n};
    uint8_t magic[4]; r.raw(magic, 4);
    if (r.bad || memcmp(magic, kNetMagic, 4) != 0) return;
    uint8_t ver = r.u8(), type = r.u8(); r.u16();
    if (ver != 1) return;
    uint8_t xn[36]; r.raw(xn, 36);
    char tag[16]; r.raw(tag, 16); tag[15] = 0;
    uint64_t xuid = r.u64();
    if (r.bad) return;
    if (from_ip == net_my_ip() && xuid == lsw_identity().xuid) return;   // our own broadcast
    { static int n = 0; if (n++ < 6) { char a[20]; net_ip_str(from_ip, a, sizeof a); NETEV("[NET] discovery %s from '%.15s' at %s\n", type == 1 ? "query" : "beacon", tag, a); } }
    std::lock_guard<std::mutex> lk(g_net_mtx);
    if (type == 1) {                                                     // query: answer if we host
        NetWire w;
        if (net_build_beacon_locked(w)) lswnet_sendto(g_net_disc, w.b.data(), (int)w.b.size(), from_ip, from_port, 1);
        return;
    }
    if (type != 2) return;
    NetFound f;
    r.raw(f.info, 60);
    f.flags = r.u32(); f.pub_max = r.u32(); f.priv_max = r.u32(); f.pub_open = r.u32(); f.priv_open = r.u32(); f.state = r.u32();
    uint16_t nctx = r.u16();
    for (uint16_t i = 0; i < nctx && !r.bad; ++i) { uint8_t c[8]; r.raw(c, 8); f.ctx.insert(f.ctx.end(), c, c + 8); }
    uint16_t nprop = r.u16();
    for (uint16_t i = 0; i < nprop && !r.bad; ++i) {
        uint8_t h[6]; r.raw(h, 6); uint16_t len = (uint16_t)((h[4] << 8) | h[5]);
        if (!r.need(len)) break;
        f.props.insert(f.props.end(), h, h + 6); f.props.insert(f.props.end(), p + r.o, p + r.o + len); r.o += len;
    }
    uint16_t ql = r.u16();
    if (ql && r.need(ql)) { f.qos.assign(p + r.o, p + r.o + ql); r.o += ql; }
    if (r.bad) return;
    f.ip = from_ip; memcpy(f.gamertag, tag, 16); f.xuid = xuid; f.seen_ms = net_now_ms();
    uint64_t id = net_xnkid(f.info);
    bool fresh = !g_net_found.count(id);
    g_net_found[id] = std::move(f);
    if (fresh) { char s[20]; net_ip_str(from_ip, s, sizeof s); NETEV("[NET] found session %016llX host '%s' at %s\n", (unsigned long long)id, tag, s); }
}

// ── worker thread: discovery, overlapped receives, WSAEventSelect, QoS completion ────────────────────────────────
// A host thread outside the GIL rotation. It writes guest memory and signals guest events, like a DMA engine.
static void net_complete_wsa_ovl(uint32_t ovl, uint32_t status, uint32_t bytes) {
    if (!ngp(ovl)) return;
    nw32(ovl + 4, bytes);
    nw32(ovl + 8, 0);
    nw32(ovl + 0, status);
    uint32_t ev = nr32(ovl + 16);
    if (ev) { HANDLE h = kobj_get(ev); if (h) SetEvent(h); }
}
// Scatter a received datagram into WSABUFs {len, ptr}.
static uint32_t net_scatter(uint32_t bufs, uint32_t nbufs, const uint8_t* data, uint32_t n) {
    uint32_t done = 0;
    for (uint32_t i = 0; i < nbufs && done < n; ++i) {
        uint32_t len = nr32(bufs + i * 8), ptr = nr32(bufs + i * 8 + 4);
        if (!ngp(ptr)) continue;
        uint32_t k = std::min(len, n - done);
        nwr(ptr, data + done, k); done += k;
    }
    return done;
}
// The game's socket is UDP port 1001; the Switch's BSD service refuses ports below 1024 (bind -> EACCES, two-Switch
// test 2026-10-14). Guest ports 1..1023 live at LSWTCS_NET_LOWPORT + port on the host (default 30000: 1001 -> 31001)
// on every machine, and are mapped back in every address the guest reads.
static uint16_t net_lowport_base() {
    static const uint16_t v = [] { const char* e = getenv("LSWTCS_NET_LOWPORT"); int b = e ? atoi(e) : 30000; return (uint16_t)(b > 1024 && b < 64000 ? b : 30000); }();
    return v;
}
static uint16_t net_host_port(uint16_t p) { return (p && p < 1024) ? (uint16_t)(p + net_lowport_base()) : p; }
static uint16_t net_guest_port(uint16_t p) { uint16_t b = net_lowport_base(); return (p >= b + 1 && p < b + 1024) ? (uint16_t)(p - b) : p; }
// First game packets each way, host-side ports (stdout.log): shows whether traffic crosses between machines.
extern "C" int lsw_qnet_verbose;   // main.cpp: the network-library probes log everything from the first game packet on
static void net_traffic_log(bool send, uint32_t ip, uint16_t port, int n, const uint8_t* data = nullptr) {
    static std::atomic<int> ns{0}, nr{0};
    if (n > 0 && !lsw_qnet_verbose) { lsw_qnet_verbose = 1; NETEV("[NET] first game packet: verbose network-library logging on\n"); }
    if ((send ? ns : nr).fetch_add(1) >= 64) return;
    char a[20]; net_ip_str(ip, a, sizeof a);
    char hex[3 * 48 + 1] = {};
    if (data && n > 0) for (int i = 0; i < n && i < 48; ++i) snprintf(hex + 3 * i, 4, "%02X ", data[i]);
    NETEV("[NET] %s %d bytes %s %s:%u  %s\n", send ? "send" : "recv", n, send ? "to" : "from", a, port, hex);
}
static void net_write_sockaddr(uint32_t sa, uint32_t lenp, uint32_t ip, uint16_t port) {
    if (ngp(sa)) { nzero(sa, 16); nw16(sa, 2); nw16(sa + 2, net_guest_port(port)); nw32(sa + 4, ip); }
    if (ngp(lenp)) nw32(lenp, 16);
}
static void net_worker() {
    static uint8_t buf[65536];
    uint64_t last_beacon = 0;
    for (;;) {
        std::vector<long long> socks; std::vector<short> ev, rev;
        std::vector<NetPendingRecv> pend; std::vector<std::pair<long long, NetSock>> evsel;
        {
            std::lock_guard<std::mutex> lk(g_net_mtx);
            pend = g_net_pending;
            for (auto& kv : g_net_socks) if (kv.second.ev_handle && (kv.second.ev_mask & 1)) evsel.emplace_back(kv.second.h, kv.second);
        }
        if (g_net_disc >= 0) { socks.push_back(g_net_disc); ev.push_back(1); }
        std::vector<std::pair<NetPendingRecv, long long>> pend_h;   // with the host socket, fixed for this round
        {
            std::lock_guard<std::mutex> lk(g_net_mtx);
            for (auto& pr : pend) {
                auto it = g_net_socks.find(pr.sock);
                if (it != g_net_socks.end()) { pend_h.emplace_back(pr, it->second.h); socks.push_back(it->second.h); ev.push_back(1); }
            }
        }
        for (auto& e : evsel) { socks.push_back(e.first); ev.push_back(1); }
        rev.assign(socks.size(), 0);
        int r = lswnet_poll((int)socks.size(), socks.data(), ev.data(), rev.data(), 4);
        if (r < 0 || socks.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(4));
        size_t k = 0;
        if (g_net_disc >= 0) {
            if (r > 0 && (rev[k] & 1)) {
                for (int i = 0; i < 16; ++i) {
                    uint32_t ip; uint16_t port;
                    int n = lswnet_recvfrom(g_net_disc, buf, sizeof buf, &ip, &port);
                    if (n <= 0) break;
                    net_handle_disc(buf, n, ip, port);
                }
            }
            ++k;
        }
        // Overlapped receives (WSARecvFrom / WSARecv returned WSA_IO_PENDING).
        for (auto& ph : pend_h) {
            const NetPendingRecv& pr = ph.first;
            bool ready = r > 0 && k < rev.size() && (rev[k] & 5);
            ++k;
            if (!ready) continue;
            long long h = ph.second; bool stream = false;
            {
                std::lock_guard<std::mutex> lk(g_net_mtx);
                auto it = g_net_socks.find(pr.sock);
                if (it == g_net_socks.end() || it->second.h != h) continue;   // closed this round
                stream = it->second.stream;
            }
            uint32_t ip = 0; uint16_t port = 0;
            int n = lswnet_recvfrom(h, buf, sizeof buf, &ip, &port);
            if (n < 0 && lswnet_last_error() == 10035) continue;
            {
                std::lock_guard<std::mutex> lk(g_net_mtx);   // still pending? (WSACancelOverlappedIO / closesocket)
                auto it = std::find_if(g_net_pending.begin(), g_net_pending.end(), [&](const NetPendingRecv& q) { return q.ovl == pr.ovl; });
                if (it == g_net_pending.end()) continue;
                g_net_pending.erase(it);
            }
            if (n < 0) { net_complete_wsa_ovl(pr.ovl, (uint32_t)lswnet_last_error(), 0); continue; }
            uint32_t got = 0;
            for (uint32_t b = 0; b < pr.nbufs && got < (uint32_t)n; ++b) {
                if (!ngp(pr.buf_ptr[b])) continue;
                uint32_t k = std::min(pr.buf_len[b], (uint32_t)n - got);
                nwr(pr.buf_ptr[b], buf + got, k); got += k;
            }
            if (!stream) net_traffic_log(false, ip, port, n, buf);
            if (!stream) net_write_sockaddr(pr.from_p, pr.fromlen_p, ip, port);
            net_complete_wsa_ovl(pr.ovl, 0, got);
        }
        // WSAEventSelect FD_READ.
        for (auto& e : evsel) {
            bool ready = r > 0 && k < rev.size() && (rev[k] & 1);
            ++k;
            if (ready) { HANDLE h = kobj_get(e.second.ev_handle); if (h) SetEvent(h); }
        }
        // QoS lookups complete shortly after the call (some titles treat count_pending == 0 on return as a
        // failed asynchronous lookup and repeat it, netplay fork notes).
        uint64_t now = net_now_ms();
        {
            std::vector<NetQosJob> due;
            {
                std::lock_guard<std::mutex> lk(g_net_mtx);
                for (auto it = g_net_qos_jobs.begin(); it != g_net_qos_jobs.end();) {
                    if (it->due_ms <= now) { due.push_back(*it); it = g_net_qos_jobs.erase(it); } else ++it;
                }
            }
            for (auto& j : due) {
                nw32(j.qos + 0, j.count); nw32(j.qos + 4, 0);
                if (j.event) { HANDLE h = kobj_get(j.event); if (h) SetEvent(h); }
            }
        }
        if (now - last_beacon >= 1000) {
            last_beacon = now;
            NetWire w; bool have; std::string why;
            {
                std::lock_guard<std::mutex> lk(g_net_mtx);
                have = net_build_beacon_locked(w);
                // Why each live session is or is not advertised (logged when it changes).
                for (auto& kv : g_net_sessions) {
                    const NetSession& s = kv.second;
                    if (!s.created || s.deleted) continue;
                    char b[160];
                    snprintf(b, sizeof b, "h=%08X flags=%03X host=%d open=%u/%u state=%u -> %s; ", s.handle, s.flags, (int)s.host,
                             s.pub_open, s.priv_open, s.state, net_advertised(s) ? "ADVERTISED" :
                             !s.host ? "not host" : !(s.flags & 0x01) ? "no HOST flag" : !(s.flags & 0x28) ? "no peer-network/matchmaking flag" :
                             (s.pub_open + s.priv_open) == 0 ? "no open slots" : "started, join in progress disabled");
                    why += b;
                }
            }
            static std::string last_why;
            if (why != last_why) { last_why = why; NETEV("[NET] hosting: %s\n", why.empty() ? "no session" : why.c_str()); }
            if (have) net_send_disc_all(w);
            std::lock_guard<std::mutex> lk(g_net_mtx);
            for (auto it = g_net_found.begin(); it != g_net_found.end();) {
                if (now - it->second.seen_ms > 6000) it = g_net_found.erase(it); else ++it;
            }
        }
    }
}
static void net_start() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!lswnet_host_init()) return;
        net_my_ip();
        if (const char* e = getenv("LSWTCS_NET_PEERS")) {
            std::string s(e); size_t a = 0;
            while (a <= s.size()) {
                size_t b = s.find(',', a); if (b == std::string::npos) b = s.size();
                std::string one = s.substr(a, b - a);
                uint32_t ip; if (!one.empty() && lswnet_parse_ip(one.c_str(), &ip)) g_net_peers.push_back(ip);
                a = b + 1;
            }
        }
        g_net_disc = lswnet_socket(2);
        if (g_net_disc >= 0) {
            lswnet_setopt(g_net_disc, 2, 1);
            lswnet_setopt(g_net_disc, 1, 1);
            if (lswnet_bind(g_net_disc, net_bind_ip(), lsw_net_discport()) != 0) {
                NETEV("[NET] discovery port %u busy (err %d): this instance can search only through peers\n", lsw_net_discport(), lswnet_last_error());
                lswnet_close(g_net_disc); g_net_disc = lswnet_socket(2);
                if (g_net_disc >= 0) { lswnet_setopt(g_net_disc, 1, 1); lswnet_bind(g_net_disc, net_bind_ip(), 0); }
            }
        }
        NETEV("[NET] online mode: discovery UDP %u, %zu direct peer(s)\n", lsw_net_discport(), g_net_peers.size());
        std::thread(net_worker).detach();
    });
}

// ── logging ─────────────────────────────────────────────────────────────────────────────────────────────────────────
// LSWTCS_NET=0: first 4 calls of each import, with args and the caller, so the online menu's call sequence is visible.
#define NET0_LOG(name) do { static int _n = 0; if (_n < 4) { _n++; \
    dbg_ram("[NET0] %s(%08X, %08X, %08X, %08X, %08X) lr=%08X\n", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, (uint32_t)ctx.lr); } } while (0)
#define NETLOG(...) do { if (lsw_netlog()) dbg_ram(__VA_ARGS__); } while (0)
#define NET_LEGACY(name) if (!lsw_net_on()) { NET0_LOG(name); ctx.r3.u32 = 0; return; } \
    else { static bool _first = true; if (_first) { _first = false; \
        NETEV("[NET] first %s(%08X, %08X, %08X, %08X) lr=%08X\n", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr); } }

// ── XAM users ───────────────────────────────────────────────────────────────────────────────────────────────────────
PPC_FUNC(__imp__XamUserGetSigninState) {
    PPC_FUNC_PROLOGUE();
    // XUSER_SIGNIN_STATE: 0 = not signed in, 1 = signed in locally, 2 = signed in to Live. Before this, every user
    // read 0 and the title waited at load. Online mode: the console account counts as the Live account.
    uint32_t user = ctx.r3.u32, st = user == 0 ? (lsw_net_on() ? 2u : 1u) : 0u;
    static std::atomic<uint32_t> s_calls{0}; uint32_t s_n = ++s_calls;
    if (s_n <= 4 || s_n % 2000 == 0) dbg_ram("[XamSigninState] #%u user=%u -> %u\n", s_n, user, st);
    ctx.r3.u32 = st;
}
// XamUserGetXUID(user r3, type_mask r4, u64* xuid r5) — Xenia xam_user.cc. Only user 0 is signed in.
PPC_FUNC(__imp__XamUserGetXUID) {
    PPC_FUNC_PROLOGUE();
    uint32_t user = ctx.r3.u32, p = ctx.r5.u32;
    static int n = 0; if (n < 4) { n++; dbg_ram("[XAM] XamUserGetXUID user=%u mask=0x%X out=0x%08X lr=0x%08X\n", user, ctx.r4.u32, p, (uint32_t)ctx.lr); }
    if (!(p >= 0x80000000u && p < 0x94000000u)) { ctx.r3.u32 = 0x80070057u; return; }   // E_INVALIDARG
    PPC_STORE_U64(p, 0);
    if (user >= 4) { ctx.r3.u32 = 0x80070057u; return; }
    if (user != 0) { ctx.r3.u32 = 0x80070525u; return; }                                 // NO_SUCH_USER
    PPC_STORE_U64(p, lsw_identity().xuid);
    ctx.r3.u32 = 0;
}
// XamUserGetName(user r3, char* buffer r4, buffer_len r5 incl. NUL) — Xenia xam_user.cc.
PPC_FUNC(__imp__XamUserGetName) {
    PPC_FUNC_PROLOGUE();
    uint32_t user = ctx.r3.u32, p = ctx.r4.u32, len = ctx.r5.u32;
    static int n = 0; if (n < 4) { n++; dbg_ram("[XAM] XamUserGetName user=%u buf=0x%08X len=%u lr=0x%08X\n", user, p, len, (uint32_t)ctx.lr); }
    bool p_ok = p >= 0x80000000u && p < 0x94000000u;
    if (user >= 4) { ctx.r3.u32 = 0x00000057u; return; }                      // INVALID_PARAMETER
    if (user != 0) { if (p_ok) PPC_STORE_U8(p, 0); ctx.r3.u32 = 0x00000525u; return; }
    const char* name = lsw_identity().gamertag;
    uint32_t size = (uint32_t)strlen(name) + 1, n_copy = len < size ? len : size;
    if (p_ok) { for (uint32_t i = 0; i < n_copy; ++i) PPC_STORE_U8(p + i, (uint8_t)name[i]); if (n_copy && n_copy < size) PPC_STORE_U8(p + n_copy - 1, 0); }
    ctx.r3.u32 = 0;
}
// XamUserReadProfileSettings(title r3, user r4, xuid_count r5, xuids r6, count r7, setting_ids r8, size* r9,
// buffer r10, [sp+54] overlapped) — Xenia xam_user.cc XamUserReadProfileSettingsEx. The network library's voice
// engine first asks for the size of the voice settings (0x1004000C muted, 0x1004000D through speakers, 0x1004000E
// volume) with no buffer and requires ERROR_INSUFFICIENT_BUFFER (122) + the size; the old stub's 0 made the voice
// engine, and with it the network library, fail to start (this->lib null: no hosting, no search; two Switches
// 2026-10-14). Result: {count, settings*} + X_USER_PROFILE_SETTING[count] (0x28: source u32, -, user_index @8,
// setting_id @0x10, X_USER_DATA @0x18: type u8, union @0x20) + string/binary data. User 0xFF = any = user 0.
PPC_FUNC(__imp__XamUserReadProfileSettings) {
    PPC_FUNC_PROLOGUE();
    if (!lsw_net_on()) { LSW_XSTUB_LOG(); ctx.r3.u32 = 0; return; }
    uint32_t user = ctx.r4.u32, count = ctx.r7.u32, ids = ctx.r8.u32, sizep = ctx.r9.u32, buf = ctx.r10.u32;
    uint32_t ovl = PPC_LOAD_U32(ctx.r1.u32 + 0x54);
    if (count < 1 || count > 32 || !ngp(ids) || !ngp(sizep)) { ctx.r3.u32 = 0x57; return; }
    uint32_t have = nr32(sizep), need = 8 + count * 0x28;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id = nr32(ids + i * 4), type = id >> 28;
        if (type == 4 || type == 6) need += (id >> 16) & 0xFFF;
    }
    { static int n = 0; if (n++ < 8) NETEV("[NET] XamUserReadProfileSettings user=%X count=%u first=%08X size=%u need=%u buf=%08X lr=%08X\n",
                                           user, count, nr32(ids), have, need, buf, (uint32_t)ctx.lr); }
    if (!ngp(buf) || have < need) {
        if (!have) nw32(sizep, need);
        ctx.r3.u32 = 0x7A;   // ERROR_INSUFFICIENT_BUFFER (synchronous, as in Xenia)
        return;
    }
    if (user != 0 && user != 0xFF) { ctx.r3.u32 = xam_ovl_return(base, ovl, 0x525u, 0); return; }   // NO_SUCH_USER
    nzero(buf, need);
    uint32_t set = buf + 8, data = set + count * 0x28;
    nw32(buf, count); nw32(buf + 4, set);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id = nr32(ids + i * 4), type = id >> 28, size = (id >> 16) & 0xFFF, e = set + i * 0x28;
        nw32(e, 1);                       // source: DEFAULT
        nw32(e + 8, 0);                   // user index 0
        nw32(e + 0x10, id);
        nw8(e + 0x18, (uint8_t)type);
        if (type == 4 || type == 6) { nw32(e + 0x20, size); nw32(e + 0x24, data); data += size; }
        else if (id == 0x1004000Eu) nw32(e + 0x20, 100);   // voice volume
        // everything else (voice muted, through speakers, ...) reads 0
    }
    ctx.r3.u32 = xam_ovl_return(base, ovl, 0, 0);
}
// XamUserCheckPrivilege(user r3, privilege r4, BOOL* result r5). Old stub: success without writing the result.
PPC_FUNC(__imp__XamUserCheckPrivilege) {
    PPC_FUNC_PROLOGUE();
    if (!lsw_net_on()) { LSW_XSTUB_LOG(); NET0_LOG("XamUserCheckPrivilege"); ctx.r3.u32 = 0; return; }
    uint32_t user = ctx.r3.u32, out = ctx.r5.u32;
    { static int n = 0; if (n++ < 4) NETEV("[NET] XamUserCheckPrivilege user=%u priv=%u -> granted lr=%08X\n", user, ctx.r4.u32, (uint32_t)ctx.lr); }
    if (ngp(out)) nw32(out, (user == 0 || user == 0xFFu) ? 1u : 0u);
    ctx.r3.u32 = (user == 0 || user == 0xFFu) ? 0u : 0x525u;
}
// XamUserAreUsersFriends(user r3, u64* xuids r4, count r5, BOOL* are_friends r6, overlapped r7): nobody is a friend.
PPC_FUNC(__imp__XamUserAreUsersFriends) {
    PPC_FUNC_PROLOGUE();
    if (!lsw_net_on()) { LSW_XSTUB_LOG(); NET0_LOG("XamUserAreUsersFriends"); ctx.r3.u32 = 0; return; }
    if (ngp(ctx.r6.u32)) nw32(ctx.r6.u32, 0);
    ctx.r3.u32 = xam_ovl_return(base, ctx.r7.u32, ctx.r3.u32 < 4 ? 0u : 0x57u, 0);
}
// XamUserGetMembershipTierFromXUID(xuid r3) -> tier (6 = Gold); XamUserGetOnlineCountryFromXUID(xuid r3) -> 103 (US).
PPC_FUNC(__imp__XamUserGetMembershipTierFromXUID) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = lsw_net_on() ? 6u : 0u; }
PPC_FUNC(__imp__XamUserGetOnlineCountryFromXUID) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = lsw_net_on() ? 103u : 0u; }
PPC_FUNC(__imp__XNetLogonGetMachineID) {
    PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG();
    if (lsw_net_on() && ngp(ctx.r3.u32)) nw64(ctx.r3.u32, net_machine_id(net_my_ip()));
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XNetLogonGetTitleID) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }

// XamSessionCreateHandle(handle* r3) / XamSessionRefObjByHandle(handle r3, obj* r4): a session is a 4-byte guest
// object holding its handle (netplay fork X_KSESSION); the XGI messages pass that object.
PPC_FUNC(__imp__XamSessionCreateHandle) {
    PPC_FUNC_PROLOGUE();
    if (!lsw_net_on()) { LSW_XSTUB_LOG(); NET0_LOG("XamSessionCreateHandle"); ctx.r3.u32 = 0; return; }
    uint32_t obj = g_phys_alloc(16);
    if (!obj || !ngp(ctx.r3.u32)) { ctx.r3.u32 = 0x8007000Eu; return; }
    uint32_t h;
    { std::lock_guard<std::mutex> lk(g_net_mtx); h = g_net_next_session++; NetSession s; s.handle = h; s.obj = obj; g_net_sessions[h] = s; }
    nw32(obj, h);
    nw32(ctx.r3.u32, h);
    NETEV("[NET] XamSessionCreateHandle -> %08X obj %08X lr=%08X\n", h, obj, (uint32_t)ctx.lr);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XamSessionRefObjByHandle) {
    PPC_FUNC_PROLOGUE();
    if (!lsw_net_on()) { LSW_XSTUB_LOG(); NET0_LOG("XamSessionRefObjByHandle"); ctx.r3.u32 = 0; return; }
    uint32_t obj = 0;
    { std::lock_guard<std::mutex> lk(g_net_mtx); auto it = g_net_sessions.find(ctx.r3.u32); if (it != g_net_sessions.end()) obj = it->second.obj; }
    if (!obj) { ctx.r3.u32 = 0xC0000008u; return; }   // STATUS_INVALID_HANDLE
    if (ngp(ctx.r4.u32)) nw32(ctx.r4.u32, obj);
    ctx.r3.u32 = 0;
}

// ── XGI session messages (XMsg app 0xFB) and XLiveBase (0xFC) ─────────────────────────────────────────────────────
// Search results: XSESSION_SEARCHRESULT_HEADER {count, ptr} then 0x5C-byte results; contexts / properties go in the
// rest of the caller's buffer (or are left out when it is full).
static uint32_t net_write_search_results(uint32_t res, uint32_t size_arg, uint32_t max_results, const std::vector<NetFound>& v) {
    if (!ngp(res)) return 0x57u;
    uint32_t size = ngp(size_arg) ? nr32(size_arg) : size_arg;   // a value in the XGI struct; a pointer in some SDKs
    uint32_t want = 8 + (uint32_t)v.size() * 0x5C;
    if (size < 8) { if (ngp(size_arg)) nw32(size_arg, want); return 0x7Au; }   // ERROR_INSUFFICIENT_BUFFER
    nzero(res, std::min(size, 0x10000u));
    uint32_t n = std::min<uint32_t>((uint32_t)v.size(), (size - 8) / 0x5C);
    if (max_results) n = std::min(n, max_results);
    uint32_t tail = res + 8 + n * 0x5C, end = res + size;
    for (uint32_t i = 0; i < n; ++i) {
        const NetFound& f = v[i];
        uint32_t r = res + 8 + i * 0x5C;
        nwr(r, f.info, 60);
        nw32(r + 0x3C, f.pub_open); nw32(r + 0x40, f.priv_open);
        nw32(r + 0x44, f.pub_max - std::min(f.pub_max, f.pub_open)); nw32(r + 0x48, f.priv_max - std::min(f.priv_max, f.priv_open));
        uint32_t nctx = (uint32_t)f.ctx.size() / 8;
        if (nctx && tail + nctx * 8 <= end) { nwr(tail, f.ctx.data(), nctx * 8); nw32(r + 0x50, nctx); nw32(r + 0x58, tail); tail += nctx * 8; }
        // properties: XUSER_PROPERTY {id, pad, XUSER_DATA {type u8, pad[7], union 8}} = 0x18
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> props;
        for (size_t o = 0; o + 6 <= f.props.size();) {
            uint32_t id = be32(&f.props[o]); uint16_t len = (uint16_t)((f.props[o + 4] << 8) | f.props[o + 5]); o += 6;
            if (o + len > f.props.size()) break;
            props.emplace_back(id, std::vector<uint8_t>(f.props.begin() + o, f.props.begin() + o + len)); o += len;
        }
        uint32_t np = (uint32_t)props.size(), parr = (tail + 7) & ~7u;
        if (np && parr + np * 0x18 <= end) {
            uint32_t data = parr + np * 0x18, written = 0;
            for (auto& p : props) {
                uint32_t e = parr + written * 0x18; uint8_t type = (uint8_t)(p.first >> 28);
                nzero(e, 0x18); nw32(e, p.first); nw8(e + 8, type);
                if (type == 4 || type == 6) {                       // WSTRING / BINARY: {size, ptr}
                    if (data + p.second.size() > end) continue;
                    nwr(data, p.second.data(), (uint32_t)p.second.size());
                    nw32(e + 0x10, (uint32_t)p.second.size()); nw32(e + 0x14, data);
                    data += ((uint32_t)p.second.size() + 3) & ~3u;
                } else {
                    nwr(e + 0x10, p.second.data(), (uint32_t)std::min<size_t>(8, p.second.size()));
                }
                ++written;
            }
            nw32(r + 0x4C, written); nw32(r + 0x54, parr); tail = data;
        }
    }
    nw32(res, n); nw32(res + 4, res + 8);
    NETEV("[NET] search -> %u session(s)\n", n);
    return 0;
}
static std::vector<NetFound> net_search_hosts() {
    net_start();
    NetWire q; net_wire_header(q, 1); net_send_disc_all(q);
    static const int ms = [] { const char* e = getenv("LSWTCS_NET_SEARCHMS"); int v = e ? atoi(e) : 400; return v < 0 ? 0 : v; }();
    if (ms) { GilYield y; std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
    std::vector<NetFound> v;
    std::lock_guard<std::mutex> lk(g_net_mtx);
    uint64_t now = net_now_ms();
    for (auto& kv : g_net_found)
        if (now - kv.second.seen_ms < 4000 && kv.second.pub_open + kv.second.priv_open > 0) v.push_back(kv.second);
    return v;
}
static void net_fill_details(const NetSession& s, uint32_t det, uint32_t size) {
    nzero(det, 0x80);
    nw32(det + 0x00, s.host ? 0u : 0xFEu);
    auto gt = g_net_ctx.find(0x800Au), gm = g_net_ctx.find(0x800Bu);
    nw32(det + 0x04, gt == g_net_ctx.end() ? 1u : gt->second);
    nw32(det + 0x08, gm == g_net_ctx.end() ? 0u : gm->second);
    nw32(det + 0x0C, s.flags);
    nw32(det + 0x10, s.pub_max); nw32(det + 0x14, s.priv_max); nw32(det + 0x18, s.pub_open); nw32(det + 0x1C, s.priv_open);
    nw32(det + 0x28, s.deleted ? 4u : s.state);
    nw64(det + 0x30, s.nonce);
    nwr(det + 0x38, s.info, 60);
    uint32_t room = size > 0x80 ? (size - 0x80) / 16 : 0, n = std::min<uint32_t>(room, (uint32_t)s.members.size());
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t m = det + 0x80 + i * 16;
        nw64(m, s.members[i].xuid); nw32(m + 8, s.members[i].user_index); nw32(m + 12, s.members[i].flags);
    }
    nw32(det + 0x20, (uint32_t)s.members.size()); nw32(det + 0x24, n);
    nw32(det + 0x7C, n ? det + 0x80 : 0u);
}
static const char* net_xgi_name(uint32_t msg) {
    switch (msg) {
        case 0x000B0006: return "XGIUserSetContext";   case 0x000B0007: return "XGIUserSetPropertyEx";
        case 0x000B0008: return "XGIUserWriteAchievements";
        case 0x000B0010: return "XSessionCreate";      case 0x000B0011: return "XSessionDelete";
        case 0x000B0012: return "XSessionJoin";        case 0x000B0013: return "XSessionLeave";
        case 0x000B0014: return "XSessionStart";       case 0x000B0015: return "XSessionEnd";
        case 0x000B0016: return "XSessionSearch";      case 0x000B0018: return "XSessionModify";
        case 0x000B0019: return "XSessionGetInvitationData"; case 0x000B001A: return "XSessionArbitrationRegister";
        case 0x000B001B: return "XSessionSearchByID";  case 0x000B001C: return "XSessionSearchEx";
        case 0x000B001D: return "XSessionGetDetails";  case 0x000B001E: return "XSessionMigrateHost";
        case 0x000B001F: return "XSessionModifySkill"; case 0x000B0020: return "XUserResetStatsView";
        case 0x000B0021: return "XUserReadStats";      case 0x000B0025: return "XSessionWriteStats";
        case 0x000B0026: return "XSessionFlushStats";  case 0x000B0060: return "XSessionSearchByIds";
        case 0x000B0065: return "XSessionSearchWeighted";
        default: return "?";
    }
}
// Returns true when handled (online mode); *result is the X_ERROR / HRESULT for the caller or the overlapped.
static bool lsw_net_xmsg(uint32_t app, uint32_t msg, uint32_t buf, uint32_t len, uint32_t* result) {
    if (app == 0xFB || app == 0xFC) {
        static int n = 0;
        // The per-frame trio (user context, logon id, friends) used up the cap before the session traffic.
        const bool periodic = msg == 0x000B0006 || msg == 0x00058004 || msg == 0x00058020;
        if (!periodic || n < 24 || lsw_netlog()) { if (periodic) n++; NETEV("[NET%s] XMsg app=%X msg=%08X %s buf=%08X len=%u\n", lsw_net_on() ? "" : "0", app, msg, app == 0xFB ? net_xgi_name(msg) : "", buf, len); }
    }
    if (!lsw_net_on()) return false;
    *result = 0;
    if (app == 0xFC) {                                     // XLiveBase (mainline Xenia xlivebase_app.cc)
        switch (msg) {
            case 0x00058003: *result = 0x001510F0u; return true;                          // LogonGetHR: connection established
            case 0x00058004: if (ngp(buf)) nw32(buf, 1); return true;                      // GetLogonId
            case 0x00058006: if (ngp(buf)) nw32(buf, 1); return true;                      // GetNatType: open
            case 0x00058007: *result = 0x80151802u; return true;                          // OnlineGetServiceInfo: no Live services
            case 0x00058020: case 0x00058023: *result = 0x80004005u; return true;         // friends / invites: none
            default: return false;
        }
    }
    if (app != 0xFB || !ngp(buf)) return false;
    net_start();
    std::unique_lock<std::mutex> lk(g_net_mtx);
    switch (msg) {
        case 0x000B0006:                                   // {user, -, xuid, context_id, value}
            g_net_ctx[nr32(buf + 0x10)] = nr32(buf + 0x14);
            return true;
        case 0x000B0007: {                                 // {user, -, xuid, property_id, size, data*}
            uint32_t id = nr32(buf + 0x10), size = std::min(nr32(buf + 0x14), 512u), data = nr32(buf + 0x18);
            std::vector<uint8_t> v(size); if (size && ngp(data)) nrd(data, v.data(), size);
            g_net_props[id] = v;
            return true;
        }
        case 0x000B0010: {                                 // XGI_SESSION_CREATE {obj, flags, pub, priv, user, info*, nonce*}
            NetSession* s = net_session_by_obj_locked(nr32(buf));
            uint32_t flags = nr32(buf + 4), info = nr32(buf + 0x14), nonce = nr32(buf + 0x18);
            if (!s) { *result = 0x6u; return true; }
            if (s->created) { *result = 0x65Bu; return true; }
            s->flags = flags; s->pub_max = s->pub_open = nr32(buf + 8); s->priv_max = s->priv_open = nr32(buf + 0xC);
            s->created = true; s->state = 0;
            bool host = (flags & 0x01) || flags == 0x706 || flags == 0 || flags == 0x04;   // HOST, single player + stats, offline, stats only
            if (host) {
                s->host = true;
                s->nonce = net_rand64();
                // XNKID type in the top byte: 0x00 system link, 0x80 online peer (sessions with Live features).
                uint64_t id = (net_rand64() & 0x00FFFFFFFFFFFFFFull) | ((flags & 0x1E) ? 0x8000000000000000ull : 0ull);
                if (!(id & 0x00FFFFFFFFFFFFFFull)) id |= 1;
                for (int i = 0; i < 8; ++i) s->info[i] = (uint8_t)(id >> (56 - 8 * i));
                net_make_xnaddr(net_my_ip(), s->info + 8);
                for (int i = 0; i < 16; i += 8) { uint64_t r = net_rand64(); memcpy(s->info + 44 + i, &r, 8); }
                if (ngp(info)) nwr(info, s->info, 60);
                if (ngp(nonce)) nw64(nonce, s->nonce);
            } else {                                       // joining: the caller passes the host's XSESSION_INFO
                if (ngp(info)) nrd(info, s->info, 60);
                uint64_t id = net_xnkid(s->info);
                auto f = g_net_found.find(id);
                if (f != g_net_found.end()) { memcpy(s->info + 8, f->second.info + 8, 36); if (ngp(info)) nwr(info + 8, s->info + 8, 36); }
                if (ngp(nonce)) nw64(nonce, s->nonce = net_rand64());
                g_net_joined_xnkid = id;
            }
            NETEV("[NET] XSessionCreate h=%08X flags=%03X pub=%u priv=%u -> %s %016llX\n", s->handle, flags, s->pub_max, s->priv_max,
                    host ? "host" : "join", (unsigned long long)net_xnkid(s->info));
            return true;
        }
        case 0x000B0011: {                                 // delete {obj, flags, nonce}
            NetSession* s = net_session_by_obj_locked(nr32(buf));
            if (!s) { *result = 0x6u; return true; }
            s->deleted = true;
            if (net_xnkid(s->info) == g_net_joined_xnkid) g_net_joined_xnkid = 0;
            NETEV("[NET] XSessionDelete h=%08X\n", s->handle);
            return true;
        }
        case 0x000B0012: case 0x000B0013: {               // XGI_SESSION_MANAGE {obj, count, xuid*, index*, private*}
            NetSession* s = net_session_by_obj_locked(nr32(buf));
            if (!s) { *result = 0x6u; return true; }
            uint32_t count = std::min(nr32(buf + 4), 16u), xuids = nr32(buf + 8), idx = nr32(buf + 0xC), priv = nr32(buf + 0x10);
            bool local = xuids == 0, join = msg == 0x000B0012;
            for (uint32_t i = 0; i < count; ++i) {
                uint64_t xuid; uint32_t ui = 0xFEu;
                if (local) { ui = ngp(idx) ? nr32(idx + i * 4) : 0u; xuid = lsw_net_user_xuid(ui); }
                else xuid = ngp(xuids) ? nr64(xuids + i * 8) : 0;
                auto it = std::find_if(s->members.begin(), s->members.end(), [&](const NetMember& m) { return m.xuid == xuid; });
                if (join) {
                    if (it != s->members.end()) continue;
                    bool want_priv = ngp(priv) && nr32(priv + i * 4);
                    uint32_t fl = 0;
                    if (want_priv && s->priv_open) { s->priv_open--; fl = 1; }
                    else if (s->pub_open) s->pub_open--;
                    else if (s->priv_open) { s->priv_open--; fl = 1; }
                    else { *result = 0x80155202u; break; }  // X_ONLINE_E_SESSION_FULL
                    s->members.push_back({xuid, ui, fl});
                } else if (it != s->members.end()) {
                    if (it->flags & 1) s->priv_open = std::min(s->priv_open + 1, s->priv_max); else s->pub_open = std::min(s->pub_open + 1, s->pub_max);
                    s->members.erase(it);
                }
                NETEV("[NET] %s%s h=%08X xuid=%016llX -> %zu member(s)\n", join ? "Join" : "Leave", local ? "Local" : "Remote", s->handle, (unsigned long long)xuid, s->members.size());
            }
            return true;
        }
        case 0x000B0014: case 0x000B0015: {               // start / end {obj, flags, nonce}
            NetSession* s = net_session_by_obj_locked(nr32(buf));
            if (!s) { *result = 0x6u; return true; }
            s->state = msg == 0x000B0014 ? 2u : 0u;
            NETEV("[NET] %s h=%08X\n", msg == 0x000B0014 ? "XSessionStart" : "XSessionEnd", s->handle);
            return true;
        }
        case 0x000B0018: {                                 // modify {obj, flags, max public, max private}
            NetSession* s = net_session_by_obj_locked(nr32(buf));
            if (!s) { *result = 0x6u; return true; }
            uint32_t pub = nr32(buf + 8), priv = nr32(buf + 0xC);
            uint32_t used_pub = s->pub_max - s->pub_open, used_priv = s->priv_max - s->priv_open;
            s->flags = (s->flags & 0x3Fu) | (nr32(buf + 4) & ~0x3Fu);   // only the modifier bits change
            s->pub_max = pub; s->priv_max = priv;
            s->pub_open = pub > used_pub ? pub - used_pub : 0; s->priv_open = priv > used_priv ? priv - used_priv : 0;
            return true;
        }
        case 0x000B0016: case 0x000B001C: case 0x000B0065: {   // search / search ex / weighted search
            uint32_t num = nr32(buf + 8);
            uint32_t size = msg == 0x000B0065 ? nr32(buf + 0x24) : nr32(buf + 0x18);
            uint32_t res  = msg == 0x000B0065 ? nr32(buf + 0x28) : nr32(buf + 0x1C);
            lk.unlock();
            std::vector<NetFound> v = net_search_hosts();
            *result = net_write_search_results(res, size, num, v);
            return true;
        }
        case 0x000B001B: case 0x000B0060: {               // by id {user, xnkid, size, results*} / by ids {user, n, ids*, size, results*}
            std::vector<uint64_t> ids;
            uint32_t size, res;
            if (msg == 0x000B001B) { uint8_t k[8]; nrd(buf + 4, k, 8); ids.push_back(net_xnkid(k)); size = nr32(buf + 0xC); res = nr32(buf + 0x10); }
            else {
                uint32_t n = std::min(nr32(buf + 4), 100u), p = nr32(buf + 8);
                for (uint32_t i = 0; i < n && ngp(p); ++i) { uint8_t k[8]; nrd(p + i * 8, k, 8); ids.push_back(net_xnkid(k)); }
                size = nr32(buf + 0xC); res = nr32(buf + 0x10);
            }
            std::vector<NetFound> v;
            for (uint64_t id : ids) { auto f = g_net_found.find(id); if (f != g_net_found.end()) v.push_back(f->second); }
            lk.unlock();
            *result = net_write_search_results(res, size, 0, v);
            return true;
        }
        case 0x000B001D: {                                 // details {obj, size*, details*}
            NetSession* s = net_session_by_obj_locked(nr32(buf));
            uint32_t sizep = nr32(buf + 4), det = nr32(buf + 8);
            if (!s) { *result = 0x6u; return true; }
            uint32_t size = ngp(sizep) ? nr32(sizep) : sizep;
            uint32_t want = 0x80 + (uint32_t)s->members.size() * 16;
            if (size < 0x80 || !ngp(det)) { if (ngp(sizep)) nw32(sizep, want); *result = 0x7Au; return true; }
            net_fill_details(*s, det, size);
            return true;
        }
        case 0x000B001E: {                                 // migrate host {obj, info*, user}: this machine becomes the host
            NetSession* s = net_session_by_obj_locked(nr32(buf));
            uint32_t info = nr32(buf + 4);
            if (!s) { *result = 0x6u; return true; }
            s->host = true; s->flags |= 0x01;
            net_make_xnaddr(net_my_ip(), s->info + 8);
            if (ngp(info)) nwr(info, s->info, 60);
            NETEV("[NET] XSessionMigrateHost h=%08X -> host\n", s->handle);
            return true;
        }
        case 0x000B001A: {                                 // arbitration {obj, flags, nonce, seconds, size, results*}
            uint32_t res = nr32(buf + 0x18);
            if (ngp(res)) { nw32(res, 0); nw32(res + 4, res + 8); }
            return true;
        }
        case 0x000B0008: case 0x000B0019: case 0x000B001F: case 0x000B0020: case 0x000B0025: case 0x000B0026:
            return true;                                   // achievements, invite data, skill, stats: accepted, not kept
        default:
            return false;
    }
}

// ── NetDll: startup, XNet addresses and keys ─────────────────────────────────────────────────────────────────────
PPC_FUNC(__imp__NetDll_XNetStartup) { PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetStartup"); net_start(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetCleanup) { PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetCleanup"); ctx.r3.u32 = 0; }
// WSAStartup(caller r3, version r4, WSADATA* r5)
PPC_FUNC(__imp__NetDll_WSAStartup) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("WSAStartup");
    net_start();
    if (ngp(ctx.r5.u32)) { nzero(ctx.r5.u32, 0x190); nw16(ctx.r5.u32, (uint16_t)ctx.r4.u32); nw16(ctx.r5.u32 + 2, 0x0202); }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NetDll_WSACleanup) { PPC_FUNC_PROLOGUE(); NET_LEGACY("WSACleanup"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetGetTitleXnAddr) {
    PPC_FUNC_PROLOGUE();
    if (lsw_net_on()) {
        // (caller r3, XNADDR* r4) -> XNADDR status flags.
        static const uint32_t st = [] { const char* e = getenv("LSWTCS_NET_XNSTATUS"); return e ? (uint32_t)strtoul(e, nullptr, 16) : 0xE6u; }();
        if (ngp(ctx.r4.u32)) { uint8_t xn[36]; net_make_xnaddr(net_my_ip(), xn); nwr(ctx.r4.u32, xn, 36); }
        static int n = 0; if (n++ < 4) NETEV("[NET] XNetGetTitleXnAddr -> status %02X lr=%08X\n", st, (uint32_t)ctx.lr);
        ctx.r3.u32 = st;
        return;
    }
    // (r3=caller id, r4=XNADDR* out). Returning 0 = XNET_GET_XNADDR_PENDING made the
    // frontend init poll forever (sub_827D7060 spin). Returning 8 (DHCP) diverted [T]10 boot
    // into a stall. Mimic Xenia instead: 1 with a plausible XNADDR (the game boots to the
    // title screen with this answer under Xenia). Note: in the XDK enum 1 is XNADDR_NONE,
    // not STATIC (4). LSWTCS_XNET=0 restores PENDING, =8 the DHCP answer, for A/B.
    static int mode = -1;
    if (mode < 0) { const char* e = getenv("LSWTCS_XNET"); mode = e ? atoi(e) : 1; }
    if (mode != 0 && ctx.r4.u32 >= 0x80000000u && ctx.r4.u32 < 0x94000000u) {
        uint8_t* xn = base + ctx.r4.u32;
        memset(xn, 0, 36);
        if (mode == 1) {
            xn[0] = 192; xn[1] = 168; xn[2] = 1; xn[3] = 100;   // ina = 192.168.1.100
            static const uint8_t mac[6] = {0x00, 0x22, 0x48, 0x01, 0x02, 0x03};
            memcpy(xn + 10, mac, 6);                             // abEnet
        }
    }
    ctx.r3.u32 = (mode == 0) ? 0u : (uint32_t)mode;
}
PPC_FUNC(__imp__NetDll_XNetGetDebugXnAddr) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetGetDebugXnAddr");
    if (ngp(ctx.r4.u32)) nzero(ctx.r4.u32, 36);
    ctx.r3.u32 = 1;   // XNADDR_NONE: no debugger
}
// XNetXnAddrToInAddr(caller r3, XNADDR* r4, XNKID* r5, IN_ADDR* r6): the peer's own IP (identity mapping).
PPC_FUNC(__imp__NetDll_XNetXnAddrToInAddr) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetXnAddrToInAddr");
    uint32_t xn = ctx.r4.u32, out = ctx.r6.u32;
    if (!ngp(xn) || !ngp(out)) { ctx.r3.u32 = 10022; return; }
    uint32_t ip = nr32(xn); if (!ip) ip = nr32(xn + 4);
    nw32(out, ip);
    NETLOG("[NET] XNetXnAddrToInAddr -> %08X\n", ip);
    ctx.r3.u32 = ip ? 0u : 10022u;
}
// XNetInAddrToXnAddr(caller r3, IN_ADDR r4, XNADDR* r5, XNKID* r6)
PPC_FUNC(__imp__NetDll_XNetInAddrToXnAddr) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetInAddrToXnAddr");
    uint32_t ip = ctx.r4.u32;
    if (ip == 0x7F000001u || ip == 0xFFFFFFFFu) ip = net_my_ip();
    if (ngp(ctx.r5.u32)) { uint8_t xn[36]; net_make_xnaddr(ip, xn); nwr(ctx.r5.u32, xn, 36); }
    if (ngp(ctx.r6.u32)) {
        uint64_t id = 0;
        {
            std::lock_guard<std::mutex> lk(g_net_mtx);
            for (auto& kv : g_net_sessions) if (kv.second.created && !kv.second.deleted && kv.second.host) id = net_xnkid(kv.second.info);
            if (!id) id = g_net_joined_xnkid;
        }
        nw64(ctx.r6.u32, id);
    }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NetDll_XNetXnAddrToMachineId) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetXnAddrToMachineId");
    if (!ngp(ctx.r4.u32) || !ngp(ctx.r5.u32)) { ctx.r3.u32 = 10022; return; }
    nw64(ctx.r5.u32, net_machine_id(nr32(ctx.r4.u32)));
    ctx.r3.u32 = 0;
}
// XNetInAddrToString(caller r3, IN_ADDR r4, char* r5, size r6)
PPC_FUNC(__imp__NetDll_XNetInAddrToString) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetInAddrToString");
    if (!ngp(ctx.r5.u32) || !ctx.r6.u32) { ctx.r3.u32 = 0x57; return; }
    char s[20]; net_ip_str(ctx.r4.u32, s, sizeof s);
    uint32_t n = std::min<uint32_t>((uint32_t)strlen(s) + 1, ctx.r6.u32);
    nwr(ctx.r5.u32, s, n); nw8(ctx.r5.u32 + n - 1, 0);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NetDll_XNetConnect) { PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetConnect"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetGetConnectStatus) { PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetGetConnectStatus"); ctx.r3.u32 = 2; }   // CONNECTED
PPC_FUNC(__imp__NetDll_XNetUnregisterInAddr) { PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetUnregisterInAddr"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetRegisterKey) { PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetRegisterKey"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetUnregisterKey) { PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetUnregisterKey"); ctx.r3.u32 = 0; }
// XNetCreateKey(caller r3, XNKID* r4, XNKEY* r5): a system-link session key.
PPC_FUNC(__imp__NetDll_XNetCreateKey) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetCreateKey");
    if (ngp(ctx.r4.u32)) { uint64_t id = (net_rand64() & 0x00FFFFFFFFFFFFFFull) | 1; nw64(ctx.r4.u32, id); }
    if (ngp(ctx.r5.u32)) { nw64(ctx.r5.u32, net_rand64()); nw64(ctx.r5.u32 + 8, net_rand64()); }
    ctx.r3.u32 = 0;
}
// XNetRandom(caller r3, buffer r4, length r5)
PPC_FUNC(__imp__NetDll_XNetRandom) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetRandom");
    uint32_t p = ctx.r4.u32, n = ctx.r5.u32;
    if (ngp(p)) for (uint32_t i = 0; i < n && i < 0x10000; i += 8) { uint64_t r = net_rand64(); nwr(p + i, &r, std::min(8u, n - i)); }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus) { PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetGetEthernetLinkStatus"); ctx.r3.u32 = 0x0B; }   // ACTIVE|100MBPS|FULL_DUPLEX
PPC_FUNC(__imp__NetDll_XNetGetBroadcastVersionStatus) { PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetGetBroadcastVersionStatus"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetGetOpt) { PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetGetOpt"); ctx.r3.u32 = 10022; }
PPC_FUNC(__imp__NetDll_XNetSetOpt) { PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetSetOpt"); ctx.r3.u32 = 0; }
// Live service lookups: there are no Microsoft servers to talk to.
PPC_FUNC(__imp__NetDll_XNetDnsLookup) {   // (caller, host*, event, XNDNS** r6)
    PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetDnsLookup");
    if (ngp(ctx.r6.u32)) nw32(ctx.r6.u32, 0);
    ctx.r3.u32 = 11001;   // WSAHOST_NOT_FOUND
}
PPC_FUNC(__imp__NetDll_XNetDnsRelease) { PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetDnsRelease"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetServerToInAddr) { PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetServerToInAddr"); ctx.r3.u32 = 10022; }
PPC_FUNC(__imp__NetDll_XNetInAddrToServer) { PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetInAddrToServer"); ctx.r3.u32 = 10022; }
PPC_FUNC(__imp__NetDll_XNetTsAddrToInAddr) { PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetTsAddrToInAddr"); ctx.r3.u32 = 10022; }
PPC_FUNC(__imp__NetDll_XNetQosServiceLookup) {   // (caller, flags, event, XNQOS** r6)
    PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetQosServiceLookup");
    if (ngp(ctx.r6.u32)) nw32(ctx.r6.u32, 0);
    ctx.r3.u32 = 10050;
}

// ── QoS: hosts publish a data blob (XNetQosListen), searchers read it with the ping (XNetQosLookup) ─────────────────
// XNetQosListen(caller r3, XNKID* r4, data* r5, size r6, bps r7, flags r8). Flags: 1 enable, 2 disable, 4 set data,
// 8 set bits/s, 0x10 release.
PPC_FUNC(__imp__NetDll_XNetQosListen) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetQosListen");
    uint32_t kid = ctx.r4.u32, data = ctx.r5.u32, size = std::min(ctx.r6.u32, 1024u), flags = ctx.r8.u32;
    if (!ngp(kid)) { ctx.r3.u32 = 10022; return; }
    uint8_t k[8]; nrd(kid, k, 8);
    uint64_t id = net_xnkid(k);
    std::lock_guard<std::mutex> lk(g_net_mtx);
    if (flags & 0x10) g_net_qos_listen.erase(id);
    else if ((flags & 0x04) && ngp(data)) { std::vector<uint8_t> v(size); if (size) nrd(data, v.data(), size); g_net_qos_listen[id] = v; }
    NETEV("[NET] XNetQosListen %016llX flags=%X size=%u\n", (unsigned long long)id, flags, size);
    ctx.r3.u32 = 0;
}
// XNetQosLookup(caller r3, n r4, XNADDR** r5, XNKID** r6, XNKEY** r7, n_gateways r8, gateways r9, service_ids r10,
//               probes [sp+54], bps [sp+5C], flags [sp+64], event [sp+6C], XNQOS** [sp+74]).
// XNQOS {count, count_pending, XNQOSINFO[n]}; XNQOSINFO 0x18 {flags u8, -, probes_xmit u16, probes_recv u16,
// data_len u16, data* u32, rtt_min u16, rtt_med u16, up_bps u32, down_bps u32}. Flags: 1 complete, 2 contacted,
// 8 data received.
PPC_FUNC(__imp__NetDll_XNetQosLookup) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetQosLookup");
    uint32_t n = std::min(ctx.r4.u32, 64u), kids = ctx.r6.u32, ngw = std::min(ctx.r8.u32, 8u);
    uint32_t probes = PPC_LOAD_U32(ctx.r1.u32 + 0x54), event = PPC_LOAD_U32(ctx.r1.u32 + 0x6C), out = PPC_LOAD_U32(ctx.r1.u32 + 0x74);
    if (!ngp(out)) { ctx.r3.u32 = 10013; return; }
    uint32_t total = n + ngw;
    std::vector<std::vector<uint8_t>> data(total);
    std::vector<bool> found(total, false);
    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        for (uint32_t i = 0; i < n && ngp(kids); ++i) {
            uint32_t kp = nr32(kids + i * 4); if (!ngp(kp)) continue;
            uint8_t k[8]; nrd(kp, k, 8);
            auto f = g_net_found.find(net_xnkid(k));
            if (f != g_net_found.end()) { found[i] = true; data[i] = f->second.qos; }
        }
    }
    uint32_t size = 8 + total * 0x18;
    for (auto& d : data) size += ((uint32_t)d.size() + 3) & ~3u;
    uint32_t q = g_phys_alloc(std::max(size, 0x20u));
    if (!q) { ctx.r3.u32 = 10055; return; }
    uint32_t dp = q + 8 + total * 0x18;
    for (uint32_t i = 0; i < total; ++i) {
        uint32_t e = q + 8 + i * 0x18;
        uint8_t fl = found[i] ? 0x03 : 0x01;
        if (found[i] && !data[i].empty()) { nwr(dp, data[i].data(), (uint32_t)data[i].size()); nw16(e + 6, (uint16_t)data[i].size()); nw32(e + 8, dp); dp += ((uint32_t)data[i].size() + 3) & ~3u; fl |= 0x08; }
        nw8(e, fl);
        if (found[i]) {
            nw16(e + 2, (uint16_t)std::max(probes, 1u)); nw16(e + 4, (uint16_t)std::max(probes, 1u));
            nw16(e + 12, 15); nw16(e + 14, 15); nw32(e + 16, 5u << 20); nw32(e + 20, 5u << 20);
        }
    }
    nw32(q + 0, 0); nw32(q + 4, total);
    nw32(out, q);
    { std::lock_guard<std::mutex> lk(g_net_mtx); g_net_qos_jobs.push_back({q, total, event, net_now_ms() + 30}); }
    net_start();
    NETEV("[NET] XNetQosLookup n=%u gateways=%u -> %08X\n", n, ngw, q);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NetDll_XNetQosRelease) {   // (caller, XNQOS*)
    PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetQosRelease");
    if (ngp(ctx.r4.u32)) {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        g_net_qos_jobs.erase(std::remove_if(g_net_qos_jobs.begin(), g_net_qos_jobs.end(), [&](const NetQosJob& j) { return j.qos == ctx.r4.u32; }), g_net_qos_jobs.end());
    }
    if (ngp(ctx.r4.u32)) g_phys_free_block(ctx.r4.u32);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NetDll_XNetQosGetListenStats) { PPC_FUNC_PROLOGUE(); NET_LEGACY("XNetQosGetListenStats"); ctx.r3.u32 = 0; }

// ── sockets ─────────────────────────────────────────────────────────────────────────────────────────────────────────
// Guest sockets are small handles (0x100+) over non-blocking host sockets. A blocking guest call waits off the GIL.
static const uint32_t kSockErr = 0xFFFFFFFFu;
static bool net_sock_get(uint32_t s, NetSock& out) {
    std::lock_guard<std::mutex> lk(g_net_mtx);
    auto it = g_net_socks.find(s);
    if (it == g_net_socks.end()) return false;
    out = it->second; return true;
}
// Wait until readable (ev 1) / writable (ev 2), at most timeout_ms (-1 = forever). Leaves the GIL while waiting.
static bool net_wait(long long h, short ev, int timeout_ms) {
    short rev = 0;
    if (lswnet_poll(1, &h, &ev, &rev, 0) > 0) return true;
    if (timeout_ms == 0) return false;
    GilYield y;
    uint64_t end = timeout_ms < 0 ? ~0ull : net_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        if (lswnet_poll(1, &h, &ev, &rev, 20) > 0) return true;
        if (net_now_ms() >= end) return false;
    }
}
static bool net_read_sockaddr(uint32_t sa, uint32_t* ip, uint16_t* port) {
    if (!ngp(sa)) return false;
    *port = net_host_port(nr16(sa + 2)); *ip = nr32(sa + 4);
    return true;
}
#define NET_SOCK_OR_FAIL(var, handle) NetSock var; if (!net_sock_get(handle, var)) { net_set_err(10038); ctx.r3.u32 = kSockErr; return; }

// socket(caller r3, af r4, type r5, protocol r6): 6 TCP, 17 UDP, 254 VDP (UDP carrying [u16 game-data length]...).
PPC_FUNC(__imp__NetDll_socket) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("socket");
    net_start();
    uint32_t type = ctx.r5.u32, proto = ctx.r6.u32;
    bool stream = type == 1 || (type == 0 && proto == 6);
    long long h = lswnet_socket(stream ? 1 : 2);
    if (h < 0) { net_set_err((uint32_t)lswnet_last_error()); ctx.r3.u32 = kSockErr; return; }
    if (!stream) lswnet_setopt(h, 1, 1);   // the 360 lets system-link titles broadcast without asking
    NetSock s; s.h = h; s.stream = stream; s.vdp = proto == 254;
    uint32_t id;
    { std::lock_guard<std::mutex> lk(g_net_mtx); id = g_net_next_sock++; g_net_socks[id] = s; }
    NETEV("[NET] socket type=%u proto=%u -> %X lr=%08X\n", type, proto, id, (uint32_t)ctx.lr);
    ctx.r3.u32 = id;
}
PPC_FUNC(__imp__NetDll_closesocket) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("closesocket");
    NetSock s; std::vector<uint32_t> ovls;
    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        auto it = g_net_socks.find(ctx.r4.u32);
        if (it == g_net_socks.end()) { net_set_err(10038); ctx.r3.u32 = kSockErr; return; }
        s = it->second; g_net_socks.erase(it);
        for (auto p = g_net_pending.begin(); p != g_net_pending.end();) { if (p->sock == ctx.r4.u32) { ovls.push_back(p->ovl); p = g_net_pending.erase(p); } else ++p; }
    }
    for (uint32_t o : ovls) net_complete_wsa_ovl(o, 995, 0);   // WSA_OPERATION_ABORTED
    lswnet_close(s.h);
    ctx.r3.u32 = 0;
}
// bind(caller r3, s r4, sockaddr* r5, len r6). XSOCKADDR_IN: family u16, port u16, addr u32 (big-endian = network order).
PPC_FUNC(__imp__NetDll_bind) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("bind");
    NET_SOCK_OR_FAIL(s, ctx.r4.u32);
    uint32_t ip = 0; uint16_t port = 0;
    if (!net_read_sockaddr(ctx.r5.u32, &ip, &port)) { net_set_err(10014); ctx.r3.u32 = kSockErr; return; }
    if (!ip) ip = net_bind_ip();
    int r = lswnet_bind(s.h, ip, port);
    NETEV("[NET] bind %X port %u -> %d (err %d)\n", ctx.r4.u32, port, r, r ? lswnet_last_error() : 0);
    if (r) { net_set_err((uint32_t)lswnet_last_error()); ctx.r3.u32 = kSockErr; return; }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NetDll_connect) {   // (caller, s, sockaddr*, len)
    PPC_FUNC_PROLOGUE(); NET_LEGACY("connect");
    NET_SOCK_OR_FAIL(s, ctx.r4.u32);
    uint32_t ip; uint16_t port;
    if (!net_read_sockaddr(ctx.r5.u32, &ip, &port)) { net_set_err(10014); ctx.r3.u32 = kSockErr; return; }
    int r = lswnet_connect(s.h, ip, port);
    if (r && s.stream && !s.guest_nb && (lswnet_last_error() == 10035 || lswnet_last_error() == 10036)) {
        r = net_wait(s.h, 2, 10000) ? 0 : -1;   // a blocking guest connect
        if (r) { net_set_err(10060); ctx.r3.u32 = kSockErr; return; }
    } else if (r) { net_set_err((uint32_t)lswnet_last_error()); ctx.r3.u32 = kSockErr; return; }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NetDll_listen) {   // (caller, s, backlog)
    PPC_FUNC_PROLOGUE(); NET_LEGACY("listen");
    NET_SOCK_OR_FAIL(s, ctx.r4.u32);
    if (lswnet_listen(s.h, (int)ctx.r5.u32)) { net_set_err((uint32_t)lswnet_last_error()); ctx.r3.u32 = kSockErr; return; }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NetDll_accept) {   // (caller, s, sockaddr*, len*)
    PPC_FUNC_PROLOGUE(); NET_LEGACY("accept");
    NET_SOCK_OR_FAIL(s, ctx.r4.u32);
    uint32_t ip = 0; uint16_t port = 0;
    long long c = lswnet_accept(s.h, &ip, &port);
    if (c < 0 && lswnet_last_error() == 10035 && !s.guest_nb && net_wait(s.h, 1, -1)) c = lswnet_accept(s.h, &ip, &port);
    if (c < 0) { net_set_err((uint32_t)lswnet_last_error()); ctx.r3.u32 = kSockErr; return; }
    NetSock ns; ns.h = c; ns.stream = true;
    uint32_t id;
    { std::lock_guard<std::mutex> lk(g_net_mtx); id = g_net_next_sock++; g_net_socks[id] = ns; }
    net_write_sockaddr(ctx.r5.u32, ctx.r6.u32, ip, port);
    ctx.r3.u32 = id;
}
PPC_FUNC(__imp__NetDll_shutdown) {   // (caller, s, how)
    PPC_FUNC_PROLOGUE(); NET_LEGACY("shutdown");
    NET_SOCK_OR_FAIL(s, ctx.r4.u32);
    lswnet_shutdown(s.h, (int)ctx.r5.u32);
    ctx.r3.u32 = 0;
}
// setsockopt(caller r3, s r4, level r5, name r6, value* r7, len r8). SOL_SOCKET = 0xFFFF.
PPC_FUNC(__imp__NetDll_setsockopt) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("setsockopt");
    NET_SOCK_OR_FAIL(s, ctx.r4.u32);
    uint32_t level = ctx.r5.u32, name = ctx.r6.u32, v = ngp(ctx.r7.u32) ? (ctx.r8.u32 >= 4 ? nr32(ctx.r7.u32) : *lsw_host(ctx.r7.u32)) : 0;
    NETLOG("[NET] setsockopt %X level=%X opt=%X val=%u\n", ctx.r4.u32, level, name, v);
    if (level == 0xFFFF) {
        switch (name) {
            case 0x0004: lswnet_setopt(s.h, 2, v ? 1 : 0); break;          // SO_REUSEADDR
            case 0x0020: lswnet_setopt(s.h, 1, v ? 1 : 0); break;          // SO_BROADCAST
            case 0x1001: lswnet_setopt(s.h, 4, (int)v); break;             // SO_SNDBUF
            case 0x1002: lswnet_setopt(s.h, 3, (int)v); break;             // SO_RCVBUF
            case 0x1006: { std::lock_guard<std::mutex> lk(g_net_mtx); g_net_socks[ctx.r4.u32].rcv_timeout_ms = v; break; }   // SO_RCVTIMEO
            default: break;                                                // SO_MARKINSECURE (0x5801) etc.: XNet security, not needed
        }
    } else if (level == 6 && name == 1) lswnet_setopt(s.h, 5, v ? 1 : 0); // TCP_NODELAY
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NetDll_getsockopt) {   // (caller, s, level, name, value*, len*)
    PPC_FUNC_PROLOGUE(); NET_LEGACY("getsockopt");
    NET_SOCK_OR_FAIL(s, ctx.r4.u32);
    if (ngp(ctx.r7.u32)) nw32(ctx.r7.u32, ctx.r6.u32 == 0x1008 ? (s.stream ? 1u : 2u) : (ctx.r6.u32 == 0x1001 || ctx.r6.u32 == 0x1002) ? 0x10000u : 0u);   // SO_TYPE, buffer sizes
    if (ngp(ctx.r8.u32)) nw32(ctx.r8.u32, 4);
    ctx.r3.u32 = 0;
}
// ioctlsocket(caller r3, s r4, cmd r5, arg* r6): FIONBIO 0x8004667E, FIONREAD 0x4004667F.
PPC_FUNC(__imp__NetDll_ioctlsocket) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("ioctlsocket");
    NET_SOCK_OR_FAIL(s, ctx.r4.u32);
    if (ctx.r5.u32 == 0x8004667Eu) {
        std::lock_guard<std::mutex> lk(g_net_mtx); g_net_socks[ctx.r4.u32].guest_nb = ngp(ctx.r6.u32) && nr32(ctx.r6.u32) != 0;
    } else if (ctx.r5.u32 == 0x4004667Fu) {
        int n = lswnet_pending(s.h);
        if (ngp(ctx.r6.u32)) nw32(ctx.r6.u32, n > 0 ? (uint32_t)n : 0u);
    } else { net_set_err(10022); ctx.r3.u32 = kSockErr; return; }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NetDll_getsockname) {   // (caller, s, sockaddr*, len*)
    PPC_FUNC_PROLOGUE(); NET_LEGACY("getsockname");
    NET_SOCK_OR_FAIL(s, ctx.r4.u32);
    uint32_t ip = 0; uint16_t port = 0;
    if (lswnet_sockname(s.h, &ip, &port, 0)) { net_set_err((uint32_t)lswnet_last_error()); ctx.r3.u32 = kSockErr; return; }
    net_write_sockaddr(ctx.r5.u32, ctx.r6.u32, ip ? ip : net_my_ip(), port);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NetDll_getpeername) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("getpeername");
    NET_SOCK_OR_FAIL(s, ctx.r4.u32);
    uint32_t ip = 0; uint16_t port = 0;
    if (lswnet_sockname(s.h, &ip, &port, 1)) { net_set_err((uint32_t)lswnet_last_error()); ctx.r3.u32 = kSockErr; return; }
    net_write_sockaddr(ctx.r5.u32, ctx.r6.u32, ip, port);
    ctx.r3.u32 = 0;
}
// One receive, honouring guest blocking mode. Returns bytes, or -1 with t_net_err set.
static int net_recv_one(uint32_t sid, const NetSock& s, uint8_t* dst, uint32_t len, uint32_t* ip, uint16_t* port) {
    for (;;) {
        int n = lswnet_recvfrom(s.h, dst, (int)len, ip, port);
        if (n >= 0) return n;
        int e = lswnet_last_error();
        if (e != 10035 || s.guest_nb) { net_set_err((uint32_t)e); return -1; }
        if (!net_wait(s.h, 1, s.rcv_timeout_ms ? (int)s.rcv_timeout_ms : -1)) { net_set_err(10060); return -1; }
        NetSock again; if (!net_sock_get(sid, again)) { net_set_err(10038); return -1; }   // closed while waiting
    }
}
// recv(caller r3, s r4, buf r5, len r6, flags r7) / recvfrom(..., sockaddr* r8, len* r9)
PPC_FUNC(__imp__NetDll_recv) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("recv");
    NET_SOCK_OR_FAIL(s, ctx.r4.u32);
    if (!ngp(ctx.r5.u32)) { net_set_err(10014); ctx.r3.u32 = kSockErr; return; }
    uint32_t ip; uint16_t port;
    int n = net_recv_one(ctx.r4.u32, s, lsw_host(ctx.r5.u32), ctx.r6.u32, &ip, &port);
    ctx.r3.u32 = n < 0 ? kSockErr : (uint32_t)n;
}
PPC_FUNC(__imp__NetDll_recvfrom) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("recvfrom");
    NET_SOCK_OR_FAIL(s, ctx.r4.u32);
    if (!ngp(ctx.r5.u32)) { net_set_err(10014); ctx.r3.u32 = kSockErr; return; }
    uint32_t ip = 0; uint16_t port = 0;
    int n = net_recv_one(ctx.r4.u32, s, lsw_host(ctx.r5.u32), ctx.r6.u32, &ip, &port);
    if (n >= 0) { net_write_sockaddr(ctx.r8.u32, ctx.r9.u32, ip, port); net_traffic_log(false, ip, port, n, lsw_host(ctx.r5.u32)); }
    ctx.r3.u32 = n < 0 ? kSockErr : (uint32_t)n;
}
// A send to the broadcast address also goes to every LSWTCS_NET_PEERS address, so system-link style discovery
// reaches peers outside the LAN.
static int net_send_one(const NetSock& s, const uint8_t* src, uint32_t len, uint32_t ip, uint16_t port, bool has_to) {
    int n = lswnet_sendto(s.h, src, (int)len, ip, port, has_to ? 1 : 0);
    if (has_to) net_traffic_log(true, ip, port, n < 0 ? -lswnet_last_error() : n, src);
    if (n < 0 && lswnet_last_error() == 10035 && !s.guest_nb && net_wait(s.h, 2, 2000)) n = lswnet_sendto(s.h, src, (int)len, ip, port, has_to ? 1 : 0);
    if (n < 0) { net_set_err((uint32_t)lswnet_last_error()); return -1; }
    if (has_to && ip == 0xFFFFFFFFu) {
        lswnet_sendto(s.h, src, (int)len, lswnet_subnet_broadcast(net_my_ip()), port, 1);
        for (uint32_t p : g_net_peers) lswnet_sendto(s.h, src, (int)len, p, port, 1);
    }
    return n;
}
PPC_FUNC(__imp__NetDll_send) {   // (caller, s, buf, len, flags)
    PPC_FUNC_PROLOGUE(); NET_LEGACY("send");
    NET_SOCK_OR_FAIL(s, ctx.r4.u32);
    if (!ngp(ctx.r5.u32)) { net_set_err(10014); ctx.r3.u32 = kSockErr; return; }
    int n = net_send_one(s, lsw_host(ctx.r5.u32), ctx.r6.u32, 0, 0, false);
    ctx.r3.u32 = n < 0 ? kSockErr : (uint32_t)n;
}
PPC_FUNC(__imp__NetDll_sendto) {   // (caller, s, buf, len, flags, sockaddr* r8, len r9)
    PPC_FUNC_PROLOGUE(); NET_LEGACY("sendto");
    NET_SOCK_OR_FAIL(s, ctx.r4.u32);
    if (!ngp(ctx.r5.u32)) { net_set_err(10014); ctx.r3.u32 = kSockErr; return; }
    uint32_t ip = 0; uint16_t port = 0;
    bool has_to = net_read_sockaddr(ctx.r8.u32, &ip, &port);
    int n = net_send_one(s, lsw_host(ctx.r5.u32), ctx.r6.u32, ip, port, has_to);
    NETLOG("[NET] sendto %X %u bytes to %08X:%u -> %d\n", ctx.r4.u32, ctx.r6.u32, ip, port, n);
    ctx.r3.u32 = n < 0 ? kSockErr : (uint32_t)n;
}
// select(caller r3, nfds r4, read* r5, write* r6, except* r7, timeval* r8). fd_set: {count u32, handles u32[64]}.
PPC_FUNC(__imp__NetDll_select) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("select");
    uint32_t sets[3] = {ctx.r5.u32, ctx.r6.u32, ctx.r7.u32};
    std::vector<long long> hs; std::vector<short> ev; std::vector<std::pair<int, uint32_t>> who;   // (set, guest handle)
    for (int k = 0; k < 3; ++k) {
        if (!ngp(sets[k])) continue;
        uint32_t c = std::min(nr32(sets[k]), 64u);
        for (uint32_t i = 0; i < c; ++i) {
            uint32_t g = nr32(sets[k] + 4 + i * 4); NetSock s;
            if (!net_sock_get(g, s)) { net_set_err(10038); ctx.r3.u32 = kSockErr; return; }
            hs.push_back(s.h); ev.push_back(k == 0 ? 1 : k == 1 ? 2 : 4); who.emplace_back(k, g);
        }
    }
    int timeout = -1;
    if (ngp(ctx.r8.u32)) timeout = (int)(nr32(ctx.r8.u32) * 1000u + nr32(ctx.r8.u32 + 4) / 1000u);
    std::vector<short> rev(hs.size(), 0);
    int r = lswnet_poll((int)hs.size(), hs.data(), ev.data(), rev.data(), 0);
    if (r == 0 && timeout != 0) {
        GilYield y;
        uint64_t end = timeout < 0 ? ~0ull : net_now_ms() + (uint64_t)timeout;
        do { r = lswnet_poll((int)hs.size(), hs.data(), ev.data(), rev.data(), 20); } while (r == 0 && net_now_ms() < end);
    }
    if (r < 0) { net_set_err((uint32_t)lswnet_last_error()); ctx.r3.u32 = kSockErr; return; }
    uint32_t cnt[3] = {0, 0, 0}, total = 0;
    for (size_t i = 0; i < who.size(); ++i) {
        int k = who[i].first;
        if (rev[i] & ev[i] || (k == 0 && (rev[i] & 4))) { nw32(sets[k] + 4 + cnt[k] * 4, who[i].second); cnt[k]++; total++; }
    }
    for (int k = 0; k < 3; ++k) if (ngp(sets[k])) nw32(sets[k], cnt[k]);
    ctx.r3.u32 = total;
}
PPC_FUNC(__imp__NetDll___WSAFDIsSet) {   // (s r3, fd_set* r4) — no caller argument
    PPC_FUNC_PROLOGUE(); NET_LEGACY("__WSAFDIsSet");
    uint32_t set = ctx.r4.u32, r = 0;
    if (ngp(set)) { uint32_t c = std::min(nr32(set), 64u); for (uint32_t i = 0; i < c; ++i) if (nr32(set + 4 + i * 4) == ctx.r3.u32) r = 1; }
    ctx.r3.u32 = r;
}
PPC_FUNC(__imp__NetDll_inet_addr) {   // (const char* r3) — no caller argument
    PPC_FUNC_PROLOGUE(); NET_LEGACY("inet_addr");
    char s[32] = {}; if (ngp(ctx.r3.u32)) for (int i = 0; i < 31; ++i) { s[i] = (char)*lsw_host(ctx.r3.u32 + i); if (!s[i]) break; }
    uint32_t ip; ctx.r3.u32 = lswnet_parse_ip(s, &ip) ? ip : 0xFFFFFFFFu;   // INADDR_NONE; the value is in network order = guest order
}
PPC_FUNC(__imp__NetDll_WSAGetLastError) { PPC_FUNC_PROLOGUE(); NET_LEGACY("WSAGetLastError"); ctx.r3.u32 = t_net_err; }
PPC_FUNC(__imp__NetDll_WSASetLastError) { PPC_FUNC_PROLOGUE(); NET_LEGACY("WSASetLastError"); t_net_err = ctx.r3.u32; }

// ── WSA overlapped I/O and events. WSAOVERLAPPED: Internal (status; 0x103 pending), InternalHigh (bytes), Offset,
// OffsetHigh, hEvent (+16). ─────────────────────────────────────────────────────────────────────────────────────
// Receive into WSABUFs now if a datagram is waiting; otherwise (overlapped) queue it for the worker thread.
static uint32_t net_wsa_recv(uint32_t sid, uint32_t bufs, uint32_t nbufs, uint32_t bytes_p, uint32_t flags_p,
                             uint32_t from_p, uint32_t fromlen_p, uint32_t ovl) {
    NetSock s; if (!net_sock_get(sid, s)) { net_set_err(10038); return kSockErr; }
    net_start();
    static thread_local uint8_t tmp[65536];
    uint32_t ip = 0; uint16_t port = 0;
    int n = lswnet_recvfrom(s.h, tmp, sizeof tmp, &ip, &port);
    { static int k = 0; if (k++ < 12) NETEV("[NET] WSARecv s=%X bufs=%08X n=%u bytes*=%08X flags*=%08X from*=%08X fromlen*=%08X ovl=%08X -> %s\n",
                                         sid, bufs, nbufs, bytes_p, flags_p, from_p, fromlen_p, ovl,
                                         n >= 0 ? "immediate" : lswnet_last_error() == 10035 ? (ngp(ovl) ? "pending" : "wouldblock") : "error"); }
    if (n < 0 && lswnet_last_error() == 10035) {
        if (ngp(ovl)) {
            nw32(ovl + 0, 0x103); nw32(ovl + 4, 0);
            std::lock_guard<std::mutex> lk(g_net_mtx);
            NetPendingRecv pr = {};
            pr.sock = sid; pr.from_p = from_p; pr.fromlen_p = fromlen_p; pr.ovl = ovl;
            pr.nbufs = std::min(nbufs, 4u);
            for (uint32_t b = 0; b < pr.nbufs && ngp(bufs); ++b) { pr.buf_len[b] = nr32(bufs + b * 8); pr.buf_ptr[b] = nr32(bufs + b * 8 + 4); }
            g_net_pending.push_back(pr);
            net_set_err(997); return kSockErr;                                        // WSA_IO_PENDING
        }
        if (!s.guest_nb && net_wait(s.h, 1, s.rcv_timeout_ms ? (int)s.rcv_timeout_ms : -1)) n = lswnet_recvfrom(s.h, tmp, sizeof tmp, &ip, &port);
    }
    if (n < 0) { net_set_err((uint32_t)lswnet_last_error()); return kSockErr; }
    uint32_t got = net_scatter(bufs, nbufs, tmp, (uint32_t)n);
    if (!s.stream) net_traffic_log(false, ip, port, n, tmp);
    if (ngp(bytes_p)) nw32(bytes_p, got);
    if (ngp(flags_p)) nw32(flags_p, 0);
    if (!s.stream) net_write_sockaddr(from_p, fromlen_p, ip, port);
    if (ngp(ovl)) net_complete_wsa_ovl(ovl, 0, got);
    return 0;
}
static uint32_t net_wsa_send(uint32_t sid, uint32_t bufs, uint32_t nbufs, uint32_t bytes_p, uint32_t to_p, uint32_t ovl) {
    NetSock s; if (!net_sock_get(sid, s)) { net_set_err(10038); return kSockErr; }
    std::vector<uint8_t> data;
    for (uint32_t i = 0; i < std::min(nbufs, 16u) && ngp(bufs); ++i) {
        uint32_t len = nr32(bufs + i * 8), ptr = nr32(bufs + i * 8 + 4);
        if (!ngp(ptr) || len > 0x10000) continue;
        size_t o = data.size(); data.resize(o + len); nrd(ptr, data.data() + o, len);
    }
    uint32_t ip = 0; uint16_t port = 0;
    bool has_to = net_read_sockaddr(to_p, &ip, &port);
    int n = net_send_one(s, data.data(), (uint32_t)data.size(), ip, port, has_to);
    if (n < 0) return kSockErr;
    if (ngp(bytes_p)) nw32(bytes_p, (uint32_t)n);
    if (ngp(ovl)) net_complete_wsa_ovl(ovl, 0, (uint32_t)n);
    return 0;
}
// WSARecvFrom(caller, s, bufs, n, bytes*, flags*, from*, fromlen*, [sp+54] ovl, [sp+5C] completion)
PPC_FUNC(__imp__NetDll_WSARecvFrom) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("WSARecvFrom");
    { static int k = 0; if (k++ < 6) NETEV("[NET] WSARecvFrom completion routine=%08X lr=%08X\n", PPC_LOAD_U32(ctx.r1.u32 + 0x5C), (uint32_t)ctx.lr); }
    ctx.r3.u32 = net_wsa_recv(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, PPC_LOAD_U32(ctx.r1.u32 + 0x54));
}
// WSARecv(caller, s, bufs, n, bytes*, flags*, ovl r9, completion r10)
PPC_FUNC(__imp__NetDll_WSARecv) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("WSARecv");
    ctx.r3.u32 = net_wsa_recv(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, 0, 0, ctx.r9.u32);
}
// WSASendTo(caller, s, bufs, n, bytes*, flags, to*, tolen, [sp+54] ovl, [sp+5C] completion)
PPC_FUNC(__imp__NetDll_WSASendTo) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("WSASendTo");
    ctx.r3.u32 = net_wsa_send(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r9.u32, PPC_LOAD_U32(ctx.r1.u32 + 0x54));
}
// WSASend(caller, s, bufs, n, bytes*, flags, ovl r9, completion r10)
PPC_FUNC(__imp__NetDll_WSASend) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("WSASend");
    ctx.r3.u32 = net_wsa_send(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, 0, ctx.r9.u32);
}
// WSAGetOverlappedResult(caller r3, s r4, ovl r5, bytes* r6, wait r7, flags* r8) -> BOOL
PPC_FUNC(__imp__NetDll_WSAGetOverlappedResult) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("WSAGetOverlappedResult");
    uint32_t ovl = ctx.r5.u32;
    if (!ngp(ovl)) { net_set_err(10014); ctx.r3.u32 = 0; return; }
    if (nr32(ovl) == 0x103) {
        if (!ctx.r7.u32) { net_set_err(996); ctx.r3.u32 = 0; return; }   // WSA_IO_INCOMPLETE
        GilYield y;
        while (nr32(ovl) == 0x103) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    uint32_t st = nr32(ovl);
    { static int n = 0; if (n++ < 8) NETEV("[NET] WSAGetOverlappedResult s=%X ovl=%08X -> status %u bytes %u wait=%u lr=%08X\n",
                                           ctx.r4.u32, ovl, st, nr32(ovl + 4), ctx.r7.u32, (uint32_t)ctx.lr); }
    if (ngp(ctx.r6.u32)) nw32(ctx.r6.u32, nr32(ovl + 4));
    if (ngp(ctx.r8.u32)) nw32(ctx.r8.u32, 0);
    if (st) { net_set_err(st); ctx.r3.u32 = 0; return; }
    ctx.r3.u32 = 1;
}
PPC_FUNC(__imp__NetDll_WSACancelOverlappedIO) {   // (caller, s)
    PPC_FUNC_PROLOGUE(); NET_LEGACY("WSACancelOverlappedIO");
    std::vector<uint32_t> ovls;
    {
        std::lock_guard<std::mutex> lk(g_net_mtx);
        for (auto p = g_net_pending.begin(); p != g_net_pending.end();) { if (p->sock == ctx.r4.u32) { ovls.push_back(p->ovl); p = g_net_pending.erase(p); } else ++p; }
    }
    for (uint32_t o : ovls) net_complete_wsa_ovl(o, 995, 0);
    ctx.r3.u32 = 0;
}
// WSAEventSelect(caller r3, s r4, event r5, mask r6): FD_READ 1, FD_WRITE 2 (always writable for UDP).
PPC_FUNC(__imp__NetDll_WSAEventSelect) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("WSAEventSelect");
    NET_SOCK_OR_FAIL(s, ctx.r4.u32);
    { std::lock_guard<std::mutex> lk(g_net_mtx); g_net_socks[ctx.r4.u32].ev_handle = ctx.r5.u32; g_net_socks[ctx.r4.u32].ev_mask = ctx.r6.u32; }
    if (ctx.r6.u32 & 2) { HANDLE h = kobj_get(ctx.r5.u32); if (h) SetEvent(h); }
    net_start();
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NetDll_WSACreateEvent) {   // () -> manual-reset event
    PPC_FUNC_PROLOGUE(); NET_LEGACY("WSACreateEvent");
    HANDLE h = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    ctx.r3.u32 = h ? kobj_new(h) : 0u;
}
PPC_FUNC(__imp__NetDll_WSACloseEvent) { PPC_FUNC_PROLOGUE(); NET_LEGACY("WSACloseEvent"); kobj_close(ctx.r3.u32); ctx.r3.u32 = 1; }
PPC_FUNC(__imp__NetDll_WSASetEvent) { PPC_FUNC_PROLOGUE(); NET_LEGACY("WSASetEvent"); HANDLE h = kobj_get(ctx.r3.u32); if (h) SetEvent(h); ctx.r3.u32 = h ? 1u : 0u; }
PPC_FUNC(__imp__NetDll_WSAResetEvent) { PPC_FUNC_PROLOGUE(); NET_LEGACY("WSAResetEvent"); HANDLE h = kobj_get(ctx.r3.u32); if (h) ResetEvent(h); ctx.r3.u32 = h ? 1u : 0u; }
// WSAWaitForMultipleEvents(n r3, events* r4, wait_all r5, timeout_ms r6, alertable r7) -> index, or 0x102 timeout.
PPC_FUNC(__imp__NetDll_WSAWaitForMultipleEvents) {
    PPC_FUNC_PROLOGUE(); NET_LEGACY("WSAWaitForMultipleEvents");
    uint32_t n = std::min(ctx.r3.u32, 64u);
    HANDLE hs[64]; uint32_t idx[64]; DWORD valid = 0;
    for (uint32_t i = 0; i < n && ngp(ctx.r4.u32); ++i) { HANDLE h = kobj_get(nr32(ctx.r4.u32 + i * 4)); if (h) { idx[valid] = i; hs[valid++] = h; } }
    if (!valid) { net_set_err(6); ctx.r3.u32 = 0xFFFFFFFFu; return; }
    BOOL all = ctx.r5.u32 != 0;
    DWORD ms = ctx.r6.u32 == 0xFFFFFFFFu ? INFINITE : ctx.r6.u32;
    DWORD r = WaitForMultipleObjects(valid, hs, all, 0);
    if (r == WAIT_TIMEOUT && ms != 0) { GilYield y; r = WaitForMultipleObjects(valid, hs, all, ms); }
    if (r == WAIT_TIMEOUT) { ctx.r3.u32 = 0x102; return; }
    uint32_t k = r - WAIT_OBJECT_0;
    ctx.r3.u32 = all ? 0u : (k < valid ? idx[k] : 0u);
}
