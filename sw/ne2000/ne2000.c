/////////////////////////////////////////////////////////////////////////
// $Id: ne2k.cc,v 1.56.2.1 2004/02/02 22:37:22 cbothamy Exp $
/////////////////////////////////////////////////////////////////////////
//
//  Copyright (C) 2002  MandrakeSoft S.A.
//
//    MandrakeSoft S.A.
//    43, rue d'Aboukir
//    75002 Paris - France
//    http://www.linux-mandrake.com/
//    http://www.mandrakesoft.com/
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA

// Peter Grehan (grehan@iprg.nokia.com) coded all of this
// NE2000/ether stuff.
//#include "vl.h"

/*
2023-10-08 
Modifications for use with Pico by Kevin Moonlight (me@yyzkevin.com)
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* #include "bswap.h" */
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "cyw43.h"
#include "cyw43_stats.h"
#include "ne2000.h"

#include "system/pico_pic.h"

uint8_t ne2000_asic_read(uint16_t offset, void *p) ;
void ne2000_asic_write(uint16_t offset, uint8_t value, void *p);
uint8_t ne2000_read(uint16_t address, void *p);
void ne2000_write(uint16_t address, uint8_t value, void *p);
uint8_t ne2000_reset_read(uint16_t offset, void *p);
void ne2000_reset_write(uint16_t offset, uint8_t value, void *p);
ne2000_t *ne2000_init();

void ne2000_rx_frame(void *p, const void *buf, int io_len);
void ne2000_tx_done(void *p);


ne2000_t *nic;
/*
uint8_t cyw43_mac[6];
char WIFI_SSID[33];
char WIFI_KEY[63];
*/


#define CYW43_LINK_DOWN         (0)     ///< link is down
#define CYW43_LINK_JOIN         (1)     ///< Connected to wifi
#define CYW43_LINK_NOIP         (2)     ///< Connected to wifi, but no IP address
#define CYW43_LINK_UP           (3)     ///< Connect to wifi with an IP address
#define CYW43_LINK_FAIL         (-1)    ///< Connection failed
#define CYW43_LINK_NONET        (-2)    ///< No matching SSID found (could be out of range, or down)
#define CYW43_LINK_BADAUTH      (-3)    ///< Authenticatation failure

wifi_infos_t PG_Wifi_info;

static char StatusStr[64];
volatile static uint32_t StatusStr_idx = 0;

// IOCTLs not in cyw43_driver; from Infineon: https://github.com/Infineon/wifi-host-driver
#define WLC_GET_RATE                       ( (uint32_t)12 )

void PG_Wifi_GetStatus(void)
{
    int16_t status, rate;
    char tmp_ssid[36] = {0};
    int32_t rssi;

    StatusStr_idx = 0;
    // 255 is a "not ready" sentinel
    StatusStr[0] = 255;

    status = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
    cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_RSSI, sizeof(rssi), (uint8_t*)&rssi, CYW43_ITF_STA);
    cyw43_ioctl(&cyw43_state, (uint32_t)WLC_GET_RATE<<1, sizeof(rate), (uint8_t*)&rate, CYW43_ITF_STA);
    cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_SSID, sizeof(tmp_ssid), (uint8_t*)tmp_ssid, CYW43_ITF_STA);

    switch(status)
    {
        case CYW43_LINK_DOWN : sprintf(StatusStr, "Err: Link Down");
                               break;
        case CYW43_LINK_JOIN :
        case CYW43_LINK_NOIP :
        case CYW43_LINK_UP   :
                               sprintf(StatusStr, "Connected to SSID %s, Signal %d dB, Rate %d", tmp_ssid + 4, rssi, rate);
                               break;
        case CYW43_LINK_FAIL : sprintf(StatusStr, "Err: Connection failed");
                               break;
        case CYW43_LINK_NONET : sprintf(StatusStr, "Err: SSID not found");
                                break;
        case CYW43_LINK_BADAUTH : sprintf(StatusStr, "Err: Authentication failure");
                                  break;
        default : sprintf(StatusStr, "Err: Unknown status");
    } 
}


char PG_Wifi_ReadStatusStr(void)
{
    if (StatusStr_idx == 64) {
        // Past end of buffer, reset the index
        StatusStr_idx = 0;
        return 0;
    }
    char ret = StatusStr[StatusStr_idx];
    if (ret == 0) {
        // End of null-terminated string, reset the index
        StatusStr_idx = 0;
    } else if (ret != 255) {
        // 255 is a "not ready" sentinel
        ++StatusStr_idx;
    }
    return ret;
}

void PG_Wifi_Connect(const char* ssid, const char* pass) {
    if (ssid) {
        strlcpy(PG_Wifi_info.WIFI_SSID, ssid, 33);
        strlcpy(PG_Wifi_info.WIFI_KEY, pass, 64);
    }
    if (!PG_Wifi_info.WIFI_SSID[0]) {
        printf("No SSID set\n");
        return;
    }

    printf("Joining SSID: %s\n", PG_Wifi_info.WIFI_SSID);                                                
    int ConnectErr=cyw43_arch_wifi_connect_async(
        PG_Wifi_info.WIFI_SSID,
        PG_Wifi_info.WIFI_KEY,
        PG_Wifi_info.WIFI_KEY[0] == 0 ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_MIXED_PSK
    );
    if (ConnectErr==0) printf("Connection started\n");
    else printf("Connection Error: %d", ConnectErr);
}

