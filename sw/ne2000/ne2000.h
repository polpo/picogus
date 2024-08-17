typedef struct wifi_infos_t {
       uint8_t cyw43_mac[6];
       char WIFI_SSID[33];
       char WIFI_KEY[63];
} wifi_infos_t;
extern wifi_infos_t PG_Wifi_info;

extern uint8_t PG_EnableWifi(void);
extern uint8_t PG_NE2000_Read(uint8_t Addr);
extern void PG_Wifi_GetStatus(void);
extern char PG_Wifi_ReadStatusStr(void);
extern void PG_Wifi_Connect(const char* ssid, const char* pass);
extern void PG_Wifi_Reconnect(void);
extern void PG_NE2000_Write(uint8_t Addr,uint8_t Data);    
extern void ne2000_initiate_send();
#define FIFO_NE2K_SEND   0x88
#define FIFO_WIFI_STATUS 0x89

typedef enum { NE2000_NE2000, NE2000_RTL8029AS } ne2000_type;

#define NETBLOCKING 0 // we won't block our pcap

//#define NE2000_DEBUG

#define BX_RESET_HARDWARE 0
#define BX_RESET_SOFTWARE 1

// Never completely fill the ne2k ring so that we never
// hit the unclear completely full buffer condition.
#define BX_NE2K_NEVER_FULL_RING (1)

#define BX_NE2K_MEMSIZ (32 * 1024) // I had set this to 128 during testing?
#define BX_NE2K_MEMSTART (16 * 1024)
#define BX_NE2K_MEMEND (BX_NE2K_MEMSTART + BX_NE2K_MEMSIZ)

