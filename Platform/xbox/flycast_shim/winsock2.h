// flycast_shim/winsock2.h -- map desktop <winsock2.h> to the Xbox WinSockX.
// The Xbox sockets API (SOCKET, sockaddr_in, setsockopt, ioctlsocket,
// INVALID_SOCKET, FIONBIO, TCP_NODELAY, SOL_SOCKET, WSAGetLastError, ...) lives
// in WinSockX.h. We add the few symbols Flycast's net_platform.h / multiboard.h
// reference that WinSockX.h lacks. Networking is stubbed on Xbox (xbox_host.cpp
// / xbox_crt_shim.cpp), so these only need to compile + link, never run.
#pragma once
#include <WinSockX.h>

#ifndef _FLY_WINSOCK_SHIM_EXTRA
#define _FLY_WINSOCK_SHIM_EXTRA

// WinSockX.h doesn't define the BSD socklen_t alias that Flycast's net code uses.
#ifndef _SOCKLEN_T_DEFINED
#define _SOCKLEN_T_DEFINED
typedef int socklen_t;
#endif

// getprotobyname() / protoent: referenced by net_platform.h set_tcp_nodelay()
// (only on _WIN32). Declared so the inline parses; a stub lives in xbox_host.cpp.
#ifndef _WINSOCK_PROTOENT_DEFINED
#define _WINSOCK_PROTOENT_DEFINED
struct protoent { char* p_name; char** p_aliases; short p_proto; };
#endif

#ifdef __cplusplus
extern "C" {
#endif
struct protoent* getprotobyname(const char* name);
#ifdef __cplusplus
}
#endif

// Winsock shutdown() how-values (net_platform.h maps SHUT_WR/SHUT_RD to these).
#ifndef SD_RECEIVE
#define SD_RECEIVE 0x00
#endif
#ifndef SD_SEND
#define SD_SEND    0x01
#endif
#ifndef SD_BOTH
#define SD_BOTH    0x02
#endif

#endif // _FLY_WINSOCK_SHIM_EXTRA
