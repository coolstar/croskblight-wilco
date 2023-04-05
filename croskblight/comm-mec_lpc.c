#include "croskblight.h"

static ULONG CrosKBLightDebugLevel = 100;
static ULONG CrosKBLightDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

/* Version of mailbox interface */
#define EC_MAILBOX_VERSION		0

/* Command to start mailbox transaction */
#define EC_MAILBOX_START_COMMAND	0xda

/* Version of EC protocol */
#define EC_MAILBOX_PROTO_VERSION	3

/* Number of header bytes to be counted as data bytes */
#define EC_MAILBOX_DATA_EXTRA		2

/* Maximum timeout */
#define EC_MAILBOX_TIMEOUT		1

/* EC response flags */
#define EC_CMDR_DATA		BIT(0)	/* Data ready for host to read */
#define EC_CMDR_PENDING		BIT(1)	/* Write pending to EC */
#define EC_CMDR_BUSY		BIT(2)	/* EC is busy processing a command */
#define EC_CMDR_CMD		BIT(3)	/* Last host write was a command */


static __inline void outb(unsigned char __val, unsigned int __port) {
	WRITE_PORT_UCHAR((PUCHAR)__port, __val);
}

static __inline void outw(unsigned short __val, unsigned int __port) {
	WRITE_PORT_USHORT((PUSHORT)__port, __val);
}

static __inline unsigned char inb(unsigned int __port) {
	return READ_PORT_UCHAR((PUCHAR)__port);
}

static __inline unsigned short inw(unsigned int __port) {
	return READ_PORT_USHORT((PUSHORT)__port);
}

FAST_MUTEX MecAccessMutex;

int wait_for_ec(int status_addr, int timeout_usec);

// Thanks @DHowett!

typedef enum _ec_xfer_direction { EC_MEC_WRITE, EC_MEC_READ } ec_xfer_direction;

enum cros_ec_lpc_mec_emi_access_mode {
	/* 8-bit access */
	MEC_EC_BYTE_ACCESS = 0x0,
	/* 16-bit access */
	MEC_EC_WORD_ACCESS = 0x1,
	/* 32-bit access */
	MEC_EC_LONG_ACCESS = 0x2,
	/*
	 * 32-bit access, read or write of MEC_EMI_EC_DATA_B3 causes the
	 * EC data register to be incremented.
	 */
	 MEC_EC_LONG_ACCESS_AUTOINCREMENT = 0x3,
};

/* EMI registers are relative to base */
#define MEC_EMI_HOST_TO_EC(MEC_EMI_BASE)	((MEC_EMI_BASE) + 0)
#define MEC_EMI_EC_TO_HOST(MEC_EMI_BASE)	((MEC_EMI_BASE) + 1)
#define MEC_EMI_EC_ADDRESS_B0(MEC_EMI_BASE)	((MEC_EMI_BASE) + 2)
#define MEC_EMI_EC_ADDRESS_B1(MEC_EMI_BASE)	((MEC_EMI_BASE) + 3)
#define MEC_EMI_EC_DATA_B0(MEC_EMI_BASE)	((MEC_EMI_BASE) + 4)
#define MEC_EMI_EC_DATA_B1(MEC_EMI_BASE)	((MEC_EMI_BASE) + 5)
#define MEC_EMI_EC_DATA_B2(MEC_EMI_BASE)	((MEC_EMI_BASE) + 6)
#define MEC_EMI_EC_DATA_B3(MEC_EMI_BASE)	((MEC_EMI_BASE) + 7)

UINT16 mec_emi_base = 0, mec_emi_end = 0;

static void ec_mec_emi_write_access(UINT16 address, enum cros_ec_lpc_mec_emi_access_mode access_type) {
	outw((address & 0xFFFC) | (UINT16)access_type, MEC_EMI_EC_ADDRESS_B0(mec_emi_base));
}

static int ec_mec_xfer(ec_xfer_direction direction, UINT16 address,
	UINT8* data, UINT16 size)
{
	if (mec_emi_base == 0 || mec_emi_end == 0)
		return 0;

	ExAcquireFastMutex(&MecAccessMutex);

	/*
	 * There's a cleverer way to do this, but it's somewhat less clear what's happening.
	 * I prefer clarity over cleverness. :)
	 */
	int pos = 0;
	UINT16 temp[2];
	if (address % 4 > 0) {
		ec_mec_emi_write_access(address, MEC_EC_BYTE_ACCESS);
		/* Unaligned start address */
		for (int i = address % 4; i < 4; ++i) {
			UINT8* storage = &data[pos++];
			if (direction == EC_MEC_WRITE)
				outb(*storage, MEC_EMI_EC_DATA_B0(mec_emi_base) + i);
			else if (direction == EC_MEC_READ)
				*storage = inb(MEC_EMI_EC_DATA_B0(mec_emi_base) + i);
		}
		address = (address + 4) & 0xFFFC;
	}

	if (size - pos >= 4) {
		ec_mec_emi_write_access(address, MEC_EC_LONG_ACCESS_AUTOINCREMENT);
		while (size - pos >= 4) {
			if (direction == EC_MEC_WRITE) {
				memcpy(temp, &data[pos], sizeof(temp));
				outw(temp[0], MEC_EMI_EC_DATA_B0(mec_emi_base));
				outw(temp[1], MEC_EMI_EC_DATA_B2(mec_emi_base));
			}
			else if (direction == EC_MEC_READ) {
				temp[0] = inw(MEC_EMI_EC_DATA_B0(mec_emi_base));
				temp[1] = inw(MEC_EMI_EC_DATA_B2(mec_emi_base));
				memcpy(&data[pos], temp, sizeof(temp));
			}

			pos += 4;
			address += 4;
		}
	}

	if (size - pos > 0) {
		ec_mec_emi_write_access(address, MEC_EC_BYTE_ACCESS);
		for (int i = 0; i < (size - pos); ++i) {
			UINT8* storage = &data[pos + i];
			if (direction == EC_MEC_WRITE)
				outb(*storage, MEC_EMI_EC_DATA_B0(mec_emi_base) + i);
			else if (direction == EC_MEC_READ)
				*storage = inb(MEC_EMI_EC_DATA_B0(mec_emi_base) + i);
		}
	}

	ExReleaseFastMutex(&MecAccessMutex);

	return 0;
}

