/*
 * Copyright (c) 2008 CO-CONV, Corp. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#if !defined(__Windows__)
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#else
#include <winsock2.h>
#include <mswsock.h>
#include <WS2tcpip.h>
#include <WS2sctp.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CLIENTS	32

#if defined(__Windows__)

#include <tchar.h>

void
err(
    int eval,
    const TCHAR *fmt,
    ...)
{
	va_list ap;
	LPVOID lpMsgBuf;

	va_start(ap, fmt);
	_vftprintf(stderr, fmt, ap);
	va_end(ap);

	FormatMessageA(
	    FORMAT_MESSAGE_ALLOCATE_BUFFER | 
	    FORMAT_MESSAGE_FROM_SYSTEM,
	    NULL,
	    WSAGetLastError(),
	    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
	    (LPSTR)&lpMsgBuf,
	    0, NULL );
	fprintf(stderr, ": %s", lpMsgBuf);

	exit(eval);
}

void
errx(
    int eval,
    const TCHAR *fmt,
    ...)
{
	va_list ap;

	va_start(ap, fmt);
	_vftprintf(stderr, fmt, ap);
	va_end(ap);

	exit(eval);
}

void
warn(
    const TCHAR *fmt,
    ...)
{
	va_list ap;
	LPVOID lpMsgBuf;

	va_start(ap, fmt);
	_vftprintf(stderr, fmt, ap);
	va_end(ap);

	FormatMessageA(
	    FORMAT_MESSAGE_ALLOCATE_BUFFER | 
	    FORMAT_MESSAGE_FROM_SYSTEM,
	    NULL,
	    WSAGetLastError(),
	    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
	    (LPSTR)&lpMsgBuf,
	    0, NULL );
	fprintf(stderr, ": %s", lpMsgBuf);
	LocalFree(lpMsgBuf);
}

#if (_WIN32_WINNT <= 0x0501)
WINSOCK_API_LINKAGE
INT
WSAAPI
GetAddrInfoW(
    IN PCWSTR pNodeName OPTIONAL,
    IN PCWSTR pServiceName OPTIONAL,
    IN const ADDRINFOW *pHints OPTIONAL,
    OUT PADDRINFOW *ppResult
    );
#ifdef UNICODE
#define GetAddrInfo     GetAddrInfoW
#else
#define GetAddrInfo     getaddrinfo
#endif

WINSOCK_API_LINKAGE
VOID
WSAAPI
FreeAddrInfoW(
    IN PADDRINFOW pAddrInfo OPTIONAL
    );

#ifdef UNICODE
#define FreeAddrInfo    FreeAddrInfoW
#else
#define FreeAddrInfo    freeaddrinfo
#endif

WINSOCK_API_LINKAGE
INT
WSAAPI
GetNameInfoW(
    IN const SOCKADDR *pSockaddr,
    IN socklen_t SockaddrLength,
    OUT PWCHAR pNodeBuffer OPTIONAL,
    IN DWORD NodeBufferSize,
    OUT PWCHAR pServiceBuffer OPTIONAL,
    DWORD ServiceBufferSize,
    IN INT Flags
    );

#ifdef UNICODE
#define GetNameInfo     GetNameInfoW
#else
#define GetNameInfo     getnameinfo
#endif
#endif

#else
typedef char TCHAR;
typedef struct addrinfo ADDRINFOT;
#define _T(str)		str
#define GetAddrInfo	getaddrinfo
#define FreeAddrInfo	freeaddrinfo
#define GetNameInfo	getnameinfo
#define _ftprintf	fprintf
#define _fgetts		fgets
#define _tcslen		strlen
#endif

void
usage(
    TCHAR *argv0)
{
	_ftprintf(stderr, _T("Usage: %s serv\n"), argv0);
	exit(1);
}

int
#if defined(__Windows__)
__cdecl
_tmain(
#else
main(
#endif
    int argc,
    TCHAR *argv[])
{
	int error = 0;
	struct servent *servent;
	ADDRINFOT hints, *res, *res0;
	int sfd = -1;
	TCHAR hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
	struct sctp_event_subscribe events;
	struct sockaddr_storage addr;
	socklen_t addrlen;
	char buf[1024];
	int n, len, i;
#if defined(__Windows__)
	DWORD bytes;

	LPFN_WSARECVMSG WSARecvMsg;
	GUID WSARecvMsg_GUID = WSAID_WSARECVMSG;
	LPFN_WSASENDMSG WSASendMsg;
	GUID WSASendMsg_GUID = WSAID_WSASENDMSG;

	WSAMSG msg;
	unsigned char msgbuf[(WSA_CMSG_SPACE(sizeof(struct sctp_sndrcvinfo)))];
	WSACMSGHDR *cmsgp;
	WSABUF iovec;
	struct sctp_sndrcvinfo *srcv;
	sctp_assoc_t assoc_id;

	WSADATA wsaData;
	int ret = 0;
	ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (ret != 0) {
		err(1, _T("WSAStartup"));
		/*NOTREACHED*/
	}
