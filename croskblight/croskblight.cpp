#define DESCRIPTOR_DEF
#include "croskblight.h"
#include <acpiioct.h>
#include <ntstrsafe.h>

extern "C" NTSTATUS comm_init_lpc_mec(PCROSKBLIGHT_CONTEXT pDevice);
extern "C" NTSTATUS wilco_ec_mailbox(PCROSKBLIGHT_CONTEXT pDevice, struct wilco_ec_message* msg);

VOID
CrosKBLightS0ixNotifyCallback(
	PCROSKBLIGHT_CONTEXT pDevice,
	ULONG NotifyCode);

static ULONG CrosKBLightDebugLevel = 100;
static ULONG CrosKBLightDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

#define WILCO_EC_COMMAND_KBBL		0x75
#define WILCO_KBBL_MODE_FLAG_PWM	BIT(1)	/* Set brightness by percent. */
#define WILCO_KBBL_DEFAULT_BRIGHTNESS   0

enum wilco_kbbl_subcommand {
	WILCO_KBBL_SUBCMD_GET_FEATURES = 0x00,
	WILCO_KBBL_SUBCMD_GET_STATE = 0x01,
	WILCO_KBBL_SUBCMD_SET_STATE = 0x02,
};

/**
 * struct wilco_keyboard_leds_msg - Message to/from EC for keyboard LED control.
 * @command: Always WILCO_EC_COMMAND_KBBL.
 * @status: Set by EC to 0 on success, 0xFF on failure.
 * @subcmd: One of enum wilco_kbbl_subcommand.
 * @reserved3: Should be 0.
 * @mode: Bit flags for used mode, we want to use WILCO_KBBL_MODE_FLAG_PWM.
 * @reserved5to8: Should be 0.
 * @percent: Brightness in 0-100. Only meaningful in PWM mode.
 * @reserved10to15: Should be 0.
 */
#include <pshpack1.h>
struct wilco_keyboard_leds_msg {
	UINT8 command;
	UINT8 status;
	UINT8 subcmd;
	UINT8 reserved3;
	UINT8 mode;
	UINT8 reserved5to8[4];
	UINT8 percent;
	UINT8 reserved10to15[6];
};
#include <poppack.h>

/* Send a request, get a response, and check that the response is good. */
static NTSTATUS send_kbbl_msg(_In_ PCROSKBLIGHT_CONTEXT pDevice,
	struct wilco_keyboard_leds_msg* request,
	struct wilco_keyboard_leds_msg* response)
{
	struct wilco_ec_message msg;
	NTSTATUS status;

	memset(&msg, 0, sizeof(msg));
	msg.type = WILCO_EC_MSG_LEGACY;
	msg.request_data = request;
	msg.request_size = sizeof(*request);
	msg.response_data = response;
	msg.response_size = sizeof(*response);

	status = wilco_ec_mailbox(pDevice, &msg);
	if (!NT_SUCCESS(status)) {
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Failed sending keyboard LEDs command: 0x%x\n", status);
		return status;
	}

	return status;
}

static NTSTATUS set_kbbl(_In_ PCROSKBLIGHT_CONTEXT pDevice, UINT8 brightness)
{
	struct wilco_keyboard_leds_msg request;
	struct wilco_keyboard_leds_msg response;
	NTSTATUS status;

	memset(&request, 0, sizeof(request));
	request.command = WILCO_EC_COMMAND_KBBL;
	request.subcmd = WILCO_KBBL_SUBCMD_SET_STATE;
	request.mode = WILCO_KBBL_MODE_FLAG_PWM;
	request.percent = brightness;

	status = send_kbbl_msg(pDevice, &request, &response);
	if (!NT_SUCCESS(status))
		return status;

	if (response.status) {
		CrosKBLightPrint(DEBUG_LEVEL_INFO, DBG_INIT,
			"EC reported failure sending keyboard LEDs command: %d\n",
			response.status);
		return STATUS_IO_DEVICE_ERROR;
	}

	return status;
}

/**
 * kbbl_init() - Initialize the state of the keyboard backlight.
 * @ec: EC device to talk to.
 *
 * Gets the current brightness, ensuring that the BIOS already initialized the
 * backlight to PWM mode. If not in PWM mode, then the current brightness is
 * meaningless, so set the brightness to WILCO_KBBL_DEFAULT_BRIGHTNESS.
 *
 * Return: Final brightness of the keyboard, or negative error code on failure.
 */
