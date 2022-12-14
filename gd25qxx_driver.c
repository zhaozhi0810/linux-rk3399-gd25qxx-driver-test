/*
* @Author: dazhi
* @Date:   2022-11-08 13:54:23
* @Last Modified by:   dazhi
* @Last Modified time: 2022-11-14 16:01:18
*/
#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/kernel.h>

#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>

#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/of_gpio.h>
#include "gd25qxx.h"

/*
 * This supports access to SPI devices using normal userspace I/O calls.
 * Note that while traditional UNIX/POSIX I/O semantics are half duplex,
 * and often mask message boundaries, full SPI support requires full duplex
 * transfers.  There are several kinds of internal message boundaries to
 * handle chipselect management and other protocol options.
 *
 * SPI has a character major number assigned.  We allocate minor numbers
 * dynamically using a bitmask.  You must use hotplug tools, such as udev
 * (or mdev with busybox) to create and destroy the /dev/spidevB.C device
 * nodes, since there is no fixed association of minor numbers with any
 * particular SPI bus or device.
 */
#define SPIDEV_MAJOR			155	/* assigned */
#define N_SPI_MINORS			32	/* ... up to 256 */
#define INVALID_GPIO_PIN        0xfffff
/*GD25QXX CMD*/
#define WRITE_ENABLE 	0x06
#define PAGE_PROGRAM	0x02
#define READ_DATA 		0x03
#define WRITE_STATUS_REG 0x01
#define READ_STATUS_REG 0x05
#define CHIP_ERASE 		0xc7
#define SECTOR_ERASE 	0x20
#define BLOCK_32KB_ERASE 0x52
#define BLOCK_64KB_ERASE 0xD8
#define READ_DEVICE_ID 	0x90
#define READ_UID 		0x9F

static DECLARE_BITMAP(minors, N_SPI_MINORS);


/* Bit masks for spi_device.mode management.  Note that incorrect
 * settings for some settings can cause *lots* of trouble for other
 * devices on a shared bus:
 *
 *  - CS_HIGH ... this device will be active when it shouldn't be
 *  - 3WIRE ... when active, it won't behave as it should
 *  - NO_CS ... there will be no explicit message boundaries; this
 *	is completely incompatible with the shared bus model
 *  - READY ... transfers may proceed when they shouldn't.
 *
 * REVISIT should changing those flags be privileged?
 */
#define SPI_MODE_MASK		(SPI_CPHA | SPI_CPOL | SPI_CS_HIGH \
				| SPI_LSB_FIRST | SPI_3WIRE | SPI_LOOP \
				| SPI_NO_CS | SPI_READY | SPI_TX_DUAL \
				| SPI_TX_QUAD | SPI_RX_DUAL | SPI_RX_QUAD)

struct spidev_data {
	dev_t			devt;
	spinlock_t		spi_lock;
	struct spi_device	*spi;
	struct list_head	device_entry;

	/* TX/RX buffers are NULL unless this device is open (users > 0) */
	struct mutex		buf_lock;
	unsigned		users;
	u8			*tx_buffer;
	u8			*rx_buffer;
	u32			speed_hz;
	unsigned int cur_addr;
	unsigned wp_gpio;
	unsigned int flash_id;
	unsigned int flash_size;
	ssize_t sector_offset;   //??????????????????
};

static LIST_HEAD(device_list);
static DEFINE_MUTEX(device_list_lock);

static unsigned bufsiz = 4096;
module_param(bufsiz, uint, S_IRUGO);
MODULE_PARM_DESC(bufsiz, "data bytes in biggest supported SPI message");

/*-------------------------------------------------------------------------*/

static char spi_gd25q_status(struct spi_device *spi)
{       
	int     status;
	char tbuf[]={READ_STATUS_REG};
	char rbuf[1] = {1};
	struct spi_transfer     t = {
		.tx_buf         = tbuf,
		.len            = ARRAY_SIZE(tbuf),
	};

	struct spi_transfer     r = {
		.rx_buf         = rbuf,
		.len            = ARRAY_SIZE(rbuf),
	};
	struct spi_message      m;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	spi_message_add_tail(&r, &m);
	status = spi_sync(spi, &m);

	return rbuf[0];
}
//modified by xzq degain for retry 5 times @20211105
static int spi_gd25q_wait_ready(struct spi_device *spi )
{
	char retval = 1;
	int retry = 5;
	dev_dbg(&spi->dev, "wait ready...");
	do {
		retval = spi_gd25q_status(spi);
		retval &= 0xff;
		retval &= 1;
		retry --;
		mdelay(5);
	}while((retval != 0) && (retry != 0));
	if(retval)
		dev_err(&spi->dev,"no ready\n");
	else
		dev_dbg(&spi->dev, "OK\n");
	return 0;
}
//modified by xzq end
static int spi_gd25q_write_enable(struct spi_device *spi)
{       
	int     status;
	char cmd_buf[1] = {WRITE_ENABLE};
	struct spi_transfer cmd = {
		.tx_buf = cmd_buf,
		.len = ARRAY_SIZE(cmd_buf),
	};

	struct spi_message      m;

	spi_message_init(&m);
	spi_message_add_tail(&cmd, &m);

	status = spi_sync(spi, &m);

	dev_dbg(&spi->dev, "write enable\n");

	return status;
}

static int spi_read_gd25q_id_0(struct spi_device *spi)
{       
	int     status;
	char tbuf[]={READ_UID};
	char rbuf[3];

	struct spi_transfer     t = {
		.tx_buf         = tbuf,
		.len            = ARRAY_SIZE(tbuf),
	};

	struct spi_transfer     r = {
		.rx_buf         = rbuf,
		.len            = ARRAY_SIZE(rbuf),
	};
	struct spi_message      m;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	spi_message_add_tail(&r, &m);
	status = spi_sync(spi, &m);

	dev_err(&spi->dev, "ID = %02x %02x %02x\n",
		rbuf[0], rbuf[1], rbuf[2]);

	return (rbuf[0]<<16| rbuf[1]<<8 | rbuf[2]);
}

