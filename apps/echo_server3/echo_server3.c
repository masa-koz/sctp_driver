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
 * $Id: udp.c,v 1.2 2007/04/26 05:21:34 kozuka Exp $
 */
#include <ntifs.h>
#include <windef.h>

#include <tdi.h>
#include <tdiinfo.h>
#include <tdikrnl.h>
#include <tdistat.h>

#if NTDDI_VERSION < NTDDI_LONGHORN
typedef unsigned short u_short;
#endif
#include <ws2def.h>

#include <sys/syscall.h>

#undef TYPE_ALIGNMENT
#define TYPE_ALIGNMENT( t ) __alignof(t)

__inline USHORT
ntohs(USHORT x)
{
    return (((x & 0xff) << 8) | ((x & 0xff00) >> 8));
}

__inline ULONG
ntohl(ULONG x)
{
    return (((x & 0xffL) << 24) | ((x & 0xff00L) << 8) |
        ((x & 0xff0000L) >> 8) | ((x &0xff000000L) >> 24));
}

#ifdef SCTP
#define UdpDevice L"\\Device\\SctpUdp"
#else
#define UdpDevice L"\\Device\\Udp"
#endif


PFILE_OBJECT UdpObject;
HANDLE UdpHandle;

#ifndef EVENT
struct ThreadCtx {
	BOOLEAN bActive;
	KEVENT event;
} *ThrCtx;

PFILE_OBJECT ThrObject;
#endif

#ifdef SCTP
/* XXX These stuffs should be replaced with include.... */
typedef unsigned long sctp_assoc_t;
typedef long ssize_t;

struct sctp_event_subscribe {
	unsigned char sctp_data_io_event;
	unsigned char sctp_association_event;
	unsigned char sctp_address_event;
	unsigned char sctp_send_failure_event;
	unsigned char sctp_peer_error_event;
	unsigned char sctp_shutdown_event;
	unsigned char sctp_partial_delivery_event;
	unsigned char sctp_adaptation_layer_event;
	unsigned char sctp_authentication_event;
	unsigned char sctp_stream_reset_events;
};

#define SCTP_ALIGN_RESV_PAD 96
#define SCTP_ALIGN_RESV_PAD_SHORT 80

struct sctp_sndrcvinfo {
	unsigned short sinfo_stream;
	unsigned short sinfo_ssn;
	unsigned short sinfo_flags;
	unsigned short sinfo_pr_policy;
	unsigned long sinfo_ppid;
	unsigned long sinfo_context;
	unsigned long sinfo_timetolive;
	unsigned long sinfo_tsn;
	unsigned long sinfo_cumtsn;
	sctp_assoc_t sinfo_assoc_id;
	unsigned char  __reserve_pad[SCTP_ALIGN_RESV_PAD];
};
#endif

struct RcvDgCtx {
	KEVENT event;
	NTSTATUS status;
	ULONG length;
	TDI_CONNECTION_INFORMATION returnInfo;
	TA_IP_ADDRESS address;
#ifdef SCTP
	UCHAR option[TDI_CMSG_SPACE(sizeof(struct sctp_sndrcvinfo))];
#endif
};

struct SndDgCtx {
	KEVENT event;
	NTSTATUS status;
	ULONG length;
	TDI_CONNECTION_INFORMATION sendDatagramInfo;
	TA_IP_ADDRESS address;
#ifdef SCTP
	UCHAR option[TDI_CMSG_SPACE(sizeof(struct sctp_sndrcvinfo))];
#endif
};