static int kbbl_init(_In_ PCROSKBLIGHT_CONTEXT pDevice)
{
	struct wilco_keyboard_leds_msg request;
	struct wilco_keyboard_leds_msg response;
	NTSTATUS status;

	memset(&request, 0, sizeof(request));
	request.command = WILCO_EC_COMMAND_KBBL;
	request.subcmd = WILCO_KBBL_SUBCMD_GET_STATE;

	status = send_kbbl_msg(pDevice, &request, &response);
	if (!NT_SUCCESS(status))
		return status;

	if (response.status) {
		CrosKBLightPrint(DEBUG_LEVEL_INFO, DBG_INIT,
			"EC reported failure sending keyboard LEDs command: %d\n",
			response.status);
		return STATUS_IO_DEVICE_ERROR;
	}

	if (response.mode & WILCO_KBBL_MODE_FLAG_PWM) {
		if (pDevice->currentBrightness == 0)
			pDevice->currentBrightness = response.percent;
		return STATUS_SUCCESS;
	}

	status = set_kbbl(pDevice, WILCO_KBBL_DEFAULT_BRIGHTNESS);
	if (!NT_SUCCESS(status))
		return status;

	return STATUS_SUCCESS;
}

static NTSTATUS kbbl_exist(_In_ PCROSKBLIGHT_CONTEXT pDevice, BOOLEAN* exists)
{
	struct wilco_keyboard_leds_msg request;
	struct wilco_keyboard_leds_msg response;
	NTSTATUS status;

	memset(&request, 0, sizeof(request));
	request.command = WILCO_EC_COMMAND_KBBL;
	request.subcmd = WILCO_KBBL_SUBCMD_GET_FEATURES;

	status = send_kbbl_msg(pDevice, &request, &response);
	if (!NT_SUCCESS(status))
		return status;

	*exists = response.status != 0xFF;

	return STATUS_SUCCESS;
}

NTSTATUS
DriverEntry(
	__in PDRIVER_OBJECT  DriverObject,
	__in PUNICODE_STRING RegistryPath
	)
{
	NTSTATUS               status = STATUS_SUCCESS;
	WDF_DRIVER_CONFIG      config;
	WDF_OBJECT_ATTRIBUTES  attributes;

	CrosKBLightPrint(DEBUG_LEVEL_INFO, DBG_INIT,
		"Driver Entry");

	WDF_DRIVER_CONFIG_INIT(&config, CrosKBLightEvtDeviceAdd);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

	//
	// Create a framework driver object to represent our driver.
	//

	status = WdfDriverCreate(DriverObject,
		RegistryPath,
		&attributes,
		&config,
		WDF_NO_HANDLE
		);

	if (!NT_SUCCESS(status))
	{
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_INIT,
			"WdfDriverCreate failed with status 0x%x\n", status);
	}

	return status;
}

NTSTATUS
OnPrepareHardware(
	_In_  WDFDEVICE     FxDevice,
	_In_  WDFCMRESLIST  FxResourcesRaw,
	_In_  WDFCMRESLIST  FxResourcesTranslated
	)
	/*++

	Routine Description:

	This routine caches the SPB resource connection ID.

	Arguments:

	FxDevice - a handle to the framework device object
	FxResourcesRaw - list of translated hardware resources that
	the PnP manager has assigned to the device
	FxResourcesTranslated - list of raw hardware resources that
	the PnP manager has assigned to the device

	Return Value:

	Status

	--*/
{
	PCROSKBLIGHT_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	UNREFERENCED_PARAMETER(FxResourcesRaw);

	//
	// Parse the peripheral's resources.
	//

	ULONG resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);

	ULONG portsFound = 0;
	for (ULONG i = 0; i < resourceCount; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR pDescriptor;
		UCHAR Class;
		UCHAR Type;

		pDescriptor = WdfCmResourceListGetDescriptor(
			FxResourcesTranslated, i);

		switch (pDescriptor->Type)
		{
		case CmResourceTypePort:
			//
			// Look for I2C or SPI resource and save connection ID.
			//
			switch (portsFound) {
			case 0:
				pDevice->ecIoData.Start = pDescriptor->u.Port.Start;
				pDevice->ecIoData.Length = pDescriptor->u.Port.Length;
				break;
			case 1:
				pDevice->ecIoCommand.Start = pDescriptor->u.Port.Start;
				pDevice->ecIoCommand.Length = pDescriptor->u.Port.Length;
				break;
			case 2:
				pDevice->ecIoPacket.Start = pDescriptor->u.Port.Start;
				pDevice->ecIoPacket.Length = pDescriptor->u.Port.Length;
				break;
			default:
				break;
			}

			portsFound++;
			break;
		default:
			//
			// Ignoring all other resource types.
			//
			break;
		}
	}

	if (portsFound < 3) {
		status = STATUS_NOT_FOUND;
		return status;
	}

	pDevice->dataBuffer = ExAllocatePoolZero(NonPagedPool, sizeof(struct wilco_ec_response) + EC_MAILBOX_DATA_SIZE, CROSKBLIGHT_POOL_TAG);
	if (!pDevice->dataBuffer) {
		status = STATUS_NO_MEMORY;
		return status;
	}

	status = comm_init_lpc_mec(pDevice);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = kbbl_exist(pDevice, &pDevice->ledExists);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = WdfFdoQueryForInterface(FxDevice,
		&GUID_ACPI_INTERFACE_STANDARD2,
		(PINTERFACE)&pDevice->S0ixNotifyAcpiInterface,
		sizeof(ACPI_INTERFACE_STANDARD2),
		1,
		NULL);

	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = pDevice->S0ixNotifyAcpiInterface.RegisterForDeviceNotifications(
		pDevice->S0ixNotifyAcpiInterface.Context,
		(PDEVICE_NOTIFY_CALLBACK2)CrosKBLightS0ixNotifyCallback,
		pDevice);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	return status;
}

