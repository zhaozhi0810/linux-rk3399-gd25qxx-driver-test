#ifndef __GD25QXX_H__
#define __GD25QXX_H__

#include <linux/types.h>
#include <linux/ioctl.h>

#define GD25QXX_SIZE 		0x800000
#define GD25QXX_PAGE_LENGTH	256
#define GD25QXX_SECTOR		4096
#define GD25QXX_32KB_BLOCK 	32768
#define GD25QXX_64KB_BLOCK 	65536

/* IOCTL commands */
#define GD25QXX_MAGIC			'J'

/*Erase SPI Flash*/
#define GD25QXX_IOC_SECTOR_ERASE			_IOW(GD25QXX_MAGIC, 6, __u32)
#define GD25QXX_IOC_32KB_BLOCK_ERASE		_IOW(GD25QXX_MAGIC, 7, __u32)
#define GD25QXX_IOC_64KB_BLOCK_ERASE		_IOW(GD25QXX_MAGIC, 8, __u32)
#define GD25QXX_IOC_CHIP_ERASE			_IOW(GD25QXX_MAGIC, 9, __u32)

#endif /* GD25QXX_H */