NTSTATUS DriverEntry(IN PDRIVER_OBJECT, IN PUNICODE_STRING);
VOID Unload(IN PDRIVER_OBJECT);
#ifdef EVENT
NTSTATUS ReceiveDatagram(IN PVOID, IN LONG, IN PVOID, IN LONG, IN PVOID, IN ULONG, IN ULONG, IN ULONG, OUT ULONG *, IN PVOID, OUT PIRP *);
#else
VOID ReceiveDatagramThread(IN PVOID);
#endif
NTSTATUS ReceiveDatagramComp(IN PDEVICE_OBJECT, IN PIRP, IN PVOID);
NTSTATUS SendDatagram(IN PUCHAR, IN ULONG, IN PIRP, IN struct SndDgCtx *, IN KEVENT);
NTSTATUS SendDatagramComp(IN PDEVICE_OBJECT, IN PIRP, IN PVOID);
NTSTATUS Dispatch(IN PDEVICE_OBJECT, IN PIRP);


NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT driverObject,
    IN PUNICODE_STRING registryPath)
{
	int i;
	NTSTATUS status;
	UNICODE_STRING devname;
	PFILE_FULL_EA_INFORMATION eaInfo;
	ULONG eaLength;
	OBJECT_ATTRIBUTES attr;
	PTA_IP_ADDRESS ipAddress;
	IO_STATUS_BLOCK statusBlock;
	SOCKET_SOCKOPT_REQUEST optReq;
	struct sctp_event_subscribe events;

#ifdef EVENT
	PIRP irp;
	PDEVICE_OBJECT deviceObject;
#else
	HANDLE thrHandle;
#endif

	DbgPrint("DriverEntry - enter\n");

	for (i = 0; i < IRP_MJ_MAXIMUM_FUNCTION; i++) {
		driverObject->MajorFunction[i] = Dispatch;
	}
	driverObject->DriverUnload = Unload;

	// Open Address Object, similar to socket() + bind(0.0.0.0)
	eaLength = sizeof(FILE_FULL_EA_INFORMATION) + sizeof(TdiTransportAddress) + sizeof(TA_IP_ADDRESS);
	eaInfo = ExAllocatePool(NonPagedPool, eaLength);
	if (eaInfo == NULL) {
		status = STATUS_INSUFFICIENT_RESOURCES;
		DbgPrint("DriverEntry - leave#1\n");
		goto done;
	}

	RtlZeroMemory(eaInfo, eaLength);
	eaInfo->EaNameLength = TDI_TRANSPORT_ADDRESS_LENGTH;
	RtlCopyMemory(eaInfo->EaName, TdiTransportAddress, sizeof(TdiTransportAddress));
	eaInfo->EaValueLength = sizeof (TA_IP_ADDRESS);

	ipAddress = (PTA_IP_ADDRESS)(eaInfo->EaName + sizeof(TdiTransportAddress));
	ipAddress->TAAddressCount = 1;
	ipAddress->Address[0].AddressLength = sizeof(TDI_ADDRESS_IP);
	ipAddress->Address[0].AddressType = TDI_ADDRESS_TYPE_IP;
	ipAddress->Address[0].Address[0].sin_port = ntohs(7);

	RtlInitUnicodeString(&devname, UdpDevice);
	InitializeObjectAttributes(&attr, &devname, OBJ_CASE_INSENSITIVE, NULL, NULL);

	status = ZwCreateFile(&UdpHandle,
	    GENERIC_READ | GENERIC_WRITE,
	    &attr,
	    &statusBlock,
	    0L,
	    FILE_ATTRIBUTE_NORMAL,
	    FILE_SHARE_READ | FILE_SHARE_WRITE,
	    FILE_OPEN_IF,
	    0L,
	    eaInfo,
	    eaLength);

	if (status != STATUS_SUCCESS) {
		DbgPrint("DriverEntry - leave#2\n");
		goto done;
	}

	status = ObReferenceObjectByHandle(UdpHandle,
	    GENERIC_READ | GENERIC_WRITE,
	    NULL,
	    KernelMode,
	    &UdpObject,
	    NULL);
	if (status != STATUS_SUCCESS) {
		ZwClose(UdpHandle);
		UdpHandle = NULL;
		DbgPrint("DriverEntry - leave#3\n");
		goto done;
	}

	RtlZeroMemory(&events, sizeof(events));
	events.sctp_data_io_event = 1;
	RtlZeroMemory(&optReq, sizeof(optReq));
	optReq.level = 132;
	optReq.optname = 0x0000000c;
	optReq.optval = (char *)&events;
	optReq.optlen = sizeof(events);
	
	status = ZwDeviceIoControlFile(UdpHandle,
	    NULL,
	    NULL,
	    NULL,
	    &statusBlock,
	    IOCTL_SOCKET_SETSOCKOPT,
	    &optReq,
	    sizeof(optReq),
	    NULL,
	    0);

#ifdef EVENT
	deviceObject = IoGetRelatedDeviceObject(UdpObject);
	irp = TdiBuildInternalDeviceControlIrp(TDI_SET_EVENT_HANDLER,
	    deviceObject,
	    UdpObject,
	    NULL,
	    NULL);

	TdiBuildSetEventHandler(irp,
	    deviceObject,
	    UdpObject,
	    NULL,
	    NULL,
	    TDI_EVENT_RECEIVE_DATAGRAM,
	    ReceiveDatagram,
	    NULL);

	status = IoCallDriver(deviceObject, irp);
	if (status != STATUS_SUCCESS) {
		Unload(driverObject);
	}
#else
	ThrCtx = ExAllocatePool(NonPagedPool, sizeof(*ThrCtx));
	KeInitializeEvent(&ThrCtx->event, SynchronizationEvent, FALSE);
	ThrCtx->bActive = TRUE;

	InitializeObjectAttributes(&attr,
	    NULL,
	    OBJ_KERNEL_HANDLE,
	    NULL,
	    NULL);

	status = PsCreateSystemThread(&thrHandle,
	    0L,
	    &attr,
	    NULL,
	    NULL,
	    ReceiveDatagramThread,
	    ThrCtx);
	if (status == STATUS_SUCCESS) {
		ObReferenceObjectByHandle(thrHandle,
		    THREAD_ALL_ACCESS,
		    NULL,
		    KernelMode,
		    (PVOID *)&ThrObject,
		    NULL);
		ZwClose(thrHandle);
	} else {
		ZwClose(thrHandle);
		ThrObject = NULL;
		ExFreePool(ThrCtx);
		Unload(driverObject);
	}
#endif
	DbgPrint("DriverEntry - leave\n");
done:
	if (eaInfo != NULL) {
		ExFreePool(eaInfo);
	}
	return status;
}


