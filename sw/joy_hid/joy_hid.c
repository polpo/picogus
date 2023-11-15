// Adapted from hid_controller example from TinyUSB for PicoGUS
/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021, Ha Thach (tinyusb.org)
 * Copyright (c) 2023, Ian Scott
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

/* #include "bsp/board_api.h" */
#include "tusb.h"
#include "xinput_host.h"

#include "joy.h"
joystate_struct_t joystate_struct;

// Sony DS4 report layout detail https://www.psdevwiki.com/ps4/DS4-USB
typedef struct TU_ATTR_PACKED
{
    uint8_t x, y, z, rz; // joystick

    struct {
        uint8_t dpad     : 4; // (hat format, 0x08 is released, 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW)
        uint8_t square   : 1; // west
        uint8_t cross    : 1; // south
        uint8_t circle   : 1; // east
        uint8_t triangle : 1; // north
    };

    struct {
        uint8_t l1     : 1;
        uint8_t r1     : 1;
        uint8_t l2     : 1;
        uint8_t r2     : 1;
        uint8_t share  : 1;
        uint8_t option : 1;
        uint8_t l3     : 1;
        uint8_t r3     : 1;
    };

    struct {
        uint8_t ps      : 1; // playstation button
        uint8_t tpad    : 1; // track pad click
        uint8_t counter : 6; // +1 each report
    };

    uint8_t l2_trigger; // 0 released, 0xff fully pressed
    uint8_t r2_trigger; // as above

    //  uint16_t timestamp;
    //  uint8_t  battery;
    //
    //  int16_t gyro[3];  // x, y, z;
    //  int16_t accel[3]; // x, y, z

    // there is still lots more info

} sony_ds4_report_t;

// check if device is Sony DualShock 4
static inline bool is_sony_ds4(uint8_t dev_addr)
{
  uint16_t vid, pid;
  tuh_vid_pid_get(dev_addr, &vid, &pid);

  return ( (vid == 0x054c && (pid == 0x09cc || pid == 0x05c4)) // Sony DualShock4
           || (vid == 0x0f0d && pid == 0x005e)                 // Hori FC4
           || (vid == 0x0f0d && pid == 0x00ee)                 // Hori PS4 Mini (PS4-099U)
           || (vid == 0x1f4f && pid == 0x1002)                 // ASW GG xrd controller
         );
}

static inline bool is_ps_classic(uint8_t dev_addr)
{
  uint16_t vid, pid;
  tuh_vid_pid_get(dev_addr, &vid, &pid);

  return ( (vid == 0x054c && pid == 0x0cda)     // PS Classic USB controller
         );
}

//--------------------------------------------------------------------+
// TinyUSB Callbacks
//--------------------------------------------------------------------+

// Invoked when device with hid interface is mounted
// Report descriptor is also available for use. tuh_hid_parse_report_descriptor()
// can be used to parse common/simple enough descriptor.
// Note: if report descriptor length > CFG_TUH_ENUMERATION_BUFSIZE, it will be skipped
// therefore report_desc = NULL, desc_len = 0
#define MAX_REPORT 4
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len)
{
    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    printf("HID device address = %d, instance = %d is mounted\r\n", dev_addr, instance);
    printf("VID = %04x, PID = %04x\r\n", vid, pid);

    tuh_hid_report_info_t report_info[MAX_REPORT];
    uint8_t report_count = tuh_hid_parse_report_descriptor(report_info, MAX_REPORT, desc_report, desc_len);

    for (int i = 0; i < report_count; ++i) {
        printf("HID device report id: %u usage: %u usage page: %u\n",
               report_info[i].report_id, report_info[i].usage, report_info[i].usage_page);
        if (report_info[i].usage == HID_USAGE_DESKTOP_JOYSTICK || report_info[i].usage == HID_USAGE_DESKTOP_GAMEPAD) {
            printf("joystick connected - requesting reports from it\n");
            if ( !tuh_hid_receive_report(dev_addr, instance) )
            {
                printf("Error: cannot request to receive report\r\n");
            }
            return;
        }
    }
}

// Invoked when device with hid interface is un-mounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    printf("HID device address = %d, instance = %d is unmounted\r\n", dev_addr, instance);
}

