#if !defined(_CROSKBLIGHT_COMMON_H_)
#define _CROSKBLIGHT_COMMON_H_

//
//These are the device attributes returned by vmulti in response
// to IOCTL_HID_GET_DEVICE_ATTRIBUTES.
//

#define CROSKBLIGHT_PID              0x0002
#define CROSKBLIGHT_VID              0x18D1
#define CROSKBLIGHT_VERSION          0x0001

//
// These are the report ids
//

#define REPORTID_KBLIGHT       0x01

#pragma pack(1)
typedef struct _CROSKBLIGHT_FEATURE_REPORT
{

	BYTE      ReportID;

	BYTE      DeviceMode;

	BYTE      DeviceIdentifier;

} CrosKBLightFeatureReport;

typedef struct _CROSKBLIGHT_MAXCOUNT_REPORT
{

	BYTE         ReportID;

	BYTE         MaximumCount;

} CrosKBLightMaxCountReport;
#pragma pack()

#pragma pack(1)
typedef struct _CROSKBLIGHT_GETLIGHT_REPORT
{

	BYTE        ReportID;

	BYTE		Brightness;

} CrosKBLightGetLightReport;
#pragma pack()

#pragma pack(1)
typedef struct _CROSKBLIGHT_SETTINGS_REPORT
{

	BYTE        ReportID;

	BYTE		SetBrightness;

	BYTE		Brightness;

} CrosKBLightSettingsReport;
#pragma pack()

#endif
#pragma once