static int
spi_gd25q_sector_erase(struct spidev_data *spidev, unsigned long size)
{
	int status;
	char cmd[4] = {SECTOR_ERASE};
	struct spi_device *spi = spidev->spi;
	struct spi_transfer t = {
		.tx_buf = cmd,
		.len = ARRAY_SIZE(cmd),
	};
	struct spi_message m;
	unsigned int flash_addr = spidev->cur_addr;
	int count = (int)size;
	printk("spi_gd25q_sector_erase flash_addr = %u\n",flash_addr);

	for ( ; count > 0; count -= GD25QXX_SECTOR) {
		cmd[1] = (unsigned char)((flash_addr & 0xff0000) >> 16);
		cmd[2] = (unsigned char)((flash_addr & 0xff00) >> 8);
		cmd[3] = (unsigned char)(flash_addr & 0xff);

		spi_gd25q_write_enable(spi);

		spi_message_init(&m);
		spi_message_add_tail(&t, &m);
		spi_gd25q_wait_ready(spi);
		status = spi_sync(spi, &m);
		
		dev_info(&spi->dev,"start addr: %x, sector erase OK\n", flash_addr);
		flash_addr += GD25QXX_SECTOR;
	}
	return status;
}

static int
spi_gd25q_32kb_block_erase(struct spidev_data *spidev)
{
	int status;
	char cmd[4] = {BLOCK_32KB_ERASE};
	struct spi_device *spi = spidev->spi;
	struct spi_transfer t = {
		.tx_buf = cmd,
		.len = ARRAY_SIZE(cmd),
	};
	struct spi_message m;

	cmd[1] = (unsigned char)((spidev->cur_addr & 0xff0000) >> 16);
	cmd[2] = (unsigned char)((spidev->cur_addr & 0xff00) >> 8);
	cmd[3] = (unsigned char)(spidev->cur_addr & 0xff);

	spi_gd25q_write_enable(spi);

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	spi_gd25q_wait_ready(spi);
	status = spi_sync(spi, &m);
	
	dev_dbg(&spi->dev,"32kb block erase OK\n");
	return status;
}

static int
spi_gd25q_64kb_block_erase(struct spidev_data *spidev)
{
	int status;
	char cmd[4] = {BLOCK_64KB_ERASE};
	struct spi_device *spi = spidev->spi;
	struct spi_transfer t = {
		.tx_buf = cmd,
		.len = ARRAY_SIZE(cmd),
	};
	struct spi_message m;

	cmd[1] = (unsigned char)((spidev->cur_addr & 0xff0000) >> 16);
	cmd[2] = (unsigned char)((spidev->cur_addr & 0xff00) >> 8);
	cmd[3] = (unsigned char)(spidev->cur_addr & 0xff);

	spi_gd25q_write_enable(spi);

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	spi_gd25q_wait_ready(spi);
	status = spi_sync(spi, &m);
	
	dev_dbg(&spi->dev,"64kb block erase OK\n");
	return status;
}

static int spi_gd25q_chip_erase(struct spi_device *spi)
{
	int status;
	char chip_erase[1] = {CHIP_ERASE};

	struct spi_transfer erase = {
		.tx_buf = chip_erase,
		.len = ARRAY_SIZE(chip_erase),
	};
	struct spi_message m;

	spi_gd25q_write_enable(spi);

	spi_message_init(&m);
	spi_message_add_tail(&erase, &m);
	spi_gd25q_wait_ready(spi);
	status = spi_sync(spi, &m);
	
	dev_dbg(&spi->dev,"chip erase OK\n");
	return status;
}

static loff_t
spi_gd25q_llseek(struct file *filp, loff_t offset, int orig)
{
	loff_t ret = 0;
	struct spidev_data	*spidev;

	spidev = filp->private_data;
	switch (orig) {
	case SEEK_SET:
		if (offset < 0) {
			ret = -EINVAL;
			break;
		}
		if ((unsigned int)offset > spidev->flash_size) {  //GD25QXX_SIZE
			ret = -EINVAL;
			break;
		}
		spidev->cur_addr = (unsigned int)offset;
		ret = spidev->cur_addr;
		break;
	case SEEK_CUR:
		if ((spidev->cur_addr + offset) > spidev->flash_size) {  //GD25QXX_SIZE
			ret = -EINVAL;
			break;
		}
		if ((spidev->cur_addr + offset) < 0) {
			ret = -EINVAL;
			break;
		}
		spidev->cur_addr += offset;
		ret = spidev->cur_addr;
		break;
	default:
		ret =  - EINVAL;
		break;
	}
	dev_dbg(&spidev->spi->dev, "set curr addr:%02X\n", (unsigned int)ret);
	return ret;

}

static ssize_t
spidev_sync(struct spidev_data *spidev, struct spi_message *message)
{
	DECLARE_COMPLETION_ONSTACK(done);
	int status;
	struct spi_device *spi;

	spin_lock_irq(&spidev->spi_lock);
	spi = spidev->spi;
	spin_unlock_irq(&spidev->spi_lock);

	if (spi == NULL)
		status = -ESHUTDOWN;
	else
		status = spi_sync(spi, message);

	if (status == 0)
		status = message->actual_length;

	return status;
}


