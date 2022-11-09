/*
* @Author: dazhi
* @Date:   2022-11-05 15:20:06
* @Last Modified by:   dazhi
* @Last Modified time: 2022-11-09 11:18:45
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <limits.h>
#include <asm/ioctls.h>
#include <time.h>
#include <pthread.h>
#include <string.h>
#include <getopt.h>
#include <linux/types.h>
#include <stdint.h>
#include "gd25qxx.h"
#include <sys/ioctl.h>


#define  num 16

static const char *device = "/dev/GD25QXX";
static uint32_t mode;
static uint32_t erase_sector_offset;
static int verbose;
static int erase_chip = 0;   //擦除整个芯片
static char *input_tx = NULL;
static int start_address;
static int op_lenght;
static int operation = 0;   //0读操作，1写操作，2是擦除操作

void print_data(const char *title, char *dat, int count)
{
   int i = 0; 

   printf("%s\n",title);

   for(i = 0; i < count; i++) 
   {
      printf(" 0x%x", dat[i]);
      if(i%16 == 15)
         printf("\n");
   }
   printf("\n");
}


static void print_usage(const char *prog)
{
   printf("Usage: %s [-DsbdlHOLC3]\n", prog);
   puts("  -D --device   device to use (default /dev/spidev1.1)\n"
        "  -v --verbose  Verbose (show tx buffer)\n"
        "  -e --erase sector  erase sector \n"
        "  -E --erase chip  erase chip\n"
        "  -a --address  start address (default 0)\n"
        "  -l --lenght  operation bytes count (default 16,or(-p) string length) \n"
        "  -w --write data           Send data (e.g. \"1234\\xde\\xad\")\n"
);
   exit(1);
}

static void parse_opts(int argc, char *argv[])
{
   while (1) {
      static const struct option lopts[] = {
         { "device",  1, 0, 'D' },
         { "erase sector",1,0,'e'},
         { "erase chip",0,0,'E'},
         { "verbose", 0, 0, 'v' },
         { "address", 1, 0, 'a' },
         { "lenght", 1, 0, 'l' },
         { "write", 1, 0, 'w' },
         { NULL, 0, 0, 0 },
      };
      int c;

      c = getopt_long(argc, argv, "D:e:Ew:va:l:", lopts, NULL);

      if (c == -1)
      {
      //   print_usage(argv[0]);
         return;
      // break;
      }  

      switch (c) {
      case 'D':
         device = optarg;
         printf("device = %s\n",device);
         break;
      case 'e':
         erase_sector_offset = atoi(optarg);
         operation = 2;
         printf("erase_sector_offset = %d\n",erase_sector_offset);
         break;
      case 'E':
         erase_chip = 1;   //设置擦除
         operation = 2;
         printf("erase_chip\n");
         break;
      case 'v':
         verbose = 1;
         printf("verbose\n");
         break;
      case 'l':
         op_lenght = atoi(optarg);
         printf("op_lenght = %d\n",op_lenght);
         break;
      case 'a':
         start_address = atoi(optarg);
         printf("start_address = %d\n",start_address);
         break;
      case 'w':
         input_tx = optarg;

         if(input_tx)  //不为空
            operation = 1;
         printf("input_tx = %s\n",input_tx);
         break;

      default:
         print_usage(argv[0]);
         break;
      }
   }
}





int main(int argc, char *argv[])
{
   int fd,ret,i;
//   int count = num;
//   int offset = 0; 
//   int rw = -1;  
   char *buf;   //读写不同时进行

   /*判断传入的参数是否合法*/
   // if(argc < 3)
   // {
   //    printf("Usage:<offset> <r | w> [length(bytes),default:64]\n");
   //    return -1;
   // }
   parse_opts(argc, argv);

   if( 2==operation ){
      if(erase_sector_offset < 0)
      {
         printf("ERROR: -e option erase_sector_offset\n");
         return -1;
      } 
   }
   if(start_address < 0)
       start_address = 0;   