VOID
Unload(
    IN PDRIVER_OBJECT driverObject)
{
	NTSTATUS status;

	DbgPrint("Unload - enter\n");
#ifndef EVENT
	if (ThrObject != NULL) {
		ThrCtx->bActive = FALSE;
		KeSetEvent(&ThrCtx->event, IO_NO_INCREMENT, FALSE);
		status = KeWaitForSingleObject(ThrObject,
		    Executive,
		    KernelMode,
		    FALSE,
		    NULL);
		ObDereferenceObject(ThrObject);
		ExFreePool(ThrCtx);
	}
#endif
	if (UdpObject != NULL) {
		ObDereferenceObject(UdpObject);
	}

	if (UdpHandle != NULL) {
		ZwClose(UdpHandle);
	}
	DbgPrint("Unload - leave\n");
}


#ifdef EVENT
NTSTATUS
ReceiveDatagram(
    IN PVOID tdiEventContext,
    IN LONG sourceAddressLength,
    IN PVOID sourceAddress,
    IN LONG optionsLength,
    IN PVOID options,
    IN ULONG receiveDatagramFlags,
    IN ULONG bytesIndicated,
    IN ULONG bytesAvailable,
    OUT ULONG *bytesTaken,
    IN PVOID tsdu,
    OUT PIRP *ioRequestPacket)
{
	struct RcvDgCtx *rcvDgCtx = NULL;
	struct SndDgCtx *sndDgCtx = NULL;
	PTRANSPORT_ADDRESS tAddr = NULL;
	PTA_ADDRESS taAddr = NULL;
	PTA_IP_ADDRESS taIpAddr = NULL;
	PUCHAR addr = NULL;
	unsigned int i;
#ifdef SCTP
	PTDI_CMSGHDR scmsgp, scmsgp2;
	struct sctp_sndrcvinfo *srcv, *srcv2;
#endif

	DbgPrint("ReceiveDatagra - enter\n");
	if (bytesIndicated == bytesAvailable) {
		DbgPrint("ReceiveDatagram: #1\n");
		if (sourceAddressLength >= sizeof(TA_ADDRESS)) {
			tAddr = (PTRANSPORT_ADDRESS)sourceAddress;
			taAddr = (PTA_ADDRESS)&tAddr->Address[0];
			if (taAddr->AddressType == TDI_ADDRESS_TYPE_IP &&
			    taAddr->AddressLength == sizeof(TA_IP_ADDRESS)) {
				taIpAddr = (PTA_IP_ADDRESS)tAddr;
				addr = (UCHAR *)&taIpAddr->Address[0].Address[0].in_addr;
				DbgPrint("from=%ld.%ld.%ld.%ld\n", addr[0], addr[1], addr[2], addr[3]);
			}
		}
#ifdef SCTP
		if (optionsLength == TDI_CMSG_SPACE(sizeof(struct sctp_sndrcvinfo))) {
			scmsgp = (PTDI_CMSGHDR)options;
			srcv = (struct sctp_sndrcvinfo *)(TDI_CMSG_DATA(scmsgp));
			DbgPrint("sinfo_stream=%d,sinfo_ssn=%d,sinfo_flags=%d,sinfo_ppid=%d,sinfo_context=%d,sinfo_timetolive=%d,sinfo_tsn=%d,sinfo_cumtsn=%d,sinfo_assoc_id=%d\n",
			    srcv->sinfo_stream,
			    srcv->sinfo_ssn,
			    srcv->sinfo_flags,
			    srcv->sinfo_ppid,
			    srcv->sinfo_context,
			    srcv->sinfo_timetolive,
			    srcv->sinfo_tsn,
			    srcv->sinfo_cumtsn,
			    srcv->sinfo_assoc_id);
		}
#endif
		DbgPrint("length=%d,data=\"", bytesAvailable);
		for (i = 0; i < bytesAvailable; i++) {
			DbgPrint("%c", ((PUCHAR)tsdu)[i]);
		}
		DbgPrint("\"\n");

		return STATUS_SUCCESS;
	}
	DbgPrint("ReceiveDatagram: #2\n");
	return STATUS_SUCCESS;
}