#if 0
static int GD25qxx_write_page(GD25qxx_typdef *GD25q64,unsigned int address,unsigned char* buf,int count)
{
   int ret = -EINVAL;
   unsigned char *tx_buf;/*???????????????*/
   
   tx_buf = (unsigned char*)kzalloc(count+4,GFP_KERNEL);
   if(!tx_buf)
       return -ENOMEM;
 

   spi_write_enable();/*???????????????*/
//   spi_cs_enable();

   tx_buf[0] = GD25X_PageProgram;/*????????????*/
   tx_buf[1] = (unsigned char)((address>>16) & 0xFF);
   tx_buf[2] = (unsigned char)((address>>8) & 0xFF);
   tx_buf[3] = (unsigned char)(address & 0xFF);
   
   memcpy(&tx_buf[4],buf,count);

   GD25q64->data.tx_buf= tx_buf; 
   GD25q64->data.tx_len = count+4;

   GD25q64->data.rx_len = 0;/*????????????*/
   
   //printk("tx_data:%d-%d-%d-%d,count=%d\n",tx_buf[4],tx_buf[5],tx_buf[6],tx_buf[7],GD25q64->data.tx_len);

   ret = GD25q64_spi_read_write(GD25q64);

//   spi_cs_disable();

   if(ret != 0)
   {
      printk("GD25qxx???write page@%d ,%d bytes failed %d\n",address,count,ret);
      kfree(tx_buf);
      spi_write_disable();/*???????????????*/
      return ret;
   }
   ret = GD25qxx_wait_idle();
   kfree(tx_buf); 
   spi_write_disable();/*???????????????*/
   return ret;
}


#endif



//????????????????????????????????????????????????256???????????????????????????
static inline ssize_t
spidev_sync_write(struct spidev_data *spidev, size_t len)
{
	int status;
	char cmd[1] = {PAGE_PROGRAM};
	unsigned char addr[3];
	struct spi_transfer c[] = {
		{
			.tx_buf = cmd,
			.len = ARRAY_SIZE(cmd),
		},
		{
			.tx_buf = addr,
			.len = ARRAY_SIZE(addr),
		},
	};
	struct spi_transfer t = {
			.tx_buf		= spidev->tx_buffer,
			.len		= len,
			.speed_hz	= spidev->speed_hz,
	};
	struct spi_message	m;

	// int i;
	// for(i=0;i<len;i++)
	// {
	// 	printk("%02x ",spidev->tx_buffer[i]);
	// 	if(i%16 == 15)
	// 		printk("\n");
	// }
	// printk("\n");


	addr[0] = (unsigned char)((spidev->cur_addr & 0xff0000) >> 16);
	addr[1] = (unsigned char)((spidev->cur_addr & 0xff00) >> 8);
	addr[2] = (unsigned char)(spidev->cur_addr & 0xff);

	spi_gd25q_write_enable(spidev->spi);

	spi_message_init(&m);
	spi_message_add_tail(&c[0], &m);
	spi_message_add_tail(&c[1], &m);
	spi_message_add_tail(&t, &m);
	spi_gd25q_wait_ready(spidev->spi);
	status = spidev_sync(spidev, &m);
	

	status -= 4;
	printk("GD25qxx???spidev_sync_write status = %d  len = %lu\n",status,len);
	if(status > 0)
		spidev->cur_addr += status;   //??????????????????

	return status;
}



//??????????????????
static int GD25qxx_read_sector(struct spidev_data *spidev)
{
	int status;
	char cmd[] = {READ_DATA};
	unsigned char addr[3];
	struct spi_transfer	t[] = {
		{
			.tx_buf = cmd,
			.len = ARRAY_SIZE(cmd),
			.speed_hz = spidev->speed_hz,
		},
		{
			.tx_buf = addr,
			.len = ARRAY_SIZE(addr),
		},
		{
			.rx_buf		= spidev->rx_buffer,
			.len		= GD25QXX_SECTOR,//len, ??????????????????
			.speed_hz	= spidev->speed_hz,
		}
	};
	struct spi_message	m;
//	unsigned int sector_first_address = spidev->cur_addr &(~(GD25QXX_SECTOR-1));
//	???12??????0
	addr[0] = (unsigned char)((spidev->cur_addr & 0xff0000) >> 16);
	addr[1] = (unsigned char)((spidev->cur_addr & 0xf000) >> 8);
	addr[2] = (unsigned char)(0);

	spi_message_init(&m);
	spi_message_add_tail(&t[0], &m);
	spi_message_add_tail(&t[1], &m);
	spi_message_add_tail(&t[2], &m);
	spi_gd25q_wait_ready(spidev->spi);
	status = spidev_sync(spidev, &m);
	
	return status;	
}


//??????????????????1??????????????????0
static int GD25qxx_need_erase(unsigned char*old,unsigned char*new,int count)
{
   int i;
   unsigned char p;

   for ( i = 0; i < count; i++)
   {
      p = *old++;

      if(p == 0xff) {  //????????????????????????????????????
      	 //???????????????
      	continue;	
      }
      else if(p != new[i])   //??????????????????0xff,???????????????????????????????????????????????????????????????????????????
      {
         return 1;   //????????????
      }
      
   }
	return 0;

}


