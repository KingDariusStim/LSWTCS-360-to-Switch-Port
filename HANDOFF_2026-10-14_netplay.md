# HANDOFF 2026-10-14: network multiplayer layer (LAN / direct-IP), Switch account = Xbox Live account

Follows `HANDOFF_2026-10-09_netcoop.md` (survey, hazards, plan). This session built the network translation layer.
**Nothing has been tested with two machines yet.** Builds: PC and Switch NRO compile and link; PC boot with
`LSWTCS_NET=1` matches `LSWTCS_NET=0` in trace.log as far as the trace goes (only timing noise differs).

## 1. Files
- `LSWTCSRuntime/source/lsw_net.inc`: guest side, included at the end of `kernel_stubs.cpp` (it uses kobj_*,
  GilYield, g_phys_alloc, xam_ovl_return). It replaces 75 old stubs: every `NetDll_*`, the `XamUser*` identity,
  privilege and friends calls, `XamSession*`, `XNetLogon*`. XMsg (`XMsgStartIORequest[Ex]`, `XMsgInProcessCall`,
  `XMsgSystemProcessCall`) now goes through `lsw_net_xmsg` first.
- `LSWTCSRuntime/source/lsw_net_host.cpp`: host sockets (Winsock / libnx BSD) and the Switch account behind a small
  C API. It's the only file that includes those headers. Added to `switch/CMakeLists.txt`; the PC build globs it and
  links `ws2_32`.
- `XamContentGetCreator` now reports the signed-in user's XUID (was the constant `kLswXuid`), so saves stay "own".

## 2. Switches
| Env | Default | Meaning |
|---|---|---|
| `LSWTCS_SWITCHID` | 1 | Switch profile nickname = gamertag, XUID = `0x0009…` + hash of the account id. `0` = old "Player" / `0xB13EBABEBABEBABE`. PC: `LSWTCS_GAMERTAG` or "Player"; XUID from the name + `COMPUTERNAME` |
| `LSWTCS_NET` | 0 | 0 = old stubs, but every network import logs its first 4 calls as `[NET0]` with args and lr (N0 instrumentation). 1 = online |
| `LSWTCS_NET_PEERS` | – | `ip,ip,…` for play outside the LAN (unicast discovery; broadcasts sent by the game are copied to them too) |
| `LSWTCS_NET_IP` | auto | Address to advertise and bind (two PCs on one machine: 127.0.0.1 / 127.0.0.2 + PEERS) |
| `LSWTCS_NET_DISCPORT` | 3076 | Discovery UDP port |
| `LSWTCS_NET_XNSTATUS` | E6 | XNetGetTitleXnAddr status in online mode (ETHERNET\|STATIC\|GATEWAY\|DNS\|ONLINE) |
| `LSWTCS_NET_SEARCHMS` | 400 | How long a session search listens |
| `LSWTCS_NETLOG` | off | Log every socket call |

## 3. What online mode answers
- **Identity:** `XamUserGetSigninState(0)` = 2 (signed in to Live), `XamUserCheckPrivilege` writes TRUE,
  membership tier Gold (6), country 103, nobody is a friend, `XNetLogonGetMachineID` from the IP.
  Switch account lookup: preselected user (forwarder NACP StartupUserAccount = Required) → only user → last opened
  user; no picker applet mid-game. Nickname folded to ASCII, ≤ 15 characters.
- **Addresses:** XNADDR is derived from the IP alone (`net_make_xnaddr`: MAC 02:4C:ip, machine id 0xFA…|ip), so every
  peer computes the same XNADDR for a given IP. `XNetXnAddrToInAddr` is an identity mapping, connect status is always
  CONNECTED, and keys are random. There are no Microsoft services: DNS, server and service lookups fail.