NTSTATUS
OnReleaseHardware(
	_In_  WDFDEVICE     FxDevice,
	_In_  WDFCMRESLIST  FxResourcesTranslated
	)
	/*++

	Routine Description:

	Arguments:

	FxDevice - a handle to the framework device object
	FxResourcesTranslated - list of raw hardware resources that
	the PnP manager has assigned to the device

	Return Value:

	Status

	--*/
{
	PCROSKBLIGHT_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	UNREFERENCED_PARAMETER(FxResourcesTranslated);

	if (pDevice->dataBuffer) {
		ExFreePoolWithTag(pDevice->dataBuffer, CROSKBLIGHT_POOL_TAG);
	}

	if (pDevice->S0ixNotifyAcpiInterface.Context) { //Used for S0ix notifications
		pDevice->S0ixNotifyAcpiInterface.UnregisterForDeviceNotifications(pDevice->S0ixNotifyAcpiInterface.Context);
	}

	return status;
}

NTSTATUS
OnD0Entry(
	_In_  WDFDEVICE               FxDevice,
	_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
	)
	/*++

	Routine Description:

	This routine allocates objects needed by the driver.

	Arguments:

	FxDevice - a handle to the framework device object
	FxPreviousState - previous power state

	Return Value:

	Status

	--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PCROSKBLIGHT_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	if (pDevice->ledExists) {
		status = kbbl_init(pDevice);
		if (!NT_SUCCESS(status)) {
			return status;
		}

		status = set_kbbl(pDevice, pDevice->currentBrightness);
	}

	return status;
}

NTSTATUS
OnD0Exit(
	_In_  WDFDEVICE               FxDevice,
	_In_  WDF_POWER_DEVICE_STATE  FxTargetState
	)
	/*++

	Routine Description:

	This routine destroys objects needed by the driver.

	Arguments:

	FxDevice - a handle to the framework device object
	FxTargetState - target power state

	Return Value:

	Status

	--*/
{
	PCROSKBLIGHT_CONTEXT pDevice = GetDeviceContext(FxDevice);

	if (FxTargetState != WdfPowerDeviceD3Final &&
		FxTargetState != WdfPowerDevicePrepareForHibernation) {
		if (pDevice->ledExists) {
			set_kbbl(pDevice, 0);
		}
	}

	return STATUS_SUCCESS;
}

VOID
CrosKBLightS0ixNotifyCallback(
	PCROSKBLIGHT_CONTEXT pDevice,
	ULONG NotifyCode) {
	if (NotifyCode) {
		OnD0Exit(pDevice->FxDevice, WdfPowerDeviceD3);
	}
	else {
		OnD0Entry(pDevice->FxDevice, WdfPowerDeviceD3);
	}
}

