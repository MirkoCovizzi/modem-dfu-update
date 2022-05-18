#include <zephyr.h>
#include <device.h>
#include <drivers/uart.h>

#include <string.h>
#include <nrf_modem_full_dfu.h>
#include <nrf_modem.h>
#include <nrf_modem_platform.h>
#include <nrf_modem_at.h>
#include "pm_config.h"

#define DBG false

static const nrf_modem_init_params_t init_params = {
        .ipc_irq_prio = NRF_MODEM_NETWORK_IRQ_PRIORITY,
        .shmem.ctrl = {
                .base = PM_NRF_MODEM_LIB_CTRL_ADDRESS,
                .size = CONFIG_NRF_MODEM_LIB_SHMEM_CTRL_SIZE,
        },
        .shmem.tx = {
                .base = PM_NRF_MODEM_LIB_TX_ADDRESS,
                .size = CONFIG_NRF_MODEM_LIB_SHMEM_TX_SIZE,
        },
        .shmem.rx = {
                .base = PM_NRF_MODEM_LIB_RX_ADDRESS,
                .size = CONFIG_NRF_MODEM_LIB_SHMEM_RX_SIZE,
        },
#if CONFIG_NRF_MODEM_LIB_TRACE_ENABLED
        .shmem.trace = {
        .base = PM_NRF_MODEM_LIB_TRACE_ADDRESS,
        .size = CONFIG_NRF_MODEM_LIB_SHMEM_TRACE_SIZE,
    },
#endif
};

#define UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)

static const struct device *uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

/* Intel HEX maximum record data size */
#define MAX_CHUNK_SIZE 256

/* Intel HEX record types */
#define DAT 0
#define EOF 1
#define ESA 2 /* Unused */
#define SSA 3 /* Unused */
#define ELA 4
#define SLA 5 /* Unused */

struct data_chunk {
    uint8_t len;
    uint16_t addr;
    uint8_t type;
    uint8_t data[MAX_CHUNK_SIZE];
    uint8_t check;
    uint8_t segment;
};

static struct data_chunk data_chunk_cb;
static struct data_chunk data_chunk_main;

static uint8_t uart_buf[MAX_CHUNK_SIZE];

static int uart_i;

static enum {
    LEN,
    ADDR,
    TYPE,
    DATA,
    CHECK,
    SEGMENT,
    END
} uart_state = LEN;

enum {
    BL = 1,
    CERT,
    FW
};

K_MSGQ_DEFINE(chunk_queue, sizeof(struct data_chunk), 128, 4);

static void print_fw_version_uuid(void) {
    int err;
    size_t off;
    char fw_version_buf[32];
    char *fw_version_end;
    char fw_uuid_buf[64];
    char *fw_uuid;
    char *fw_uuid_end;

    err = nrf_modem_at_cmd(fw_version_buf, sizeof(fw_version_buf), "AT+CGMR");
    if (err == 0) {
        /* Get first string before "\r\n"
         * which corresponds to FW version string.
         */
        fw_version_end = strstr(fw_version_buf, "\r\n");
        off = fw_version_end - fw_version_buf - 1;
        fw_version_buf[off + 1] = '\0';
        printk("Modem FW version: %s\n", fw_version_buf);
    } else {
        printk("Unable to obtain modem FW version (ERR: %d, ERR TYPE: %d)\n",
               nrf_modem_at_err(err), nrf_modem_at_err_type(err));
    }

    err = nrf_modem_at_cmd(fw_uuid_buf, sizeof(fw_uuid_buf), "AT%%XMODEMUUID");
    if (err == 0) {
        /* Get string that starts with " " after "%XMODEMUUID:",
         * then move to the next string before "\r\n"
         * which corresponds to the FW UUID string.
         */
        fw_uuid = strstr(fw_uuid_buf, " ");
        fw_uuid++;
        fw_uuid_end = strstr(fw_uuid_buf, "\r\n");
        off = fw_uuid_end - fw_uuid_buf - 1;
        fw_uuid_buf[off + 1] = '\0';
        printk("Modem FW UUID: %s\n", fw_uuid);
    } else {
        printk("Unable to obtain modem FW UUID (ERR: %d, ERR TYPE: %d)\n",
               nrf_modem_at_err(err), nrf_modem_at_err_type(err));
    }
}

uint16_t swap_16(uint16_t num) {
    return (num >> 8) | (num << 8);
}