void update_joystate(uint8_t dpad, uint8_t x, uint8_t y, uint8_t z, uint8_t rz, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4) {
    joystate_struct.button_mask = (!b1 << 4) | (!b2 << 5) | (!b3 << 6) | (!b4 << 7);
    switch (dpad) {
    case 0: // N
        joystate_struct.joy1_x = 127;
        joystate_struct.joy1_y = 0;
        break;
    case 1: // NE
        joystate_struct.joy1_x = 255;
        joystate_struct.joy1_y = 0;
        break;
    case 2: // E
        joystate_struct.joy1_x = 255;
        joystate_struct.joy1_y = 127;
        break;
    case 3: // SE
        joystate_struct.joy1_x = 255;
        joystate_struct.joy1_y = 255;
        break;
    case 4: // S
        joystate_struct.joy1_x = 127;
        joystate_struct.joy1_y = 255;
        break;
    case 5: // SW
        joystate_struct.joy1_x = 0;
        joystate_struct.joy1_y = 255;
        break;
    case 6: // W
        joystate_struct.joy1_x = 0;
        joystate_struct.joy1_y = 127;
        break;
    case 7: // NW
        joystate_struct.joy1_x = 0;
        joystate_struct.joy1_y = 0;
        break;
    case 8: // None - use analog stick instead
        joystate_struct.joy1_x = x;
        joystate_struct.joy1_y = y;
        break;
    }
    joystate_struct.joy2_x = z;
    joystate_struct.joy2_y = rz;
}

inline void process_sony_ds4(uint8_t const* report, uint16_t len)
{
    uint8_t const report_id = report[0];
    report++;
    len--;

    // all buttons state is stored in ID 1
    if (report_id == 1)
    {
        sony_ds4_report_t ds4_report;
        memcpy(&ds4_report, report, sizeof(ds4_report));

        update_joystate(ds4_report.dpad, ds4_report.x, ds4_report.y, ds4_report.z, ds4_report.rz,
                ds4_report.cross, ds4_report.circle, ds4_report.square, ds4_report.triangle);
        /*
        const char* dpad_str[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW", "none" };

        printf("(x, y, z, rz) = (%u, %u, %u, %u)\r\n", ds4_report.x, ds4_report.y, ds4_report.z, ds4_report.rz);
        printf("DPad = %s ", dpad_str[ds4_report.dpad]);

        if (ds4_report.square   ) printf("Square ");
        if (ds4_report.cross    ) printf("Cross ");
        if (ds4_report.circle   ) printf("Circle ");
        if (ds4_report.triangle ) printf("Triangle ");

        if (ds4_report.l1       ) printf("L1 ");
        if (ds4_report.r1       ) printf("R1 ");
        if (ds4_report.l2       ) printf("L2 ");
        if (ds4_report.r2       ) printf("R2 ");

        if (ds4_report.share    ) printf("Share ");
        if (ds4_report.option   ) printf("Option ");
        if (ds4_report.l3       ) printf("L3 ");
        if (ds4_report.r3       ) printf("R3 ");

        if (ds4_report.ps       ) printf("PS ");
        if (ds4_report.tpad     ) printf("TPad ");

        printf("\r\n");
        */
    }
}

// Invoked when received report from device via interrupt endpoint
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
    /*
    for (uint16_t i = 0; i < len; ++i) {
        printf("%02x ", report[i]);
    }
    putchar('\n');
    */
    if (is_sony_ds4(dev_addr)) {
        process_sony_ds4(report, len);
    }

    // continue to request to receive report
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        printf("Error: cannot request to receive report\r\n");
    }
}

