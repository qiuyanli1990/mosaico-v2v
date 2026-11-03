#pragma once

#include "tusb_config_uac.h"

/* This audio function contains separate speaker and microphone AS interfaces. */
#undef CFG_TUD_AUDIO_FUNC_1_N_AS_INT
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT 2