/**
 * wilco_ec_response_timed_out() - Wait for EC response.
 * @ec: EC device.
 *
 * Return: true if EC timed out, false if EC did not time out.
 */
static BOOLEAN wilco_ec_response_timed_out(PCROSKBLIGHT_CONTEXT pDevice)
{
	LARGE_INTEGER CurrentTime;
	KeQuerySystemTimePrecise(&CurrentTime);

	LARGE_INTEGER Timeout;
	Timeout.QuadPart = CurrentTime.QuadPart + (10 * 1000 * 1000);

	do {
		UINT8 readByte = inb(pDevice->ecIoCommand.Start.LowPart);
		if (!(readByte &
			(EC_CMDR_PENDING | EC_CMDR_BUSY)))
			return FALSE;

		LARGE_INTEGER Interval;
		Interval.QuadPart = -10 * 100;
		KeDelayExecutionThread(KernelMode, FALSE, &Interval);

		KeQuerySystemTimePrecise(&CurrentTime);
	} while (CurrentTime.QuadPart < Timeout.QuadPart);

	return TRUE;
}

/**
 * wilco_ec_checksum() - Compute 8-bit checksum over data range.
 * @data: Data to checksum.
 * @size: Number of bytes to checksum.
 *
 * Return: 8-bit checksum of provided data.
 */
static UINT8 wilco_ec_checksum(const void* data, size_t size)
{
	UINT8* data_bytes = (UINT8*)data;
	UINT8 checksum = 0;
	size_t i;

	for (i = 0; i < size; i++)
		checksum += data_bytes[i];

	return checksum;
}

NTSTATUS wilco_ec_mailbox(PCROSKBLIGHT_CONTEXT pDevice, struct wilco_ec_message *msg) {
	NTSTATUS status = STATUS_SUCCESS;
	struct wilco_ec_response* rs = pDevice->dataBuffer;
	UINT8 checksum, flag;
	WdfWaitLockAcquire(pDevice->ecLock, NULL);

	struct wilco_ec_request rq = { 0 };
	rq.struct_version = EC_MAILBOX_PROTO_VERSION;
	rq.mailbox_id = msg->type;
	rq.mailbox_version = EC_MAILBOX_VERSION;
	rq.data_size = msg->request_size;

	/* Checksum header and data */
	rq.checksum = wilco_ec_checksum(&rq, sizeof(rq));
	rq.checksum += wilco_ec_checksum(msg->request_data, msg->request_size);
	rq.checksum = -rq.checksum;

	//Start transfer

	ec_mec_xfer(EC_MEC_WRITE, 0, &rq, sizeof(rq));
	ec_mec_xfer(EC_MEC_WRITE, sizeof(rq), msg->request_data, msg->request_size);

	//Start the command
	outb(EC_MAILBOX_START_COMMAND, pDevice->ecIoCommand.Start.LowPart);

	/* For some commands (eg shutdown) the EC will not respond, that's OK */
	if (msg->flags & WILCO_EC_FLAG_NO_RESPONSE) {
		CrosKBLightPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"EC does not respond to this command\n");
		status = STATUS_SUCCESS;
		goto out;
	}

	/* Wait for it to complete */
	if (wilco_ec_response_timed_out(pDevice)) {
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"response timed out\n");
		status = STATUS_IO_TIMEOUT;
		goto out;
	}

	/* Check result */
	flag = inb(pDevice->ecIoData.Start.LowPart);
	if (flag) {
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"bad response: 0x%02x\n", flag);
		status = STATUS_IO_DEVICE_ERROR;
		goto out;
	}

	/* Read back response */
	ec_mec_xfer(EC_MEC_READ, 0, rs, sizeof(*rs) + EC_MAILBOX_DATA_SIZE);

	if (rs->result) {
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"EC reported failure: 0x%02x\n", rs->result);
		status = STATUS_IO_DEVICE_ERROR;
		goto out;
	}

	if (rs->data_size != EC_MAILBOX_DATA_SIZE) {
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"unexpected packet size (%u != %u)\n",
			rs->data_size, EC_MAILBOX_DATA_SIZE);
		status = STATUS_IO_DEVICE_ERROR;
		goto out;
	}

	if (rs->data_size < msg->response_size) {
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"EC didn't return enough data (%u < %zu)\n",
			rs->data_size, msg->response_size);
		status = STATUS_IO_DEVICE_ERROR;
		goto out;
	}

	RtlCopyMemory(msg->response_data, rs->data, msg->response_size);

out:
	WdfWaitLockRelease(pDevice->ecLock);
	return status;
}

NTSTATUS comm_init_lpc_mec(PCROSKBLIGHT_CONTEXT pDevice)
{
	/* This function assumes some setup was done by comm_init_lpc. */

	ExInitializeFastMutex(&MecAccessMutex);

	mec_emi_base = pDevice->ecIoPacket.Start.LowPart;
	mec_emi_end = pDevice->ecIoPacket.Start.LowPart + EC_MAILBOX_DATA_SIZE;

	return STATUS_SUCCESS;
}