#include "crosfingerprint.h"
#include "ec_commands.h"

static ULONG CrosFPDebugLevel = 0;
static ULONG CrosFPDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

NTSTATUS CrosFPSensorStatus
(
	_In_ PCROSFP_CONTEXT devContext,
	PWINBIO_SENSOR_STATUS sensorMode
)
{
	struct ec_params_fp_mode p;
	struct ec_response_fp_mode r;
	NTSTATUS status;

	p.mode = FP_MODE_DONT_CHANGE;

	status = cros_ec_command(devContext, EC_CMD_FP_MODE, 0, &p, sizeof(p), &r, sizeof(r));
	if (!NT_SUCCESS(status)) {
		return status;
	}

	CrosFPPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
		"FP Mode: 0x%x, Calibrated? %d\n", r.mode, devContext->DeviceCalibrated);

	if (!devContext->DeviceCalibrated) {
		*sensorMode = WINBIO_SENSOR_NOT_CALIBRATED;
	}
	else {
		*sensorMode = WINBIO_SENSOR_READY;
		if (r.mode & FP_MODE_SENSOR_MAINTENANCE)
			*sensorMode = WINBIO_SENSOR_BUSY;
		else if (r.mode & FP_MODE_ANY_CAPTURE)
			*sensorMode = WINBIO_SENSOR_READY;
	}
	return status;
}

#define NT_RETURN_IF(status, condition)                \
	do {                                           \
		const NTSTATUS __statusRet = (status); \
		if((condition)) {                      \
			return __statusRet;            \
		}                                      \
	} while((void)0, 0)

NTSTATUS CrosECIoctlXCmd(_In_ PCROSFP_CONTEXT pDevice, _In_ WDFREQUEST Request) {
	int i = 0;
	while (!pDevice->DeviceReady) {
		Sleep(200);
		i++;
	}
	CrosFPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
		"Ready after %d iters\n", i);

	PCROSEC_COMMAND cmd;
	size_t cmdLen;
	NTSTATUS status = WdfRequestRetrieveInputBuffer(Request, sizeof(*cmd), (PVOID*)&cmd, &cmdLen);
	if (!NT_SUCCESS(status)) {
		CrosFPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL, "Failed to get input buffer\n");
		return status;
	}

	PCROSEC_COMMAND outCmd;
	size_t outLen;
	status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*cmd), &outCmd, &outLen);
	if (!NT_SUCCESS(status)) {
		CrosFPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL, "Failed to get output buffer\n");
		return status;
	}

	// User tried to send/receive too much data
	NT_RETURN_IF(STATUS_BUFFER_OVERFLOW, cmdLen > (sizeof(CROSEC_COMMAND) + pDevice->MaxOutsize));
	NT_RETURN_IF(STATUS_BUFFER_OVERFLOW, outLen > (sizeof(CROSEC_COMMAND) + pDevice->MaxInsize));
	// User tried to send/receive more bytes than they offered in storage
	NT_RETURN_IF(STATUS_BUFFER_TOO_SMALL, cmdLen < (sizeof(CROSEC_COMMAND) + cmd->OutSize));
	NT_RETURN_IF(STATUS_BUFFER_TOO_SMALL, outLen < (sizeof(CROSEC_COMMAND) + cmd->InSize));

	RtlCopyMemory(outCmd, cmd, sizeof(*cmd)); //Copy header

	NTSTATUS cmdStatus = cros_ec_command(pDevice, cmd->Command, cmd->Version, outCmd->Data, cmd->OutSize, cmd->Data, cmd->InSize);

	if (!NT_SUCCESS(cmdStatus)) {
		cmd->Result = cmdStatus;
	} 
	else {
		cmd->Result = 0;  // 0 = SUCCESS
	}

	int requiredReplySize = sizeof(CROSEC_COMMAND) + cmd->InSize;
	if (requiredReplySize > outLen) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	WdfRequestSetInformation(Request, requiredReplySize);
	return STATUS_SUCCESS;
}