void PG_Wifi_Reconnect(void)
{
    int32_t rssi;
    cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_RSSI, sizeof(rssi), (uint8_t*)&rssi, CYW43_ITF_STA);
    printf("rssi %d ", rssi);
    if (rssi == 0) {
        PG_Wifi_Connect(NULL, NULL);
        return;
    }
    int16_t status = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
    printf("status: %d\n", status);
    switch(status)
    {
        case CYW43_LINK_JOIN :
        case CYW43_LINK_NOIP :
        case CYW43_LINK_UP   :
            break;
        default:
            PG_Wifi_Connect(NULL, NULL);
    } 
}

uint8_t PG_EnableWifi(void) {
    if (cyw43_arch_init()) {
        printf("Init failed\n");
        return 1;
    }

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);

    printf("cyw43_wifi_set_up(&cyw43_state,CYW43_ITF_STA,true,CYW43_COUNTRY_WORLDWIDE);\n");        
    cyw43_wifi_set_up(&cyw43_state, CYW43_ITF_STA, true, CYW43_COUNTRY_WORLDWIDE);   
    cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, PG_Wifi_info.cyw43_mac);          
    printf("WIFI Address: %02x:%02x:%02x:%02x:%02x:%02x\n\r",
           PG_Wifi_info.cyw43_mac[0], PG_Wifi_info.cyw43_mac[1], PG_Wifi_info.cyw43_mac[2],
           PG_Wifi_info.cyw43_mac[3], PG_Wifi_info.cyw43_mac[4], PG_Wifi_info.cyw43_mac[5]);    
    cyw43_wifi_pm(&cyw43_state, cyw43_pm_value(CYW43_NO_POWERSAVE_MODE, 20, 1, 1, 1));

    nic = ne2000_init();
    printf("Inited\n");
    return 0;
}

uint8_t PG_NE2000_Read(uint8_t Addr) {        
    if(Addr <= 0xF) {
        return ne2000_read(Addr,nic);
    }
    else if(Addr == 0x1F) {
        ne2000_reset_read(Addr,nic);
    }
    else {
        return ne2000_asic_read(Addr,nic);
    }        
}
void PG_NE2000_Write(uint8_t Addr,uint8_t Data) {              
    if(Addr <= 0xF) {
        ne2000_write(Addr,Data,nic);
    }
    else {
        ne2000_asic_write(Addr,Data,nic);
    }
}

void ne2000_initiate_send() {        
    cyw43_send_ethernet(&cyw43_state, CYW43_ITF_STA, nic->tx_bytes, &nic->mem[nic->tx_page_start * 256 - BX_NE2K_MEMSTART], false);                  
    sleep_us(100+nic->tx_bytes);   //1 microsecond per byte plus 100us safety?                                
    ne2000_tx_done(nic); 
}
             
/*
I rename cyw43_cb_process_ethernet in the library,  may need to handle this differently in the future.
One reason is we need option to directly process the packets, but also want the option to let them go
to lwip stack when using tcp virtual modem,  or contacting ntp servers etc.  this is work in progress.
*/             
void _cyw43_cb_process_ethernet(void *cb_data, int itf, size_t len, const uint8_t *buf);

void cyw43_cb_process_ethernet(void *cb_data, int itf, size_t len, const uint8_t *buf) 
{                                                
        ne2000_rx_frame(nic, buf, len);             
        CYW43_STAT_INC(PACKET_IN_COUNT);       
}

void cyw43_cb_tcpip_init(cyw43_t *self, int itf) {}
void cyw43_cb_tcpip_deinit(cyw43_t *self, int itf) {}
void cyw43_cb_tcpip_set_link_up(cyw43_t *self, int itf) {}
void cyw43_cb_tcpip_set_link_down(cyw43_t *self, int itf) {}
struct pbuf;
uint16_t pbuf_copy_partial(const struct pbuf *p, void *dataptr, uint16_t len, uint16_t offset) {}


static void ne2000_raise_irq(ne2000_t *ne2000) {                
        PIC_ActivateIRQ();        
}
static void ne2000_lower_irq(ne2000_t *ne2000) {
        PIC_DeActivateIRQ();        
}

