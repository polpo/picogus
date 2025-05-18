/*
https://elixir.bootlin.com/linux/2.0.29/source/include/linux/sbpcd.h
CR-562-B is classified as Family1 in this driver, so uses the CMD1_ prefix. 
*/
#define CDROM_STATUS_DOOR         0x80
#define CDROM_STATUS_DISC_IN      0x40
#define CDROM_STATUS_SPIN_UP      0x20
#define CDROM_STATUS_ERROR        0x10
#define CDROM_STATUS_DOUBLE_SPEED 0x02
#define CDROM_STATUS_READY        0x01


//Status returned from device
#define STAT_READY	0x01
#define STAT_PLAY	0x08
#define STAT_ERROR	0x10
#define STAT_DISK	0x40
#define STAT_TRAY	0x80    //Seems Correct


#define CMD1_PAUSERESUME 0x0D
#define CMD1_RESET	    0x0a
#define CMD1_LOCK_CTL	0x0c
#define CMD1_TRAY_CTL	0x07
#define CMD1_MULTISESS	0x8d
#define CMD1_SUBCHANINF	0x11
#define CMD1_ABORT	    0x08
//#define CMD1_PATH_CHECK	0x???
#define CMD1_SEEK       0x01
#define CMD1_READ	    0x10
#define CMD1_SPINUP	    0x02
#define CMD1_SPINDOWN	0x06
#define CMD1_READ_UPC	0x88
//#define CMD1_PLAY	0x???
#define CMD1_PLAY_MSF	0x0e
#define CMD1_PLAY_TI	0x0f
#define CMD1_STATUS	    0x05
#define CMD1_READ_ERR   0x82
#define CMD1_READ_VER	0x83
#define CMD1_SETMODE	0x09
#define CMD1_GETMODE	0x84
#define CMD1_CAPACITY	0x85
#define CMD1_READSUBQ	0x87
#define CMD1_DISKINFO   0x8b
#define CMD1_READTOC    0x8c
#define CMD1_PAU_RES	0x0d
#define CMD1_PACKET     0x8e
#define CMD1_SESSINFO   0x8d



