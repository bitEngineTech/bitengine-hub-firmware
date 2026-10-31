#pragma once
#include "sdk_plat.h"
#include "crc.h"
uint16_t halCommonGetInt16uMillisecondTick(void);
uint32_t halCommonGetInt32uMillisecondTick(void);
void     halResetWatchdog(void);
void     halReboot(void);
void     simulatedTimePasses(void);