//len 1-4096 ??????????????????????????????16????????????256??????
static int GD25qxx_write_pages(struct spidev_data *spidev, size_t len)
{
   int ret = -EINVAL;
   unsigned int remain_of_page,need_to_write;
   unsigned int sector_first_address,sector_offset;
//   unsigned int write_address;
 //  unsigned char *write_buf;/*???????????????*/
   ssize_t total_write;

   // write_buf = (unsigned char*)kzalloc(GD25QXX_SECTOR,GFP_KERNEL);  //GD25QXX_SECTOR_SIZE == 4096
   // if(!write_buf)
   //     return -ENOMEM;

   /*????????????????????????????????????????????????*/    //??????????????????????????????????????????????????????????????????????????????????????????????????????
   sector_first_address = spidev->cur_addr & (~(GD25QXX_SECTOR-1));//(~(GD25Qxx_PAGE_SIZE-1)) ;   //?????????????????????
   //write_address = spidev->cur_addr;
   /*????????????????????????????????????????????????*/
   sector_offset = spidev->cur_addr % GD25QXX_SECTOR;    //???????????????????????????GD25QXX_SECTOR_SIZE == 4096

   if(len + spidev->sector_offset > GD25QXX_SECTOR)  //????????????
   		return -EMSGSIZE;

   //ret = GD25qxx_read_bytes(GD25q64,sector_first_address,write_buf,GD25QXX_SECTOR_SIZE);//??????????????????
   ret =  GD25qxx_read_sector(spidev);   //????????????????????????
   if(ret < 0 )
   {
      return ret;
   }
   // if(spidev->sector_offset > 0)
   // 		memcpy(write_buf,spidev->rx_buffer,GD25QXX_SECTOR);   //?????????????????????????????????
   // 	else
   // 		memcpy(write_buf,spidev->tx_buffer,GD25QXX_SECTOR);


   printk("1.GD25qxx GD25qxx_write_pages???spidev->sector_offset = %lu\n",spidev->sector_offset);

   /*????????????????????????*/
   if(GD25qxx_need_erase(&spidev->rx_buffer[spidev->sector_offset],spidev->tx_buffer,len))
   {
      printk("2.GD25qxx GD25qxx_write_pages???GD25qxx???erase\n");
      //GD25qxx_erase_sector(GD25q64,sector_first_address);
      spi_gd25q_sector_erase(spidev, GD25QXX_SECTOR-1);
    //  msleep(500);
      //?????????????????????????????????????????????????????????????????????
    //  memcpy(write_buf+spidev->sector_offset,spidev->tx_buffer,len);   //????????????
      memcpy(spidev->tx_buffer,spidev->rx_buffer,spidev->sector_offset);
      remain_of_page = GD25QXX_PAGE_LENGTH;    //?????????????????????????????????
      
      len = GD25QXX_SECTOR;   //count?????????????????????????????????????????????????????????????????????????????????????????????????????????
      //sector_offset = 0;   //????????????????????????0
      spidev->cur_addr  = sector_first_address;
      //spidev->cur_addr  = sector_first_address;  //??????????????????
      //memcpy(spidev->tx_buffer,write_buf,remain_of_page);  //?????????????????????
    //  buf = write_buf;    //?????????????????????????????????buf
   }
   else //???????????????????????????????????????????????????
   {
   		remain_of_page = GD25QXX_PAGE_LENGTH - spidev->cur_addr%GD25QXX_PAGE_LENGTH;//????????????????????????????????????????????????
   	//	spidev->cur_addr  = write_address; 
   }
    
   need_to_write = remain_of_page;/*?????????????????????remain_of_page?????????*/
   total_write = 0;
   printk("3.GD25qxx GD25qxx_write_pages???sector_first_address=%d,sector_offset=%d\n",sector_first_address,sector_offset);

   printk("4.GD25qxx???cur_addr=%u,len=%lu\n",spidev->cur_addr,len);

   if(len <= need_to_write) //????????????
   {
   	 printk("5.GD25qxx GD25qxx_write_pages???len <= need_to_write,len=%lu\n",len);
      /*??????????????????????????????????????????  ???????????????????????????*/
      //ret = GD25qxx_write_page(GD25q64,address,buf,count);
      ret = spidev_sync_write(spidev, len);
      return ret;
   }
   else
   {    
      do
      {
         ret = spidev_sync_write(spidev, need_to_write);
         if(ret != need_to_write )
         {
         	printk("7.GD25qxx GD25qxx_write_pages???ret =%d ,need_to_write=%d\n",ret,need_to_write);
            return ret;
         }
         total_write += ret;
         if(need_to_write == len)
         {        	
//         	 printk("8.GD25qxx GD25qxx_write_pages??????need_to_write == len??? =%d\n",need_to_write);
             break;
         }
         else
         {
         //   total_write += ret;//need_to_write;  //???????????????????????? 
            len -=   ret;//need_to_write ;   //????????????          
            if(len > GD25QXX_PAGE_LENGTH)
            {
               need_to_write = GD25QXX_PAGE_LENGTH;  //??????256??????
            }
            else
            {
               need_to_write = len;
            }

            //???????????????
            memcpy(spidev->tx_buffer,spidev->tx_buffer+total_write,need_to_write);  //????????????????????? 
         } 
         printk("!!!9.GD25qxx GD25qxx_write_pages???total_write =%lu ,need_to_write=%d\n",total_write,need_to_write); 

      } while (1);  
   }

//	kfree(write_buf);
	printk("10.GD25qxx GD25qxx_write_pages???total_write =%lu\n",total_write); 
	return total_write;
}




#if 0
static int GD25qxx_write_more_bytes(struct spidev_data *spidev, size_t len)
{
   int ret = -EINVAL;
   unsigned int num_of_sector,remain_of_sector,sector_offset;
   unsigned int need_to_write;//sector_first_address
   unsigned char *read_buf;/*???????????????*/
   unsigned char *pbuf = spidev->tx_buffer;  //??????????????????
   //1. ??????????????????4096??????????????????????????????????????????????????????????????????????????????
   read_buf = (unsigned char*)kzalloc(GD25QXX_SECTOR_SIZE,GFP_KERNEL);
   if(!read_buf)
       return -ENOMEM;
   //2. ??????????????????????????????????????????????????????
//   num_of_sector = spidev->cur_addr / GD25QXX_SECTOR_SIZE;  //GD25QXX_SECTOR_SIZE == 4096
   sector_offset = spidev->cur_addr % GD25QXX_SECTOR_SIZE;
   //2.1 ????????????
   remain_of_sector = GD25QXX_SECTOR_SIZE - sector_offset;/*???????????????????????? ?????????????????????*/
   
   need_to_write = remain_of_sector;   //???????????????
   //3. ????????????
	//3.1 ?????????????????? ?????? ?????????????????????????????????
   if(count <= need_to_write)   //?????????????????????????????????????????????????????????????????????
   {
      ret = GD25qxx_write_pages(GD25q64,address,buf,count);
      return ret;
   }
   //3.2 ?????????????????????????????????
   else
   {
       do  //??????buf??????????????????4096??????????????????
      {
         ret = GD25qxx_write_pages(GD25q64,address,buf,need_to_write);
         if(ret !=0)
         {
            return ret;
         }
         if(need_to_write == count)
         {
             break;
         }
         else
         {
            buf+=need_to_write;   //buf????????????????????????
            address+=need_to_write;  //???????????????????????????
            count-=need_to_write;      //????????????????????????    
            if(count > GD25QXX_SECTOR_SIZE)  //????????????????????????????????????????????????
            {
               need_to_write = GD25QXX_SECTOR_SIZE;  //?????????????????????????????????
            }
            else
            {
               need_to_write = count;   //?????????????????????????????????
            }
         }        
      } while (1);  
   }
   return ret;
}
#endif




