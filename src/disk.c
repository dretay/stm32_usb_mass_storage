#include "disk.h"

// Linker symbols for user data flash region (defined in linker script)
// Declared as char arrays to avoid "reading N bytes from 4-byte object" warnings
extern char _user_data_start[];
extern char _user_data_size[];

// Use linker symbols instead of hardcoded addresses
#define APP_BASE ((uint32_t)_user_data_start)
#define APP_SIZE ((uint32_t)_user_data_size)

// constants
#define SECTOR_SIZE 512
#define SECTOR_CNT 4096
#define FILE_ENTRY_CNT 8
#define FILE_ROW_CNT 2048  // Max length of a single config line (for private keys)
#define FILE_CHAR_CNT 8192 // Max total file content size
// Data area starts at sector 64 (cluster 2)
#define DATA_FIRST_SECTOR 64
#define SECTOR_TO_CLUSTER(s) ((s) - DATA_FIRST_SECTOR + 2)
static uc32 VOLUME = 0x40DD8D18;
static const u8 fat_data[] = {0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static u8 CONFIG_FILENAME[] = "CONFIG  TXT";

// globals - increased buffer for larger config files
static u8 disk_buffer[0x4000]; // 16KB buffer for larger configs
static u32 disk_buffer_temp[(SECTOR_SIZE + 32 + 28) / 4];
static u8 *pdisk_buffer_temp = (u8 *)&disk_buffer_temp[0];
static u8 file_buffer[SECTOR_SIZE * 16]; // 8KB for reading file content
static u8 page_dirty_mask[32];			 // Increased for larger buffer
static u32 entry_usage_mask = 0;

// Static parsing buffer (too large for stack)
static u8 parse_buffer[FILE_ENTRY_CNT][FILE_ROW_CNT];
static u8 file_content_buffer[FILE_CHAR_CNT];

// Deferred flash write state
static uint32_t last_write_tick = 0;
static bool pending_flash_write = false;
#define FLASH_WRITE_DELAY_MS 500

static FILE_ENTRY entries[FILE_ENTRY_CNT];

// pointers - layout in disk_buffer (16KB total)
//  0x000-0x1FF: FAT1 (512 bytes)
//  0x200-0x3FF: FAT2 (512 bytes)
//  0x400-0x5FF: ROOT_SECTOR (512 bytes)
//  0x600-0x3FFF: FILE_SECTOR (~14KB for file data)
static u8 *FAT1_SECTOR = &disk_buffer[0x000];
static u8 *FAT2_SECTOR = &disk_buffer[0x200];
static u8 *ROOT_SECTOR = &disk_buffer[0x400];
static u8 *VOLUME_BASE = &disk_buffer[0x416];
static u8 *OTHER_FILES = &disk_buffer[0x420];
static u8 *FILE_SECTOR = &disk_buffer[0x600];
#define FILE_SECTOR_SIZE (0x4000 - 0x600) // Available space for file data

uc8 BOOT_SEC[SECTOR_SIZE] = {
	0xEB, 0x3C, 0x90,									   // code to jump to the bootstrap code
	'm', 'k', 'd', 'o', 's', 'f', 's', 0x00,			   // OEM ID
	0x00, 0x02,											   // bytes per sector
	0x01,												   // sectors per cluster
	0x08, 0x00,											   // # of reserved sectors
	0x02,												   // FAT copies
	0x00, 0x02,											   // root entries
	0x50, 0x00,											   // total number of sectors
	0xF8,												   // media descriptor (0xF8 = Fixed disk)
	0x0c, 0x00,											   // sectors per FAT
	0x01, 0x00,											   // sectors per track
	0x01, 0x00,											   // number of heads
	0x00, 0x00, 0x00, 0x00,								   // hidden sectors
	0x00, 0x00, 0x00, 0x00,								   // large number of sectors
	0x00,												   // drive number
	0x00,												   // reserved
	0x29,												   // extended boot signature
	0xA2, 0x98, 0xE4, 0x6C,								   // volume serial number
	'R', 'A', 'M', 'D', 'I', 'S', 'K', ' ', ' ', ' ', ' ', // volume label
	'F', 'A', 'T', '1', '2', ' ', ' ', ' '				   // filesystem type
};

// util functions
static void Upper(u8 *str, u16 len)
{
	u16 i;
	for (i = 0; i < len; i++)
	{
		if (str[i] >= 'a' && str[i] <= 'z')
		{
			str[i] -= 32;
		}
	}
}

// flash interface functions
#if defined(STM32F411xE)
static uint32_t GetSectorNumber(uint32_t Address)
{
	if (Address < ADDR_FLASH_SECTOR_1)
		return FLASH_SECTOR_0;
	else if (Address < ADDR_FLASH_SECTOR_2)
		return FLASH_SECTOR_1;
	else if (Address < ADDR_FLASH_SECTOR_3)
		return FLASH_SECTOR_2;
	else if (Address < ADDR_FLASH_SECTOR_4)
		return FLASH_SECTOR_3;
	else if (Address < ADDR_FLASH_SECTOR_5)
		return FLASH_SECTOR_4;
	else if (Address < ADDR_FLASH_SECTOR_6)
		return FLASH_SECTOR_5;
	else if (Address < ADDR_FLASH_SECTOR_7)
		return FLASH_SECTOR_6;
	else
		return FLASH_SECTOR_7;
}
#endif

static HAL_StatusTypeDef erase_flash_page(u32 Address)
{
	unsigned long page_error;
	static FLASH_EraseInitTypeDef EraseInitStruct;
	HAL_StatusTypeDef status;

#if defined(STM32F103xB)
	EraseInitStruct.TypeErase = FLASH_TYPEERASE_PAGES;
	EraseInitStruct.PageAddress = Address;
	EraseInitStruct.NbPages = 1;
#elif defined(STM32F411xE)
	EraseInitStruct.TypeErase = FLASH_TYPEERASE_SECTORS;
	EraseInitStruct.Sector = GetSectorNumber(Address);
	EraseInitStruct.NbSectors = 1;
	EraseInitStruct.VoltageRange = FLASH_VOLTAGE_RANGE_3; // 2.7V - 3.6V
#endif

	status = HAL_FLASHEx_Erase(&EraseInitStruct, &page_error);
	if (status != HAL_OK)
	{
		app_log_error("Unable to erase flash page: %d", status);
	}
	return status;
}
static HAL_StatusTypeDef write_flash_halfword(u32 Address, u16 Data)
{
	HAL_StatusTypeDef status;

#if defined(STM32F103xB)
	// https://stackoverflow.com/questions/28498191/cant-write-to-flash-memory-after-erase
	// grr... looks like by default you can only write once to flash w/o clearing this...
	CLEAR_BIT(FLASH->CR, (FLASH_CR_PG));
	status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, Address, Data);
#elif defined(STM32F411xE)
	// F4 series uses word programming more efficiently, but halfword still works
	status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, Address, Data);