#else
VOID
ReceiveDatagramThread(
    IN PVOID ctx)
{
	struct ThreadCtx *thrCtx = ctx;
	struct RcvDgCtx *rcvDgCtx = NULL;
	struct SndDgCtx *sndDgCtx = NULL;
	PDEVICE_OBJECT deviceObject;
	PIRP irp = NULL;
	PMDL mdl = NULL;
	PVOID events[2];
	UCHAR *data;
	ULONG maxLength = 1500;
	NTSTATUS status = STATUS_SUCCESS, waitStatus = STATUS_SUCCESS;
	size_t i;
	PUCHAR addr;
#ifdef SCTP
	PTDI_CMSGHDR scmsgp, scmsgp2;
	struct sctp_sndrcvinfo *srcv, *srcv2;
#endif

	DbgPrint("ReceiveDatagramThread - enter\n");

	deviceObject = IoGetRelatedDeviceObject(UdpObject);

	irp = IoAllocateIrp(deviceObject->StackSize + 1, FALSE);
	if (irp == NULL) {
		status = STATUS_INSUFFICIENT_RESOURCES;
		DbgPrint("ReceiveDatagramThread - leave#1\n");
		goto done;
	}

	data = ExAllocatePool(PagedPool, maxLength);
	if (data == NULL) {
		status = STATUS_INSUFFICIENT_RESOURCES;
		DbgPrint("ReceiveDatagramThread - leave#2\n");
		goto done;
	}

	rcvDgCtx = ExAllocatePool(NonPagedPool, sizeof(*rcvDgCtx));
	if (rcvDgCtx == NULL) {
		status = STATUS_INSUFFICIENT_RESOURCES;
		DbgPrint("ReceiveDatagramThread - leave#3\n");
		goto done;
	}

	RtlZeroMemory(rcvDgCtx, sizeof(*rcvDgCtx));
	KeInitializeEvent(&rcvDgCtx->event, SynchronizationEvent, FALSE);
	

	sndDgCtx = ExAllocatePool(NonPagedPool, sizeof(*sndDgCtx));
	if (sndDgCtx == NULL) {
		status = STATUS_INSUFFICIENT_RESOURCES;
		DbgPrint("ReceiveDatagramThread - leave#4\n");
		goto done;
	}

	RtlZeroMemory(sndDgCtx, sizeof(*sndDgCtx));
	KeInitializeEvent(&sndDgCtx->event, SynchronizationEvent, FALSE);

	sndDgCtx->sendDatagramInfo.RemoteAddressLength = sizeof(TA_IP_ADDRESS);
	sndDgCtx->sendDatagramInfo.RemoteAddress = &sndDgCtx->address;

#ifdef SCTP
	sndDgCtx->sendDatagramInfo.OptionsLength = sizeof(sndDgCtx->option);
	sndDgCtx->sendDatagramInfo.Options = &sndDgCtx->option;
	scmsgp2 = (PTDI_CMSGHDR)&sndDgCtx->option;
	scmsgp2->cmsg_len = TDI_CMSG_LEN(sizeof(struct sctp_sndrcvinfo));
	scmsgp2->cmsg_level = 132; /* IPPROTO_SCTP */
	scmsgp2->cmsg_type = 0x0002; /* SCTP_SNDRCV */
	srcv2 = (struct sctp_sndrcvinfo *)(TDI_CMSG_DATA(scmsgp2));
#endif

	events[0] = &rcvDgCtx->event;
	events[1] = &thrCtx->event;

	while (thrCtx->bActive == TRUE) {
		mdl = IoAllocateMdl(data, maxLength, FALSE, FALSE, NULL);
		if (mdl == NULL) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			DbgPrint("ReceiveDatagramThread - leave#5\n");
			goto done;
		}

		__try {
			MmProbeAndLockPages(mdl, KernelMode, IoModifyAccess);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			IoFreeMdl(mdl);
			status = STATUS_INSUFFICIENT_RESOURCES;
		}
		if (status == STATUS_INSUFFICIENT_RESOURCES) {
			DbgPrint("ReceiveDatagramThread - leave#6\n");
			goto done;
		}

		RtlZeroMemory(&rcvDgCtx->address, sizeof(rcvDgCtx->address));
		rcvDgCtx->returnInfo.RemoteAddressLength = sizeof(TA_IP_ADDRESS);
		rcvDgCtx->returnInfo.RemoteAddress = &rcvDgCtx->address;

#ifdef SCTP
		RtlZeroMemory(&rcvDgCtx->option, sizeof(rcvDgCtx->option));
		rcvDgCtx->returnInfo.OptionsLength = sizeof(rcvDgCtx->option);
		rcvDgCtx->returnInfo.Options = &rcvDgCtx->option;
		scmsgp = (PTDI_CMSGHDR)&rcvDgCtx->option;
		scmsgp->cmsg_len = TDI_CMSG_LEN(sizeof(struct sctp_sndrcvinfo));
		scmsgp->cmsg_level = 132; /* IPPROTO_SCTP */
		scmsgp->cmsg_type = 0x0002; /* SCTP_SNDRCV */
		srcv = (struct sctp_sndrcvinfo *)(TDI_CMSG_DATA(scmsgp));
#endif

		TdiBuildReceiveDatagram(irp,
		    deviceObject,
		    UdpObject,
		    ReceiveDatagramComp,
		    rcvDgCtx,
		    mdl,
		    maxLength,
		    NULL,
		    &rcvDgCtx->returnInfo,
		    TDI_RECEIVE_NORMAL);

		status = IoCallDriver(deviceObject, irp);
		//DbgPrint("ReceiveDatagramThread: IoCallDriver status=%X\n", status);
		if (status != STATUS_PENDING) {
			ReceiveDatagramComp(deviceObject, irp, rcvDgCtx);
		}

		waitStatus = KeWaitForMultipleObjects(2, events, WaitAny, Executive, KernelMode, FALSE, NULL, NULL);
		switch (waitStatus) {
		case STATUS_WAIT_0:
#ifdef SCTP
#if 0
			addr = (UCHAR *)&((PTDI_ADDRESS_IP)rcvDgCtx->address.Address[0].Address)->in_addr;
			DbgPrint("status=%X,length=%d,from=%ld.%ld.%ld.%ld\n", rcvDgCtx->status, rcvDgCtx->length,
			    addr[0], addr[1], addr[2], addr[3]);
			if (rcvDgCtx->returnInfo.OptionsLength == TDI_CMSG_SPACE(sizeof(struct sctp_sndrcvinfo))) {
				DbgPrint("sinfo_stream=%d,sinfo_ssn=%d,sinfo_flags=%d,sinfo_ppid=%d,sinfo_context=%d,sinfo_timetolive=%d,sinfo_tsn=%d,sinfo_cumtsn=%d,sinfo_assoc_id=%d\n",
				    srcv->sinfo_stream,
				    srcv->sinfo_ssn,
				    srcv->sinfo_flags,
				    srcv->sinfo_ppid,
				    srcv->sinfo_context,
				    srcv->sinfo_timetolive,
				    srcv->sinfo_tsn,
				    srcv->sinfo_cumtsn,
				    srcv->sinfo_assoc_id);
			}
#endif
			srcv2->sinfo_assoc_id = srcv->sinfo_assoc_id;
#endif
			RtlCopyMemory(sndDgCtx->sendDatagramInfo.RemoteAddress, rcvDgCtx->returnInfo.RemoteAddress,
			    rcvDgCtx->returnInfo.RemoteAddressLength);
			sndDgCtx->sendDatagramInfo.RemoteAddressLength = rcvDgCtx->returnInfo.RemoteAddressLength;
			SendDatagram(data, rcvDgCtx->length, irp, sndDgCtx, thrCtx->event);
			break;
		case STATUS_WAIT_1:
			DbgPrint("ReceiveDatagramThread: try to cancel irp=%p\n", irp);
			IoCancelIrp(irp);
			DbgPrint("ReceiveDatagramThread: wait for completion\n");
			waitStatus = KeWaitForSingleObject(&rcvDgCtx->event, Executive, KernelMode, FALSE, NULL);
			break;
		default:
			break;
		}

		if (thrCtx->bActive == FALSE) {
			break;
		}
	}
