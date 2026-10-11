# HANDOFF 2026-10-09: Xbox Live / network co-op — state of the stubs and what co-op would need

Audience: the next agent working on online / network co-op. Nothing here is implemented yet; this is a survey of
what the game uses, what our runtime currently answers, and a proposed path. Standing project rules apply
(see `HANDOFF_2026-10-08_switch.md` §4–§6; `EXPERIMENTAL_REGEN/` is one-way; ask before SD-card copies).

## 1. Summary

- The game has **full online co-op built in**: Microsoft's QNet library (Xbox 360 "QuickNet") on top of Xbox Live
  sessions — host, search, lobby, join-in-progress, invites, peers joining/leaving, game packets, voice.
- Our runtime answers almost every network call with **"success, nothing happened"** (`return 0`). Single-player
  never touches the network code, so this is harmless today. In all our logs (PC, Eden, hardware) the only Live
  message ever sent is `0xFB/0x000B0006` (XGIUserSetContext = rich presence).
- Several of those "success" answers are wrong in ways that would hang or confuse the game if the online menu
  were opened (§4). First milestone should be making the online path fail cleanly, before trying real play.
- **Decided direction (user, 2026-10-09): use what the Switch already has, do not rebuild Xbox Live.** The game's
  session/socket calls must still be answered (game code makes them), but only by a thin translation layer onto
  the Switch's own local-wireless stack (LDN for host/scan/join, BSD sockets for the data). See §5.
- **Identity (user, 2026-10-09): the Switch's current profile nickname is the player's gamertag**, and the Switch
  account gives a stable XUID. See §7.
- Cheapest co-op win first: the game's built-in **same-console drop-in co-op** (2nd controller = player 2) does not
  work today because our input layer only ever reports user 0 (§5, P0).
- Never connect to real Microsoft services. Xenia Canary's **netplay fork** (AdrianCassar) is only a reference for
  the session message structure layouts.

## 2. What the game uses (evidence)

### 2.1 Strings in the executable (`Convert 360/LSWTCS/image.bin`, offsets from `strings -t x`)
- `.\360\360SessionManager.cpp` (0x30068), QNet state names `QNET_STATE_IDLE … SESSION_HOSTING / JOINING /
  GAME_LOBBY / SESSION_REGISTERING / SESSION_STARTING / GAME_PLAY / SESSION_ENDING / LEAVING / DELETING`.
- `Hosting game`, `SearchForHosts() called`, `Create game search`, `Game search 0x%p completed successfully, %u results`,
  `JoinGameFromSearchResult returned %x`, `NotifyGameInvite`, `Set join in progress to %d`,
  `SetHostVisibility() - visible all/none - public %d private %d`, `NotifyNewHost`, `Host has left the game.`,
  `Adding peer %s and calling PeerJoined`, `NotifyPlayerJoined %s, IsLocal = %d`, `NotifyPlayerLeaving %s`,
  `XONLINE_E_SESSION_FULL`, `ERROR_NOT_LOGGED_ON`, `networklog%s.txt`.
- Packet classes: `PlayerPacket`, `CoinPacket`, `TorpedoPacket`, `TagPacket`, `ContextPacket`; stats
  `Held/Split Packets`, `Ping Time`, `Out Queue`, `Send Fails`; voice: `Voice packet post failed`.
- Game-side: `NetworkGameOnGoing`, `Network_Icon`, `network_door`, `door_to_network`, `Game Network`,
  `{Connect}` / `{Disconnect}`, `Carry Co-op`. (Menu text lives in GAME.DAT localisation, not searched yet.)
- Also present: Live web-service paths (`/xmatch/xmatchhost2.ashx`, `/xuacs/...`) — part of the linked SDK, not used
  directly by game code.