#endif

	if (status != HAL_OK)
	{
		app_log_error("Unable to write halfword: %d", status);
	}
	return status;
}
u8 rewrite_dirty_flash_pages(void)
{
	u32 i, j;
	u8 result;
	u16 *f_buff;
	HAL_StatusTypeDef status;

	status = HAL_FLASH_Unlock();
	if (status != HAL_OK)
	{
		app_log_error("Unable to unlock flash: %d", status);
	}

#if defined(STM32F103xB)
	// F1: Multiple 1KB pages
	for (i = 0; i < 16; i++)
	{
		if (page_dirty_mask[i])
		{
			page_dirty_mask[i] = 0;
			erase_flash_page(APP_BASE + i * FLASH_PAGE_SIZE);
			f_buff = (u16 *)&disk_buffer[i * FLASH_PAGE_SIZE];
			for (j = 0; j < FLASH_PAGE_SIZE; j += 2)
			{
				if (write_flash_halfword((u32)(APP_BASE + i * FLASH_PAGE_SIZE + j), *f_buff++) != HAL_OK)
				{
					app_log_error("Unable to program flash at index %lu", j);
				}
			}
			break;
		}
	}
#elif defined(STM32F411xE)
	// F4: Single 16KB sector - check if any page is dirty
	for (i = 0; i < sizeof(page_dirty_mask); i++)
	{
		if (page_dirty_mask[i])
		{
			// Erase the entire sector and rewrite all data
			app_log_trace("Erasing flash sector...", NULL);
			memset(page_dirty_mask, 0, sizeof(page_dirty_mask));
			erase_flash_page(APP_BASE);
			app_log_trace("Writing %u bytes to flash...", sizeof(disk_buffer));
			f_buff = (u16 *)disk_buffer;
			for (j = 0; j < sizeof(disk_buffer); j += 2)
			{
				if (write_flash_halfword((u32)(APP_BASE + j), *f_buff++) != HAL_OK)
				{
					app_log_error("Unable to program flash at index %lu", j);
				}
			}
			app_log_trace("Flash write loop completed", NULL);
			break;
		}
	}
#endif

	if (HAL_FLASH_Lock() != HAL_OK)
	{
		app_log_error("Unable to lock flash", NULL);
	}
	return 0;
}