- **Sockets:** real non-blocking host sockets. A blocking guest call waits under `GilYield`. VDP (254) = plain UDP
  (the guest's 2-byte header passes through). UDP sockets get SO_BROADCAST. Overlapped `WSARecv[From]` is completed by a
  worker thread, which also signals `WSAEventSelect` events. Winsock's UDP WSAECONNRESET is hidden.
- **Sessions (XGI 0xFB):** create (host → XNKID 0x80… for Live-flagged sessions, else 0x00…, our XNADDR, random key and
  nonce; join → host XNADDR from the discovery cache), delete, join/leave local+remote with slot accounting, start/end,
  modify, details (0x80 + members), migrate host, search / search-ex / weighted / by-id(s), arbitration (empty).
  Contexts and properties the host sets (0xB0006/7) travel with its beacon; `XPROPERTY_GAMER_HOSTNAME` is added.
  XLiveBase (0xFC): logon HR "connected", logon id, NAT open.
- **Discovery:** "LSWN" v1 on UDP 3076. A host session with HOST plus peer-network/matchmaking flags, open slots, and
  not (started + join-in-progress disabled) is beaconed every second to 255.255.255.255 + peers, and answers queries.
  Search = query + `LSWTCS_NET_SEARCHMS` wait + cache (entries < 4 s old).
- **QoS:** `XNetQosListen` data goes in the beacon. `XNetQosLookup` returns XNQOS with count_pending = n, completed by the
  worker 30 ms later (event set), rtt 15 ms.

## 4. Next steps (in order)
1. **Find the online menu path (N0).** Run with `LSWTCS_NET=0`, open the game's online menu, and read `[NET0]` lines in
   trace.log. Then do the same with `LSWTCS_NET=1` and read `[NET]` lines: which XSessionCreate flags, does it search,
   does it use QoS or its own broadcast. Today's PC boot calls only `WSAStartup` (lr 827D7178) and XNetGetTitleXnAddr.
2. **Two-machine test:** PC + PC (or PC + Switch) on one LAN, both `LSWTCS_NET=1`. Host on one, search on the other.
   Check `[NET] found session`, `search -> N`, Join, then gameplay packets (`LSWTCS_NETLOG=1`). Windows Firewall must allow
   UDP 3076 and the game's ports (see `[NET] bind … port`).
3. Internet: `LSWTCS_NET_PEERS=<other public IP>` both ways + port forwards, or a VPN (ZeroTier/Tailscale) where it
   behaves like a LAN.
4. Switch: put `LSWTCS_NET=1` in `sdmc:/lswtcs/env.txt`; the console must be on Wi-Fi (sockets go over nifm, not LDN).
   LDN (local wireless, no router) is still open: it can replace the discovery transport later (HANDOFF_2026-10-09 §5).
5. Same-console player 2 (P0 in the 2026-10-09 handoff) is not done: input still reports only user 0, and users 1–3
   are not signed in.

## 5. Known gaps / risks
- Guest `GetLastError()` after a socket call reads `t_net_err` only through `WSAGetLastError`; a title reading the
  KTHREAD error field directly would see 0.
- Session handles are never freed (`NtClose` does not know them); 4 KB guest page per session.
- XUSER_PROPERTY layout in search results (id, XUSER_DATA at +8, union at +0x10) is from the netplay fork, unverified
  in this game.
- Two instances on ONE PC share the fallback XUID unless their `LSWTCS_GAMERTAG`s differ.
- `LSWTCS_NET_XNSTATUS` E6 is new for this game's boot; old note: status 8 alone once stalled boot (with all-zero
  stubs). If online-mode boot stalls on hardware, try `LSWTCS_NET_XNSTATUS=1`.

## 6. Two-Switch test log (2026-10-14/15) — what was found and fixed
Consoles: A "julian" 192.168.0.6 (larger SD, host), B "Naimah" 192.168.0.10. env.txt on each has LSWTCS_NET=1 and
LSWTCS_NET_PEERS=<other IP>. Logs: switch/hwlogs/net*_dev*, join*_dev* (dev0 = A, dev1 = B).

The game does NOT use system link: its online lobby is the QNet library (sub_829Bxxxx..829Fxxxx) over Xbox Live
sessions, driven by the session manager (360SessionManager.cpp, sub_82493xxx..82497xxx). Fixed, in order:
1. Game UDP socket port 1001 -> EACCES on the Switch (ports < 1024): guest ports 1..1023 map to host +30000
   (LSWTCS_NET_LOWPORT), mapped back in every address the guest reads.
2. Network library never created (this+708 null): LibInit -> voice engine create (sub_82ADB058) failed:
   - XamUserReadProfileSettings size query must return 122 + size (was 0) — implemented (online mode only);
   - NtCreateEvent/Semaphore/Mutant/Timer/ExCreateThread dropped handles written to stacks >= 0x94000000 (main thread
     stack is 0xA111xxxx) — kout_ok() bound 0xE0000000 (LSWTCS_KOBJOUT=0 = old); NtDuplicateObject writes the handle.
3. "load failed": sync XMsg calls must set the guest thread last error (KTHREAD [r13+256]+352; game GetLastError =
   sub_822A3938); the join wrapper read a stale 183 -> FUNCTION_FAILED. guest_set_last_error() in XMsgStartIORequest[Ex].
4. "connection failed" (E_OUTOFMEMORY): ExAllocatePool*/XamAlloc were 0-returning stubs — real pool on g_phys_alloc
   (LSWTCS_EXPOOL=0 = old). XamVoiceCreate returns ACCESS_DENIED + handle 0 in online mode (Xenia).
5. Host crash at the first join packet (my bug): deferred WSARecvFrom completion wrote bytes/flags through the
   caller's dead stack pointers — now the result goes only to the overlapped; WSABUFs are copied at call time.
6. Host crash, still at the first join packet: QNet message pump (sub_829BF1D0) re-entered itself with r1 dropping
   ~0x90 per level; its dequeue (sub_829C0788 -> sub_829CB7A8) returned hr 0 WITHOUT a message and the pump dispatched
   on the word at address 4. Guard in main.cpp (LSWTCS_QNETGUARD): out slot zeroed before the call, "success without a
   message" reported as S_FALSE. With it A no longer crashes (final run).