static inline ssize_t
spidev_sync_read(struct spidev_data *spidev, size_t len)
{
	int status;
	char cmd[] = {READ_DATA};
	unsigned char addr[3];
	struct spi_transfer	t[] = {
		{
			.tx_buf = cmd,
			.len = ARRAY_SIZE(cmd),
			.speed_hz = spidev->speed_hz,
		},
		{
			.tx_buf = addr,
			.len = ARRAY_SIZE(addr),
		},
		{
			.rx_buf		= spidev->rx_buffer,
			.len		= len,
			.speed_hz	= spidev->speed_hz,
		}
	};
	struct spi_message	m;

	addr[0] = (unsigned char)((spidev->cur_addr & 0xff0000) >> 16);
	addr[1] = (unsigned char)((spidev->cur_addr & 0xff00) >> 8);
	addr[2] = (unsigned char)(spidev->cur_addr & 0xff);

	spi_message_init(&m);
	spi_message_add_tail(&t[0], &m);
	spi_message_add_tail(&t[1], &m);
	spi_message_add_tail(&t[2], &m);
	spi_gd25q_wait_ready(spidev->spi);
	status = spidev_sync(spidev, &m);
	


	status -= 4;  //?????????4?????????
	printk("GD25qxx???spidev_sync_read status = %d  len = %lu\n",status,len);
	if(status > 0)
		spidev->cur_addr += status;   //??????????????????

	return status;
}

/* Read-only message with current device setup */
static ssize_t
spidev_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	struct spidev_data	*spidev;
	ssize_t			status = 0;
	ssize_t al_read_size = 0;
	ssize_t ready_read_size = 0;
	/* chipselect only toggles at start or end of operation */
	// if (count > bufsiz)
	// 	return -EMSGSIZE;

	spidev = filp->private_data;
	printk("GD25qxx???spidev_sync_read count = %lu",count);	
	while(count > 0)
	{
		if(count > bufsiz)
			ready_read_size = bufsiz;   //??????????????????????????????
		else
			ready_read_size = count;
		printk("GD25qxx???spidev_sync_read count = %lu  ready_read_size = %lu\n",count,ready_read_size);
		mutex_lock(&spidev->buf_lock);
		status = spidev_sync_read(spidev, ready_read_size);
		if (status <= ready_read_size) {
			unsigned long	missing;			
			count -= status;  //?????????????????????????????????

			missing = copy_to_user(buf+al_read_size, spidev->rx_buffer, status);
			if (missing == status)
			{
				status = -EFAULT;
				mutex_unlock(&spidev->buf_lock);
				return status;
			}	
			else{
				status = status - missing;
			}
			al_read_size += status;   //????????????????????????
		}
		else  //????????????
		{
			printk("GD25qxx???error status > ready_read_size %lu\n",status);
			mutex_unlock(&spidev->buf_lock);
			return -EFAULT;
		}
		mutex_unlock(&spidev->buf_lock);		
	}
	return status;
}

/* Write-only message with current device setup */
static ssize_t
spidev_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *f_pos)
{
	struct spidev_data	*spidev;
	ssize_t			status = 0,write_total = 0;
	unsigned long		missing;
	size_t need_write;
	size_t offset;
//	unsigned char *write_buf;/*???????????????*/
	/* chipselect only toggles at start or end of operation */
	// if (count > bufsiz)
	// 	return -EMSGSIZE;

	spidev = filp->private_data;
	if((count + spidev->cur_addr) >= spidev->flash_size) //??????????????????????????????flash???????????????????????????????????????
		return -EMSGSIZE;

	// write_buf = (unsigned char*)kzalloc(count,GFP_KERNEL);  //GD25QXX_SECTOR_SIZE == 4096
 //   if(!write_buf)
 //       return -ENOMEM;


	printk( "1.spidev_write:count = %lu\n", count);
	
	if(spidev->wp_gpio != INVALID_GPIO_PIN)
		gpio_set_value(spidev->wp_gpio, 1);
	
	//????????????????????????????????????????????????
	offset = spidev->cur_addr % GD25QXX_SECTOR;   //????????????????????????????????????????????????
	printk( "2.spidev_write:first offset = %lu,pidev->cur_addr = %u\n", offset,spidev->cur_addr);
	need_write = count;
	if(count > GD25QXX_SECTOR || offset)  //????????????4096????????????????????????????????????????????????
	{
		if (count > GD25QXX_SECTOR) { //??????????????????4096
			need_write = GD25QXX_SECTOR - offset;  //?????????4096?????????????????????			
		} 			
		else if(count + offset > GD25QXX_SECTOR){//??????????????????????????? ?????????????????????
			need_write = GD25QXX_SECTOR - offset;  //????????????????????????????????????????????????
		} 
		else {
			need_write = count;
		}			
	}
//	printk("3.spidev_write:first count = %lu need_write = %lu\n", count,need_write);

	while(count > 0)
	{		
		printk("4.spidev_write:while count = %lu need_write = %lu\n", count,need_write);
		printk("!!!4.1.spidev_write:pidev->cur_addr = %u,write_total = %lu,offset = %lu\n", spidev->cur_addr,write_total,offset);
		spidev->sector_offset = offset;//GD25QXX_SECTOR - need_write;
		mutex_lock(&spidev->buf_lock);
		missing = copy_from_user(spidev->tx_buffer+offset, buf, need_write);
		if (missing == 0){
		//	status = spidev_sync_write(spidev, need_write);
				// int i;
				// for(i=0;i<need_write;i++)
				// {
				// 	printk("%02x ",spidev->tx_buffer[i]);
				// 	if(i%16 == 15)
				// 		printk("\n");
				// }
				// printk("\n-----------------------------------------\n");
			status = GD25qxx_write_pages(spidev, need_write);
			write_total += status;
		}
		else
		{
			printk("5.spidev_write: ERROR: copy_from_user\n");
			status = -EFAULT;
			mutex_unlock(&spidev->buf_lock);
			if(spidev->wp_gpio != INVALID_GPIO_PIN)
				gpio_set_value(spidev->wp_gpio, 0);
			return status;
		}	
		mutex_unlock(&spidev->buf_lock);

		count -= need_write;  //??????????????????????????????
		buf += need_write;   //????????????
		offset = 0;  //????????????????????????????????????

		if (count > bufsiz)  //??????????????????4096
			need_write = bufsiz;
		else
			need_write = count;

		printk("++++++6.spidev_write: count = %lu need_write = %lu\n", count,need_write);
	}

	if(spidev->wp_gpio != INVALID_GPIO_PIN)
		gpio_set_value(spidev->wp_gpio, 0);

	printk("+++++7.spidev_write: write_total = %lu \n", write_total);
	return status;
}