u8 rewrite_all_flash_pages(void)
{
	u16 i;
	u8 result;
	u16 *f_buff = (u16 *)disk_buffer;
	HAL_StatusTypeDef status;

	status = HAL_FLASH_Unlock();
	if (status != HAL_OK)
	{
		app_log_error("Unable to unlock flash: %d", status);
	}

#if defined(STM32F103xB)
	// F1: Erase multiple 1KB pages
	for (i = 0; i < 8; i++)
	{
		result = erase_flash_page(APP_BASE + i * FLASH_PAGE_SIZE);
		if (result != HAL_OK)
		{
			return result;
		}
	}
	for (i = 0; i < (SECTOR_CNT / 2); i += 2)
	{
		result = write_flash_halfword((u32)(APP_BASE + i), *f_buff++);
		if (result != HAL_OK)
		{
			app_log_error("Error while programming flash at %d", i);
			return result;
		}
	}
#elif defined(STM32F411xE)
	// F4: Erase single 16KB sector
	result = erase_flash_page(APP_BASE);
	if (result != HAL_OK)
	{
		return result;
	}
	// Write the entire buffer
	for (i = 0; i < sizeof(disk_buffer); i += 2)
	{
		result = write_flash_halfword((u32)(APP_BASE + i), *f_buff++);
		if (result != HAL_OK)
		{
			app_log_error("Error while programming flash at %d", i);
			return result;
		}
	}
#endif

	if (HAL_FLASH_Lock() != HAL_OK)
	{
		app_log_error("Unable to lock flash", NULL);
	}
	return HAL_OK;
}

// Helper to get CONFIG.TXT's starting cluster from directory entry
static u16 get_config_start_cluster(void)
{
	u8 *entry = ROOT_SECTOR;
	u8 name[12];
	for (int i = 0; i < 16; i++)
	{
		memcpy(name, entry, 11);
		name[11] = '\0';
		Upper(name, 11);
		if (memcmp(name, CONFIG_FILENAME, 11) == 0)
		{
			return entry[0x1A] | (entry[0x1B] << 8);
		}
		entry += 32;
	}
	return 0; // Not found
}

// Helper to find comment start in a value (tab followed by #)
static u8 *find_comment_start(u8 *value)
{
	u8 *p = value;
	while (*p)
	{
		// Comment starts with tab+# or just # preceded by whitespace
		if (*p == '\t' && *(p + 1) == '#')
			return p;
		p++;
	}
	return NULL;
}

// Helper to set a FAT12 entry value
// FAT12 entries are 12 bits each, packed as: [low8_0][high4_0|low4_1][high8_1]
static void set_fat12_entry(u8 *fat, u16 cluster, u16 value)
{
	u32 offset = cluster + (cluster / 2); // 1.5 bytes per entry
	if (cluster & 1)
	{
		// Odd cluster: high nibble of byte[offset], full byte[offset+1]
		fat[offset] = (fat[offset] & 0x0F) | ((value & 0x0F) << 4);
		fat[offset + 1] = (value >> 4) & 0xFF;
	}
	else
	{
		// Even cluster: full byte[offset], low nibble of byte[offset+1]
		fat[offset] = value & 0xFF;
		fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F);
	}
}

