/*
 *  Host test for sw/dfs/dfs_transport.c.
 *
 *  Drives the register sequence from PROTOCOL.md against the transport with
 *  a local ECHO-only stand-in for the server (dfs_process etc.), so the
 *  transport's state machine is tested in isolation from FatFs.
 *
 *  Build and run:  make -f Makefile.transport check
 *
 *  Copyright (C) 2026  PicoGUS contributors, GPL v2+.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "dfs.h"
#include "dfs_server.h"

/* ---- server stand-ins (placeholder-compatible) ---------------------------- */

static bool     stub_drive_present;
static char     stub_info[64];
static uint16_t stub_dos_time, stub_dos_date;
static int      stub_time_calls;
static int      stub_init_calls, stub_mount_calls, stub_unmount_calls;
static int      stub_process_calls;
static uint32_t stub_millis;

uint16_t dfs_process(uint8_t *buf, uint16_t req_len, uint16_t buf_size) {
    stub_process_calls++;
    uint16_t plen = (req_len > DFS_HDR_LEN) ? (uint16_t)(req_len - DFS_HDR_LEN) : 0;
    uint8_t al = buf[3];
    uint16_t ax = 0;
    uint16_t alen = DFS_HDR_LEN;
    if (al == DFS_AL_ECHO) {
        alen = (uint16_t)(DFS_HDR_LEN + plen);
    } else {
        ax = 0x16;
    }
    if (alen > buf_size) alen = buf_size;
    buf[0] = (uint8_t)(alen & 0xFF);
    buf[1] = (uint8_t)(alen >> 8);
    buf[2] = (uint8_t)(ax & 0xFF);
    buf[3] = (uint8_t)(ax >> 8);
    return alen;
}

void dfs_server_init(void) { stub_init_calls++; stub_drive_present = false; stub_info[0] = 0; }
void dfs_server_drive_mounted(void) { stub_mount_calls++; stub_drive_present = true; strcpy(stub_info, "TEST|FAT32|123|DEADBEEF"); }
void dfs_server_drive_unmounted(void) { stub_unmount_calls++; stub_drive_present = false; stub_info[0] = 0; }
bool dfs_server_drive_present(void) { return stub_drive_present; }
void dfs_server_set_dos_time(uint16_t dos_time, uint16_t dos_date) { stub_time_calls++; stub_dos_time = dos_time; stub_dos_date = dos_date; }
const char *dfs_server_info_string(void) { return stub_info; }
uint32_t dfs_platform_millis(void) { return stub_millis; }

/* ---- tiny check framework -------------------------------------------------- */

static int checks, failures;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_EQ(a, b) do { \
    checks++; \
    unsigned long _a = (unsigned long)(a), _b = (unsigned long)(b); \
    if (_a != _b) { failures++; printf("FAIL %s:%d: %s == %s (0x%lx != 0x%lx)\n", __FILE__, __LINE__, #a, #b, _a, _b); } \
} while (0)

/* ---- driver-side helpers (mirror the sequence in PROTOCOL.md) -------------- */

/* out 1D0h,81h ; rep outsb ; out 1D0h,82h ; out 1D2h,x */
static void send_frame(const uint8_t *frame, uint16_t len) {
    dfs_ctl_select_req();
    for (uint16_t i = 0; i < len; i++) dfs_data_write(frame[i]);
    dfs_ctl_exec();
}

static void make_frame(uint8_t *frame, uint16_t declared_len, uint8_t al, const uint8_t *payload, uint16_t plen) {
    frame[0] = (uint8_t)(declared_len & 0xFF);
    frame[1] = (uint8_t)(declared_len >> 8);
    frame[2] = 0;                          /* drive 0, no flags */
    frame[3] = al;
    if (plen) memcpy(frame + DFS_HDR_LEN, payload, plen);
}

/* out 1D0h,83h ; insb... */
static uint16_t read_answer(uint8_t *out, uint16_t max) {
    dfs_ctl_select_resp();
    uint16_t n = 0;
    while (n < max) {
        out[n++] = dfs_data_read();
    }
    return n;
}

/* ---- tests ----------------------------------------------------------------- */

static uint8_t frame[DFS_BUF_SIZE + 16];
static uint8_t answer[DFS_BUF_SIZE + 16];

static void test_init_and_nodrive(void) {
    printf("- init / NODRIVE reporting\n");
    dfs_init();
    CHECK_EQ(stub_init_calls, 1);
    CHECK_EQ(dfs_ctl_max_payload(), DFS_MAX_PAYLOAD);
    /* no drive: IDLE is reported as NODRIVE */
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_NODRIVE);
    /* reads before any transaction */
    CHECK_EQ(dfs_data_read(), 0xFF);
    /* a rejected request without a drive is also NODRIVE (ABORTED masked) */
    dfs_ctl_select_req();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_RECEIVING);   /* RECEIVING is never masked */
    dfs_ctl_exec();                                     /* 0 bytes: rejected */
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_NODRIVE);
    /* mount hook forwards to the server and un-masks the status */
    dfs_on_drive_mounted();
    CHECK_EQ(stub_mount_calls, 1);
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_ABORTED);
    dfs_ctl_select_req();
    dfs_ctl_abort();                                     /* back to a clean ABORTED/IDLE-ish state */
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_ABORTED);
}

