#include "transfer_pool.h"

#include <psp2/kernel/threadmgr.h>
#include <stdlib.h>
#include <string.h>
#include "unicode_upper_table.h"

#define FTPVITA_TRANSFER_PATH_MAX 1024U
#define FTPVITA_TRANSFER_KEY_MAX ((FTPVITA_TRANSFER_PATH_MAX * 2U) + 1U)

typedef struct {
	void *buffer;
	int in_use;
	int direction;
	char path_key[FTPVITA_TRANSFER_KEY_MAX];
} ftpvita_transfer_slot_t;

static SceUID transfer_pool_mutex = -1;
static ftpvita_transfer_slot_t transfer_slots[FTPVITA_TRANSFER_POOL_SLOTS];

static void transfer_pool_clear(void)
{
	unsigned int index;

	for (index = 0; index < FTPVITA_TRANSFER_POOL_SLOTS; ++index) {
		transfer_slots[index].buffer = NULL;
		transfer_slots[index].in_use = 0;
		transfer_slots[index].direction = FTPVITA_TRANSFER_READ;
		transfer_slots[index].path_key[0] = '\0';
	}
}

static uint32_t transfer_unicode_upper(uint32_t value)
{
	size_t high = FTPVITA_UNICODE_UPPER_RANGE_COUNT;
	size_t low = 0;

	if (value > UINT16_MAX)
		return value;

	while (low < high) {
		const ftpvita_unicode_upper_range_t *range;
		size_t middle = low + (high - low) / 2;

		range = &ftpvita_unicode_upper_ranges[middle];
		if (value < range->first) {
			high = middle;
		} else if (value > range->last) {
			low = middle + 1;
		} else {
			if ((value - range->first) % range->step == 0)
				return (uint32_t)((int32_t)value + range->delta);
			return value;
		}
	}

	return value;
}

static size_t transfer_utf8_decode(const unsigned char *input,
	size_t input_size, uint32_t *value)
{
	unsigned char first;

	if (input_size == 0)
		return 0;

	first = input[0];
	if (first <= 0x7F) {
		*value = first;
		return 1;
	}
	if (first >= 0xC2 && first <= 0xDF && input_size >= 2 &&
		(input[1] & 0xC0) == 0x80) {
		*value = ((uint32_t)(first & 0x1F) << 6) |
			(uint32_t)(input[1] & 0x3F);
		return 2;
	}
	if (first >= 0xE0 && first <= 0xEF && input_size >= 3 &&
		(input[1] & 0xC0) == 0x80 && (input[2] & 0xC0) == 0x80 &&
		(first != 0xE0 || input[1] >= 0xA0) &&
		(first != 0xED || input[1] <= 0x9F)) {
		*value = ((uint32_t)(first & 0x0F) << 12) |
			((uint32_t)(input[1] & 0x3F) << 6) |
			(uint32_t)(input[2] & 0x3F);
		return 3;
	}
	if (first >= 0xF0 && first <= 0xF4 && input_size >= 4 &&
		(input[1] & 0xC0) == 0x80 && (input[2] & 0xC0) == 0x80 &&
		(input[3] & 0xC0) == 0x80 &&
		(first != 0xF0 || input[1] >= 0x90) &&
		(first != 0xF4 || input[1] <= 0x8F)) {
		*value = ((uint32_t)(first & 0x07) << 18) |
			((uint32_t)(input[1] & 0x3F) << 12) |
			((uint32_t)(input[2] & 0x3F) << 6) |
			(uint32_t)(input[3] & 0x3F);
		return 4;
	}

	return 0;
}