// Update FAT chain for CONFIG.TXT based on file size
static void update_fat_chain(u32 file_size)
{
	u32 clusters_needed = (file_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
	if (clusters_needed == 0)
		clusters_needed = 1;

	// Reset FAT (keep clusters 0 and 1 as reserved)
	memset(FAT1_SECTOR + 3, 0, SECTOR_SIZE - 3);

	// Chain clusters starting from 2
	for (u32 i = 0; i < clusters_needed; i++)
	{
		u16 cluster = 2 + i;
		u16 next = (i == clusters_needed - 1) ? 0xFFF : (cluster + 1); // EOF or next
		set_fat12_entry(FAT1_SECTOR, cluster, next);
	}

	// Copy to FAT2
	memcpy(FAT2_SECTOR, FAT1_SECTOR, SECTOR_SIZE);
}

// Static buffer for extracted values (to avoid modifying parse_buffer during extraction)
static u8 value_buffer[FILE_ROW_CNT];

u8 validate_file(u8 *p_file, u16 root_addr)
{
	u32 i, j, k, m, line_idx;
	u8 illegal = 0;
	u8 *value_start;
	u8 *comment_start;
	size_t value_len;

	app_log_trace("starting, root_addr=%d", root_addr);

	// Use static buffers instead of stack allocation
	memset(parse_buffer, 0x00, sizeof(parse_buffer));

	// Determine where to read file content from:
	// - If FILE_SECTOR (cluster 2) already has valid content, use it
	//   (this handles the case where we've already normalized)
	// - Otherwise, use p_file (wherever macOS wrote the data)
	u8 *read_source = p_file;

	// Check if FILE_SECTOR starts with a valid entry (previously normalized)
	// Look for any registered entry name at the start
	bool file_sector_valid = false;
	for (k = 0; k < FILE_ENTRY_CNT && !file_sector_valid; k++)
	{
		if (entries[k].entry[0] != '\0')
		{
			size_t entry_len = strlen(entries[k].entry);
			if (memcmp(FILE_SECTOR, entries[k].entry, entry_len) == 0 &&
				FILE_SECTOR[entry_len] == '=')
			{
				file_sector_valid = true;
			}
		}
	}

	// Also check p_file location for valid content
	bool p_file_valid = false;
	if (p_file != FILE_SECTOR)
	{
		for (k = 0; k < FILE_ENTRY_CNT && !p_file_valid; k++)
		{
			if (entries[k].entry[0] != '\0')
			{
				size_t entry_len = strlen(entries[k].entry);
				if (memcmp(p_file, entries[k].entry, entry_len) == 0 &&
					p_file[entry_len] == '=')
				{
					p_file_valid = true;
				}
			}
		}
	}

	// Prefer p_file if it has valid content (fresh write from macOS)
	// Otherwise use FILE_SECTOR if it has valid content (previously normalized)
	if (p_file_valid)
	{
		read_source = p_file;
		app_log_trace("reading from p_file (macOS location)");
	}
	else if (file_sector_valid)
	{
		read_source = FILE_SECTOR;
		app_log_trace("reading from FILE_SECTOR (normalized)");
	}
	else
	{
		// Neither RAM location has valid content - this can happen if macOS
		// dot files corrupted our RAM buffer. Try to recover from flash.
		app_log_warn("no valid content in RAM, reloading from flash");

		// Reload FILE_SECTOR from flash
		u8 *flash_file_sector = (u8 *)APP_BASE + 0x600;  // FILE_SECTOR offset in flash
		memcpy(FILE_SECTOR, flash_file_sector, FILE_SECTOR_SIZE);

		// Check again if FILE_SECTOR now has valid content
		for (k = 0; k < FILE_ENTRY_CNT; k++)
		{
			if (entries[k].entry[0] != '\0')
			{
				size_t entry_len = strlen(entries[k].entry);
				if (memcmp(FILE_SECTOR, entries[k].entry, entry_len) == 0 &&
					FILE_SECTOR[entry_len] == '=')
				{
					file_sector_valid = true;
					read_source = FILE_SECTOR;
					app_log_debug("recovered from flash");
					break;
				}
			}
		}

		if (!file_sector_valid)
		{
			// Flash also doesn't have valid content - use defaults
			read_source = p_file;
			app_log_trace("flash also invalid, using defaults");
		}
	}

	memcpy((u8 *)file_buffer, read_source, sizeof(file_buffer));

	// Log first 64 bytes of file content for debugging
	app_log_trace("first bytes: %.60s", file_buffer);

	// Parse each line from the file into parse_buffer
	// Handle both CRLF (Windows) and LF (Unix/macOS) line endings
	m = 0;
	for (line_idx = 0; line_idx < FILE_ENTRY_CNT; line_idx++)
	{
		j = 0;
		u8 line_ending_len = 1; // Default for LF
		for (i = m; i < sizeof(file_buffer) && file_buffer[i] != '\0'; i++)
		{
			// Check for CRLF (Windows) or LF (Unix/macOS)
			if (file_buffer[i] == 0x0D && file_buffer[i + 1] == 0x0A)
			{
				line_ending_len = 2; // CRLF
				break;
			}
			else if (file_buffer[i] == 0x0A)
			{
				line_ending_len = 1; // LF only
				break;
			}
			else
			{
				if (j < FILE_ROW_CNT - 1)
				{
					parse_buffer[line_idx][j++] = file_buffer[i];
				}
				m++;
			}
		}
		parse_buffer[line_idx][j] = '\0';
		m = i + line_ending_len;

		// Stop if we've reached end of file content
		if (i >= sizeof(file_buffer) || file_buffer[i] == '\0')
			break;
	}

	// Debug: log how many lines were parsed
	u32 parsed_count = 0;
	for (u32 idx = 0; idx < FILE_ENTRY_CNT; idx++)
	{
		if (parse_buffer[idx][0] != '\0')
		{
			parsed_count++;
			app_log_trace("line %lu: %.40s...", idx, parse_buffer[idx]);
		}
	}
	app_log_trace("parsed %lu lines", parsed_count);

	// Process each registered entry - search for it in parsed lines
	for (k = 0; k < FILE_ENTRY_CNT; k++)
	{
		// Skip unregistered entries
		if (entries[k].entry[0] == '\0')
			continue;

		u8 found = 0;
		size_t entry_len = strlen(entries[k].entry);

		// Search all parsed lines for this entry
		for (line_idx = 0; line_idx < FILE_ENTRY_CNT; line_idx++)
		{
			if (parse_buffer[line_idx][0] == '\0')
				continue;

			// Check if line starts with entry name followed by '='
			if (memcmp(parse_buffer[line_idx], entries[k].entry, entry_len) == 0 &&
				parse_buffer[line_idx][entry_len] == '=')
			{
				found = 1;
				value_start = parse_buffer[line_idx] + entry_len + 1;

				// Find and strip comment from value
				comment_start = find_comment_start(value_start);
				if (comment_start)
				{
					value_len = comment_start - value_start;
				}
				else
				{
					value_len = strlen((char *)value_start);
				}

				// Copy clean value to buffer
				memset(value_buffer, 0, sizeof(value_buffer));
				memcpy(value_buffer, value_start, MIN(value_len, FILE_ROW_CNT - 1));

				// Validate and update with clean value (no comment)
				if (entries[k].validate == NULL || entries[k].validate(value_buffer))
				{
					if (entries[k].update)
						entries[k].update(value_buffer);
					// Printer writes clean ENTRY=value to parse_buffer[k]
					if (entries[k].print)
						entries[k].print((char *)parse_buffer[k], FILE_ROW_CNT);
				}
				else
				{
					// Validation failed, use default
					snprintf((char *)parse_buffer[k], FILE_ROW_CNT, "%s=%s",
							 entries[k].entry, entries[k].default_value ? entries[k].default_value : "");
					illegal = 1;
				}
				break;
			}
		}

		if (!found)
		{
			// Entry not found in file → use default
			snprintf(
				(char *)parse_buffer[k],
				FILE_ROW_CNT,
				"%s=%s",
				entries[k].entry,
				entries[k].default_value ? entries[k].default_value : "");

			if (entries[k].update && entries[k].default_value)
			{
				entries[k].update((u8 *)entries[k].default_value);
			}

			illegal = 1;
		}
	}

	// Rebuild file content from entries (in registration order)
	memset(file_content_buffer, 0x00, FILE_CHAR_CNT);
	m = 0;
	for (k = 0; k < FILE_ENTRY_CNT; k++)
	{
		// Skip unregistered entries
		if (entries[k].entry[0] == '\0')
			continue;

		size_t line_len = strlen((char *)parse_buffer[k]);
		size_t comment_len = strlen((char *)entries[k].comment);

		if (m + line_len + comment_len < FILE_CHAR_CNT - 1)
		{
			memcpy(file_content_buffer + m, parse_buffer[k], line_len);
			m += line_len;
			memcpy(file_content_buffer + m, entries[k].comment, comment_len);
			m += comment_len;
		}
	}

	app_log_trace("rebuilt file, size=%lu bytes", m);

	// Update file size in directory entry (support sizes > 255 bytes)
	// ROOT_SECTOR + root_addr*32 + 0x1C is where file size is stored
	u8 *dir_entry = ROOT_SECTOR + (root_addr * 32);
	dir_entry[0x1C] = m & 0xFF;
	dir_entry[0x1D] = (m >> 8) & 0xFF;
	dir_entry[0x1E] = (m >> 16) & 0xFF;
	dir_entry[0x1F] = (m >> 24) & 0xFF;

	// ALWAYS force file to start at cluster 2 for consistency
	// macOS may allocate different clusters, but we normalize to cluster 2
	// This ensures directory entry, FAT chain, and data location are all aligned
	dir_entry[0x1A] = 0x02; // Starting cluster low byte
	dir_entry[0x1B] = 0x00; // Starting cluster high byte

	app_log_trace("forcing cluster=2, size=%lu", m);

	// Update FAT chain for the new file size (always starts at cluster 2)
	update_fat_chain(m);

	// Mark pages dirty (FAT and directory)
	page_dirty_mask[0] = 1; // FAT was updated
	page_dirty_mask[1] = 1; // Root directory

	// ALWAYS write content to FILE_SECTOR (cluster 2 = sector 64)
	// regardless of where macOS wrote it, to match our FAT chain
	memcpy(FILE_SECTOR, file_content_buffer, m);
	// Clear remaining space to avoid stale data
	if (m < FILE_SECTOR_SIZE)
	{
		memset(FILE_SECTOR + m, 0, FILE_SECTOR_SIZE - m);
	}

	return illegal;
}
u8 *find_file(u8 *pfilename, u16 *pfilelen, u16 *root_addr)
{
	u16 n, sector;
	u8 str_name[11];
	u8 *pdiraddr;

	pdiraddr = ROOT_SECTOR;

	for (n = 0; n < 16; n++)
	{
		memcpy(str_name, pdiraddr, 11);
		Upper(str_name, 11);
		if (memcmp(str_name, pfilename, 11) == 0)
		{
			memcpy((u8 *)pfilelen, pdiraddr + 0x1C, 2);
			memcpy((u8 *)&sector, pdiraddr + 0x1A, 2);
			if (root_addr)
				*root_addr = n; // Return directory entry index
			return (u8 *)FILE_SECTOR + (sector - 2) * SECTOR_SIZE;
		}

		pdiraddr += 32;
	}
	app_log_info("file search did not find requested file", NULL);
	return NULL;
}
static u8 flush_file(void)
{
	u32 k, m;
	u8 illegal;
	u16 file_len;
	u8 *p_file;
	u16 root_addr;

	root_addr = 0;

	if ((p_file = find_file((u8 *)&CONFIG_FILENAME, &file_len, &root_addr)))
	{
		illegal = validate_file(p_file, root_addr);
		if (illegal)
		{
			// Defer flash write to avoid blocking USB enumeration
			pending_flash_write = true;
			last_write_tick = HAL_GetTick();
		}
	}
	else
	{
		memset(disk_buffer, 0x00, sizeof(disk_buffer));
		memcpy(ROOT_SECTOR, &CONFIG_FILENAME, 0xC);
		memcpy(FAT1_SECTOR, fat_data, 6);
		memcpy(FAT2_SECTOR, fat_data, 6);

		m = 0;
		for (k = 0; k < FILE_ENTRY_CNT; k++)
		{
			// Skip unregistered entries
			if (entries[k].entry[0] == '\0')
				continue;
			// Build line: entry=default_value\tcomment\r\n
			size_t line_len = snprintf((char *)(FILE_SECTOR + m), FILE_SECTOR_SIZE - m,
									   "%s=%s%s",
									   entries[k].entry,
									   entries[k].default_value ? entries[k].default_value : "",
									   entries[k].comment);
			m += line_len;
		}

		disk_buffer[0x40B] = 0x0; // attributes
		*(u32 *)VOLUME_BASE = VOLUME;
		disk_buffer[0x41A] = 0x02; // cluster number
		// File size (4 bytes for sizes > 255)
		disk_buffer[0x41C] = m & 0xFF;
		disk_buffer[0x41D] = (m >> 8) & 0xFF;
		disk_buffer[0x41E] = (m >> 16) & 0xFF;
		disk_buffer[0x41F] = (m >> 24) & 0xFF;
		// Update FAT chain for the file size
		update_fat_chain(m);
		// Defer flash write to avoid blocking USB enumeration
		pending_flash_write = true;
		last_write_tick = HAL_GetTick();
		memset(page_dirty_mask, 1, sizeof(page_dirty_mask)); // Mark all pages dirty
	}

	return 0;
}
void read_sector(u8 *pbuffer, u32 disk_addr)
{
	// disk_addr is sector number (not byte offset)
	// Boot sector layout (from BOOT_SEC):
	//   Reserved sectors: 8 (sectors 0-7, boot at 0)
	//   FAT1: sectors 8-19 (12 sectors)
	//   FAT2: sectors 20-31 (12 sectors)
	//   Root dir: sectors 32-63 (32 sectors for 512 entries)
	//   Data: sectors 64+ (cluster 2 starts here)

	if (disk_addr == 0)
	{
		// Boot sector
		app_log_trace("Reading BOOT sector: %lu", disk_addr);
		memcpy(pbuffer, BOOT_SEC, SECTOR_SIZE);
	}
	else if (disk_addr >= 1 && disk_addr <= 7)
	{
		// Reserved sectors (after boot) - return zeros
		memset(pbuffer, 0, SECTOR_SIZE);
	}
	else if (disk_addr >= 8 && disk_addr <= 19)
	{
		// FAT1 (12 sectors) - only first sector has data
		if (disk_addr == 8)
		{
			app_log_trace("Reading FAT1 sector: %lu", disk_addr);
			memcpy(pbuffer, FAT1_SECTOR, SECTOR_SIZE);
		}
		else
		{
			memset(pbuffer, 0, SECTOR_SIZE);
		}
	}
	else if (disk_addr >= 20 && disk_addr <= 31)
	{
		// FAT2 (12 sectors) - only first sector has data
		if (disk_addr == 20)
		{
			app_log_trace("Reading FAT2 sector: %lu", disk_addr);
			memcpy(pbuffer, FAT2_SECTOR, SECTOR_SIZE);
		}
		else
		{
			memset(pbuffer, 0, SECTOR_SIZE);
		}
	}
	else if (disk_addr >= 32 && disk_addr <= 63)
	{
		// Root directory (32 sectors) - only first sector has entries
		if (disk_addr == 32)
		{
			app_log_trace("Reading DIR sector: %lu", disk_addr);
			memcpy(pbuffer, ROOT_SECTOR, SECTOR_SIZE);
			// Log CONFIG.TXT entry details
			u32 fsize = ROOT_SECTOR[0x1C] | (ROOT_SECTOR[0x1D] << 8) |
						(ROOT_SECTOR[0x1E] << 16) | (ROOT_SECTOR[0x1F] << 24);
			u16 cluster = ROOT_SECTOR[0x1A] | (ROOT_SECTOR[0x1B] << 8);
			app_log_trace("DIR: CONFIG.TXT cluster=%u, size=%lu", cluster, fsize);
		}
		else
		{
			memset(pbuffer, 0, SECTOR_SIZE);
		}
	}
	else if (disk_addr >= 64 && disk_addr < SECTOR_CNT)
	{
		// Data area (cluster 2 = sector 64)
		u32 data_offset = (disk_addr - 64) * SECTOR_SIZE;
		// Check bounds - FILE_SECTOR has limited space in disk_buffer
		// disk_buffer is 0x4000 bytes (16KB), FILE_SECTOR starts at 0x600
		// Available for file data: FILE_SECTOR_SIZE (~14KB)
		if (data_offset + SECTOR_SIZE <= FILE_SECTOR_SIZE)
		{
			app_log_trace("Reading FILE sector: %lu", disk_addr);
			memcpy(pbuffer, FILE_SECTOR + data_offset, SECTOR_SIZE);
			// Log first bytes of sector 64 (start of file)
			if (disk_addr == 64)
			{
				app_log_trace("FILE sector 64 content: %.40s", pbuffer);
			}
		}
		else
		{
			// Beyond actual data - return zeros (unallocated space)
			memset(pbuffer, 0, SECTOR_SIZE);
		}
	}
	else
	{
		app_log_warn("Unrecognized disk sector read attempt: %lu", disk_addr);
		memset(pbuffer, 0, SECTOR_SIZE);
	}
}
u8 write_sector(u8 *buff, u32 diskaddr, u32 length) // PC Save data call
{
	u32 i;
	u8 illegal = 0;
	u8 ver[20];
	static u16 Config_flag = 0;
	static u8 txt_flag = 0;
	u8 config_filesize = 0;

	// diskaddr is sector number, length is number of sectors
	// Copy incoming data to temp buffer
	for (i = 0; i < length * SECTOR_SIZE; i++)
	{
		*(u8 *)(pdisk_buffer_temp + i) = buff[i];
	}

	// Process each sector
	for (u32 s = 0; s < length; s++)
	{
		u32 sector = diskaddr + s;
		u8 *sector_data = pdisk_buffer_temp + (s * SECTOR_SIZE);

		if (sector >= 8 && sector <= 19)
		{
			// Write FAT1 sector
			if (sector == 8)
			{
				if (memcmp(sector_data, FAT1_SECTOR, SECTOR_SIZE))
				{
					memcpy(FAT1_SECTOR, sector_data, SECTOR_SIZE);
					page_dirty_mask[0] = 1;
				}
			}
		}
		else if (sector >= 20 && sector <= 31)
		{
			// Write FAT2 sector
			if (sector == 20)
			{
				if (memcmp(sector_data, FAT2_SECTOR, SECTOR_SIZE))
				{
					memcpy(FAT2_SECTOR, sector_data, SECTOR_SIZE);
					page_dirty_mask[0] = 1;
				}
			}
		}
		else if (sector >= 32 && sector <= 63)
		{
			// Write ROOT DIR sector
			if (sector == 32)
			{
				if (memcmp(sector_data, ROOT_SECTOR, SECTOR_SIZE))
				{
					memcpy(ROOT_SECTOR, sector_data, SECTOR_SIZE);
					page_dirty_mask[1] = 1;

					// Check for CONFIG.TXT entry
					// DON'T force cluster here - let find_file() use macOS's cluster
					// so we can read from where macOS wrote the data.
					// We'll normalize to cluster 2 in validate_file() AFTER reading.
					u8 *entry = ROOT_SECTOR;
					for (i = 0; i < 16; i++)
					{
						memcpy(ver, entry, 12);
						if (memcmp(ver, CONFIG_FILENAME, 11) == 0)
						{
							Config_flag = entry[0x1A];
							config_filesize = entry[0x1C] | (entry[0x1D] << 8);
							txt_flag = 1;
							app_log_trace("CONFIG.TXT cluster=%u, size=%u",
										  entry[0x1A] | (entry[0x1B] << 8), config_filesize);
							break;
						}
						entry += 32;
					}
					if (config_filesize == 0 && txt_flag == 1)
					{
						txt_flag = 0;
						page_dirty_mask[1] = 0;
						page_dirty_mask[0] = 0;
					}
					else
					{
						page_dirty_mask[0] = 1;
					}
				}
			}
		}
		else if (sector >= 64 && sector < SECTOR_CNT)
		{
			// Write DATA sector
			u32 data_offset = (sector - 64) * SECTOR_SIZE;
			// Check bounds - FILE_SECTOR_SIZE bytes available for file data
			if (data_offset + SECTOR_SIZE > FILE_SECTOR_SIZE)
			{
				// Beyond buffer - ignore write
				continue;
			}

			// PROTECTION: Block macOS dot files from overwriting our normalized config data.
			//
			// Strategy: Check what file this cluster belongs to (from directory entry).
			// - If this cluster is CONFIG.TXT's starting cluster, allow the write
			// - If this cluster is NOT CONFIG.TXT's but would land on our normalized data
			//   at cluster 2 (sector 64+), block it unless it looks like valid config
			//
			// This allows CONFIG.TXT writes to ANY cluster while blocking dot files
			// that try to reuse cluster 2 after macOS "deletes" the old file.
			{
				u16 write_cluster = SECTOR_TO_CLUSTER(sector);
				u16 config_cluster = get_config_start_cluster();

				// Check if FILE_SECTOR already has valid CONFIG.TXT data (normalized)
				bool file_sector_has_config = false;
				for (u32 k = 0; k < FILE_ENTRY_CNT; k++)
				{
					if (entries[k].entry[0] != '\0')
					{
						size_t entry_len = strlen(entries[k].entry);
						if (memcmp(FILE_SECTOR, entries[k].entry, entry_len) == 0 &&
							FILE_SECTOR[entry_len] == '=')
						{
							file_sector_has_config = true;
							break;
						}
					}
				}

				// If this write is to CONFIG.TXT's cluster (per directory), allow it
				if (config_cluster > 0 && write_cluster == config_cluster)
				{
					// This is CONFIG.TXT data - allow write
					app_log_trace("allowing CONFIG.TXT write to cluster %u (sector %lu)", write_cluster, sector);
				}
				// If this write is to cluster 2 (sector 64) - our normalized location
				else if (write_cluster == 2)
				{
					// Check if incoming data looks like CONFIG.TXT
					bool looks_like_config = false;
					for (u32 k = 0; k < FILE_ENTRY_CNT; k++)
					{
						if (entries[k].entry[0] != '\0')
						{
							size_t entry_len = strlen(entries[k].entry);
							if (memcmp(sector_data, entries[k].entry, entry_len) == 0 &&
								sector_data[entry_len] == '=')
							{
								looks_like_config = true;
								break;
							}
						}
					}

					if (!looks_like_config)
					{
						// This is NOT CONFIG.TXT - likely a dot file trying to use cluster 2
						app_log_trace("rejecting non-config write to cluster 2 (sector %lu, first byte: 0x%02X)", sector, sector_data[0]);
						continue;
					}
				}
				// If this write is to clusters 3+ (sectors 65+) and we have normalized data
				else if (write_cluster > 2 && write_cluster <= 2 + (FILE_SECTOR_SIZE / SECTOR_SIZE) && file_sector_has_config)
				{
					// Check if this is a continuation of CONFIG.TXT or a dot file
					// Dot files have characteristic patterns at start
					bool is_dot_file = (sector_data[0] == 0x00 ||  // Resource fork padding
									   sector_data[0] == 0x05 ||   // Deleted entry marker
									   (sector_data[0] == '.' && sector_data[1] != '\0')); // Dot file content

					if (is_dot_file)
					{
						app_log_trace("rejecting dot file write to cluster %u (sector %lu)", write_cluster, sector);
						continue;
					}
				}
			}

			if (memcmp(sector_data, FILE_SECTOR + data_offset, SECTOR_SIZE))
			{
				memcpy(FILE_SECTOR + data_offset, sector_data, SECTOR_SIZE);
				page_dirty_mask[(data_offset / FLASH_PAGE_SIZE) + 1] = 1;
			}
			// Don't validate here - defer to process() when all sectors received
		}
	}

	// Mark pending write instead of writing immediately
	pending_flash_write = true;
	last_write_tick = HAL_GetTick();

	return HAL_OK;
}
static u32 get_sector_size(void)
{
	return SECTOR_SIZE;
}
static u32 get_sector_count(void)
{
	return SECTOR_CNT;
}
static void load_from_flash(void)
{
	memcpy(disk_buffer, (u8 *)APP_BASE, sizeof(disk_buffer));
	memset(page_dirty_mask, 0, sizeof(page_dirty_mask));
	app_log_debug("Loaded data from flash", NULL);
}
static void init(void)
{
	load_from_flash();
	flush_file(); // validate, normalize, create defaults
}
static u32 get_unused_idx()
{
	u32 i = 0;
	for (i = 0; i < FILE_ENTRY_CNT; i++)
	{
		if (bitRead(entry_usage_mask, i) == 0)
		{
			bitSet(entry_usage_mask, i);
			return i;
		}
	}
	return -1;
}
static bool register_entry(char *entry, char *default_val, char *comment, void *validator, void *updater, void *printer)
{
	u32 idx = get_unused_idx();
	if (idx < FILE_ENTRY_CNT)
	{
		strncpy(entries[idx].entry, entry, MAX_ENTRY_LABEL_LENGTH - 1);
		entries[idx].entry[MAX_ENTRY_LABEL_LENGTH - 1] = '\0';
		snprintf(entries[idx].comment, MAX_ENTRY_COMMENT_LENGTH, "\t%s\r\n", comment);
		entries[idx].default_value = default_val; // Store pointer (caller must keep string valid)
		entries[idx].validate = validator;
		entries[idx].update = updater;
		entries[idx].print = printer;
		return true;
	}
	return false;
}

static void process(void)
{
	// Check if we have pending writes and enough time has passed
	if (pending_flash_write && (HAL_GetTick() - last_write_tick >= FLASH_WRITE_DELAY_MS))
	{
		app_log_trace("Flushing deferred flash write", NULL);

		// Validate CONFIG.TXT before writing to flash (all sectors now received)
		u16 file_len;
		u16 root_addr = 0;
		u8 *p_file = find_file((u8 *)&CONFIG_FILENAME, &file_len, &root_addr);
		if (p_file && file_len > 0)
		{
			validate_file(p_file, root_addr);
		}

		app_log_debug("Starting flash write...", NULL);
		if (rewrite_dirty_flash_pages() != HAL_OK)
		{
			app_log_error("Error during deferred flash write", NULL);
		}
		else
		{
			app_log_debug("Flash write completed successfully", NULL);
		}
		pending_flash_write = false;
	}
}

const struct disk Disk = {
	.init = init,
	.load_from_flash = load_from_flash,
	.process = process,
	.Disk_SecWrite = write_sector,
	.Disk_SecRead = read_sector,
	.get_sector_size = get_sector_size,
	.get_sector_count = get_sector_count,
	.register_entry = register_entry,
};