//   return 0;

   /*解析传入的参数*/
   // offset =atoi(argv[1]);
   // printf("offset = %d\n", offset);
   if(op_lenght <= 0 || op_lenght > 4096)
      op_lenght = 16;
   buf = malloc(op_lenght + 1);
   if(!buf)
   {
      printf("ERROR: malloc\n");
      return -1;
   }

   memset(buf,0,op_lenght + 1);

    /*打开设备文件*/
   fd = open(device, O_RDWR);
   if(fd < 0)
   {
      printf("open %s failed ,error = %s\n",device,strerror(fd)); 
      return fd;
   }


   if(!operation)  //读操作
   {
      printf("operation : read \n");

      ret = lseek(fd,start_address,SEEK_SET);
      printf("lseek = %d\n",ret);
      
      ret = read(fd, buf, op_lenght);
      if(ret < 0)
      {
         printf("read from w25qxx error\n");
         close(fd);
         return ret;
      }

      /*打印数据*/
      print_data("read from w25qxx: \n\r",buf, op_lenght);

   }
   else if(1==operation){   //写操作
      printf("operation : write \n");


      /*写入数据*/ 
      lseek(fd,start_address,SEEK_SET);
      ret = write(fd,input_tx,strlen(input_tx));
      if(ret < 0)
      {
         printf("write to w25qxx error ret = %d\n",ret);
         close(fd);
         return ret;
      }   
      /*打印数据*/
      print_data("write to w25qxx: \n\r", input_tx, strlen(input_tx));

   }
   else {  //擦除操作
      printf("operation : erase \n");
      if(erase_chip)
      {
         printf("operation : erase chip\n");
         ioctl(fd, GD25QXX_IOC_CHIP_ERASE, op_lenght);
      }
      else
      {
         ret = lseek(fd,erase_sector_offset,SEEK_SET);
         ret = ioctl(fd, GD25QXX_IOC_SECTOR_ERASE, op_lenght);         
      }
   }


   //程序结束，做一下处理工作
   free(buf);
   close(fd);
   return 0;

   // if(0 == strcmp(argv[2],"r"))
   //    rw = 0;
   // else if(0 == strcmp(argv[2],"w")) 
   //    rw = 1;
   // else
   // {
   //    printf("ERROR: R or W\n");
   //    printf("Usage:<offset> <r | w> <length(bytes)>\n");
   //    return -1;
   // }

   // if(argc >= 4){
   //    count = atoi(argv[3]);
   //    if(count <= 0)
   //       count = num;

   //    printf("count = %d\n",count);
   // }




   // if(1 == rw)
   // {

   //       /*缓存数组赋值*/
   // //memset(write_buf, 0x55, num);
   //    for(i = 0; i < count; i++)
   //    {
   //       write_buf[i] = i;
   //    }
   //        /*写入数据*/ 
   //    lseek(fd,offset,SEEK_SET);
   //    ret = write(fd,write_buf,count);
   //    if(ret < 0)
   //    {
   //       printf("write to w25qxx error\n");
   //       close(fd);
   //       return ret;
   //    }   
   //    /*打印数据*/
   //    print_data("write to w25qxx: \n\r", write_buf, count);  
   // }
   // else if(0 == rw)
   // {
   //    /*读取数据*/
   //    ret = lseek(fd,offset,SEEK_SET);
   //    printf("lseek = %d\n",ret);
      
   //    ret = read(fd, read_buf, count);
   //    if(ret < 0)
   //    {
   //       printf("read from w25qxx error\n");
   //       close(fd);
   //       return ret;
   //    }
      
   //    /*打印数据*/
   //    print_data("read from w25qxx: \n\r",read_buf, count);
   // }


   // ret = memcmp(write_buf, read_buf, count);
   // if(ret)
   // {
   //    printf("Writing data is different from reading data...\n");
   // }
   // else
   // {
   //    printf("Write data is the same as read data...\n");
   // }
//    close(fd);
//    return 0;   
}
