/* PLATFORM_HEADER 的替身（主机编 SDK 的 ash-ncp.c / ash-common.c 用） */
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define DECLARE_INTERRUPT_STATE
#define DISABLE_INTERRUPTS()
#define RESTORE_INTERRUPTS()
#ifndef SL_STATUS_OK
typedef uint32_t sl_status_t;
#define SL_STATUS_OK     0x0000u
#define SL_STATUS_EMPTY  0x0009u
#endif