static void update_brightness(PCROSKBLIGHT_CONTEXT pDevice, BYTE brightness) {
	_CROSKBLIGHT_GETLIGHT_REPORT report;
	report.ReportID = REPORTID_KBLIGHT;
	report.Brightness = brightness;

	size_t bytesWritten;
	CrosKBLightProcessVendorReport(pDevice, &report, sizeof(report), &bytesWritten);
}

NTSTATUS
CrosKBLightEvtDeviceAdd(
	IN WDFDRIVER       Driver,
	IN PWDFDEVICE_INIT DeviceInit
	)
{
	NTSTATUS                      status = STATUS_SUCCESS;
	WDF_IO_QUEUE_CONFIG           queueConfig;
	WDF_OBJECT_ATTRIBUTES         attributes;
	WDFDEVICE                     device;
	WDF_INTERRUPT_CONFIG interruptConfig;
	WDFQUEUE                      queue;
	UCHAR                         minorFunction;
	PCROSKBLIGHT_CONTEXT               devContext;

	UNREFERENCED_PARAMETER(Driver);

	PAGED_CODE();

	CrosKBLightPrint(DEBUG_LEVEL_INFO, DBG_PNP,
		"CrosKBLightEvtDeviceAdd called\n");

	//
	// Tell framework this is a filter driver. Filter drivers by default are  
	// not power policy owners. This works well for this driver because
	// HIDclass driver is the power policy owner for HID minidrivers.
	//

	WdfFdoInitSetFilter(DeviceInit);

	{
		WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
		WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

		pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
		pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
		pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
		pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;

		WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);
	}

	//
	// Setup the device context
	//

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, CROSKBLIGHT_CONTEXT);

	//
	// Create a framework device object.This call will in turn create
	// a WDM device object, attach to the lower stack, and set the
	// appropriate flags and attributes.
	//

	status = WdfDeviceCreate(&DeviceInit, &attributes, &device);

	if (!NT_SUCCESS(status))
	{
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfDeviceCreate failed with status code 0x%x\n", status);

		return status;
	}

	{
		WDF_DEVICE_STATE deviceState;
		WDF_DEVICE_STATE_INIT(&deviceState);

		deviceState.NotDisableable = WdfFalse;
		WdfDeviceSetDeviceState(device, &deviceState);
	}

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);

	queueConfig.EvtIoInternalDeviceControl = CrosKBLightEvtInternalDeviceControl;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&queue
		);

	if (!NT_SUCCESS(status))
	{
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create manual I/O queue to take care of hid report read requests
	//

	devContext = GetDeviceContext(device);
	devContext->FxDevice = device;

	WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

	queueConfig.PowerManaged = WdfTrue;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->ReportQueue
		);

	if (!NT_SUCCESS(status))
	{
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &devContext->ecLock);
	if (!NT_SUCCESS(status))
	{
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfWaitLockCreate failed 0x%x\n", status);

		return status;
	}

	return status;
}

