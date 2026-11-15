#pragma once

#include <stddef.h>

#define FTPVITA_TRANSFER_POOL_SLOTS 4U
#define FTPVITA_TRANSFER_INVALID_SLOT ((unsigned int)-1)

enum ftpvita_transfer_direction {
	FTPVITA_TRANSFER_READ = 0,
	FTPVITA_TRANSFER_WRITE = 1
};

enum ftpvita_transfer_result {
	FTPVITA_TRANSFER_OK = 0,
	FTPVITA_TRANSFER_NO_SLOT = 1,
	FTPVITA_TRANSFER_PATH_BUSY = 2
};

int ftpvita_transfer_pool_init(size_t buffer_size);
void ftpvita_transfer_pool_fini(void);
int ftpvita_transfer_pool_acquire(const char *path, int direction,
	void **buffer, unsigned int *slot);
void ftpvita_transfer_pool_release(unsigned int slot);
unsigned int ftpvita_transfer_pool_available(void);