inline void update_joystate_xinput(uint16_t wButtons, int16_t sThumbLX, int16_t sThumbLY, int16_t sThumbRX, int16_t sThumbRY, uint8_t bLeftTrigger, uint8_t bRightTrigger) {
    uint8_t dpad = wButtons & 0xf;
    if (!dpad) {
        joystate_struct.joy1_x = ((int32_t)sThumbLX + 32768) >> 8;
        joystate_struct.joy1_y = ((-(int32_t)sThumbLY) + 32767) >> 8;
    } else {
        joystate_struct.joy1_x = (dpad & XINPUT_GAMEPAD_DPAD_RIGHT) ? 255 : ((dpad & XINPUT_GAMEPAD_DPAD_LEFT) ? 0 : 127);
        joystate_struct.joy1_y = (dpad & XINPUT_GAMEPAD_DPAD_DOWN) ? 255 : ((dpad & XINPUT_GAMEPAD_DPAD_UP) ? 0 : 127);
    }
    if (bLeftTrigger) {
        joystate_struct.joy1_y = 127u + (bLeftTrigger >> 1);
    } else if (bRightTrigger) {
        joystate_struct.joy1_y = 127u - (bRightTrigger >> 1);
    }
    joystate_struct.joy2_x = ((int32_t)sThumbRX + 32768) >> 8;
    joystate_struct.joy2_y = ((-(int32_t)sThumbRY) + 32767) >> 8;
    joystate_struct.button_mask = (~(wButtons >> 12)) << 4;
    /* printf("%04x %04x\n", wButtons, joystate_struct.button_mask); */
}

void tuh_xinput_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len)
{
    xinputh_interface_t *xid_itf = (xinputh_interface_t *)report;
    xinput_gamepad_t *p = &xid_itf->pad;
    /*
    const char* type_str;
    switch (xid_itf->type)
    {
        case 1: type_str = "Xbox One";          break;
        case 2: type_str = "Xbox 360 Wireless"; break;
        case 3: type_str = "Xbox 360 Wired";    break;
        case 4: type_str = "Xbox OG";           break;
        default: type_str = "Unknown";
    }
    */

    if (xid_itf->connected && xid_itf->new_pad_data) {
        /*
        printf("[%02x, %02x], Type: %s, Buttons %04x, LT: %02x RT: %02x, LX: %d, LY: %d, RX: %d, RY: %d\n",
             dev_addr, instance, type_str, p->wButtons, p->bLeftTrigger, p->bRightTrigger, p->sThumbLX, p->sThumbLY, p->sThumbRX, p->sThumbRY);
        */
        update_joystate_xinput(p->wButtons, p->sThumbLX, p->sThumbLY, p->sThumbRX, p->sThumbRY, p->bLeftTrigger, p->bRightTrigger);
    }
    tuh_xinput_receive_report(dev_addr, instance);
}

void tuh_xinput_mount_cb(uint8_t dev_addr, uint8_t instance, const xinputh_interface_t *xinput_itf)
{
    printf("XINPUT MOUNTED %02x %d\n", dev_addr, instance);
    // If this is a Xbox 360 Wireless controller we need to wait for a connection packet
    // on the in pipe before setting LEDs etc. So just start getting data until a controller is connected.
    if (xinput_itf->type == XBOX360_WIRELESS && xinput_itf->connected == false) {
        tuh_xinput_receive_report(dev_addr, instance);
        return;
    } else if (xinput_itf->type == XBOX360_WIRED) {
        /*
         * Some third-party Xbox 360-style controllers require this message to finish initialization.
         * Idea taken from Linux drivers/input/joystick/xpad.c
         */
        uint8_t dummy[20];
        tusb_control_request_t const request =
        {
            .bmRequestType_bit =
            {
                .recipient = TUSB_REQ_RCPT_INTERFACE,
                .type      = TUSB_REQ_TYPE_VENDOR,
                .direction = TUSB_DIR_IN
            },
            .bRequest = tu_htole16(0x01),
            .wValue   = tu_htole16(0x100),
            .wIndex   = tu_htole16(0x00),
            .wLength  = 20
        };
        tuh_xfer_t xfer =
        {
            .daddr       = dev_addr,
            .ep_addr     = 0,
            .setup       = &request,
            .buffer      = dummy,
            .complete_cb = NULL,
            .user_data   = 0
        };
        tuh_control_xfer(&xfer);
    }
    tuh_xinput_set_led(dev_addr, instance, 0, true);
    tuh_xinput_set_led(dev_addr, instance, 1, true);
    tuh_xinput_set_rumble(dev_addr, instance, 0, 0, true);
    tuh_xinput_receive_report(dev_addr, instance);
}

void tuh_xinput_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    printf("XINPUT UNMOUNTED %02x %d\n", dev_addr, instance);
}
