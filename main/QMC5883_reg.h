//
// Created by l on 2026/11/2.
//

#ifndef HELLO_WORLD_QMC5883_REG_H
#define HELLO_WORLD_QMC5883_REG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 数据寄存器：QMC5883L 是 LSB 在前 ----
 * 0x00 存的是 X 的**低**字节，0x01 才是高字节。这和 MPU6050 正好相反，
 * 照抄 MPU 的拼字节顺序会得到"看起来合理但完全错乱"的磁场值。 */
#define    QMC5883_DATA_X_LSB	0x00
#define    QMC5883_DATA_X_MSB	0x01
#define    QMC5883_DATA_Y_LSB	0x02
#define    QMC5883_DATA_Y_MSB	0x03
#define    QMC5883_DATA_Z_LSB	0x04
#define    QMC5883_DATA_Z_MSB	0x05
#define    QMC5883_STATUS		0x06	/* bit0 DRDY / bit1 OVL / bit2 DOR */
#define    QMC5883_ContrReg_1	0x09	/* 不是 0x90！写错芯片永远不进测量模式，读出来全 0 */
#define    QMC5883_CTRL2		0x0A
#define    QMC5883_WHO_AM_I	    0x0D
#define    QMC5883_SetReg	    0x0B

/* 状态位（0x06） */
#define    QMC5883_ST_DRDY		0x01	/* 有新数据 */
#define    QMC5883_ST_OVL		0x02	/* 量程溢出：铁磁物太近，数据已削顶，必须丢弃 */
#define    QMC5883_ST_DOR		0x04	/* 数据被覆盖（读得太慢） */

/* CTRL1 位域： [7:6]OSR  [5:4]RNG  [3:2]ODR  [1:0]MODE
 * 0x15 = 01 01 01 01 = OSR512 / ±8G / ODR 50Hz / 连续测量
 *   ODR 必须和 sensor_task 的 20ms 对齐，否则每帧读到的是同一份重复数据 */
#define    QMC5883_CTRL1_50HZ_8G	0x15

/* CTRL2：bit7 SOFT_RST / bit6 ROL_PNT / bit5 INT_ENB
 * 0x40 = 打开指针回卷：地址指针到 0x06 后自动回到 0x00，
 *        这样一次 7 字节事务就能同时拿到 6 字节数据和状态寄存器，省一半总线时间。 */
#define    QMC5883_CTRL2_ROL_PNT	0x40
#define    QMC5883_CTRL2_SOFT_RST	0x80

/* ---- 以下两个宏是旧名字，地址没错但语义是反的（0x00 是低字节却叫 X_H）。
 * 留着只为不破坏老引用，新代码一律用上面的 QMC5883_DATA_* ---- */
#define    QMC5883_X_H			0x00
#define    QMC5883_X_L			0x01
#define    QMC5883_Y_H			0x02
#define    QMC5883_Y_L			0x03
#define    QMC5883_Z_H			0x04
#define    QMC5883_Z_L			0x05

#ifdef __cplusplus
}
#endif

#endif //HELLO_WORLD_QMC5883_REG_H
