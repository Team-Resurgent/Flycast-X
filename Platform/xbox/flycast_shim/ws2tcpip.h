// flycast_shim/ws2tcpip.h -- desktop name resolution header.
// net_platform.h includes ONLY this on _WIN32, expecting it to pull in the
// sockets API too, so we route through our winsock2.h shim. addrinfo/getaddrinfo
// are declared for the files that reference them (compiled network code is
// stubbed and never actually resolves on Xbox).
#pragma once
#include <winsock2.h>   // flycast_shim -> WinSockX.h

#ifndef _FLY_WS2TCPIP_SHIM_EXTRA
#define _FLY_WS2TCPIP_SHIM_EXTRA

#ifndef _WS2TCPIP_ADDRINFO_DEFINED
#define _WS2TCPIP_ADDRINFO_DEFINED
struct addrinfo {
    int               ai_flags;
    int               ai_family;
    int               ai_socktype;
    int               ai_protocol;
    size_t            ai_addrlen;
    char*             ai_canonname;
    struct sockaddr*  ai_addr;
    struct addrinfo*  ai_next;
};
#endif

#ifdef __cplusplus
extern "C" {
#endif
int  getaddrinfo(const char* node, const char* service,
                 const struct addrinfo* hints, struct addrinfo** res);
void freeaddrinfo(struct addrinfo* res);
const char* gai_strerror(int ecode);
#ifdef __cplusplus
}
#endif

#endif // _FLY_WS2TCPIP_SHIM_EXTRA