//
// reset - restore state to power-up, cancelling all i/o
//
static void ne2000_reset(int type, void *p) {
        ne2000_t *ne2000 = (ne2000_t *)p;
        int i;                       
        ne2000->macaddr[0] = ne2000->physaddr[0];
        ne2000->macaddr[1] = ne2000->physaddr[1];
        ne2000->macaddr[2] = ne2000->physaddr[2];
        ne2000->macaddr[3] = ne2000->physaddr[3];
        ne2000->macaddr[4] = ne2000->physaddr[4];
        ne2000->macaddr[5] = ne2000->physaddr[5];
        // ne2k signature
        for (i = 12; i < 32; i++) {
                ne2000->macaddr[i] = 0x57;
        }        
        // Zero out registers and memory
        memset(&ne2000->CR, 0, sizeof(ne2000->CR));
        memset(&ne2000->ISR, 0, sizeof(ne2000->ISR));
        memset(&ne2000->IMR, 0, sizeof(ne2000->IMR));
        memset(&ne2000->DCR, 0, sizeof(ne2000->DCR));
        memset(&ne2000->TCR, 0, sizeof(ne2000->TCR));
        memset(&ne2000->TSR, 0, sizeof(ne2000->TSR));
        // memset( & ne2000->RCR, 0, sizeof(ne2000->RCR));
        memset(&ne2000->RSR, 0, sizeof(ne2000->RSR));
        ne2000->tx_timer_active = 0;
        ne2000->local_dma = 0;
        ne2000->page_start = 0;
        ne2000->page_stop = 0;
        ne2000->bound_ptr = 0;
        ne2000->tx_page_start = 0;
        ne2000->num_coll = 0;
        ne2000->tx_bytes = 0;
        ne2000->fifo = 0;
        ne2000->remote_dma = 0;
        ne2000->remote_start = 0;
        ne2000->remote_bytes = 0;
        ne2000->tallycnt_0 = 0;
        ne2000->tallycnt_1 = 0;
        ne2000->tallycnt_2 = 0;
        ne2000->curr_page = 0;
        ne2000->rempkt_ptr = 0;
        ne2000->localpkt_ptr = 0;
        ne2000->address_cnt = 0;
        memset(&ne2000->mem, 0, sizeof(ne2000->mem));
        // Set power-up conditions
        ne2000->CR.stop = 1;
        ne2000->CR.rdma_cmd = 4;
        ne2000->ISR.reset = 1;
        ne2000->DCR.longaddr = 1;
        ne2000_raise_irq(ne2000);//maybe remove.
        ne2000_lower_irq(ne2000);        
}

//
// chipmem_read/chipmem_write - access the 64K private RAM.
// The ne2000 memory is accessed through the data port of
// the asic (offset 0) after setting up a remote-DMA transfer.
// Both byte and word accesses are allowed.
// The first 16 bytes contains the MAC address at even locations,
// and there is 16K of buffer memory starting at 16K
//
static inline uint8_t ne2000_chipmem_read(uint32_t address, ne2000_t *ne2000) {
        if ((address >= 0) && (address <= 31))
                return ne2000->macaddr[address];// TODO: confirm 8bit vs 16bit access

        if ((address >= BX_NE2K_MEMSTART) && (address < BX_NE2K_MEMEND))
                return ne2000->mem[address - BX_NE2K_MEMSTART];
        else
                return 0xff;
}

static inline void ne2000_chipmem_write(uint32_t address, uint8_t value, ne2000_t *ne2000) {
        if ((address >= BX_NE2K_MEMSTART) && (address < BX_NE2K_MEMEND))
                ne2000->mem[address - BX_NE2K_MEMSTART] = value & 0xff;
}

//
// asic_read/asic_write - This is the high 16 bytes of i/o space
// (the lower 16 bytes is for the DS8390). Only two locations
// are used: offset 0, which is used for data transfer, and
// offset 0xf, which is used to reset the device.
// The data transfer port is used to as 'external' DMA to the
// DS8390. The chip has to have the DMA registers set up, and
// after that, insw/outsw instructions can be used to move
// the appropriate number of bytes to/from the device.
//
uint16_t ne2000_dma_read(int io_len, void *p) {
        ne2000_t *ne2000 = (ne2000_t *)p;
        //
        // The 8390 bumps the address and decreases the byte count
        // by the selected word size after every access, not by
        // the amount of data requested by the host (io_len).
        //
        ne2000->remote_dma += io_len;
        if (ne2000->remote_dma == ne2000->page_stop << 8)
                ne2000->remote_dma = ne2000->page_start << 8;

        // keep s.remote_bytes from underflowing
        if (ne2000->remote_bytes > 1)
                ne2000->remote_bytes -= io_len;
        else
                ne2000->remote_bytes = 0;

        // If all bytes have been written, signal remote-DMA complete
        if (ne2000->remote_bytes == 0) {
                ne2000->ISR.rdma_done = 1;
                if (ne2000->IMR.rdma_inte) {
                        ne2000_raise_irq(ne2000);
                }
        }

        return 0;
}

uint8_t ne2000_asic_read(uint16_t offset, void *p) {
        ne2000_t *ne2000 = (ne2000_t *)p;
        int retval;
                
        //ne2000->DCR.wdsize should be 0
        retval = ne2000_chipmem_read(ne2000->remote_dma, ne2000);
        ne2000_dma_read(1, ne2000);
        
        return retval;
}

void ne2000_dma_write(int io_len, void *p) {
        ne2000_t *ne2000 = (ne2000_t *)p;
        
        // is this right ??? asic_read uses DCR.wordsize
        ne2000->remote_dma += io_len;
        if (ne2000->remote_dma == ne2000->page_stop << 8)
                ne2000->remote_dma = ne2000->page_start << 8;

        ne2000->remote_bytes -= io_len;
        if (ne2000->remote_bytes > BX_NE2K_MEMSIZ)
                ne2000->remote_bytes = 0;
        
        // If all bytes have been written, signal remote-DMA complete
        if (ne2000->remote_bytes == 0) {
                ne2000->ISR.rdma_done = 1;
                if (ne2000->IMR.rdma_inte) {
                        ne2000_raise_irq(ne2000);
                }
        }
}

