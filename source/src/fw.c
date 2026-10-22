#include <swilib.h>
#include <string.h>
#include "fw.h"

int fw_model = FW_UNKNOWN;

/* Prologues of BTA_Accessor_New and enip_module_init (identical code, unique in both dumps) */
static const uint8_t sig_bta[] = {0xfe, 0xb5, 0x07, 0x1c, 0x0e, 0x1c, 0x15, 0x1c, 0x74, 0x4c};
static const uint8_t sig_enip[] = {0xf8, 0xb5, 0x06, 0xaf, 0x04, 0x1c, 0x9c, 0x46, 0x00, 0x23};

static int at(uint32_t bta, uint32_t enip) {
	return !memcmp((const void *)bta, sig_bta, sizeof(sig_bta)) &&
		   !memcmp((const void *)enip, sig_enip, sizeof(sig_enip));
}

int fw_detect(void) {
	if (at(0xA07DD994u, 0xA09798AEu))
		fw_model = FW_EL71;
	else if (at(0xA07C5F84u, 0xA096FB2Au))
		fw_model = FW_E71;
	else
		fw_model = FW_UNKNOWN;
	return fw_model;
}

const char *fw_model_name(void) {
	return fw_model == FW_EL71 ? "EL71" : fw_model == FW_E71 ? "E71" : "unknown";
}