#endif

	if (argc < 2) {
		usage(argv[0]);
		/*NOTREACHED*/
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_PASSIVE;

	error = GetAddrInfo(NULL,
	    argv[1],
	    &hints, &res0);

	if (error) {
#if defined(__Windows__)
		err(1, _T("GetAddrInfo"));
#else
		errx(1, _T("%s"), gai_strerror(error));
#endif
		/*NOTREACHED*/
	}

	for (res = res0; res; res = res->ai_next) {
		res->ai_socktype = SOCK_SEQPACKET;
		res->ai_protocol = IPPROTO_SCTP;
		sfd = socket(res->ai_family,
		    res->ai_socktype,
		    res->ai_protocol);
		if (sfd < 0) {
			warn(_T("socket(domain=%d,type=%d,protocol=%d)"),
			    res->ai_family,
			    res->ai_socktype,
			    res->ai_protocol);
			continue;
		}

		error = GetNameInfo(res->ai_addr, res->ai_addrlen,
		    hbuf, sizeof(hbuf),
		    sbuf, sizeof(sbuf),
		    NI_NUMERICHOST | NI_NUMERICSERV);
		if (error) {
#if defined(__Windows__)
			err(1, _T("GetNameInfo"));
#else
			errx(1, _T("%s"), gai_strerror(error));
#endif
			/*NOTREACHED*/
		}

		if (WSAIoctl(sfd, SIO_GET_EXTENSION_FUNCTION_POINTER,
			&WSARecvMsg_GUID, sizeof(WSARecvMsg_GUID),
			&WSARecvMsg, sizeof(WSARecvMsg),
			&bytes, NULL, NULL) != 0
		    ) {
			err(1, _T("WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER,WSARecvMsg)"));
			/*NOTREACHED*/
		}

		if (WSAIoctl(sfd, SIO_GET_EXTENSION_FUNCTION_POINTER,
			&WSASendMsg_GUID, sizeof(WSASendMsg_GUID),
			&WSASendMsg, sizeof(WSASendMsg),
			&bytes, NULL, NULL) != 0
		    ) {
			err(1, _T("WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER,WSASendMsg)"));
			/*NOTREACHED*/
		}

		memset(&events, 0, sizeof(events));
		events.sctp_data_io_event = 1;
		if (setsockopt(sfd, IPPROTO_SCTP, SCTP_EVENTS,
			(char *)&events, sizeof(events)) < 0
		    ) {
			err(1, _T("setsockopt(IPPROTO_SCTP, SCTP_EVENTS)"));
			/*NOTREACHED*/
		}
		_ftprintf(stderr, _T("Binding to [%s]:%s ...\n"), hbuf, sbuf);

		if (bind(sfd,
			res->ai_addr,
			res->ai_addrlen)
		    < 0) {
			warn(_T("Bind to [%s]:%s"), hbuf, sbuf);
#if defined(__Windows__)
			closesocket(sfd);
#else
			close(sfd);
#endif
			sfd = -1;
			continue;
		}

		if (listen(sfd, 5) < 0) {
			warn(_T("Listen to [%s]:%s"), hbuf, sbuf);
#if defined(__Windows__)
			closesocket(sfd);
#else
			close(sfd);
#endif
			sfd = -1;
			continue;
		}
					
		_ftprintf(stderr, _T("Listening Completed.\n"));
		break;
	}
	if (sfd < 0) {
		return 1;
	}

	memset(&msgbuf, 0, sizeof(msgbuf));
	cmsgp = (WSACMSGHDR *)msgbuf;
	cmsgp->cmsg_level = IPPROTO_SCTP;
	cmsgp->cmsg_type = SCTP_SNDRCV;
	cmsgp->cmsg_len = sizeof(msgbuf);

	srcv = (struct sctp_sndrcvinfo *)(WSA_CMSG_DATA(cmsgp));

	memset(&msg, 0, sizeof(msg));
	iovec.buf = buf;
	iovec.len = sizeof(buf);

	msg.lpBuffers = &iovec;
	msg.dwBufferCount = 1;
	msg.name = (struct sockaddr *)&addr;
	msg.namelen = sizeof(addr);
	msg.Control.buf = msgbuf;
	msg.Control.len = sizeof(msgbuf);


	for (;;) {
		if (WSARecvMsg(sfd, &msg, &len,
			NULL, NULL) < 0
		    ) {
			warn(_T("WSARecvMsg"));
			continue;
		}
		error = GetNameInfo(msg.name, msg.namelen,
		    hbuf, sizeof(hbuf),
		    sbuf, sizeof(sbuf),
		    NI_NUMERICHOST | NI_NUMERICSERV);
		if (error) {
#if defined(__Windows__)
			err(1, _T("GetNameInfo"));
#else
			errx(1, _T("%s"), gai_strerror(error));
#endif
			/*NOTREACHED*/
		}
		assoc_id = srcv->sinfo_assoc_id;

		_ftprintf(stderr, _T("WSARecvMsg from [%s]:%s,sinfo_assoc_id=%x\n"),
		    hbuf, sbuf,
		    assoc_id);

		memset(srcv, 0, sizeof(struct sctp_sndrcvinfo));
		srcv->sinfo_assoc_id = assoc_id;

#if 0
		if (WSASendMsg(sfd, &msg, 0, &len,
			NULL, NULL) < 0
		    ) {
#else
		if (sctp_send(sfd, buf, sizeof(buf), srcv, 0) < 0) {
#endif
			warn(_T("WSASendMsg"));
			continue;
		}
	}

#if defined(__Windows__)
	closesocket(sfd);
#else
	close(sfd);
#endif
	FreeAddrInfo(res0);

	return 0;
}