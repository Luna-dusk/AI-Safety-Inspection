//
// Created by l on 2026/11/2.
//

#ifndef HELLO_WORLD_QMC5883_REG_H
#define HELLO_WORLD_QMC5883_REG_H

#ifdef __cplusplus
extern "C" {
#endif

#define    QMC5883_X_H			0x00
#define    QMC5883_X_L			0x01
#define    QMC5883_Y_H			0x02
#define    QMC5883_Y_L			0x03
#define    QMC5883_Z_H			0x04
#define    QMC5883_Z_L			0x05
#define    QMC5883_ContrReg_1	0x09
//0100 8 0001 1 :0x81

#define    QMC5883_WHO_AM_I	    0x0D
#define    QMC5883_SetReg	    0x0B

#ifdef __cplusplus
}
#endif

#endif //HELLO_WORLD_QMC5883_REG_H