VOID
CrosKBLightEvtInternalDeviceControl(
	IN WDFQUEUE     Queue,
	IN WDFREQUEST   Request,
	IN size_t       OutputBufferLength,
	IN size_t       InputBufferLength,
	IN ULONG        IoControlCode
	)
{
	NTSTATUS            status = STATUS_SUCCESS;
	WDFDEVICE           device;
	PCROSKBLIGHT_CONTEXT     devContext;
	BOOLEAN             completeRequest = TRUE;

	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	device = WdfIoQueueGetDevice(Queue);
	devContext = GetDeviceContext(device);

	CrosKBLightPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
		"%s, Queue:0x%p, Request:0x%p\n",
		DbgHidInternalIoctlString(IoControlCode),
		Queue,
		Request
		);

	//
	// Please note that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl. So depending on the ioctl code, we will either
	// use retreive function or escape to WDM to get the UserBuffer.
	//

	switch (IoControlCode)
	{

	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
		//
		// Retrieves the device's HID descriptor.
		//
		status = CrosKBLightGetHidDescriptor(device, Request);
		break;

	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
		//
		//Retrieves a device's attributes in a HID_DEVICE_ATTRIBUTES structure.
		//
		status = CrosKBLightGetDeviceAttributes(Request);
		break;

	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		//
		//Obtains the report descriptor for the HID device.
		//
		status = CrosKBLightGetReportDescriptor(device, Request);
		break;

	case IOCTL_HID_GET_STRING:
		//
		// Requests that the HID minidriver retrieve a human-readable string
		// for either the manufacturer ID, the product ID, or the serial number
		// from the string descriptor of the device. The minidriver must send
		// a Get String Descriptor request to the device, in order to retrieve
		// the string descriptor, then it must extract the string at the
		// appropriate index from the string descriptor and return it in the
		// output buffer indicated by the IRP. Before sending the Get String
		// Descriptor request, the minidriver must retrieve the appropriate
		// index for the manufacturer ID, the product ID or the serial number
		// from the device extension of a top level collection associated with
		// the device.
		//
		status = CrosKBLightGetString(Request);
		break;

	case IOCTL_HID_WRITE_REPORT:
	case IOCTL_HID_SET_OUTPUT_REPORT:
		//
		//Transmits a class driver-supplied report to the device.
		//
		status = CrosKBLightWriteReport(devContext, Request);
		break;

	case IOCTL_HID_READ_REPORT:
	case IOCTL_HID_GET_INPUT_REPORT:
		//
		// Returns a report from the device into a class driver-supplied buffer.
		// 
		status = CrosKBLightReadReport(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_SET_FEATURE:
		//
		// This sends a HID class feature report to a top-level collection of
		// a HID class device.
		//
		status = CrosKBLightSetFeature(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_GET_FEATURE:
		//
		// returns a feature report associated with a top-level collection
		//
		status = CrosKBLightGetFeature(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_ACTIVATE_DEVICE:
		//
		// Makes the device ready for I/O operations.
		//
	case IOCTL_HID_DEACTIVATE_DEVICE:
		//
		// Causes the device to cease operations and terminate all outstanding
		// I/O requests.
		//
	default:
		status = STATUS_NOT_SUPPORTED;
		break;
	}

	if (completeRequest)
	{
		WdfRequestComplete(Request, status);

		CrosKBLightPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"%s completed, Queue:0x%p, Request:0x%p\n",
			DbgHidInternalIoctlString(IoControlCode),
			Queue,
			Request
			);
	}
	else
	{
		CrosKBLightPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"%s deferred, Queue:0x%p, Request:0x%p\n",
			DbgHidInternalIoctlString(IoControlCode),
			Queue,
			Request
			);
	}

	return;
}

NTSTATUS
CrosKBLightGetHidDescriptor(
	IN WDFDEVICE Device,
	IN WDFREQUEST Request
	)
{
	NTSTATUS            status = STATUS_SUCCESS;
	size_t              bytesToCopy = 0;
	WDFMEMORY           memory;

	UNREFERENCED_PARAMETER(Device);

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightGetHidDescriptor Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputMemory(Request, &memory);

	if (!NT_SUCCESS(status))
	{
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputMemory failed 0x%x\n", status);

		return status;
	}

	//
	// Use hardcoded "HID Descriptor" 
	//
	bytesToCopy = DefaultHidDescriptor.bLength;

	if (bytesToCopy == 0)
	{
		status = STATUS_INVALID_DEVICE_STATE;

		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"DefaultHidDescriptor is zero, 0x%x\n", status);

		return status;
	}

	status = WdfMemoryCopyFromBuffer(memory,
		0, // Offset
		(PVOID)&DefaultHidDescriptor,
		bytesToCopy);

	if (!NT_SUCCESS(status))
	{
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfMemoryCopyFromBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, bytesToCopy);

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightGetHidDescriptor Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
CrosKBLightGetReportDescriptor(
	IN WDFDEVICE Device,
	IN WDFREQUEST Request
	)
{
	NTSTATUS            status = STATUS_SUCCESS;
	ULONG_PTR           bytesToCopy;
	WDFMEMORY           memory;

	UNREFERENCED_PARAMETER(Device);

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightGetReportDescriptor Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputMemory(Request, &memory);
	if (!NT_SUCCESS(status))
	{
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputMemory failed 0x%x\n", status);

		return status;
	}

	//
	// Use hardcoded Report descriptor
	//
	bytesToCopy = DefaultHidDescriptor.DescriptorList[0].wReportLength;

	if (bytesToCopy == 0)
	{
		status = STATUS_INVALID_DEVICE_STATE;

		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"DefaultHidDescriptor's reportLength is zero, 0x%x\n", status);

		return status;
	}

	status = WdfMemoryCopyFromBuffer(memory,
		0,
		(PVOID)DefaultReportDescriptor,
		bytesToCopy);
	if (!NT_SUCCESS(status))
	{
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfMemoryCopyFromBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, bytesToCopy);

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightGetReportDescriptor Exit = 0x%x\n", status);

	return status;
}