static size_t transfer_utf8_encode(uint32_t value, unsigned char output[4])
{
	if (value <= 0x7F) {
		output[0] = (unsigned char)value;
		return 1;
	}
	if (value <= 0x7FF) {
		output[0] = (unsigned char)(0xC0 | (value >> 6));
		output[1] = (unsigned char)(0x80 | (value & 0x3F));
		return 2;
	}
	if (value <= 0xFFFF) {
		output[0] = (unsigned char)(0xE0 | (value >> 12));
		output[1] = (unsigned char)(0x80 | ((value >> 6) & 0x3F));
		output[2] = (unsigned char)(0x80 | (value & 0x3F));
		return 3;
	}

	output[0] = (unsigned char)(0xF0 | (value >> 18));
	output[1] = (unsigned char)(0x80 | ((value >> 12) & 0x3F));
	output[2] = (unsigned char)(0x80 | ((value >> 6) & 0x3F));
	output[3] = (unsigned char)(0x80 | (value & 0x3F));
	return 4;
}

static int transfer_append_upper(const char *input, size_t input_size,
	char *key, size_t key_size, size_t *used)
{
	while (input_size > 0) {
		unsigned char encoded[4];
		uint32_t value;
		size_t consumed;
		size_t encoded_size;

		consumed = transfer_utf8_decode((const unsigned char *)input,
			input_size, &value);
		if (consumed == 0) {
			if (*used + 1 >= key_size)
				return -1;
			key[(*used)++] = *input++;
			--input_size;
			continue;
		}

		value = transfer_unicode_upper(value);
		encoded_size = transfer_utf8_encode(value, encoded);
		if (encoded_size >= key_size - *used)
			return -1;
		memcpy(key + *used, encoded, encoded_size);
		*used += encoded_size;
		input += consumed;
		input_size -= consumed;
	}

	return 0;
}

static int transfer_path_key(const char *path, char *key, size_t key_size)
{
	const char *component;
	const char *cursor;
	const char *device_separator;
	size_t component_size;
	size_t root_size = 0;
	size_t used = 0;

	if (!path || !key || key_size == 0)
		return -1;

	device_separator = strchr(path, ':');
	cursor = path;
	if (device_separator) {
		while (cursor < device_separator && *cursor == '/')
			++cursor;
		if (transfer_append_upper(cursor,
			(size_t)(device_separator - cursor), key, key_size, &used) < 0)
			return -1;
		if (used + 2 >= key_size)
			return -1;
		key[used++] = ':';
		key[used++] = '/';
		root_size = used;
		cursor = device_separator + 1;
		while (*cursor == '/')
			++cursor;
	} else if (*cursor == '/') {
		if (used + 1 >= key_size)
			return -1;
		key[used++] = '/';
		root_size = used;
		while (*cursor == '/')
			++cursor;
	}

	while (*cursor) {
		component = cursor;
		while (*cursor && *cursor != '/')
			++cursor;
		component_size = (size_t)(cursor - component);
		while (*cursor == '/')
			++cursor;

		if (component_size == 0 ||
			(component_size == 1 && component[0] == '.'))
			continue;

		if (component_size == 2 && component[0] == '.' &&
			component[1] == '.') {
			while (used > root_size && key[used - 1] != '/')
				--used;
			if (used > root_size && key[used - 1] == '/')
				--used;
			continue;
		}

		if (used > 0 && key[used - 1] != '/') {
			if (used + 1 >= key_size)
				return -1;
			key[used++] = '/';
		}
		if (transfer_append_upper(component, component_size,
			key, key_size, &used) < 0)
			return -1;
	}

	key[used] = '\0';
	return 0;
}

int ftpvita_transfer_pool_init(size_t buffer_size)
{
	unsigned int index;
	SceUID mutex;

	if (buffer_size == 0 || transfer_pool_mutex >= 0)
		return -1;

	mutex = sceKernelCreateMutex("FTPVita_transfer_pool_mutex", 0, 0, NULL);
	if (mutex < 0)
		return mutex;

	transfer_pool_clear();
	transfer_pool_mutex = mutex;
	for (index = 0; index < FTPVITA_TRANSFER_POOL_SLOTS; ++index) {
		transfer_slots[index].buffer = malloc(buffer_size);
		if (!transfer_slots[index].buffer) {
			unsigned int cleanup_index;
			for (cleanup_index = 0; cleanup_index < index; ++cleanup_index)
				free(transfer_slots[cleanup_index].buffer);
			transfer_pool_clear();
			sceKernelDeleteMutex(transfer_pool_mutex);
			transfer_pool_mutex = -1;
			return -1;
		}
	}

	return 0;
}