static long
spidev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int			err = 0;
	int			retval = 0;
	struct spidev_data	*spidev;
	struct spi_device	*spi;
	u32			tmp;

	/* Check type and command number */
	if (_IOC_TYPE(cmd) != GD25QXX_MAGIC)
		return -ENOTTY;

	/* Check access direction once here; don't repeat below.
	 * IOC_DIR is from the user perspective, while access_ok is
	 * from the kernel perspective; so they look reversed.
	 */
	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE,
				(void __user *)arg, _IOC_SIZE(cmd));
	if (err == 0 && _IOC_DIR(cmd) & _IOC_WRITE)
		err = !access_ok(VERIFY_READ,
				(void __user *)arg, _IOC_SIZE(cmd));
	if (err)
		return -EFAULT;

	/* guard against device removal before, or while,
	 * we issue this ioctl.
	 */
	spidev = filp->private_data;
	spin_lock_irq(&spidev->spi_lock);
	spi = spi_dev_get(spidev->spi);
	spin_unlock_irq(&spidev->spi_lock);

	if (spi == NULL)
		return -ESHUTDOWN;

	/* use the buffer lock here for triple duty:
	 *  - prevent I/O (from us) so calling spi_setup() is safe;
	 *  - prevent concurrent SPI_IOC_WR_* from morphing
	 *    data fields while SPI_IOC_RD_* reads them;
	 *  - SPI_IOC_MESSAGE needs the buffer locked "normally".
	 */
	mutex_lock(&spidev->buf_lock);

	switch (cmd) {
	/* read requests */
	case GD25QXX_IOC_SECTOR_ERASE:
		retval = spi_gd25q_sector_erase(spidev, arg==0?1:arg);
		break;
	case GD25QXX_IOC_32KB_BLOCK_ERASE:
		retval = spi_gd25q_32kb_block_erase(spidev);
		break;
	case GD25QXX_IOC_64KB_BLOCK_ERASE:
		retval = spi_gd25q_64kb_block_erase(spidev);
		break;
	case GD25QXX_IOC_CHIP_ERASE:
		retval = spi_gd25q_chip_erase(spi);
		break;

	case GD25QXX_IOC_GET_CAPACITY:  //?????????????????????dts?????????
	// 	printk("ioctrl spidev->flash_size = %u\n",spidev->flash_size);
		retval = __put_user(spidev->flash_size,(__u32 __user *)arg);
		break;	
	case GD25QXX_IOC_GET_ID:  //????????????ID???probe??????
	//	printk("ioctrl spidev->flash_id = %u\n",spidev->flash_id);
		retval = __put_user(spidev->flash_id,(__u32 __user *)arg);
		break;	
	case SPI_IOC_RD_MODE:
		retval = __put_user(spi->mode & SPI_MODE_MASK,
					(__u8 __user *)arg);
		break;
	case SPI_IOC_RD_MODE32:
		retval = __put_user(spi->mode & SPI_MODE_MASK,
					(__u32 __user *)arg);
		break;
	case SPI_IOC_RD_LSB_FIRST:
		retval = __put_user((spi->mode & SPI_LSB_FIRST) ?  1 : 0,
					(__u8 __user *)arg);
		break;
	case SPI_IOC_RD_BITS_PER_WORD:
		retval = __put_user(spi->bits_per_word, (__u8 __user *)arg);
		break;
	case SPI_IOC_RD_MAX_SPEED_HZ:
		retval = __put_user(spidev->speed_hz, (__u32 __user *)arg);
		break;

	/* write requests */
	case SPI_IOC_WR_MODE:
	case SPI_IOC_WR_MODE32:
		if (cmd == SPI_IOC_WR_MODE)
			retval = __get_user(tmp, (u8 __user *)arg);
		else
			retval = __get_user(tmp, (u32 __user *)arg);
		if (retval == 0) {
			u32	save = spi->mode;

			if (tmp & ~SPI_MODE_MASK) {
				retval = -EINVAL;
				break;
			}

			tmp |= spi->mode & ~SPI_MODE_MASK;
			spi->mode = (u16)tmp;
			retval = spi_setup(spi);
			if (retval < 0)
				spi->mode = save;
			else
				dev_dbg(&spi->dev, "spi mode %x\n", tmp);
		}
		break;
	case SPI_IOC_WR_LSB_FIRST:
		retval = __get_user(tmp, (__u8 __user *)arg);
		if (retval == 0) {
			u32	save = spi->mode;

			if (tmp)
				spi->mode |= SPI_LSB_FIRST;
			else
				spi->mode &= ~SPI_LSB_FIRST;
			retval = spi_setup(spi);
			if (retval < 0)
				spi->mode = save;
			else
				dev_dbg(&spi->dev, "%csb first\n",
						tmp ? 'l' : 'm');
		}
		break;
	case SPI_IOC_WR_BITS_PER_WORD:
		retval = __get_user(tmp, (__u8 __user *)arg);
		if (retval == 0) {
			u8	save = spi->bits_per_word;

			spi->bits_per_word = tmp;
			retval = spi_setup(spi);
			if (retval < 0)
				spi->bits_per_word = save;
			else
				dev_dbg(&spi->dev, "%d bits per word\n", tmp);
		}
		break;
	case SPI_IOC_WR_MAX_SPEED_HZ:
		retval = __get_user(tmp, (__u32 __user *)arg);
		if (retval == 0) {
			u32	save = spi->max_speed_hz;

			spi->max_speed_hz = tmp;
			retval = spi_setup(spi);
			if (retval >= 0)
				spidev->speed_hz = tmp;
			else
				dev_dbg(&spi->dev, "%d Hz (max)\n", tmp);
			spi->max_speed_hz = save;
		}
		break;
		default:
			return -EINVAL;

	}

	mutex_unlock(&spidev->buf_lock);
	spi_dev_put(spi);
	return retval;
}