void ne2000_asic_write(uint16_t offset, uint8_t value, void *p) {
        ne2000_t *ne2000 = (ne2000_t *)p;

        if (ne2000->remote_bytes == 0)
                return;

        //ne2000->DCR.wdsize should be 0
        ne2000_chipmem_write(ne2000->remote_dma, value, ne2000);
        ne2000_dma_write(1, ne2000);        
}

uint8_t ne2000_reset_read(uint16_t offset, void *p) {
        ne2000_t *ne2000 = (ne2000_t *)p;
        ne2000_reset(BX_RESET_SOFTWARE, ne2000);
        return 0;
}

void ne2000_reset_write(uint16_t offset, uint8_t value, void *p) {}


uint8_t ne2000_read(uint16_t address, void *p) {
    /* printf("ne2000_read %x\n", address); */
        ne2000_t *ne2000 = (ne2000_t *)p;
        int ret = 0;
        address &= 0xf;                
        if (address == 0x00) { // CR READ                
                ret = (((ne2000->CR.pgsel & 0x03) << 6) | ((ne2000->CR.rdma_cmd & 0x07) << 3) | (ne2000->CR.tx_packet << 2) |
                       (ne2000->CR.start << 1) | (ne2000->CR.stop));                
        } else {
                switch (ne2000->CR.pgsel) {
                case 0x00:                                                
                        switch (address) {
                        case 0x1: // CLDA0
                                return ne2000->local_dma & 0xff;

                        case 0x2: // CLDA1
                                return ne2000->local_dma >> 8;

                        case 0x3: // BNRY
                                return ne2000->bound_ptr;

                        case 0x4: // TSR
                                return ((ne2000->TSR.ow_coll << 7) | (ne2000->TSR.cd_hbeat << 6) | (ne2000->TSR.fifo_ur << 5) |
                                        (ne2000->TSR.no_carrier << 4) | (ne2000->TSR.aborted << 3) | (ne2000->TSR.collided << 2) |
                                        (ne2000->TSR.tx_ok));

                        case 0x5: // NCR
                                return ne2000->num_coll;

                        case 0x6: // FIFO
                                  // reading FIFO is only valid in loopback mode
                                return ne2000->fifo;

                        case 0x7: // ISR
                                return ((ne2000->ISR.reset << 7) | (ne2000->ISR.rdma_done << 6) | (ne2000->ISR.cnt_oflow << 5) |
                                        (ne2000->ISR.overwrite << 4) | (ne2000->ISR.tx_err << 3) | (ne2000->ISR.rx_err << 2) |
                                        (ne2000->ISR.pkt_tx << 1) | (ne2000->ISR.pkt_rx));

                        case 0x8: // CRDA0
                                return ne2000->remote_dma & 0xff;

                        case 0x9: // CRDA1
                                return ne2000->remote_dma >> 8;

                        case 0xa: // reserved                                
                                return 0xff;

                        case 0xb: // reserved                                
                                return 0xff;

                        case 0xc: // RSR
                                return ((ne2000->RSR.deferred << 7) | (ne2000->RSR.rx_disabled << 6) |
                                        (ne2000->RSR.rx_mbit << 5) | (ne2000->RSR.rx_missed << 4) | (ne2000->RSR.fifo_or << 3) |
                                        (ne2000->RSR.bad_falign << 2) | (ne2000->RSR.bad_crc << 1) | (ne2000->RSR.rx_ok));

                        case 0xd: // CNTR0
                                return ne2000->tallycnt_0;

                        case 0xe: // CNTR1
                                return ne2000->tallycnt_1;

                        case 0xf: // CNTR2
                                return ne2000->tallycnt_2;
                        }

                        return 0;

                case 0x01:                        
                        switch (address) {
                        case 0x1: // PAR0-5
                        case 0x2:
                        case 0x3:
                        case 0x4:
                        case 0x5:
                        case 0x6:
                                return ne2000->physaddr[address - 1];

                        case 0x7: // CURR                          
                                return ne2000->curr_page;

                        case 0x8: // MAR0-7
                        case 0x9:
                        case 0xa:
                        case 0xb:
                        case 0xc:
                        case 0xd:
                        case 0xe:
                        case 0xf:
                                return ne2000->mchash[address - 8];
                        }

                        return 0;

                case 0x02:                       
                        switch (address) {
                        case 0x1: // PSTART
                                return (ne2000->page_start);

                        case 0x2: // PSTOP
                                return ne2000->page_stop;

                        case 0x3: // Remote Next-packet pointer
                                return ne2000->rempkt_ptr;

                        case 0x4: // TPSR
                                return ne2000->tx_page_start;

                        case 0x5: // Local Next-packet pointer
                                return ne2000->localpkt_ptr;

                        case 0x6: // Address counter (upper)
                                return ne2000->address_cnt >> 8;

                        case 0x7: // Address counter (lower)
                                return ne2000->address_cnt & 0xff;

                        case 0x8: // Reserved
                        case 0x9:
                        case 0xa:
                        case 0xb:                                
                                break;

                        case 0xc: // RCR
                                return ((ne2000->RCR.monitor << 5) | (ne2000->RCR.promisc << 4) | (ne2000->RCR.multicast << 3) |
                                        (ne2000->RCR.broadcast << 2) | (ne2000->RCR.runts_ok << 1) | (ne2000->RCR.errors_ok));

                        case 0xd: // TCR
                                return ((ne2000->TCR.coll_prio << 4) | (ne2000->TCR.ext_stoptx << 3) |
                                        ((ne2000->TCR.loop_cntl & 0x3) << 1) | (ne2000->TCR.crc_disable));

                        case 0xe: // DCR
                                return (((ne2000->DCR.fifo_size & 0x3) << 5) | (ne2000->DCR.auto_rx << 4) |
                                        (ne2000->DCR.loop << 3) | (ne2000->DCR.longaddr << 2) | (ne2000->DCR.endian << 1) |
                                        (ne2000->DCR.wdsize));

                        case 0xf: // IMR
                                return ((ne2000->IMR.rdma_inte << 6) | (ne2000->IMR.cofl_inte << 5) |
                                        (ne2000->IMR.overw_inte << 4) | (ne2000->IMR.txerr_inte << 3) |
                                        (ne2000->IMR.rxerr_inte << 2) | (ne2000->IMR.tx_inte << 1) | (ne2000->IMR.rx_inte));
                        }
                        break;

                case 0x03:                        
                        switch (address) {
                                case 0x1: /*9346CR*/
                                        return ne2000->_9346cr;

                                case 0x3:            /*CONFIG0*/
                                        return 0x00; /*Cable not BNC*/

                                case 0x5:                              /*CONFIG2*/
                                        return ne2000->config2 & 0xe0; /*No boot ROM*/

                                case 0x6: /*CONFIG3*/
                                        return ne2000->config3 & 0x46;

                                case 0xe: /*8029ASID0*/
                                        return 0x29;

                                case 0xf: /*8029ASID1*/
                                        return 0x08;
                        }                        
                        break;
                }
        }
        return ret;
}