void print_chunk(struct data_chunk *chunk) {
    printk("%d %d %d %s %d %d\n",
           chunk->len, chunk->addr,
           chunk->type, chunk->data,
           chunk->check, chunk->segment);
}

void serial_cb(const struct device *dev, void *user_data) {
    if (!uart_irq_update(uart_dev)) {
        return;
    }

    while (uart_irq_rx_ready(uart_dev)) {
        uart_fifo_read(uart_dev, &uart_buf[uart_i], 1);
        uart_i++;

        if (uart_i == 1 && uart_state == LEN) {
            uart_state = ADDR;
            uart_i = 0;

            memcpy(&data_chunk_cb.len, uart_buf, 1);
        } else if (uart_i == 2 && uart_state == ADDR) {
            uint16_t addr;

            uart_state = TYPE;
            uart_i = 0;

            memcpy(&addr, uart_buf, 2);

            /* Swap endianness */
            data_chunk_cb.addr = swap_16(addr);
        } else if (uart_i == 1 && uart_state == TYPE) {
            uart_state = DATA;
            uart_i = 0;

            if (data_chunk_cb.len == 0) {
                /* Skip DATA read */
                uart_state = CHECK;
            }

            memcpy(&data_chunk_cb.type, uart_buf, 1);
        } else if (uart_i == data_chunk_cb.len && uart_state == DATA) {
            uart_state = CHECK;
            uart_i = 0;

            memcpy(&data_chunk_cb.data, uart_buf, data_chunk_cb.len);
        } else if (uart_i == 1 && uart_state == CHECK) {
            uart_state = SEGMENT;
            uart_i = 0;

            memcpy(&data_chunk_cb.check, uart_buf, 1);
        } else if (uart_i == 1 && uart_state == SEGMENT) {
            uart_state = END;
            uart_i = 0;

            memcpy(&data_chunk_cb.segment, uart_buf, 1);
        }

        if (uart_state == END) {
            uart_state = LEN;

            k_msgq_put(&chunk_queue, &data_chunk_cb, K_NO_WAIT);
        }
    }
}


void main(void) {
    int err;
    uint32_t base_addr = 0;

    if (!device_is_ready(uart_dev)) {
        printk("UART device not found!");
        return;
    }

    nrf_modem_init(&init_params, NORMAL_MODE);

    print_fw_version_uuid();

    nrf_modem_shutdown();

    err = nrf_modem_init(&init_params, FULL_DFU_MODE);

    if (err) {
        printk("init err %d\n", err);
    }

    err = nrf_modem_full_dfu_init(NULL);

    if (err) {
        printk("DFU init error\n");
        return;
    }

    uart_irq_callback_user_data_set(uart_dev, serial_cb, NULL);
    uart_irq_rx_enable(uart_dev);

    printk("DFU start\n");

    /* FSM */
    while (k_msgq_get(&chunk_queue, &data_chunk_main, K_FOREVER) == 0) {
#if DBG
        print_chunk(&data_chunk_main);
#endif

        switch (data_chunk_main.type) {
            case ELA: {
                uint32_t addr;

                memcpy(&addr, data_chunk_main.data, data_chunk_main.len);

                base_addr = swap_16(addr) << 16;

                printk("ELA base addr 0x%08x\n", base_addr);

                break;
            }
            case DAT: {
                if (data_chunk_main.segment == BL) {
                    err = nrf_modem_full_dfu_bl_write(data_chunk_main.len,
                                                      data_chunk_main.data);
                } else {
                    uint32_t addr = base_addr + data_chunk_main.addr;

                    /* UART based modem update can take up to 5 minutes */
                    err = nrf_modem_full_dfu_fw_write(addr,
                                                      data_chunk_main.len,
                                                      data_chunk_main.data);
                }

                if (err) {
                    printk("full dfu err: %d\n", err);
                }

                break;
            }
            case EOF: {
                err = nrf_modem_full_dfu_apply();

                if (err) {
                    printk("full dfu apply err: %d\n", err);
                } else {
                    printk("full dfu apply success\n");
                }

                if (data_chunk_main.segment == FW) {
                    goto end;
                }

                break;
            }
        }
    }

    end:

    nrf_modem_shutdown();

    nrf_modem_init(&init_params, NORMAL_MODE);

    print_fw_version_uuid();
}