static void test_echo_transaction(void) {
    printf("- normal ECHO transaction end to end\n");
    static const uint8_t payload[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
    uint16_t len = DFS_HDR_LEN + sizeof(payload);
    make_frame(frame, len, DFS_AL_ECHO, payload, sizeof(payload));

    send_frame(frame, len);
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_BUSY);
    /* while BUSY, data port is dead in both directions */
    dfs_data_write(0xAA);
    CHECK_EQ(dfs_data_read(), 0xFF);
    /* CMD_DFSREQ while BUSY is ignored (state stays BUSY) */
    dfs_ctl_select_req();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_BUSY);

    int calls = stub_process_calls;
    dfs_tasks();                                        /* core 1 serves it */
    CHECK_EQ(stub_process_calls, calls + 1);
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_READY);
    dfs_tasks();                                        /* idempotent when not BUSY */
    CHECK_EQ(stub_process_calls, calls + 1);

    read_answer(answer, len);
    CHECK_EQ(answer[0], len & 0xFF);
    CHECK_EQ(answer[1], len >> 8);
    CHECK_EQ(answer[2], 0);                             /* AX = 0 */
    CHECK_EQ(answer[3], 0);
    CHECK(memcmp(answer + DFS_HDR_LEN, payload, sizeof(payload)) == 0);

    /* reads past the answer end -> 0xFF */
    CHECK_EQ(dfs_data_read(), 0xFF);
    CHECK_EQ(dfs_data_read(), 0xFF);
    /* CMD_DFSRESP rewinds: the answer can be read again */
    dfs_ctl_select_resp();
    CHECK_EQ(dfs_data_read(), len & 0xFF);
    /* still READY; the next CMD_DFSREQ discards it */
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_READY);
    dfs_ctl_select_req();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_RECEIVING);
    CHECK_EQ(dfs_data_read(), 0xFF);
    dfs_ctl_abort();
}

static void test_max_payload(void) {
    printf("- full-size ECHO (DFS_MAX_PAYLOAD bytes)\n");
    uint16_t len = DFS_BUF_SIZE;
    for (uint16_t i = 0; i < DFS_MAX_PAYLOAD; i++) frame[DFS_HDR_LEN + i] = (uint8_t)(i * 7 + 3);
    make_frame(frame, len, DFS_AL_ECHO, NULL, 0);
    send_frame(frame, len);
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_BUSY);
    dfs_tasks();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_READY);
    read_answer(answer, len);
    CHECK(memcmp(answer + DFS_HDR_LEN, frame + DFS_HDR_LEN, DFS_MAX_PAYLOAD) == 0);
    CHECK_EQ(dfs_data_read(), 0xFF);
}

static void test_unknown_subfunction(void) {
    printf("- non-ECHO subfunction answer header\n");
    uint16_t len = DFS_HDR_LEN + 2;
    static const uint8_t payload[] = { 1, 2 };
    make_frame(frame, len, 0x0C, payload, 2);
    send_frame(frame, len);
    dfs_tasks();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_READY);
    read_answer(answer, DFS_HDR_LEN);
    CHECK_EQ(answer[0], DFS_HDR_LEN);
    CHECK_EQ(answer[1], 0);
    CHECK_EQ(answer[2], 0x16);
    CHECK_EQ(answer[3], 0);
    CHECK_EQ(dfs_data_read(), 0xFF);                     /* answer is header only */
}