void ne2000_write(uint16_t address, uint8_t value, void *p) {
    /* printf("ne2000_write %x %x\n", address, value); */
        ne2000_t *ne2000 = (ne2000_t *)p;
        address &= 0xf;

        //
        // The high 16 bytes of i/o space are for the ne2000 asic -
        //  the low 16 bytes are for the DS8390, with the current
        //  page being selected by the PS0,PS1 registers in the
        //  command register
        //
        if (address == 0x00) { //CR
                // Validate remote-DMA
                if ((value & 0x38) == 0x00) { //CR WRITE - INVALID rDMA value of 0
                        value |= 0x20; /* dma_cmd == 4 is a safe default */
                                       // value = 0x22; /* dma_cmd == 4 is a safe default */
                }
                // Check for s/w reset
                if (value & 0x01) {
                        ne2000->ISR.reset = 1;
                        ne2000->CR.stop = 1;
                } else
                        ne2000->CR.stop = 0;

                ne2000->CR.rdma_cmd = (value & 0x38) >> 3;

                // If start command issued, the RST bit in the ISR
                // must be cleared
                if ((value & 0x02) && !ne2000->CR.start)
                        ne2000->ISR.reset = 0;

                ne2000->CR.start = ((value & 0x02) == 0x02);
                ne2000->CR.pgsel = (value & 0xc0) >> 6;

                // Check for send-packet command
                if (ne2000->CR.rdma_cmd == 3) {                        
                        // Set up DMA read from receive ring
                        ne2000->remote_start = ne2000->remote_dma = ne2000->bound_ptr * 256;
                        ne2000->remote_bytes = *((uint16_t *)&ne2000->mem[ne2000->bound_ptr * 256 + 2 - BX_NE2K_MEMSTART]);
                        
                }
                
                // Check for start-tx
                if ((value & 0x04) && ne2000->TCR.loop_cntl) {                                
                        ne2000_rx_frame(ne2000, &ne2000->mem[ne2000->tx_page_start * 256 - BX_NE2K_MEMSTART],
                                        ne2000->tx_bytes);

                        // do a TX interrupt
                        // Generate an interrupt if not masked and not one in progress
                        if (ne2000->IMR.tx_inte && !ne2000->ISR.pkt_tx) {
                                // LOG_MSG("tx complete interrupt");
                                ne2000_raise_irq(ne2000);
                        }
                        ne2000->ISR.pkt_tx = 1;                        
                } else if (value & 0x04) {
                        // start-tx and no loopback
                        //ne2000->pending_tx=1;
                        //ne2000->CR.tx_packet=1;                                               
                        multicore_fifo_push_blocking(FIFO_NE2K_SEND);                           
                } // end transmit-start branch

                // Linux probes for an interrupt by setting up a remote-DMA read
                // of 0 bytes with remote-DMA completion interrupts enabled.
                // Detect this here
                if (ne2000->CR.rdma_cmd == 0x01 && ne2000->CR.start && ne2000->remote_bytes == 0) {
                        ne2000->ISR.rdma_done = 1;
                        if (ne2000->IMR.rdma_inte) {
                                ne2000_raise_irq(ne2000);                                
                        }
                }
        } else {
                switch (ne2000->CR.pgsel) {
                case 0x00:
                        // It appears to be a common practice to use outw on page0 regs...

                        switch (address) {
                        case 0x1: // PSTART
                                ne2000->page_start = value;
                                break;

                        case 0x2: // PSTOP
                                ne2000->page_stop = value;
                                break;

                        case 0x3: // BNRY
                                ne2000->bound_ptr = value;
                                break;

                        case 0x4: // TPSR
                                ne2000->tx_page_start = value;
                                break;

                        case 0x5: // TBCR0
                                // Clear out low byte and re-insert
                                ne2000->tx_bytes &= 0xff00;
                                ne2000->tx_bytes |= (value & 0xff);
                                break;

                        case 0x6: // TBCR1
                                // Clear out high byte and re-insert
                                ne2000->tx_bytes &= 0x00ff;
                                ne2000->tx_bytes |= ((value & 0xff) << 8);
                                break;

                        case 0x7:              // ISR
                                value &= 0x7f; // clear RST bit - status-only bit
                                // All other values are cleared iff the ISR bit is 1
                                ne2000->ISR.pkt_rx &= ~((int)((value & 0x01) == 0x01));
                                ne2000->ISR.pkt_tx &= ~((int)((value & 0x02) == 0x02));
                                ne2000->ISR.rx_err &= ~((int)((value & 0x04) == 0x04));
                                ne2000->ISR.tx_err &= ~((int)((value & 0x08) == 0x08));
                                ne2000->ISR.overwrite &= ~((int)((value & 0x10) == 0x10));
                                ne2000->ISR.cnt_oflow &= ~((int)((value & 0x20) == 0x20));
                                ne2000->ISR.rdma_done &= ~((int)((value & 0x40) == 0x40));
                                value = ((ne2000->ISR.rdma_done << 6) | (ne2000->ISR.cnt_oflow << 5) |
                                         (ne2000->ISR.overwrite << 4) | (ne2000->ISR.tx_err << 3) | (ne2000->ISR.rx_err << 2) |
                                         (ne2000->ISR.pkt_tx << 1) | (ne2000->ISR.pkt_rx));
                                value &= ((ne2000->IMR.rdma_inte << 6) | (ne2000->IMR.cofl_inte << 5) |
                                          (ne2000->IMR.overw_inte << 4) | (ne2000->IMR.txerr_inte << 3) |
                                          (ne2000->IMR.rxerr_inte << 2) | (ne2000->IMR.tx_inte << 1) | (ne2000->IMR.rx_inte));
                                if (value == 0)
                                        ne2000_lower_irq(ne2000);
                                // DEV_pic_lower_irq(ne2000->base_irq);
                                break;

                        case 0x8: // RSAR0
                                // Clear out low byte and re-insert
                                ne2000->remote_start &= 0xff00;
                                ne2000->remote_start |= (value & 0xff);
                                ne2000->remote_dma = ne2000->remote_start;
                                break;

                        case 0x9: // RSAR1
                                // Clear out high byte and re-insert
                                ne2000->remote_start &= 0x00ff;
                                ne2000->remote_start |= ((value & 0xff) << 8);
                                ne2000->remote_dma = ne2000->remote_start;
                                break;

                        case 0xa: // RBCR0
                                // Clear out low byte and re-insert
                                ne2000->remote_bytes &= 0xff00;
                                ne2000->remote_bytes |= (value & 0xff);
                                break;

                        case 0xb: // RBCR1
                                // Clear out high byte and re-insert
                                ne2000->remote_bytes &= 0x00ff;
                                ne2000->remote_bytes |= ((value & 0xff) << 8);
                                break;

                        case 0xc: // RCR
                                  // Check if the reserved bits are set
                                // Set all other bit-fields
                                ne2000->RCR.errors_ok = ((value & 0x01) == 0x01);
                                ne2000->RCR.runts_ok = ((value & 0x02) == 0x02);
                                ne2000->RCR.broadcast = ((value & 0x04) == 0x04);
                                ne2000->RCR.multicast = ((value & 0x08) == 0x08);
                                ne2000->RCR.promisc = ((value & 0x10) == 0x10);
                                ne2000->RCR.monitor = ((value & 0x20) == 0x20);

                                // Monitor bit is a little suspicious...
                                break;

                        case 0xd: // TCR
                                  // Check reserved bits
                                // Test loop mode (not supported)
                                if (value & 0x06) {
                                        ne2000->TCR.loop_cntl = (value & 0x6) >> 1;
                                } else
                                        ne2000->TCR.loop_cntl = 0;

                                // Inhibit-CRC not supported.
                                if (value & 0x01) {
                                        // fatal("ne2000 TCR write, inhibit-CRC not supported\n");
                                        return;
                                }

                                // Auto-transmit disable very suspicious
                                if (value & 0x08) {
                                        // fatal("ne2000 TCR write, auto transmit disable not supported\n");
                                }

                                // Allow collision-offset to be set, although not used
                                ne2000->TCR.coll_prio = ((value & 0x08) == 0x08);
                                break;

                        case 0xe: // DCR
                                  // the loopback mode is not suppported yet
                                // Set other values.
                                ne2000->DCR.wdsize = ((value & 0x01) == 0x01);
                                ne2000->DCR.endian = ((value & 0x02) == 0x02);
                                ne2000->DCR.longaddr = ((value & 0x04) == 0x04); // illegal ?
                                ne2000->DCR.loop = ((value & 0x08) == 0x08);
                                ne2000->DCR.auto_rx = ((value & 0x10) == 0x10); // also illegal ?
                                ne2000->DCR.fifo_size = (value & 0x50) >> 5;
                                break;

                        case 0xf: // IMR
                                  // Check for reserved bit
                                // Set other values
                                ne2000->IMR.rx_inte = ((value & 0x01) == 0x01);
                                ne2000->IMR.tx_inte = ((value & 0x02) == 0x02);
                                ne2000->IMR.rxerr_inte = ((value & 0x04) == 0x04);
                                ne2000->IMR.txerr_inte = ((value & 0x08) == 0x08);
                                ne2000->IMR.overw_inte = ((value & 0x10) == 0x10);
                                ne2000->IMR.cofl_inte = ((value & 0x20) == 0x20);
                                ne2000->IMR.rdma_inte = ((value & 0x40) == 0x40);
                                value = ((ne2000->ISR.rdma_done << 6) | (ne2000->ISR.cnt_oflow << 5) |
                                         (ne2000->ISR.overwrite << 4) | (ne2000->ISR.tx_err << 3) | (ne2000->ISR.rx_err << 2) |
                                         (ne2000->ISR.pkt_tx << 1) | (ne2000->ISR.pkt_rx));
                                value &= ((ne2000->IMR.rdma_inte << 6) | (ne2000->IMR.cofl_inte << 5) |
                                          (ne2000->IMR.overw_inte << 4) | (ne2000->IMR.txerr_inte << 3) |
                                          (ne2000->IMR.rxerr_inte << 2) | (ne2000->IMR.tx_inte << 1) | (ne2000->IMR.rx_inte));
                                if (value)
                                        ne2000_raise_irq(ne2000);
                                else
                                        ne2000_lower_irq(ne2000);
                                break;
                        }
                        break;

                case 0x01:
                        switch (address) {
                        case 0x1: // PAR0-5
                        case 0x2:
                        case 0x3:
                        case 0x4:
                        case 0x5:
                        case 0x6:
                                ne2000->physaddr[address - 1] = value;
                                break;

                        case 0x7: // CURR
                                ne2000->curr_page = value;
                                break;

                        case 0x8: // MAR0-7
                        case 0x9:
                        case 0xa:
                        case 0xb:
                        case 0xc:
                        case 0xd:
                        case 0xe:
                        case 0xf:
                                ne2000->mchash[address - 8] = value;
                                break;
                        }
                        break;

                case 0x02:
                        switch (address) {
                        case 0x1: // CLDA0
                                // Clear out low byte and re-insert
                                ne2000->local_dma &= 0xff00;
                                ne2000->local_dma |= (value & 0xff);
                                break;

                        case 0x2: // CLDA1
                                // Clear out high byte and re-insert
                                ne2000->local_dma &= 0x00ff;
                                ne2000->local_dma |= ((value & 0xff) << 8);
                                break;

                        case 0x3: // Remote Next-pkt pointer
                                ne2000->rempkt_ptr = value;
                                break;

                        case 0x4:
                                // fatal("page 2 write to reserved offset 4\n");
                                // OS/2 Warp can cause this to freak out.
                                break;

                        case 0x5: // Local Next-packet pointer
                                ne2000->localpkt_ptr = value;
                                break;

                        case 0x6: // Address counter (upper)
                                // Clear out high byte and re-insert
                                ne2000->address_cnt &= 0x00ff;
                                ne2000->address_cnt |= ((value & 0xff) << 8);
                                break;

                        case 0x7: // Address counter (lower)
                                // Clear out low byte and re-insert
                                ne2000->address_cnt &= 0xff00;
                                ne2000->address_cnt |= (value & 0xff);
                                break;

                        case 0x8:
                        case 0x9:
                        case 0xa:
                        case 0xb:
                        case 0xc:
                        case 0xd:
                        case 0xe:
                        case 0xf:
                                // fatal("page 2 write to reserved offset %0x\n", address);
                        default:
                                break;
                        }
                        break;

                case 0x03:
                        if (ne2000->type != NE2000_RTL8029AS) {
                                
                                break;
                        }
                        switch (address) {
                        case 0x1: /*9346CR*/
                                ne2000->_9346cr = value & 0xfe;
                                break;

                        case 0x5: /*CONFIG2*/
                                ne2000->config2 = value & 0xe0;
                                break;

                        case 0x6: /*CONFIG3*/
                                ne2000->config3 = value & 0x46;
                                break;

                        case 0x9: /*HLTCLK*/
                                break;

                        default:                                
                                break;
                        }
                        break;
                }
        }        
}

