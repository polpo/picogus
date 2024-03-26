/*
 * Command buffers for CMS and Tandy bus events
 */

typedef struct cms_buffer_t {
    struct {
        uint16_t addr;
        uint8_t data;
    } cmds[256];
    volatile uint8_t head;
    volatile uint8_t tail;
} cms_buffer_t;

typedef struct tandy_buffer_t {
    uint8_t cmds[256];
    volatile uint8_t head;
    volatile uint8_t tail;
} tandy_buffer_t;
