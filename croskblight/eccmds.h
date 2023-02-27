#ifndef __CROS_EC_REGS_H__
#define __CROS_EC_REGS_H__

#define BIT(nr) (1UL << (nr))

/* Message flags for using the mailbox() interface */
#define WILCO_EC_FLAG_NO_RESPONSE	BIT(0) /* EC does not respond */

/* Normal commands have a maximum 32 bytes of data */
#define EC_MAILBOX_DATA_SIZE		32

#include <pshpack1.h>

/**
 * struct wilco_ec_request - Mailbox request message format.
 * @struct_version: Should be %EC_MAILBOX_PROTO_VERSION
 * @checksum: Sum of all bytes must be 0.
 * @mailbox_id: Mailbox identifier, specifies the command set.
 * @mailbox_version: Mailbox interface version %EC_MAILBOX_VERSION
 * @reserved: Set to zero.
 * @data_size: Length of following data.
 */
struct wilco_ec_request {
	UINT8 struct_version;
	UINT8 checksum;
	UINT16 mailbox_id;
	UINT8 mailbox_version;
	UINT8 reserved;
	UINT16 data_size;
};

/**
 * struct wilco_ec_response - Mailbox response message format.
 * @struct_version: Should be %EC_MAILBOX_PROTO_VERSION
 * @checksum: Sum of all bytes must be 0.
 * @result: Result code from the EC.  Non-zero indicates an error.
 * @data_size: Length of the response data buffer.
 * @reserved: Set to zero.
 * @data: Response data buffer.  Max size is %EC_MAILBOX_DATA_SIZE_EXTENDED.
 */
struct wilco_ec_response {
	UINT8 struct_version;
	UINT8 checksum;
	UINT16 result;
	UINT16 data_size;
	UINT8 reserved[2];
	UINT8 data[];
};

#include <poppack.h>

/**
 * enum wilco_ec_msg_type - Message type to select a set of command codes.
 * @WILCO_EC_MSG_LEGACY: Legacy EC messages for standard EC behavior.
 * @WILCO_EC_MSG_PROPERTY: Get/Set/Sync EC controlled NVRAM property.
 * @WILCO_EC_MSG_TELEMETRY: Request telemetry data from the EC.
 */
enum wilco_ec_msg_type {
	WILCO_EC_MSG_LEGACY = 0x00f0,
	WILCO_EC_MSG_PROPERTY = 0x00f2,
	WILCO_EC_MSG_TELEMETRY = 0x00f5,
};

/**
 * struct wilco_ec_message - Request and response message.
 * @type: Mailbox message type.
 * @flags: Message flags, e.g. %WILCO_EC_FLAG_NO_RESPONSE.
 * @request_size: Number of bytes to send to the EC.
 * @request_data: Buffer containing the request data.
 * @response_size: Number of bytes to read from EC.
 * @response_data: Buffer containing the response data, should be
 *                 response_size bytes and allocated by caller.
 */
struct wilco_ec_message {
	enum wilco_ec_msg_type type;
	UINT8 flags;
	size_t request_size;
	void* request_data;
	size_t response_size;
	void* response_data;
};

#endif /* __CROS_EC_REGS_H__ */