static void test_length_mismatch(void) {
    printf("- length mismatch -> ABORTED\n");
    static const uint8_t payload[] = { 1, 2, 3, 4 };
    uint16_t len = DFS_HDR_LEN + sizeof(payload);

    /* declared length larger than sent */
    make_frame(frame, len + 1, DFS_AL_ECHO, payload, sizeof(payload));
    send_frame(frame, len);
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_ABORTED);
    int calls = stub_process_calls;
    dfs_tasks();
    CHECK_EQ(stub_process_calls, calls);                 /* nothing served */
    CHECK_EQ(dfs_data_read(), 0xFF);

    /* declared length smaller than sent */
    make_frame(frame, len - 1, DFS_AL_ECHO, payload, sizeof(payload));
    send_frame(frame, len);
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_ABORTED);

    /* fewer than DFS_HDR_LEN bytes */
    make_frame(frame, 3, DFS_AL_ECHO, NULL, 0);
    send_frame(frame, 3);
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_ABORTED);

    /* declared length beyond the buffer, matching bytes not possible anyway */
    make_frame(frame, 0xFFFF, DFS_AL_ECHO, payload, sizeof(payload));
    send_frame(frame, len);
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_ABORTED);

    /* CMD_DFSEXEC without a preceding CMD_DFSREQ (state ABORTED) stays ABORTED */
    dfs_ctl_exec();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_ABORTED);

    /* ABORTED is sticky until CMD_DFSREQ, and a good frame then works again */
    make_frame(frame, len, DFS_AL_ECHO, payload, sizeof(payload));
    send_frame(frame, len);
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_BUSY);
    dfs_tasks();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_READY);
    read_answer(answer, len);
    CHECK(memcmp(answer + DFS_HDR_LEN, payload, sizeof(payload)) == 0);
}

static void test_overflow(void) {
    printf("- overflow -> ABORTED\n");
    /* header claims exactly DFS_BUF_SIZE bytes, but the driver sends more */
    uint16_t len = DFS_BUF_SIZE;
    make_frame(frame, len, DFS_AL_ECHO, NULL, 0);
    dfs_ctl_select_req();
    for (uint16_t i = 0; i < len; i++) dfs_data_write(frame[i]);
    dfs_data_write(0xEE);                               /* one too many */
    dfs_data_write(0xEE);
    dfs_ctl_exec();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_ABORTED);
    int calls = stub_process_calls;
    dfs_tasks();
    CHECK_EQ(stub_process_calls, calls);

    /* overflow flag is cleared by the next CMD_DFSREQ */
    static const uint8_t payload[] = { 9 };
    make_frame(frame, DFS_HDR_LEN + 1, DFS_AL_ECHO, payload, 1);
    send_frame(frame, DFS_HDR_LEN + 1);
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_BUSY);
    dfs_tasks();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_READY);
}

static void test_abort_while_busy(void) {
    printf("- abort while BUSY -> result dropped\n");
    static const uint8_t payload[] = { 0xDE, 0xAD };
    uint16_t len = DFS_HDR_LEN + sizeof(payload);
    make_frame(frame, len, DFS_AL_ECHO, payload, sizeof(payload));
    send_frame(frame, len);
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_BUSY);

    dfs_ctl_abort();                                     /* out 1D2h, 0 on CMD_DFSSTAT */
    /* core 1 still owns the buffer: the status stays BUSY so the driver's
     * next request waits instead of streaming into a buffer being written */
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_BUSY);
    dfs_ctl_select_req();                                /* all ignored while BUSY */
    dfs_data_write(0x55);
    dfs_ctl_exec();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_BUSY);
    int calls = stub_process_calls;
    dfs_tasks();                                        /* core 1 finishes and drops the result */
    CHECK_EQ(stub_process_calls, calls + 1);
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_ABORTED);
    CHECK_EQ(dfs_data_read(), 0xFF);
    dfs_ctl_select_resp();
    CHECK_EQ(dfs_data_read(), 0xFF);

    /* CMD_DFSEXEC in ABORTED is rejected */
    dfs_ctl_exec();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_ABORTED);

    /* abort in RECEIVING and in READY also lands in ABORTED */
    dfs_ctl_select_req();
    dfs_data_write(1);
    dfs_ctl_abort();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_ABORTED);
    send_frame(frame, len);
    dfs_tasks();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_READY);
    dfs_ctl_abort();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_ABORTED);
    CHECK_EQ(dfs_data_read(), 0xFF);

    /* and the next transaction is clean */
    send_frame(frame, len);
    dfs_tasks();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_READY);
    read_answer(answer, len);
    CHECK(memcmp(answer + DFS_HDR_LEN, payload, sizeof(payload)) == 0);
}