NTSTATUS
CrosKBLightGetDeviceAttributes(
	IN WDFREQUEST Request
	)
{
	NTSTATUS                 status = STATUS_SUCCESS;
	PHID_DEVICE_ATTRIBUTES   deviceAttributes = NULL;

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightGetDeviceAttributes Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputBuffer(Request,
		sizeof(HID_DEVICE_ATTRIBUTES),
		(PVOID *)&deviceAttributes,
		NULL);
	if (!NT_SUCCESS(status))
	{
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Set USB device descriptor
	//

	deviceAttributes->Size = sizeof(HID_DEVICE_ATTRIBUTES);
	deviceAttributes->VendorID = CROSKBLIGHT_VID;
	deviceAttributes->ProductID = CROSKBLIGHT_PID;
	deviceAttributes->VersionNumber = CROSKBLIGHT_VERSION;

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, sizeof(HID_DEVICE_ATTRIBUTES));

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightGetDeviceAttributes Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
CrosKBLightGetString(
	IN WDFREQUEST Request
	)
{

	NTSTATUS status = STATUS_SUCCESS;
	PWSTR pwstrID;
	size_t lenID;
	WDF_REQUEST_PARAMETERS params;
	void *pStringBuffer = NULL;

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightGetString Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	switch ((ULONG_PTR)params.Parameters.DeviceIoControl.Type3InputBuffer & 0xFFFF)
	{
	case HID_STRING_ID_IMANUFACTURER:
		pwstrID = L"CrosKBLight.\0";
		break;

	case HID_STRING_ID_IPRODUCT:
		pwstrID = L"MaxTouch Touch Screen\0";
		break;

	case HID_STRING_ID_ISERIALNUMBER:
		pwstrID = L"123123123\0";
		break;

	default:
		pwstrID = NULL;
		break;
	}

	lenID = pwstrID ? wcslen(pwstrID)*sizeof(WCHAR) + sizeof(UNICODE_NULL) : 0;

	if (pwstrID == NULL)
	{

		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"CrosKBLightGetString Invalid request type\n");

		status = STATUS_INVALID_PARAMETER;

		return status;
	}

	status = WdfRequestRetrieveOutputBuffer(Request,
		lenID,
		&pStringBuffer,
		&lenID);

	if (!NT_SUCCESS(status))
	{

		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"CrosKBLightGetString WdfRequestRetrieveOutputBuffer failed Status 0x%x\n", status);

		return status;
	}

	RtlCopyMemory(pStringBuffer, pwstrID, lenID);

	WdfRequestSetInformation(Request, lenID);

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightGetString Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
CrosKBLightWriteReport(
	IN PCROSKBLIGHT_CONTEXT DevContext,
	IN WDFREQUEST Request
	)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;
	size_t bytesWritten = 0;

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightWriteReport Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"CrosKBLightWriteReport Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"CrosKBLightWriteReport No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			case REPORTID_KBLIGHT: {
				CrosKBLightSettingsReport *pReport = (CrosKBLightSettingsReport *)transferPacket->reportBuffer;

				int reg = pReport->SetBrightness;
				int val = pReport->Brightness;

				if (reg == 0) {
					int brightness = DevContext->currentBrightness;
					update_brightness(DevContext, brightness);
				}
				else if (reg == 1) {
					DevContext->currentBrightness = val;
					if (DevContext->ledExists) {
						set_kbbl(DevContext, DevContext->currentBrightness);
					}
				}
				break;
			}
			default:
				CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"CrosKBLightWriteReport Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightWriteReport Exit = 0x%x\n", status);

	return status;

}

