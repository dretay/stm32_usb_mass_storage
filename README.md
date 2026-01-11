# STM32 USB Mass Storage Library

A library that presents STM32 internal flash as a USB Mass Storage device with a virtual FAT12 filesystem. When plugged in, the device mounts as a USB drive containing a `CONFIG.TXT` file that can be edited to configure the device.

## Supported Devices

- STM32F103xB (Blue Pill)
- STM32F411xE (Black Pill)

## How It Works

The library creates a virtual FAT12 filesystem in RAM that maps to internal flash storage. When the host computer reads/writes to the USB drive:

1. **Read operations**: The library serves FAT12 filesystem structures (boot sector, FAT tables, root directory) and file contents from RAM/flash
2. **Write operations**: When `CONFIG.TXT` is modified and saved, the library validates each entry, updates internal state via callbacks, and persists changes to flash

### Virtual Disk Layout

```
Sector 0       : Boot sector (FAT12 BPB)
Sectors 1-7    : Reserved
Sectors 8-19   : FAT1 (12 sectors)
Sectors 20-31  : FAT2 (12 sectors)
Sectors 32-63  : Root directory (32 sectors, 512 entries)
Sectors 64+    : Data area (file contents)
```

Total virtual disk size: 4096 sectors x 512 bytes = 2MB

### Flash Storage

The library stores filesystem metadata and file contents in a dedicated flash region defined by linker symbols:
- `_user_data_start` - Start address of user data region
- `_user_data_size` - Size of user data region

For STM32F411, Sector 7 (0x08060000, 128KB) is typically used.

## Integration Guide

### 1. Add Library to Project

Add the library paths to your build:
- Include path: `libraries/stm32_usb_mass_storage/inc`
- Source files: `libraries/stm32_usb_mass_storage/src/disk.c`

### 2. Configure USB Device (CubeMX)

1. Enable USB_OTG_FS in Device Only mode
2. Enable USB_DEVICE middleware
3. Set Class to Mass Storage Class (MSC)
4. Generate code

### 3. Modify Linker Script

Add a dedicated flash section for user data. Example for STM32F411 (in `.ld` file):

```ld
MEMORY
{
  RAM    (xrw)    : ORIGIN = 0x20000000, LENGTH = 128K
  FLASH  (rx)     : ORIGIN = 0x08000000, LENGTH = 384K
  USER_DATA (rw)  : ORIGIN = 0x08060000, LENGTH = 128K
}

SECTIONS
{
  /* ... existing sections ... */

  .user_data :
  {
    . = ALIGN(4);
    _user_data_start = .;
    . = . + 0x20000;  /* 128KB */
    _user_data_size = . - _user_data_start;
  } > USER_DATA
}
```

### 4. Provide LOGGER.h

The library expects a `LOGGER.h` header with logging macros:

```c
#pragma once

// Define these macros for your logging system, or leave empty
#define app_log_debug(fmt, ...)  // printf(fmt, ##__VA_ARGS__)
#define app_log_info(fmt, ...)
#define app_log_warn(fmt, ...)
#define app_log_error(fmt, ...)
```

### 5. Modify usbd_storage_if.c

Replace the generated USB storage callbacks:

```c
/* USER CODE BEGIN INCLUDE */
#include "disk.h"
/* USER CODE END INCLUDE */

/* In STORAGE_Init_FS */
int8_t STORAGE_Init_FS(uint8_t lun)
{
  UNUSED(lun);
  Disk.init();
  return (USBD_OK);
}

/* In STORAGE_GetCapacity_FS */
int8_t STORAGE_GetCapacity_FS(uint8_t lun, uint32_t *block_num, uint16_t *block_size)
{
  UNUSED(lun);
  *block_num  = Disk.get_sector_count();
  *block_size = Disk.get_sector_size();
  return (USBD_OK);
}

/* In STORAGE_Read_FS */
int8_t STORAGE_Read_FS(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len)
{
  UNUSED(lun);
  for (uint16_t i = 0; i < blk_len; i++) {
    Disk.Disk_SecRead(buf + (i * STORAGE_BLK_SIZ), blk_addr + i);
  }
  return (USBD_OK);
}

/* In STORAGE_Write_FS */
int8_t STORAGE_Write_FS(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len)
{
  UNUSED(lun);
  Disk.Disk_SecWrite(buf, blk_addr, blk_len);
  return (USBD_OK);
}
```

### 6. Register Configuration Entries

Register entries **before** USB initialization (before `MX_USB_DEVICE_Init()`):