typedef struct ne2000_t {
        //
        // ne2k register state

        //
        // Page 0
        //
        //  Command Register - 00h read/write
        struct CR_t {
                int stop;         // STP - Software Reset command
                int start;        // START - start the NIC
                int tx_packet;    // TXP - initiate packet transmission
                uint8_t rdma_cmd; // RD0,RD1,RD2 - Remote DMA command
                uint8_t pgsel;    // PS0,PS1 - Page select
        } CR;
        // Interrupt Status Register - 07h read/write
        struct ISR_t {
                int pkt_rx;    // PRX - packet received with no errors
                int pkt_tx;    // PTX - packet transmitted with no errors
                int rx_err;    // RXE - packet received with 1 or more errors
                int tx_err;    // TXE - packet tx'd       "  " "    "    "
                int overwrite; // OVW - rx buffer resources exhausted
                int cnt_oflow; // CNT - network tally counter MSB's set
                int rdma_done; // RDC - remote DMA complete
                int reset;     // RST - reset status
        } ISR;
        // Interrupt Mask Register - 0fh write
        struct IMR_t {
                int rx_inte;    // PRXE - packet rx interrupt enable
                int tx_inte;    // PTXE - packet tx interrput enable
                int rxerr_inte; // RXEE - rx error interrupt enable
                int txerr_inte; // TXEE - tx error interrupt enable
                int overw_inte; // OVWE - overwrite warn int enable
                int cofl_inte;  // CNTE - counter o'flow int enable
                int rdma_inte;  // RDCE - remote DMA complete int enable
                int reserved;   //  D7 - reserved
        } IMR;
        // Data Configuration Register - 0eh write
        struct DCR_t {
                int wdsize;        // WTS - 8/16-bit select
                int endian;        // BOS - byte-order select
                int longaddr;      // LAS - long-address select
                int loop;          // LS  - loopback select
                int auto_rx;       // AR  - auto-remove rx packets with remote DMA
                uint8_t fifo_size; // FT0,FT1 - fifo threshold
        } DCR;
        // Transmit Configuration Register - 0dh write
        struct TCR_t {
                int crc_disable;   // CRC - inhibit tx CRC
                uint8_t loop_cntl; // LB0,LB1 - loopback control
                int ext_stoptx;    // ATD - allow tx disable by external mcast
                int coll_prio;     // OFST - backoff algorithm select
                uint8_t reserved;  //  D5,D6,D7 - reserved
        } TCR;
        // Transmit Status Register - 04h read
        struct TSR_t {
                int tx_ok;      // PTX - tx complete without error
                int reserved;   //  D1 - reserved
                int collided;   // COL - tx collided >= 1 times
                int aborted;    // ABT - aborted due to excessive collisions
                int no_carrier; // CRS - carrier-sense lost
                int fifo_ur;    // FU  - FIFO underrun
                int cd_hbeat;   // CDH - no tx cd-heartbeat from transceiver
                int ow_coll;    // OWC - out-of-window collision
        } TSR;
        // Receive Configuration Register - 0ch write
        struct RCR_t {
                int errors_ok;    // SEP - accept pkts with rx errors
                int runts_ok;     // AR  - accept < 64-byte runts
                int broadcast;    // AB  - accept eth broadcast address
                int multicast;    // AM  - check mcast hash array
                int promisc;      // PRO - accept all packets
                int monitor;      // MON - check pkts, but don't rx
                uint8_t reserved; //  D6,D7 - reserved
        } RCR;
        // Receive Status Register - 0ch read
        struct RSR_t {
                int rx_ok;       // PRX - rx complete without error
                int bad_crc;     // CRC - Bad CRC detected
                int bad_falign;  // FAE - frame alignment error
                int fifo_or;     // FO  - FIFO overrun
                int rx_missed;   // MPA - missed packet error
                int rx_mbit;     // PHY - unicast or mcast/bcast address match
                int rx_disabled; // DIS - set when in monitor mode
                int deferred;    // DFR - collision active
        } RSR;

        uint16_t local_dma;    // 01,02h read ; current local DMA addr
        uint8_t page_start;    // 01h write ; page start register
        uint8_t page_stop;     // 02h write ; page stop register
        uint8_t bound_ptr;     // 03h read/write ; boundary pointer
        uint8_t tx_page_start; // 04h write ; transmit page start register
        uint8_t num_coll;      // 05h read  ; number-of-collisions register
        uint16_t tx_bytes;     // 05,06h write ; transmit byte-count register
        uint8_t fifo;          // 06h read  ; FIFO
        uint16_t remote_dma;   // 08,09h read ; current remote DMA addr
        uint16_t remote_start; // 08,09h write ; remote start address register
        uint16_t remote_bytes; // 0a,0bh write ; remote byte-count register
        uint8_t tallycnt_0;    // 0dh read  ; tally counter 0 (frame align errors)
        uint8_t tallycnt_1;    // 0eh read  ; tally counter 1 (CRC errors)
        uint8_t tallycnt_2;    // 0fh read  ; tally counter 2 (missed pkt errors)
        
        //
        // Page 1
        //
        //   Command Register 00h (repeated)
        //
        uint8_t physaddr[6]; // 01-06h read/write ; MAC address
        uint8_t curr_page;   // 07h read/write ; current page register
        uint8_t mchash[8];   // 08-0fh read/write ; multicast hash array

        //
        // Page 2  - diagnostic use only
        //
        //   Command Register 00h (repeated)
        //
        //   Page Start Register 01h read  (repeated)
        //   Page Stop Register  02h read  (repeated)
        //   Current Local DMA Address 01,02h write (repeated)
        //   Transmit Page start address 04h read (repeated)
        //   Receive Configuration Register 0ch read (repeated)
        //   Transmit Configuration Register 0dh read (repeated)
        //   Data Configuration Register 0eh read (repeated)
        //   Interrupt Mask Register 0fh read (repeated)
        //
        uint8_t rempkt_ptr;   // 03h read/write ; remote next-packet pointer
        uint8_t localpkt_ptr; // 05h read/write ; local next-packet pointer
        uint16_t address_cnt; // 06,07h read/write ; address counter

        //
        // Page 3  - should never be modified.
        //

        // Novell ASIC state
        uint8_t macaddr[32];         // ASIC ROM'd MAC address, even bytes
        uint8_t mem[BX_NE2K_MEMSIZ]; // on-chip packet memory

        // ne2k internal state
        uint32_t base_address;
        int base_irq;
        int tx_timer_index;
        int tx_timer_active;
        uint8_t pending_tx;

        ne2000_type type;

        /*PCI stuff*/
        int is_pci;
        int card;
        uint32_t base_addr;
        uint8_t pci_command;
        uint8_t int_line;

        /*RTL8029AS registers*/
        uint8_t config0, config2, config3;
        uint8_t _9346cr;
} ne2000_t;