#ifdef CONFIG_COMPAT
static long
spidev_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return spidev_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#else
#define spidev_compat_ioctl NULL
#endif /* CONFIG_COMPAT */

static int spidev_open(struct inode *inode, struct file *filp)
{
	struct spidev_data	*spidev;
	int			status = -ENXIO;

	printk("bufsiz = %d\n",bufsiz);
	mutex_lock(&device_list_lock);

	list_for_each_entry(spidev, &device_list, device_entry) {
		if (spidev->devt == inode->i_rdev) {
			status = 0;
			break;
		}
	}

	if (status) {
		pr_debug("spidev: nothing for minor %d\n", iminor(inode));
		goto err_find_dev;
	}

	if (!spidev->tx_buffer) {
		spidev->tx_buffer = kmalloc(bufsiz, GFP_KERNEL);
		if (!spidev->tx_buffer) {
			dev_err(&spidev->spi->dev, "open/ENOMEM\n");
			status = -ENOMEM;
			goto err_find_dev;
		}
	}

	if (!spidev->rx_buffer) {
		spidev->rx_buffer = kmalloc(bufsiz, GFP_KERNEL);
		if (!spidev->rx_buffer) {
			dev_err(&spidev->spi->dev, "open/ENOMEM\n");
			status = -ENOMEM;
			goto err_alloc_rx_buf;
		}
	}

	spidev->users++;
	filp->private_data = spidev;

	mutex_unlock(&device_list_lock);
	return 0;

err_alloc_rx_buf:
	kfree(spidev->tx_buffer);
	spidev->tx_buffer = NULL;
err_find_dev:
	mutex_unlock(&device_list_lock);
	return status;
}

static int spidev_release(struct inode *inode, struct file *filp)
{
	struct spidev_data	*spidev;

	mutex_lock(&device_list_lock);
	spidev = filp->private_data;
	filp->private_data = NULL;

	/* last close? */
	spidev->users--;
	if (!spidev->users) {
		int		dofree;

		kfree(spidev->tx_buffer);
		spidev->tx_buffer = NULL;

		kfree(spidev->rx_buffer);
		spidev->rx_buffer = NULL;

		spin_lock_irq(&spidev->spi_lock);
		if (spidev->spi)
			spidev->speed_hz = spidev->spi->max_speed_hz;

		/* ... after we unbound from the underlying device? */
		dofree = (spidev->spi == NULL);
		spin_unlock_irq(&spidev->spi_lock);

		if (dofree)
			kfree(spidev);
	}
	mutex_unlock(&device_list_lock);

	return 0;
}

static const struct file_operations spidev_fops = {
	.owner =	THIS_MODULE,
	/* REVISIT switch to aio primitives, so that userspace
	 * gets more complete API coverage.  It'll simplify things
	 * too, except for the locking.
	 */
	.write =	spidev_write,
	.read =		spidev_read,
	.unlocked_ioctl = spidev_ioctl,
	.compat_ioctl = spidev_compat_ioctl,
	.open =		spidev_open,
	.release =	spidev_release,
	.llseek =	spi_gd25q_llseek,
};

/*-------------------------------------------------------------------------*/

/* The main reason to have this class is to make mdev/udev create the
 * /dev/spidevB.C character device nodes exposing our userspace API.
 * It also simplifies memory management.
 */

static struct class *spidev_class;

#ifdef CONFIG_OF
static const struct of_device_id spidev_dt_ids[] = {
	{ .compatible = "rockchip,GD25QXX" },
	{},
};
MODULE_DEVICE_TABLE(of, spidev_dt_ids);
#endif

/*-------------------------------------------------------------------------*/

/*
  W25Q80DV ????????? 8M-bit???16 Block???256 Sector???4096 Page
  W25Q16   ????????? 16M-Bit???32 Block???512 Sector???8192 Page
  W25Q32   ????????? 32M-Bit???64 Block???1024 Sector???16384 Page
  W25Q64   ????????? 64M-Bit???128 Block???2048 Sector???32768 Page
  W25Q128  ????????? 128M-Bit???256 Block???4096 Sector???65536 Page
 */