### 2.2 Imports used by game code (grep of `__imp__` in `Convert 360/LSWTCS/output`)
- ~60 `NetDll_*`: BSD-style sockets (`socket bind connect listen accept send sendto recv recvfrom select
  setsockopt getsockopt ioctlsocket getsockname getpeername shutdown closesocket inet_addr __WSAFDIsSet`),
  WSA overlapped I/O and events (`WSASend/Recv[From/To] WSACreateEvent WSAEventSelect WSAWaitForMultipleEvents
  WSAGetOverlappedResult …`), and XNet (`XNetStartup/Cleanup XNetGetTitleXnAddr XNetXnAddrToInAddr
  XNetInAddrToXnAddr XNetRegisterKey/UnregisterKey XNetCreateKey XNetConnect XNetGetConnectStatus
  XNetQosListen/Lookup/Release/ServiceLookup/GetListenStats XNetDnsLookup XNetRandom XNetGetEthernetLinkStatus …`).
- XAM: `XamSessionCreateHandle`, `XamSessionRefObjByHandle` (15 sites), `XamUserGetSigninState`, `XamUserGetXUID`,
  `XamUserCheckPrivilege`, `XamUserAreUsersFriends`, `XamUserGetMembershipTierFromXUID`,
  `XamUserGetOnlineCountryFromXUID`, `XamShowSigninUI[p]`, `XamShowFriendsUI`, `XamShowGamerCardUIForXUID`,
  `XamVoice*`, `XNetLogonGetTitleID/MachineID`, `XNotify*`.
- Sockets opened (`NetDll_socket` wrapper `sub_827D71A0`, callers): `AF_INET, SOCK_DGRAM`, protocol **17 (UDP)** in
  `sub_82494160` (game code) and `sub_829C3500`; protocol **254 (IPPROTO_VDP)** in `sub_829D0CC0` (QNet).
  VDP = Xbox "voice/data protocol": UDP payload with a 2-byte game-data length header; game data + voice.

### 2.3 XMsg messages the code can send (SDK wrappers, extracted from `lis r4 / ori r4 / li r3` before each call)

App `0xFB` = XGI (users, sessions, stats). Names from `xenia-canary/src/xenia/kernel/xam/apps/xgi_app.cc` where
it has them; "?" = not named there (check the netplay fork).

| Wrapper | Message | Name |
|---|---|---|
| sub_825355B8 | 0x000B0006 | XGIUserSetContext (seen in every run) |
| sub_82535658 | 0x000B0007 | XGIUserSetPropertyEx |
| sub_82535530 | 0x000B0008 | XGIUserWriteAchievements |
| sub_825357A8 / sub_82535838 | 0x000B0020 / 0x000B0021 | ? (stats) |
| sub_829F3D78 | 0x000B0010 | XSessionCreate |
| sub_829F3FD0 | 0x000B0011 | XGISessionDelete |
| sub_829F4080 / sub_829F4128 | 0x000B0012 | XGISessionJoinLocal |
| sub_829F41D0 / sub_829F4278 | 0x000B0013 | ? (JoinRemote / leave) |
| sub_829F43F8 | 0x000B0014 | XSessionStart |
| sub_829F4490 | 0x000B0015 | XSessionEnd |
| sub_829F4540 | 0x000B0016 | ? |
| sub_829F3F18 | 0x000B0018 | ? (modify) |
| sub_829F4800 (InProcessCall) | 0x000B0019 | ? |
| sub_829F4320 | 0x000B001A | ? (arbitration) |
| sub_829F4720 / sub_829F4630 | 0x000B001B / 0x000B001C | ? (search) |
| sub_8280E4A0 | 0x000B001D | ? |
| sub_829F4A90 | 0x000B001E | ? (details) |
| sub_829F4980 / sub_829F4860 | 0x000B0025 / 0x000B0026 | ? (write / flush stats) |