/*
 * mcast_index() - return the 6-bit index into the multicast
 * table. Stolen unashamedly from FreeBSD's if_ed.c
 */
static int mcast_index(const void *dst) {
#define POLYNOMIAL 0x04c11db6
        unsigned long crc = 0xffffffffL;
        int carry, i, j;
        unsigned char b;
        unsigned char *ep = (unsigned char *)dst;

        for (i = 6; --i >= 0;) {
                b = *ep++;
                for (j = 8; --j >= 0;) {
                        carry = ((crc & 0x80000000L) ? 1 : 0) ^ (b & 0x01);
                        crc <<= 1;
                        b >>= 1;
                        if (carry)
                                crc = ((crc ^ POLYNOMIAL) | carry);
                }
        }
        return crc >> 26;
#undef POLYNOMIAL
}

/*
 * rx_frame() - called by the platform-specific code when an
 * ethernet frame has been received. The destination address
 * is tested to see if it should be accepted, and if the
 * rx ring has enough room, it is copied into it and
 * the receive process is updated
 */
void ne2000_rx_frame(void *p, const void *buf, int io_len) {
        ne2000_t *ne2000 = (ne2000_t *)p;

        int pages;
        int avail;
        int idx;
        int nextpage;
        uint8_t pkthdr[4];
        uint8_t *pktbuf = (uint8_t *)buf;
        uint8_t *startptr;
        static uint8_t bcast_addr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

        if ((ne2000->CR.stop != 0) ||
		(ne2000->page_start == 0) /*||
                ((ne2000->DCR.loop == 0) &&
                (ne2000->TCR.loop_cntl != 0))*/) {                        
                        return;
                }
                

        // Add the pkt header + CRC to the length, and work
        // out how many 256-byte pages the frame would occupy
        pages = (io_len + 4 + 4 + 255) / 256;

        if (ne2000->curr_page < ne2000->bound_ptr)
                avail = ne2000->bound_ptr - ne2000->curr_page;
        else
                avail = (ne2000->page_stop - ne2000->page_start) - (ne2000->curr_page - ne2000->bound_ptr);

        // Avoid getting into a buffer overflow condition by not attempting
        // to do partial receives. The emulation to handle this condition
        // seems particularly painful.
        if ((avail < pages)
#if BX_NE2K_NEVER_FULL_RING
            || (avail == pages)
#endif
        ) {                        
                return;
        }

        if ((io_len < 40 /*60*/) && !ne2000->RCR.runts_ok) {                
                return;
        }
        // some computers don't care...
        if (io_len < 60)
                io_len = 60;

        // Do address filtering if not in promiscuous mode
        if (!ne2000->RCR.promisc) {
                if (!memcmp(buf, bcast_addr, 6)) {
                        if (!ne2000->RCR.broadcast)
                                return;
                } else if (pktbuf[0] & 0x01) {
                        if (!ne2000->RCR.multicast)
                                return;
                        idx = mcast_index(buf);
                        if (!(ne2000->mchash[idx >> 3] & (1 << (idx & 0x7))))
                                return;
                } else if (0 != memcmp(buf, ne2000->physaddr, 6))
                        return;
        }
        nextpage = ne2000->curr_page + pages;
        if (nextpage >= ne2000->page_stop) {
                nextpage -= ne2000->page_stop - ne2000->page_start;
        }
        // Setup packet header
        pkthdr[0] = 0; // rx status - old behavior
        pkthdr[0] = 1; // Probably better to set it all the time
        // rather than set it to 0, which is clearly wrong.
        if (pktbuf[0] & 0x01) {
                pkthdr[0] |= 0x20; // rx status += multicast packet
        }
        pkthdr[1] = nextpage;            // ptr to next packet
        pkthdr[2] = (io_len + 4) & 0xff; // length-low
        pkthdr[3] = (io_len + 4) >> 8;   // length-hi

        // copy into buffer, update curpage, and signal interrupt if config'd
        startptr = &ne2000->mem[ne2000->curr_page * 256 - BX_NE2K_MEMSTART];
        if ((nextpage > ne2000->curr_page) || ((ne2000->curr_page + pages) == ne2000->page_stop)) {
                memcpy(startptr, pkthdr, 4);
                memcpy(startptr + 4, buf, io_len);
                ne2000->curr_page = nextpage;
        } 
        else {
                int endbytes = (ne2000->page_stop - ne2000->curr_page) * 256;
                memcpy(startptr, pkthdr, 4);
                memcpy(startptr + 4, buf, endbytes - 4);
                startptr = &ne2000->mem[ne2000->page_start * 256 - BX_NE2K_MEMSTART];
                memcpy(startptr, (void *)(pktbuf + endbytes - 4), io_len - endbytes + 8);
                ne2000->curr_page = nextpage;
        }

        ne2000->RSR.rx_ok = 1;
        if (pktbuf[0] & 0x80) {
                ne2000->RSR.rx_mbit = 1;
        }

        ne2000->ISR.pkt_rx = 1;

        if (ne2000->IMR.rx_inte) {                                
                ne2000_raise_irq(ne2000);                
        }        
}


void ne2000_tx_done(void *p) {
        ne2000_t *ne2000 = (ne2000_t *)p;
        ne2000->TSR.tx_ok = 1;                        
        if (ne2000->IMR.tx_inte && !ne2000->ISR.pkt_tx) {                
                ne2000_raise_irq(ne2000);                
        }                
        ne2000->ISR.pkt_tx = 1;
        ne2000->tx_timer_active = 0;         
}


void *ne2000_common_init() {        
        int rc;
        unsigned int macint[6];
        

        ne2000_t *ne2000 = malloc(sizeof(ne2000_t));
        memset(ne2000, 0, sizeof(ne2000_t));
               
        memcpy(ne2000->physaddr, PG_Wifi_info.cyw43_mac, 6);

        ne2000_reset(BX_RESET_HARDWARE, ne2000);
        
        return ne2000;
}

ne2000_t *ne2000_init() {
        ne2000_t *ne2000 = ne2000_common_init();
        ne2000->type = NE2000_NE2000;
        return ne2000;
}

void ne2000_close(void *p) {
        ne2000_t *ne2000 = (ne2000_t *)p;
        free(ne2000);         
}






























