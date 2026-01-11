#pragma once

#include <string.h>

#if defined(STM32F103xB)
#include "stm32f1xx_hal.h"
#elif defined(STM32F411xE)
#include "stm32f4xx_hal.h"
#endif
#include "flashpages.h"
#include "LOGGER.h"
#include "types.h"
#include "minmax.h"
#include "bithelper.h"
#include <stdbool.h>

#define MAX_ENTRY_LABEL_LENGTH 64
#define MAX_ENTRY_VALUE_LENGTH 2048  // For long values like private keys
#define MAX_ENTRY_COMMENT_LENGTH 64

typedef struct {
	char entry[MAX_ENTRY_LABEL_LENGTH];
	char comment[MAX_ENTRY_COMMENT_LENGTH];
	char* default_value;  // Pointer to value string (not copied, must be static/const)
	bool(*validate)(u8 str[]);
	void(*update)(u8 str[]);
	void(*print)(char *buffer, size_t buffer_size);
} FILE_ENTRY;

struct disk {
	void(*init)(void);
	void(*load_from_flash)(void);
	void(*process)(void);  // Call from main loop to flush deferred flash writes
	u8(*Disk_SecWrite)(u8* pbuffer, u32 diskaddr, u32 length);
	void(*Disk_SecRead)(u8* pbuffer, u32 disk_addr);
	u32(*get_sector_size)(void);
	u32(*get_sector_count)(void);
	bool(*register_entry)(char* entry, char* default_val, char* comment, void* validator, void* updater, void* printer);
};

extern const struct disk Disk;