App `0xFC` = XLiveBase (`xlivebase_app.cc`): `0x00058004` XLiveBaseGetLogonId, `0x00058006` GetNatType,
`0x00058007` OnlineGetServiceInfo, also `0x0005800E 0x00058019 0x0005801E 0x00058020 0x00058023 0x00058044
0x00058046` (InProcessCall, `sub_8280C100…sub_8280E280`, `sub_829F4A40`). App `0xFE` = XAM content
(`0x0002000B`, `0x0002000D`). App `0xFA` = XMP music player (`0x0007xxxx`, unrelated to networking).

## 3. What our runtime answers today (`LSWTCSRuntime/source/kernel_stubs.cpp`, main folder)

| Function(s) | Line | Current behaviour |
|---|---|---|
| `XMsgStartIORequest[Ex]` | ~2411, ~5872 | Logs first 12; **completes every request inline as success** (`XOVERLAPPED` result 0, length 0, event set) whatever the message. |
| `XMsgInProcessCall`, `XMsgSystemProcessCall` | ~5456, ~5880 | return 0, output untouched |
| `XMsgCancelIORequest` | ~2419 | return 0 |
| `XamSessionCreateHandle`, `XamSessionRefObjByHandle` | ~2391 | return 0, **no handle / object written** |
| `XamUserGetSigninState` | ~2376 | user 0 = 1 (signed in locally), others 0. Never 2 (Live). |
| `XamUserGetXUID` / `XamUserGetName` | ~5897 / ~6184 | user 0: XUID `0xB13EBABEBABEBABE`, name "Player"; others: no such user |
| `XamUserCheckPrivilege`, `XamUserAreUsersFriends`, `…MembershipTier…`, `…OnlineCountry…` | ~2388 | return 0, out-params untouched |
| `XamShowSigninUI[p]`, `XamShowFriendsUI`, `XamShowGamerCardUIForXUID` | ~6473–6489 | return 0, no UI, no notification |
| `XamVoice*` | ~2265 | return 0 (no headset) |
| `NetDll_XNetGetTitleXnAddr` | ~5545 | `LSWTCS_XNET` (default 1): STATIC with fake XNADDR `192.168.1.100`, MAC `00:22:48:01:02:03` — needed to get past boot (frontend polled forever on PENDING; offline (8) stalled) |
| every other `NetDll_*` (sockets, WSA, XNet, QoS, DNS) | ~5499–5646 | `return 0`, nothing written |
| `XNetLogonGetTitleID/MachineID` | ~5887 | return 0 |

## 4. Hazards if the online path is entered with these stubs

- `NetDll_socket` returns **0, which is a valid socket** (INVALID_SOCKET is -1): the game believes it has sockets.
- `recv`/`recvfrom` return 0 (= peer closed); `send`/`sendto` return 0 bytes; `select` returns 0 and writes no
  fd_sets; `WSAGetLastError` always 0; `WSACreateEvent` returns handle 0.
- `XSessionCreate` / search / join complete as success with **no data written** (no session nonce/handle, search
  result count left as garbage) → the QNet state machine may advance with uninitialised fields or wait forever.
- `XamSessionCreateHandle` reports success without a handle; `XamSessionRefObjByHandle` without an object
  (15 call sites dereference it).
- `XamUserCheckPrivilege` returns success but never writes the BOOL result (the game reads stack garbage:
  multiplayer may look allowed or denied at random).
- `XamShowSigninUI` succeeds without the XN_SYS_UI open/close notifications; the device-selector fix (§18 of the
  old handoffs) showed this game waits for those.

## 5. Plan (revised 2026-10-09: Switch-native, no Xbox Live rebuild)

### 5.1 Options considered