static int spidev_probe(struct spi_device *spi)
{
	struct spidev_data	*spidev;
	struct device_node *np = spi->dev.of_node;
	int			status;
	unsigned long		minor;
	dev_err(&spi->dev, "probe,rk3399.0\n");
	/*
	 * spidev should never be referenced in DT without a specific
	 * compatible string, it is a Linux implementation thing
	 * rather than a description of the hardware.
	 */
	if (spi->dev.of_node && !of_match_device(spidev_dt_ids, &spi->dev)) {
		dev_err(&spi->dev, "buggy DT: spidev listed directly in DT\n");
		WARN_ON(spi->dev.of_node &&
			!of_match_device(spidev_dt_ids, &spi->dev));
	}

	/* Allocate driver data */
	spidev = kzalloc(sizeof(*spidev), GFP_KERNEL);
	if (!spidev)
		return -ENOMEM;

	/* Initialize the driver data */
	spidev->spi = spi;
	spin_lock_init(&spidev->spi_lock);
	mutex_init(&spidev->buf_lock);

	INIT_LIST_HEAD(&spidev->device_entry);

	/* If we can allocate a minor number, hook up this device.
	 * Reusing minors is fine so long as udev or mdev is working.
	 */
	mutex_lock(&device_list_lock);
	minor = find_first_zero_bit(minors, N_SPI_MINORS);
	if (minor < N_SPI_MINORS) {
		struct device *dev;

		spidev->devt = MKDEV(SPIDEV_MAJOR, minor);
		dev = device_create(spidev_class, &spi->dev, spidev->devt,
				    spidev, "GD25QXX");
		status = PTR_ERR_OR_ZERO(dev);
	} else {
		dev_err(&spi->dev, "no minor number available!\n");
		status = -ENODEV;
	}
	if (status == 0) {
		set_bit(minor, minors);
		list_add(&spidev->device_entry, &device_list);
	}
	mutex_unlock(&device_list_lock);

	spidev->speed_hz = spi->max_speed_hz;

	if (status == 0)
		spi_set_drvdata(spi, spidev);
	else
		kfree(spidev);


	spidev->wp_gpio = of_get_named_gpio(np, "wp-gpio", 0);
	if (!gpio_is_valid(spidev->wp_gpio)) {
        dev_err(&spi->dev, "wp-gpio:  is invalid\n");
    //    return -ENODEV;
        spidev->wp_gpio = INVALID_GPIO_PIN;   //??????wp??????
    }
    else
    {
		status = gpio_request(spidev->wp_gpio, "wp-gpio");
		if (status) {
	        dev_err(&spi->dev, "wp-gpio: %d request failed!\n", spidev->wp_gpio);
	        gpio_free(spidev->wp_gpio);
	        return -ENODEV;
	    }

		gpio_direction_output(spidev->wp_gpio, 0);
		gpio_export(spidev->wp_gpio, 0);    	
    }

	if (of_property_read_u32(np, "flash_size", &spidev->flash_size)) {
        dev_err(&spi->dev, "flash_size is invalid,please define flash_size,use default size 1MB\n");
    //    return -ENODEV;
        spidev->flash_size = 0x100000;   //?????????1M
    }
	dev_info(&spi->dev, "get flash_size: %#x \n", spidev->flash_size);

	spidev->flash_id = spi_read_gd25q_id_0(spi);

	return status;
}

static int spidev_remove(struct spi_device *spi)
{
	struct spidev_data	*spidev = spi_get_drvdata(spi);

	if(spidev->wp_gpio != INVALID_GPIO_PIN)   //???????????????
		gpio_free(spidev->wp_gpio);

	/* make sure ops on existing fds can abort cleanly */
	spin_lock_irq(&spidev->spi_lock);
	spidev->spi = NULL;
	spin_unlock_irq(&spidev->spi_lock);

	/* prevent new opens */
	mutex_lock(&device_list_lock);
	list_del(&spidev->device_entry);
	device_destroy(spidev_class, spidev->devt);
	clear_bit(MINOR(spidev->devt), minors);
	if (spidev->users == 0)
		kfree(spidev);
	mutex_unlock(&device_list_lock);

	return 0;
}

static struct spi_driver spidev_spi_driver = {
	.driver = {
		.name =		"GD25QXX",
		.of_match_table = of_match_ptr(spidev_dt_ids),
	},
	.probe =	spidev_probe,
	.remove =	spidev_remove,

	/* NOTE:  suspend/resume methods are not necessary here.
	 * We don't do anything except pass the requests to/from
	 * the underlying controller.  The refrigerator handles
	 * most issues; the controller driver handles the rest.
	 */
};

/*-------------------------------------------------------------------------*/

static int __init spidev_init(void)
{
	int status;
	
	/* Claim our 256 reserved device numbers.  Then register a class
	 * that will key udev/mdev to add/remove /dev nodes.  Last, register
	 * the driver which manages those device numbers.
	 */
	BUILD_BUG_ON(N_SPI_MINORS > 256);
	status = register_chrdev(SPIDEV_MAJOR, "GD25QXX", &spidev_fops);
	if (status < 0)
		return status;

	spidev_class = class_create(THIS_MODULE, "GD25QXX");
	if (IS_ERR(spidev_class)) {
		unregister_chrdev(SPIDEV_MAJOR, spidev_spi_driver.driver.name);
		return PTR_ERR(spidev_class);
	}

	status = spi_register_driver(&spidev_spi_driver);
	if (status < 0) {
		class_destroy(spidev_class);
		unregister_chrdev(SPIDEV_MAJOR, spidev_spi_driver.driver.name);
	}
	return status;
}
module_init(spidev_init);

static void __exit spidev_exit(void)
{
	spi_unregister_driver(&spidev_spi_driver);
	class_destroy(spidev_class);
	unregister_chrdev(SPIDEV_MAJOR, spidev_spi_driver.driver.name);
}
module_exit(spidev_exit);

MODULE_AUTHOR("Zhaodazhi");
MODULE_DESCRIPTION("User mode SPI device interface");
MODULE_LICENSE("GPL");

