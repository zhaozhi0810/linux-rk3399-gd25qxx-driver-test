2022-11-09
1.借助的是网络的w25qxx的驱动.
2.进行了一些完善，比如分页写入，写入时如果有数据，进行部分擦除再写入的操作等。
3.配合应用程序，应用还再继续完善。
4.目前的makefile指定了交叉编译环境，是适配rk3399的。
5.dts中的设置：（本身在spi5控制总线上，使用CS0片选，wp和hold引脚硬件已经上拉）
&spi5 {
    status = "okay";
    max-freq = <50000000>;
    GD25Q64CSIG@0 {    //CS0  使用0
        compatible = "rockchip,GD25QXX";//RK3399,GD25Q64";//"rockchip,spidev";  //"Flash,rk3399-spi";//
        reg = <0>;
        spi-max-frequency = <5000000>;
		flash_size = <0x800000>;    //flash 字节大小    
//		spi-cpha;
//		spi-cpol;
    };
};
6.只是进行了简单的读写测试，估计还有问题，后期加入文件的读与写，尽量更加完善。