| Option | Verdict |
|---|---|
| **P0 Same-console co-op** (the game's built-in drop-in player 2) | **Do first.** Cheapest, no networking. Broken today: input reports only user 0. |
| **P1–P4 Two Switches over LDN** (Nintendo's local wireless): LDN for host/scan/join, BSD sockets over the LDN network for game data | **Chosen** for multi-console co-op. Maps 1:1 onto what the game asks for. |
| Same layer over plain IP (LAN discovery / direct IP), incl. internet via LAN-play tunnels | Later, reuses P1–P4. |
| Rebuild an Xbox Live-style service (Xenia netplay approach: REST server for sessions) | Rejected: more work, a server to run. |
| Lockstep input sync between two game instances | Rejected: the engine is not deterministic (threads, frame-time capping). |

Mapping of what the game calls onto the Switch:

| Game asks for | Switch side |
|---|---|
| `XSessionCreate` (0xFB/0x000B0010), host visible | `ldnCreateNetwork` (we are the access point); session info in the LDN advertise data |
| Game search (0x000B001B/1C, QoS lookup) | `ldnScan`; each found network = one search result (host gamertag, slots, XNKID from advertise data) |
| Join (`JoinGameFromSearchResult`, 0x000B0012/13) | `ldnConnect` |
| Start / end / leave / delete (0x000B0014/15/11) | local session state; leave = `ldnDisconnect` / `ldnDestroyNetwork` |
| XNADDR / XNKID / XNKEY, `XNetRegisterKey`, `XNetConnect` | XNADDR.ina = our LDN IPv4; keys = random bytes; register/connect = no-ops (LDN is already encrypted) |
| `XNetXnAddrToInAddr` / `XNetInAddrToXnAddr` | identity mapping onto LDN IPv4 addresses |
| UDP (17) and VDP (254) sockets | BSD UDP sockets bound to the LDN interface; VDP = UDP with the 2-byte game-data length header |
| `XNetQos*` (latency probes) | answer locally from LDN link info (or a ping over the socket) |

Known homebrew paths that make this testable:
- libnx `services/ldn.h` (`ldnCreateNetwork`, `ldnScan`, `ldnConnect`, states AccessPoint/Station) — LDN is the
  same service retail Switch games use for local wireless. `local_communication_id = -1` takes the id from our NACP
  (`switch/CMakeLists.txt` nacp): set one there. `services/lp2p.h` also exists (not needed).
- Atmosphère **ldn_mitm**: carries LDN over normal Wi-Fi/LAN, so the same code works console↔console on a home network.
- **Eden's multiplayer rooms** emulate LDN: two Eden instances on the PC = first test bench, before hardware.
- Xenia Canary netplay fork: XSESSION_INFO / XSESSION_SEARCHRESULT / XNADDR / XNKID layouts.

### 5.2 Milestones

- **P0 Same-console co-op.** Switch: `padConfigureInput(1, …)` in `switch/source/win32_switch.cpp` → up to 4 pads
  (`HidNpadIdType_No1..No4` + handheld for No1), one `PadState` per player. Guest: `XamInputGetCapabilities` /
  `XamInputGetState` / `XamInputSetState` in `kernel_stubs.cpp` must answer users 1–3 when a pad is connected
  (today any user != 0 is DEVICE_NOT_CONNECTED). Also `XamUserGetSigninState` / `XamUserGetXUID` /
  `XamUserGetName` for user 1 if the game requires a signed-in profile for player 2 (check: does the HUD's
  "Insert controller" slot accept a pad without a profile?). PC side: the XInput slot loop already exists; extend
  `lsw_host_pad` to return per-user state. Test on PC with two pads, then Eden (two controllers), then hardware.
- **N0 Instrument.** Log every `NetDll_*`, `XamSession*`, `XamUser*Privilege`, XMsg app 0xFB/0xFC call with args
  and caller lr (most are silent today). Find the online menu: Ghidra oracle (`ghidra_360_oracle` memory) xrefs to
  `SearchForHosts() called` / `Hosting game` / `Create game search`, and the GAME.DAT menu text. **Answer first:**
  does the game offer System Link (LAN) or only Xbox Live? If Live-only, the layer must report signed in to Live
  (`XamUserGetSigninState` = 2, `XamUserCheckPrivilege` multiplayer = TRUE) — see §7.
- **N1 Fail cleanly.** Until P1 works, the online menu must report "no network" instead of hanging:
  `XamUserCheckPrivilege` writes FALSE, `NetDll_socket` returns -1 + `WSAGetLastError` = WSAENETDOWN, session
  messages complete with `ERROR_NOT_LOGGED_ON` in the XOVERLAPPED. Boot must keep the STATIC XNADDR answer (§3).
- **P1 Sockets.** `NetDll_*` → libnx BSD sockets (`socketInitializeDefault()`; PC build: Winsock for the bench).
  Big-endian sockaddr / fd_set marshalling; non-blocking only (a blocking call under the GIL scheduler stalls every
  guest thread; wrap any wait with `lswtcs_gil_blocking`). WSA events → host events via `kobj_*`.
- **P2 LDN sessions.** Host = `ldnCreateNetwork` + advertise data {magic, version, host gamertag (§7), XNKID, open
  slots}; search = `ldnScan` → XSESSION_SEARCHRESULT list, completed asynchronously through the XOVERLAPPED (not
  inline: the game polls/waits on it); join = `ldnConnect`, XNADDR = LDN IPv4. Test: two Eden instances in an Eden
  multiplayer room.
- **P3 Two consoles.** Switch↔Switch over LDN (no router), then ldn_mitm over LAN.
- **P4 (optional) IP mode.** Same session layer over plain IP (broadcast discovery / direct IP in `env.txt`) for
  PC↔Switch and internet play via tunnels.

## 6. Notes / constraints

- Keep every change behind an env switch with the current behaviour as `=0` (project convention).
- The GIL scheduler is temporary (`switch_multithreading_constraint` memory); network threads must not hold the
  GIL while blocked.
- The tree was promoted 2026-10-09: the registers-as-locals build is now main (see `HANDOFF_2026-10-09_regen.md`
  top note); `kernel_stubs.cpp` in main now carries the `r2` guards and CS diagnostics. Edit it in place.
- Never contact real Microsoft / Xbox Live services.

## 7. Identity: the Switch profile is the Xbox Live account (user decision 2026-10-09)

The player's Switch profile nickname is used wherever the game shows or sends a gamertag (HUD, "NotifyPlayerJoined
%s", search results, LDN advertise data), and the Switch account gives a stable XUID.

- **Which profile** (libnx `services/acc.h`, `accountInitialize(AccountServiceType_Application)`), in order:
  1. `accountGetPreselectedUser` — set when the NRO is launched through a title override / forwarder with a user
     picked by the system;
  2. `accountTrySelectUserWithoutInteraction(…, false)` — succeeds when the console has exactly one user;
  3. `pselShowUserSelector` (applets/psel.h) — the system's own "who is playing?" picker, once at boot;
  4. fallback: `accountGetLastOpenedUser`, then the name "Player".
  Note: launched from hbmenu in applet (album) mode, some of these are unavailable; the title-override launch is
  the normal case.
- **Name:** `accountGetProfile` → `accountProfileGet` → `AccountProfileBase.nickname` (UTF-8, max 10 characters on the
  Switch). Xbox gamertags are ≤ 15 characters and the game's font may lack some glyphs: keep ASCII as is, map
  common accented letters to ASCII, replace anything else with `?` (check which glyphs the game font has).
- **XUID:** stable per Switch account: `0x0009000000000000 | (XXH3(AccountUid) & 0x0000FFFFFFFFFFFF)` (the 0x0009
  prefix is the shape of online XUIDs; keep the current offline `0xB13EBABEBABEBABE` for `LSWTCS_SWITCHID=0`).
  Distinct per console/profile, which session join and "IsLocal" checks need.
- **Where it plugs in** (`kernel_stubs.cpp`): `XamUserGetName` (today "Player"), `XamUserGetXUID`,
  `XamUserGetSigninState` (1 → 2 once online play is wanted, see N0), `XamUserReadProfileSettings` if the game reads
  the gamertag setting there, and the session layer's advertise / search-result data. Resolve once at boot in
  `switch/source/platform_switch.cpp` and hand the result to kernel_stubs (PC build: keep "Player" or read
  `LSWTCS_GAMERTAG`).
- **Same-console player 2 (P0):** if the game needs a profile for player 2, use the Switch profile chosen for that
  controller when the system offers one (`pselShowUserSelector` again), otherwise "Player 2" with a derived XUID.
- The save folder already uses `saves/00000001/` (user index, not XUID): unchanged by this.

## 8. Packaging: NRO vs installable title (NSP) vs forwarder (discussed 2026-10-09, nothing built)

Today: `E:/switch/lswtcs.nro`, launched with full memory (hardware log: TotalMemory 3189 MB = title-override /
application mode). Options:

| | NRO (today) | Full NSP (program installed) | **NSP forwarder** (recommended) |
|---|---|---|---|
| Home-menu icon, no hbmenu / title override | no | yes | yes |
| Full application memory every launch | only via title override | yes | yes |
| System "who is playing?" picker at launch (NACP StartupUserAccount) → `accountGetPreselectedUser` works (§7) | no | yes | yes |
| LDN `local_communication_id` from our own NACP (§5) | NRO NACP | yes | yes |
| New build = copy one file | yes | no: repack + reinstall each build | yes (forwarder loads the NRO from SD) |
| nxlink / quick test | yes | no | NRO still usable |
| Needs Atmosphère sigpatches (update them after every firmware/Atmosphère update) | no | yes | yes |
| Needs the user's own console keys to pack | no | yes (hacBrewPack-style tools) | yes, once |

Rules if this is done: never put game.DAT / game data into the package (5 GB installs, and a redistributable copy
of game data): it stays on sdmc:/lswtcs. Use emuMMC only. Never read, print or copy key contents (memory
`eden_emulator`). Choose a fixed, unused homebrew title ID (the 0x01xxxxxxxxxxxxxx homebrew range, checked against
known titles) and keep it forever: save data and the LDN communication id depend on it. Set NACP
StartupUserAccount = Required so §7 gets the profile from the system picker.

**Update 2026-10-09 (later): forwarder BUILT and on the SD card.** sigpatches do not work on the user's Atmosphère,
but sys-patch is installed (atmosphere/contents/420000000000000B, fs/ldr/es patches on emuMMC), so a self-built
NSP works. `switch/forwarder/`: build_forwarder.sh (patched switchbrew nx-hbloader: starts sdmc:/lswtcs/lswtcs.nro,
exits to Home when the game quits, 39-bit address space), pkg/control (NACP StartupUserAccount=Required, LDN id =
save owner = 054C535443530000, icon from LSWTCSPortMod/icon/icon.jpeg in all 12 languages), hbp_build/hacbrewpack.exe
(built from the rlaphoenix mirror; Eden's prod.keys passed by path only). SD: game at `E:/lswtcs/lswtcs.nro`
(not listed by hbmenu), installer file `E:/LSWTCS_forwarder_054C535443530000.nsp`, old `E:/switch/lswtcs.nro`
deleted at the user's request. Copy new builds to E:/lswtcs/lswtcs.nro from now on.
**Fix (same day):** the first NSP failed on the console ("an error occurred") and in Eden (`Failed to load ROM (Error
32)` = unable to parse kernel metadata). Cause: hbl.json is made for the Album applet. build_forwarder.sh now sets
`application_type` 1 and `debug_flags.force_debug` false (map_region kept). Fixed NSP boots in Eden with full memory
(3285 MB); E:/LSWTCS_forwarder_054C535443530000.nsp must be replaced with switch/forwarder/pkg/nsp/054c535443530000.nsp.

## 9. Final plan (wrapped up 2026-10-09): order of work, concrete changes, tests

Order: **P0 same-console co-op → N0 instrument the online menu → N1 fail cleanly → identity (§7) → P1 sockets →
P2 LDN sessions (Eden rooms) → P3 two consoles → P4 optional IP mode.** Each step is shippable on its own.

### 9.1 P0 — player 2 on one Switch (no networking)
Current code (all single-player only):
- `switch/source/win32_switch.cpp` `nx_xinput_get_state(slot)`: `slot != 0` → not connected; one
  `padConfigureInput(1, HidNpadStyleSet_NpadStandard)` + `padInitializeDefault` (player 1 + handheld).
- `LSWTCSRuntime/source/kernel_stubs.cpp`: `XamInputGetState` (~6412) and `XamInputGetCapabilities` (~6350) return
  DEVICE_NOT_CONNECTED for every user != 0; `lsw_host_pad()` (PC) polls the *first* connected XInput slot for user 0.
Changes:
1. Switch: `padConfigureInput(4, HidNpadStyleSet_NpadStandard)`; `PadState g_pads[4]`, `padInitialize(&g_pads[0],
   HidNpadIdType_No1, HidNpadIdType_Handheld)`, `padInitialize(&g_pads[i], HidNpadIdType_No1 + i)` for i = 1..3;
   `nx_xinput_get_state(slot)` = that pad, `padIsConnected` → ERROR_DEVICE_NOT_CONNECTED. Single Joy-Con
   (sideways) per player: `hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal)` + rotate the stick/button
   mapping for `HidNpadStyleTag_NpadJoyLeft/Right`. Optional: offer the system controller applet
   (`applets/hid_la.h`, `hidLaShowControllerSupport`) from a hotkey so players can pair/order controllers.
2. kernel_stubs: `lsw_host_pad(user, out)` per user (PC: user i = XInput slot i; Switch: pad i).
   `XamInputGetState/GetCapabilities/SetState` answer users 0–3 when that pad is connected. Keyboard, press_now and
   AUTOSTART stay on user 0 only.
3. Hot-plug: broadcast `XN_SYS_INPUTDEVICESCHANGED` (check the id in Xenia `xam_notify`/`kernel_state.cc`) through
   the existing XNotify listeners when a pad connects/disconnects, in case the game waits for it.
4. Find out (N0-style logging of `XamUserGetSigninState(1)`, `XamUserGetXUID(1)`) whether drop-in player 2 needs a
   signed-in profile; if so give user 1 a local profile ("Player 2", derived XUID) — §7 last bullet.
Tests: PC with two pads → Eden with player 2 configured in Eden's input settings → hardware (Joy-Con pair split,
or Pro Controller + Joy-Cons). Check: "Insert controller" slot fills, P2 moves independently, P2 leaves cleanly,
save/load unaffected.

### 9.2 N0/N1 — online menu
Instrument as in §5.2 N0, then make it fail cleanly (N1) so the online menu cannot hang a player who opens it.
Ship N1 before P1 starts.

### 9.3 Identity (§7) — independent of P1/P2
Resolve the Switch profile at boot (platform_switch.cpp): `accountGetPreselectedUser` (works now: the forwarder's
NACP has StartupUserAccount = Required, so the system picker runs before the game) → nickname → gamertag, account
uid → XUID. Can ship with P0 (the HUD/pause menu show the name).

### 9.4 P1–P3 — LDN co-op
As §5.1/§5.2: BSD sockets for `NetDll_*`, XSession messages → LDN create/scan/connect, XNADDR = LDN IPv4,
`local_communication_id` = 0x054C535443530000 (already in the forwarder NACP; LDN uses the NACP of the running
title, so both consoles must run the forwarder, not a raw NRO). Test bench: two Eden instances in an Eden
multiplayer room (Eden must load the forwarder NSP so it has the NACP), then two consoles, then ldn_mitm.

### 9.5 Open questions to answer early
- Does the game's online menu offer System Link / LAN, or Xbox Live only? (decides SigninState 1 vs 2 handling)
- Does player 2 need a profile? (P0 step 4)
- Does the game's font have glyphs for non-ASCII Switch nicknames? (§7)