static void test_late_ready_after_abort(void) {
    printf("- abort racing core 1's READY write stays ABORTED\n");
    /* Simulate the window where core 1 has passed its generation check but
     * core 0 aborts before core 1 stores READY. We can only approximate it
     * on the host: serve, then abort, and confirm ABORTED wins and the buffer
     * is not readable. */
    static const uint8_t payload[] = { 7, 7, 7 };
    uint16_t len = DFS_HDR_LEN + sizeof(payload);
    make_frame(frame, len, DFS_AL_ECHO, payload, sizeof(payload));
    send_frame(frame, len);
    dfs_tasks();
    dfs_ctl_abort();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_ABORTED);
    dfs_ctl_select_resp();
    CHECK_EQ(dfs_data_read(), 0xFF);
    dfs_ctl_select_req();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_RECEIVING);
    dfs_ctl_abort();
}

static void test_info_string(void) {
    printf("- info string iteration with rewind\n");
    const char *expect = "TEST|FAT32|123|DEADBEEF";
    dfs_ctl_info_rewind();
    char got[64];
    size_t n = 0;
    uint8_t c;
    while ((c = dfs_ctl_info_read()) != 0 && n < sizeof(got) - 1) got[n++] = (char)c;
    got[n] = 0;
    CHECK(strcmp(got, expect) == 0);
    /* the terminator rewound: reading again starts over */
    CHECK_EQ(dfs_ctl_info_read(), (uint8_t)expect[0]);
    CHECK_EQ(dfs_ctl_info_read(), (uint8_t)expect[1]);
    /* explicit rewind mid-string */
    dfs_ctl_info_rewind();
    CHECK_EQ(dfs_ctl_info_read(), (uint8_t)expect[0]);
    /* empty string when nothing is mounted */
    dfs_on_drive_unmounted();
    CHECK_EQ(stub_unmount_calls, 1);
    dfs_ctl_info_rewind();
    CHECK_EQ(dfs_ctl_info_read(), 0);
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_NODRIVE);
    dfs_on_drive_mounted();
    CHECK_EQ(dfs_ctl_status(), DFS_STATUS_ABORTED);
}

static void test_time_write(void) {
    printf("- time write\n");
    dfs_ctl_time_rewind();
    int calls = stub_time_calls;
    dfs_ctl_time_write(0x34);                            /* time lo */
    dfs_ctl_time_write(0x12);                            /* time hi */
    dfs_ctl_time_write(0x78);                            /* date lo */
    CHECK_EQ(stub_time_calls, calls);                    /* not yet */
    dfs_ctl_time_write(0x56);                            /* date hi */
    dfs_tasks();                                        /* core 1 applies it */
    CHECK_EQ(stub_time_calls, calls + 1);
    CHECK_EQ(stub_dos_time, 0x1234);
    CHECK_EQ(stub_dos_date, 0x5678);
    /* the counter wrapped: a second 4-byte group lands too */
    dfs_ctl_time_write(0x01);
    dfs_ctl_time_write(0x02);
    dfs_ctl_time_write(0x03);
    dfs_ctl_time_write(0x04);
    dfs_tasks();
    CHECK_EQ(stub_time_calls, calls + 2);
    CHECK_EQ(stub_dos_time, 0x0201);
    CHECK_EQ(stub_dos_date, 0x0403);
    /* rewind discards partial input */
    dfs_ctl_time_write(0xAA);
    dfs_ctl_time_write(0xBB);
    dfs_ctl_time_rewind();
    dfs_ctl_time_write(0x11);
    dfs_ctl_time_write(0x22);
    dfs_ctl_time_write(0x33);
    dfs_ctl_time_write(0x44);
    dfs_tasks();
    CHECK_EQ(stub_time_calls, calls + 3);
    CHECK_EQ(stub_dos_time, 0x2211);
    CHECK_EQ(stub_dos_date, 0x4433);
}

static void test_millis(void) {
    printf("- platform millis is the harness's\n");
    stub_millis = 4242;
    CHECK_EQ(dfs_platform_millis(), 4242);
}

int main(void) {
    printf("test_transport: DFS_MAX_PAYLOAD=%d DFS_BUF_SIZE=%d\n", DFS_MAX_PAYLOAD, DFS_BUF_SIZE);
    test_init_and_nodrive();
    test_echo_transaction();
    test_max_payload();
    test_unknown_subfunction();
    test_length_mismatch();
    test_overflow();
    test_abort_while_busy();
    test_late_ready_after_abort();
    test_info_string();
    test_time_write();
    test_millis();
    printf("%d checks, %d failures: %s\n", checks, failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