State after the final run: B finds A ("search -> 1"), XSessionCreate(join)/JoinLocal succeed, B sends its 39-byte
join request (VDP, starts "00 25"); A receives and processes it (PktProcess -> 1) but NEVER SENDS A REPLY; the pump
keeps getting "success, no message" (299 of 300 verbose dequeues). B waits and times out. A's HUD shows the player-2
slot; A's captured frame is normal 16:9 1280x720 — the horizontal stretch the user sees is not in the rendered frame
(present_frame blits 1280x720 1:1 into the 1280x720 window), so it is downstream of the game (to be checked: photo of
the TV/handheld, nwindow crop / operation mode).

Next: find why sub_829CB7A8 returns 0 with no message after the join packet — sub_829C9478 (called when the event
queue is empty, returns 0 = "message ready") is the prime suspect; probe its out-msg and the event types it reads
(all probes log 300 lines after the first game packet: lsw_qnet_verbose). The join reply A should send is built in
that path. Diagnostics added this session (main.cpp QNET_* probes, [QNETLOG] game network log hook sub_82496F10,
[XSTUB] first-call lines, [NET] first <import> lines, packet hex dumps) can be trimmed once joining works.

### 6.1 Targeted run (2026-10-15, logs switch/hwlogs/join9_dev*)
- Stretch: the Switch album screenshot AND the game's own capture after the join both show a narrower horizontal field
  of view (doors further apart, door III off screen) with the HUD unchanged -> the game renders it (camera aspect
  changes when player 2 is being added), not the Switch output path. Album on console A: emuMMC/SD00/Nintendo/Album.
- Join stall: B's join packet -> one event type 8 on the socket object (event 0x84141108, +8 = socket obj). Type
  dispatch (byte table 0x820CF088, jump base 0x829CB818): 1 CB390, 2 C4928, 3 C4A20, 4 C4AE8, 5 C8A28, 6 CAAD0,
  7 CB6E0, 8 CAD10, 9 free. sub_829CAD10 (type 8 = new connection on the socket) NEVER RETURNS: inside it something
  re-enters the library DoWork (sub_829BF370 -> pump sub_829BF1D0 -> dequeue), which dequeues the same, still
  unconsumed type-8 event and calls CAD10 again, ~0x90 bytes deeper each level. The pump guard only hides the empty
  results. Next: find the call in CAD10 that re-enters DoWork (probe its callees for re-entry), or guard CAD10
  against re-entry on the same event (return "pending" while the outer call is active).

### 6.2 Root cause of the join stall: untranslated event-type switch in sub_829CB7A8 (fixed 2026-10-15, untested)
§6.1's "CAD10 re-enters DoWork" was wrong. The proof is `R1 MISMATCH StateUpdate: r1 835FF8B0 -> 835FF820`: the
dequeue sub_829CB7A8 returns with r1 0x90 (its own frame) lower than on normal calls (835FF730 vs 835FF7C0). Its
event-type switch is a byte-offset table (lbzx 0x820CF088, x4, + 0x829CB818). XenonAnalyse does not detect that
pattern, so XenonRecomp emitted `PPC_CALL_INDIRECT_FUNC(ctr); return;`. The case code then ran as a "function" on
the dequeue's frame, which was never popped. Every level is one more stack drop and pump pass on the same event. Type
8 (new connection) is the first event that ever went through this switch; earlier messages came from C9478.
Fix: tools/fix_byte_switch_829CB7A8.py (patches output/ppc_recomp.524.cpp, marker LSW_BYTESWITCH_FIX, re-run after a
regen): a switch on r0 to labels at the 9 case starts. An unknown offset keeps the old path. A scan of all generated
code found no other byte-table switch emitted as an indirect call. The re-entry guard was not built. QNETGUARD and
the probes stay until a join completes. The FOV change on join (§6.1) may be the same half-finished join.
- v1 of the patch (02:04) crashed A in the first join test (logs join10_dev*): the `ctx.r4 = r4;` sync sat only on the
  default path, so CAD10 read a stale event pointer and its final sub_829CDFC8 release ran an atomic on the misaligned
  0x841409CE+4 (stack: CAD10 -> CDFC8 -> D7008 -> D4B98 lwarx). The stack stayed level, so the switch fix works. v2
  syncs ctx.r4 before the switch (the script upgrades v1 in place). Pushed to both consoles 02:19, not tested yet.
- **2026-10-15: v2 tested on both Switches: online join works** (user: "everything works perfectly"). B joins A's
  session and play continues. Open items: trim the QNET_* probes, [QNETLOG] and packet dumps once joins have been
  stable for a while; decide whether LSWTCS_QNETGUARD stays (it was a crash guard for the leaked-frame symptom);
  internet play (§4.3) and same-console player 2 (§4.5) are untested.