NTSTATUS
CrosKBLightProcessVendorReport(
	IN PCROSKBLIGHT_CONTEXT DevContext,
	IN PVOID ReportBuffer,
	IN ULONG ReportBufferLen,
	OUT size_t* BytesWritten
	)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDFREQUEST reqRead;
	PVOID pReadReport = NULL;
	size_t bytesReturned = 0;

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightProcessVendorReport Entry\n");

	status = WdfIoQueueRetrieveNextRequest(DevContext->ReportQueue,
		&reqRead);

	if (NT_SUCCESS(status))
	{
		status = WdfRequestRetrieveOutputBuffer(reqRead,
			ReportBufferLen,
			&pReadReport,
			&bytesReturned);

		if (NT_SUCCESS(status))
		{
			//
			// Copy ReportBuffer into read request
			//

			if (bytesReturned > ReportBufferLen)
			{
				bytesReturned = ReportBufferLen;
			}

			RtlCopyMemory(pReadReport,
				ReportBuffer,
				bytesReturned);

			//
			// Complete read with the number of bytes returned as info
			//

			WdfRequestCompleteWithInformation(reqRead,
				status,
				bytesReturned);

			CrosKBLightPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
				"CrosKBLightProcessVendorReport %d bytes returned\n", bytesReturned);

			//
			// Return the number of bytes written for the write request completion
			//

			*BytesWritten = bytesReturned;

			CrosKBLightPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
				"%s completed, Queue:0x%p, Request:0x%p\n",
				DbgHidInternalIoctlString(IOCTL_HID_READ_REPORT),
				DevContext->ReportQueue,
				reqRead);
		}
		else
		{
			CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"WdfRequestRetrieveOutputBuffer failed Status 0x%x\n", status);
		}
	}
	else
	{
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfIoQueueRetrieveNextRequest failed Status 0x%x\n", status);
	}

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightProcessVendorReport Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
CrosKBLightReadReport(
	IN PCROSKBLIGHT_CONTEXT DevContext,
	IN WDFREQUEST Request,
	OUT BOOLEAN* CompleteRequest
	)
{
	NTSTATUS status = STATUS_SUCCESS;

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightReadReport Entry\n");

	//
	// Forward this read request to our manual queue
	// (in other words, we are going to defer this request
	// until we have a corresponding write request to
	// match it with)
	//

	status = WdfRequestForwardToIoQueue(Request, DevContext->ReportQueue);

	if (!NT_SUCCESS(status))
	{
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestForwardToIoQueue failed Status 0x%x\n", status);
	}
	else
	{
		*CompleteRequest = FALSE;
	}

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightReadReport Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
CrosKBLightSetFeature(
	IN PCROSKBLIGHT_CONTEXT DevContext,
	IN WDFREQUEST Request,
	OUT BOOLEAN* CompleteRequest
	)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;
	CrosKBLightFeatureReport* pReport = NULL;

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightSetFeature Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"CrosKBLightSetFeature Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"CrosKBLightWriteReport No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			default:

				CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"CrosKBLightSetFeature Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightSetFeature Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
CrosKBLightGetFeature(
	IN PCROSKBLIGHT_CONTEXT DevContext,
	IN WDFREQUEST Request,
	OUT BOOLEAN* CompleteRequest
	)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightGetFeature Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.OutputBufferLength < sizeof(HID_XFER_PACKET))
	{
		CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"CrosKBLightGetFeature Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"CrosKBLightGetFeature No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			default:

				CrosKBLightPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"CrosKBLightGetFeature Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	CrosKBLightPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"CrosKBLightGetFeature Exit = 0x%x\n", status);

	return status;
}

PCHAR
DbgHidInternalIoctlString(
	IN ULONG IoControlCode
	)
{
	switch (IoControlCode)
	{
	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
		return "IOCTL_HID_GET_DEVICE_DESCRIPTOR";
	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		return "IOCTL_HID_GET_REPORT_DESCRIPTOR";
	case IOCTL_HID_READ_REPORT:
		return "IOCTL_HID_READ_REPORT";
	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
		return "IOCTL_HID_GET_DEVICE_ATTRIBUTES";
	case IOCTL_HID_WRITE_REPORT:
		return "IOCTL_HID_WRITE_REPORT";
	case IOCTL_HID_SET_FEATURE:
		return "IOCTL_HID_SET_FEATURE";
	case IOCTL_HID_GET_FEATURE:
		return "IOCTL_HID_GET_FEATURE";
	case IOCTL_HID_GET_STRING:
		return "IOCTL_HID_GET_STRING";
	case IOCTL_HID_ACTIVATE_DEVICE:
		return "IOCTL_HID_ACTIVATE_DEVICE";
	case IOCTL_HID_DEACTIVATE_DEVICE:
		return "IOCTL_HID_DEACTIVATE_DEVICE";
	case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
		return "IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST";
	case IOCTL_HID_SET_OUTPUT_REPORT:
		return "IOCTL_HID_SET_OUTPUT_REPORT";
	case IOCTL_HID_GET_INPUT_REPORT:
		return "IOCTL_HID_GET_INPUT_REPORT";
	default:
		return "Unknown IOCTL";
	}
}