void ftpvita_transfer_pool_fini(void)
{
	unsigned int index;
	SceUID mutex = transfer_pool_mutex;

	if (mutex < 0)
		return;

	transfer_pool_mutex = -1;
	for (index = 0; index < FTPVITA_TRANSFER_POOL_SLOTS; ++index)
		free(transfer_slots[index].buffer);
	transfer_pool_clear();
	sceKernelDeleteMutex(mutex);
}

int ftpvita_transfer_pool_acquire(const char *path, int direction,
	void **buffer, unsigned int *slot)
{
	char path_key[FTPVITA_TRANSFER_KEY_MAX];
	unsigned int index;
	int result;

	if (buffer)
		*buffer = NULL;
	if (slot)
		*slot = FTPVITA_TRANSFER_INVALID_SLOT;

	if (transfer_pool_mutex < 0 || !path || !buffer || !slot ||
		(direction != FTPVITA_TRANSFER_READ &&
		 direction != FTPVITA_TRANSFER_WRITE))
		return -1;
	if (transfer_path_key(path, path_key, sizeof(path_key)) < 0)
		return -1;

	result = sceKernelLockMutex(transfer_pool_mutex, 1, NULL);
	if (result < 0)
		return result;

	for (index = 0; index < FTPVITA_TRANSFER_POOL_SLOTS; ++index) {
		if (transfer_slots[index].in_use &&
			strcmp(transfer_slots[index].path_key, path_key) == 0 &&
			(transfer_slots[index].direction == FTPVITA_TRANSFER_WRITE ||
			 direction == FTPVITA_TRANSFER_WRITE)) {
			sceKernelUnlockMutex(transfer_pool_mutex, 1);
			return FTPVITA_TRANSFER_PATH_BUSY;
		}
	}

	for (index = 0; index < FTPVITA_TRANSFER_POOL_SLOTS; ++index) {
		if (!transfer_slots[index].in_use) {
			transfer_slots[index].in_use = 1;
			transfer_slots[index].direction = direction;
			strcpy(transfer_slots[index].path_key, path_key);
			*buffer = transfer_slots[index].buffer;
			*slot = index;
			sceKernelUnlockMutex(transfer_pool_mutex, 1);
			return FTPVITA_TRANSFER_OK;
		}
	}

	sceKernelUnlockMutex(transfer_pool_mutex, 1);
	return FTPVITA_TRANSFER_NO_SLOT;
}

void ftpvita_transfer_pool_release(unsigned int slot)
{
	if (transfer_pool_mutex < 0 || slot >= FTPVITA_TRANSFER_POOL_SLOTS)
		return;
	if (sceKernelLockMutex(transfer_pool_mutex, 1, NULL) < 0)
		return;

	transfer_slots[slot].in_use = 0;
	transfer_slots[slot].direction = FTPVITA_TRANSFER_READ;
	transfer_slots[slot].path_key[0] = '\0';
	sceKernelUnlockMutex(transfer_pool_mutex, 1);
}

unsigned int ftpvita_transfer_pool_available(void)
{
	unsigned int index;
	unsigned int available = 0;

	if (transfer_pool_mutex < 0)
		return 0;
	if (sceKernelLockMutex(transfer_pool_mutex, 1, NULL) < 0)
		return 0;

	for (index = 0; index < FTPVITA_TRANSFER_POOL_SLOTS; ++index) {
		if (!transfer_slots[index].in_use)
			++available;
	}

	sceKernelUnlockMutex(transfer_pool_mutex, 1);
	return available;
}