```c
#include "disk.h"

// Validator: returns true if value is valid
bool brightness_validator(uint8_t str[]) {
    int val = atoi((char*)str);
    return (val >= 0 && val <= 100);
}

// Updater: called when valid value is written
void brightness_updater(uint8_t str[]) {
    int val = atoi((char*)str);
    set_brightness(val);
}

// Printer: formats the current value for display
void brightness_printer(char* buffer) {
    sprintf(buffer, "brightness=%d", get_brightness());
}

void app_config(void) {
    // Register before Disk.init() is called
    Disk.register_entry(
        "brightness",           // Entry name (appears as "brightness=VALUE")
        "50",                   // Default value
        "#(0~100)",            // Comment shown after value
        brightness_validator,
        brightness_updater,
        brightness_printer
    );
}

int main(void) {
    HAL_Init();
    SystemClock_Config();

    // Register entries first
    app_config();

    // Then initialize USB (which calls Disk.init())
    MX_USB_DEVICE_Init();

    while (1) {
        // IMPORTANT: Call Disk.process() to flush deferred flash writes
        Disk.process();
    }
}
```

## API Reference

### Disk Interface

```c
extern const struct disk Disk;

struct disk {
    void (*init)(void);
    // Initialize the virtual disk, load from flash

    void (*process)(void);
    // Call from main loop - flushes deferred flash writes after 500ms idle

    u8 (*Disk_SecWrite)(u8* pbuffer, u32 diskaddr, u32 length);
    // Write sectors to virtual disk

    void (*Disk_SecRead)(u8* pbuffer, u32 disk_addr);
    // Read a sector from virtual disk

    u32 (*get_sector_size)(void);
    // Returns 512 (bytes per sector)

    u32 (*get_sector_count)(void);
    // Returns 4096 (total sectors)

    bool (*register_entry)(char* entry, char* default_val, char* comment,
                          void* validator, void* updater, void* printer);
    // Register a configuration entry (max 4 entries)
};
```

### Deferred Flash Writes

To avoid slow USB responses, flash writes are deferred until 500ms after the last write operation. This batches multiple USB writes into a single flash erase/write cycle. **You must call `Disk.process()` from your main loop** for this to work.

### FILE_ENTRY Callbacks

```c
typedef struct {
    char entry[32];           // Entry name
    char comment[32];         // Comment text
    char default_line[96];    // Default "name=value" string
    bool (*validate)(u8 str[]);   // Validate new value
    void (*update)(u8 str[]);     // Apply new value
    void (*print)(char* buffer);  // Format current value
} FILE_ENTRY;
```

## CONFIG.TXT Format

The generated file looks like:

```
brightness=50	#(0~100)
volume=75	#(0~100)
mode=1	#(1~3)
```

Each line contains:
- Entry name and equals sign
- Current value
- Tab character
- Comment (from registration)
- CRLF line ending

## Limitations

- Maximum 4 configuration entries
- Entry names max 32 characters
- Total file content limited to ~254 characters
- FAT12 filesystem (small file support only)
- Single file (CONFIG.TXT) supported

## Troubleshooting

### Device not mounting
- Verify USB descriptors are correct
- Check that `Disk.init()` is called in `STORAGE_Init_FS`
- Ensure linker symbols `_user_data_start` and `_user_data_size` are defined

### CONFIG.TXT is empty
- Entries must be registered **before** `Disk.init()` is called
- `Disk.init()` is called from `STORAGE_Init_FS` during USB enumeration

### Changes not persisting
- Check flash region is not write-protected
- Verify linker script reserves correct flash sector
- For STM32F411, ensure using Sector 7 (or another unused sector)

### Erase flash (fresh start)

If CONFIG.TXT appears empty or contains stale data, erase the user data flash sector and re-flash:

**Using J-Link:**
```bash
echo -e "erase 0x08060000 0x08080000\nexit" > /tmp/jlink_erase.jlink
JLinkExe -device STM32F411CE -if SWD -speed 4000 -autoconnect 1 -CommandFile /tmp/jlink_erase.jlink
```

**Using ST-Link:**
```bash
st-flash erase
```
(Note: st-flash erase clears all flash - re-flash your firmware afterward)

After erasing, re-flash your firmware. On next USB connection, `Disk.init()` will detect no valid data and create a fresh CONFIG.TXT with default values.

## File Structure

```
stm32_usb_mass_storage/
├── inc/
│   ├── disk.h         # Public API
│   ├── flashpages.h   # Flash sector definitions
│   ├── types.h        # Integer type aliases
│   ├── bithelper.h    # Bit manipulation macros
│   └── minmax.h       # MIN/MAX macros
├── src/
│   └── disk.c         # Implementation
└── README.md
```
