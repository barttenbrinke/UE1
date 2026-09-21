/*============================================================================
	UnSocket.h: Common interface for WinSock and BSD sockets.

	Revision history:
		* Created by Mike Danylchuk
============================================================================*/

/*-----------------------------------------------------------------------------
	Definitions.
-----------------------------------------------------------------------------*/

#ifdef PLATFORM_WIN32
typedef int socklen_t;
#else
typedef int SOCKET;
typedef struct sockaddr SOCKADDR;
typedef struct sockaddr_in SOCKADDR_IN;
typedef struct hostent HOSTENT;
typedef struct linger LINGER;
typedef struct sockaddr* LPSOCKADDR;
typedef struct hostent* LPHOSTENT;
typedef char* LPSTR;
#endif

#ifdef PLATFORM_WIN32
#define IPBYTE(A, N) A.S_un.S_un_b.s_b##N
#else
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAENOTSOCK ENOTSOCK
#define WSAEISCONN EISCONN
#define WSATRY_AGAIN TRY_AGAIN
#define WSAHOST_NOT_FOUND HOST_NOT_FOUND
#define WSANO_DATA NO_ADDRESS
#define closesocket close
#if defined(PLATFORM_PSVITA) || defined(__PSP__)
// this is only used for FIONBIO
#define ioctlsocket( fd, opt, arg ) setsockopt( (fd), SOL_SOCKET, SO_NONBLOCK, (const void*)(arg), sizeof(*(arg)) )
#else
#define ioctlsocket ioctl
#endif
#ifdef __PSP__
//
// PSP: networking is compiled in but deliberately inert.
//
// IpDrv has to be built -- Entry.unr (the menu map) imports it, and without it
// the engine dies with "Can't find file 'Entry'". But the PSP network stack
// must be brought up explicitly (sceUtilityLoadNetModule -> sceNetInit ->
// sceNetInetInit) before ANY socket call, because libcglue's socket()/bind()/
// connect() go straight through to sceNetInet*. Calling them without that init
// is a kernel fault that REBOOTS the console.
//
// GInitialized alone is not enough: it only guards UdpLink::BindPort, while
// TcpLink.cpp and IpDrv.cpp call socket()/gethostbyname()/closesocket()
// unguarded. So make the operations themselves inert here. The classes still
// register, so map loading works; every socket op just reports failure, which
// the engine already handles as "no network available".
//
// These are macros, and this header is included after the system socket
// headers, so the real declarations are untouched. Note closesocket is
// #defined to close() above -- that must NOT stay, since close() is also the
// file-descriptor close used everywhere else.
//
#undef  closesocket
#define closesocket( fd )                   ( 0 )
#undef  ioctlsocket
#define ioctlsocket( fd, opt, arg )         ( -1 )
#define socket( af, type, proto )           ( -1 )
#define bind( fd, addr, len )               ( -1 )
#define connect( fd, addr, len )            ( -1 )
#define listen( fd, backlog )               ( -1 )
#define accept( fd, addr, len )             ( -1 )
#define setsockopt( a, b, c, d, e )         ( -1 )
#define getsockopt( a, b, c, d, e )         ( -1 )
#define send( a, b, c, d )                  ( -1 )
#define recv( a, b, c, d )                  ( -1 )
#define sendto( a, b, c, d, e, f )          ( -1 )
#define recvfrom( a, b, c, d, e, f )        ( -1 )
#define gethostbyname( name )               ( (HOSTENT*)0 )
// inet_addr also goes through libcglue to sceNetInetInetAddr, so it is a kernel
// call too despite looking like pure string parsing.
#define inet_addr( str )                    ( INADDR_NONE )
#define gethostname( name, len )            ( -1 )
#endif // __PSP__

#define WSAGetLastError() errno
#ifndef INADDR_NONE
// newlib on PSP does not define this.
#define INADDR_NONE ((unsigned long)0xffffffff)
#endif
#define IPBYTE(A, N) ((BYTE*)&A.s_addr)[N-1]
#endif

/*----------------------------------------------------------------------------
	Functions.
----------------------------------------------------------------------------*/

UBOOL InitSockets( char* Error256 );
const char* SocketError( INT Code=-1 );
UBOOL IpMatches( sockaddr_in& A, sockaddr_in& B );
void IpGetInt( in_addr Addr, DWORD& Ip );
void IpSetInt( in_addr& Addr, DWORD Ip );

/*----------------------------------------------------------------------------
	The End.
----------------------------------------------------------------------------*/