done:
	if (irp != NULL) {
		IoFreeIrp(irp);
	}
	if (data != NULL) {
		ExFreePool(data);
	}
	if (rcvDgCtx != NULL) {
		ExFreePool(rcvDgCtx);
	}
#if 0
	if (events != NULL) {
		ExFreePool(events);
	}
#endif
	DbgPrint("ReceiveDatagramThread - leave\n");
	PsTerminateSystemThread(status);
}
#endif


NTSTATUS
ReceiveDatagramComp(
    IN PDEVICE_OBJECT deviceObject,
    IN PIRP irp,
    IN PVOID ctx)
{
	PIO_STACK_LOCATION irpSp;
	PMDL mdl = NULL, nextMdl = NULL;
	struct RcvDgCtx *rcvDgCtx = ctx;
	PTDI_CONNECTION_INFORMATION returnInformation;

	irpSp = IoGetCurrentIrpStackLocation(irp);

	rcvDgCtx->status = irp->IoStatus.Status;
	rcvDgCtx->length = irp->IoStatus.Information;

	if (irp->MdlAddress != NULL) {
		for (mdl = irp->MdlAddress; mdl != NULL; mdl = nextMdl) {
			nextMdl = mdl->Next;
			MmUnlockPages(mdl);
			IoFreeMdl(mdl);
		}

		irp->MdlAddress = NULL;
	}
	KeSetEvent(&rcvDgCtx->event, IO_NO_INCREMENT, FALSE);

	return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
SendDatagram(
    IN PUCHAR data,
    IN ULONG length,
    IN PIRP irp,
    IN struct SndDgCtx *sndDgCtx,
    IN KEVENT event)
{
	NTSTATUS status, waitStatus;
	PDEVICE_OBJECT deviceObject;
	PMDL mdl = NULL;
	PVOID events[2];
	LARGE_INTEGER timeout;

	deviceObject = IoGetRelatedDeviceObject(UdpObject);

	mdl = IoAllocateMdl(data, length, FALSE, FALSE, NULL);
	if (mdl == NULL) {
		DbgPrint("SendDatagram - leave#1\n");
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	__try {
		MmProbeAndLockPages(mdl, KernelMode, IoModifyAccess);
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		IoFreeMdl(mdl);
		DbgPrint("SendDatagram - leave#2\n");
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	TdiBuildSendDatagram(irp,
	    deviceObject,
	    UdpObject,
	    SendDatagramComp,
	    sndDgCtx,
	    mdl,
	    length,
	    &sndDgCtx->sendDatagramInfo);

	events[0] = &sndDgCtx->event;
	events[1] = &event;

	status = IoCallDriver(deviceObject, irp);
	if (status != STATUS_PENDING) {
		SendDatagramComp(deviceObject, irp, sndDgCtx);
	}
	waitStatus = KeWaitForMultipleObjects(2, events, WaitAny, Executive, KernelMode, FALSE, NULL, NULL);
	switch (waitStatus) {
	case STATUS_WAIT_0:
		return sndDgCtx->status;
	case STATUS_WAIT_1:
		IoCancelIrp(irp);
		KeWaitForSingleObject(&sndDgCtx->event, Executive, KernelMode, FALSE, NULL);
		return sndDgCtx->status;
	default:
		break;
	}

	return waitStatus;
}


NTSTATUS
SendDatagramComp(
    IN PDEVICE_OBJECT deviceObject,
    IN PIRP irp,
    IN PVOID ctx)
{
	PIO_STACK_LOCATION irpSp;
	PMDL mdl = NULL, nextMdl = NULL;
	struct SndDgCtx *sndDgCtx = ctx;

	irpSp = IoGetCurrentIrpStackLocation(irp);

	sndDgCtx->status = irp->IoStatus.Status;
	sndDgCtx->length = irp->IoStatus.Information;

	if (irp->MdlAddress != NULL) {
		for (mdl = irp->MdlAddress; mdl != NULL; mdl = nextMdl) {
			nextMdl = mdl->Next;
			MmUnlockPages(mdl);
			IoFreeMdl(mdl);
		}

		irp->MdlAddress = NULL;
	}
	KeSetEvent(&sndDgCtx->event, IO_NO_INCREMENT, FALSE);

	return STATUS_MORE_PROCESSING_REQUIRED;
}


NTSTATUS
Dispatch(
    IN PDEVICE_OBJECT deviceObject,
    IN PIRP irp)
{
	return STATUS_NOT_SUPPORTED;
}
