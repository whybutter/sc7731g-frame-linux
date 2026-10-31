// SPDX-License-Identifier: Apache-2.0
/*
 * c3-hci -- an ESP32-C3 Super Mini as the VT10101's BLE controller: HCI H4 over
 * UART1 (TX GPIO5, RX GPIO6, 1 Mbaud, no flow control) to the frame's ttyS1,
 * attached there with btattach (docs/ble-c3.md).
 *
 * From ESP-IDF 5.5's examples/bluetooth/hci/controller_hci_uart_esp32c3_and_
 * esp32s3 (Unlicense/CC0): the UHCI + GDMA transport that moves H4 between
 * UART1 and the controller is the example's. What is new is the frame:
 *
 * The VT10101's vendor SPL/U-Boot talk on that same UART at every power-on and
 * reset, and U-Boot's bootdelay is 0: one byte in its RX FIFO at that moment
 * stops autoboot and leaves the frame at a U-Boot prompt. The C3 boots while
 * U-Boot runs (nothing holds it in reset then) and hears U-Boot's 115200 banner
 * as noise at 1 Mbaud. So this firmware puts nothing on the wire until the host
 * has spoken HCI:
 * - every log (ROM, bootloader, app) goes to USB-Serial-JTAG (sdkconfig);
 * - GPIO5 (TX) stays a pulled-up input -- not driven at all -- until then. The
 *   example instead made it a plain GPIO output, low, before routing it to the
 *   UART: a low pulse on the frame's RX is a byte;
 * - UART1 first runs RX-only through the plain driver and waits for the host's
 *   first command, HCI_Reset (01 03 0c 00), which the kernel sends first on
 *   btattach. Everything before it (U-Boot's banner, line noise) is dropped and
 *   only counted on the USB log;
 * - then TX goes live (set high as a GPIO first, then handed to the already
 *   configured UART, which idles high), the controller starts, and that caught
 *   HCI_Reset is replayed into it: its first receive requests are served from a
 *   4-byte buffer by a feeder task instead of DMA, so the controller answers
 *   the host's reset itself.
 * A reset of the C3 (GPIO 729 -> diode -> EN, sc7731g-bt-ext) starts it over.
 */

#include <string.h>
#include "esp_private/periph_ctrl.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "soc/lldesc.h"
#include "esp_private/gdma.h"
#include "hal/uhci_ll.h"
#include "rom/gpio.h"
#include "soc/gpio_sig_map.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "c3-hci";

#define UART_HCI_NUM       (1)
#define UART_RX_THRS       (120)
#define GPIO_UART_TXD_OUT  (5)
#define GPIO_UART_RXD_IN   (6)

static const uint8_t HCI_RESET[4] = {0x01, 0x03, 0x0c, 0x00};

/* --- the controller's transport (the example's, plus the replay) --- */

static bool hci_uart_tl_init(void);
static void hci_uart_tl_deinit(void);
static void hci_uart_tl_recv_async(uint8_t *buf, uint32_t size, esp_bt_hci_tl_callback_t callback, void *arg);
static void hci_uart_tl_send_async(uint8_t *buf, uint32_t size, esp_bt_hci_tl_callback_t callback, void *arg);
static void hci_uart_tl_flow_on(void);
static bool hci_uart_tl_flow_off(void);
static void hci_uart_tl_finish_transfers(void);

struct uart_txrxchannel {
    esp_bt_hci_tl_callback_t callback;
    void *arg;
    lldesc_t link;
};

struct uart_env_tag {
    struct uart_txrxchannel tx;
    struct uart_txrxchannel rx;
};

static struct uart_env_tag uart_env;
static uhci_dev_t *s_uhci_hw = &UHCI0;
static gdma_channel_handle_t s_rx_channel;
static gdma_channel_handle_t s_tx_channel;

static esp_bt_hci_tl_t s_hci_uart_tl_funcs = {
    ._magic = ESP_BT_HCI_TL_MAGIC_VALUE,
    ._version = ESP_BT_HCI_TL_VERSION,
    ._reserved = 0,
    ._open = (void *)hci_uart_tl_init,
    ._close = (void *)hci_uart_tl_deinit,
    ._finish_transfers = (void *)hci_uart_tl_finish_transfers,
    ._recv = (void *)hci_uart_tl_recv_async,
    ._send = (void *)hci_uart_tl_send_async,
    ._flow_on = (void *)hci_uart_tl_flow_on,
    ._flow_off = (void *)hci_uart_tl_flow_off,
};

/* the caught HCI_Reset, handed to the controller before any DMA receive */
static uint8_t s_replay[sizeof(HCI_RESET)];
static volatile uint32_t s_replay_len, s_replay_pos;
static SemaphoreHandle_t s_feed;
static uint8_t *s_feed_buf;
static uint32_t s_feed_size;

static bool hci_uart_tl_init(void)
{
    return true;
}

static void hci_uart_tl_deinit(void)
{
}

static IRAM_ATTR void hci_uart_tl_recv_async(uint8_t *buf, uint32_t size, esp_bt_hci_tl_callback_t callback, void *arg)
{
    assert(buf != NULL);
    assert(size != 0);
    assert(callback != NULL);
    uart_env.rx.callback = callback;
    uart_env.rx.arg = arg;

    if (s_replay_pos < s_replay_len) {
        /* still replaying: the feeder task completes this request, so the
         * callback runs outside the controller's own call, as with DMA */
        s_feed_buf = buf;
        s_feed_size = size;
        xSemaphoreGive(s_feed);
        return;
    }

    memset(&uart_env.rx.link, 0, sizeof(lldesc_t));
    uart_env.rx.link.buf = buf;
    uart_env.rx.link.size = size;
    s_uhci_hw->pkt_thres.thrs = size;
    gdma_start(s_rx_channel, (intptr_t)(&uart_env.rx.link));
}

static void feeder_task(void *unused)
{
    for (;;) {
        xSemaphoreTake(s_feed, portMAX_DELAY);
        uint32_t n = s_replay_len - s_replay_pos;
        if (n > s_feed_size)
            n = s_feed_size;
        memcpy(s_feed_buf, s_replay + s_replay_pos, n);
        s_replay_pos += n;
        if (n < s_feed_size) {
            /* the controller asked for more than the replay holds; never the
             * case for one 4-byte command (1 + 3 + 0), but finish it by DMA */
            memset(&uart_env.rx.link, 0, sizeof(lldesc_t));
            uart_env.rx.link.buf = s_feed_buf + n;
            uart_env.rx.link.size = s_feed_size - n;
            s_uhci_hw->pkt_thres.thrs = s_feed_size - n;
            gdma_start(s_rx_channel, (intptr_t)(&uart_env.rx.link));
            continue;
        }
        esp_bt_hci_tl_callback_t callback = uart_env.rx.callback;
        void *arg = uart_env.rx.arg;
        uart_env.rx.callback = NULL;
        uart_env.rx.arg = NULL;
        callback(arg, ESP_BT_HCI_TL_STATUS_OK);
        esp_bt_h4tl_eif_io_event_notify(1);
    }
}

static IRAM_ATTR void hci_uart_tl_send_async(uint8_t *buf, uint32_t size, esp_bt_hci_tl_callback_t callback, void *arg)
{
    assert(buf != NULL);
    assert(size != 0);
    assert(callback != NULL);

    uart_env.tx.callback = callback;
    uart_env.tx.arg = arg;

    memset(&uart_env.tx.link, 0, sizeof(lldesc_t));
    uart_env.tx.link.length = size;
    uart_env.tx.link.buf = buf;
    uart_env.tx.link.eof = 1;

    gdma_start(s_tx_channel, (intptr_t)(&uart_env.tx.link));
}

static void hci_uart_tl_flow_on(void)
{
}

static bool hci_uart_tl_flow_off(void)
{
    return true;
}

static void hci_uart_tl_finish_transfers(void)
{
}

static IRAM_ATTR bool hci_uart_tl_rx_eof_callback(gdma_channel_handle_t dma_chan, gdma_event_data_t *event_data, void *user_data)
{
    assert(dma_chan == s_rx_channel);
    assert(uart_env.rx.callback != NULL);
    esp_bt_hci_tl_callback_t callback = uart_env.rx.callback;
    void *arg = uart_env.rx.arg;

    uart_env.rx.callback = NULL;
    uart_env.rx.arg = NULL;
    callback(arg, ESP_BT_HCI_TL_STATUS_OK);
    esp_bt_h4tl_eif_io_event_notify(1);
    return true;
}

static IRAM_ATTR bool hci_uart_tl_tx_eof_callback(gdma_channel_handle_t dma_chan, gdma_event_data_t *event_data, void *user_data)
{
    assert(dma_chan == s_tx_channel);
    assert(uart_env.tx.callback != NULL);
    esp_bt_hci_tl_callback_t callback = uart_env.tx.callback;
    void *arg = uart_env.tx.arg;

    uart_env.tx.callback = NULL;
    uart_env.tx.arg = NULL;
    callback(arg, ESP_BT_HCI_TL_STATUS_OK);
    esp_bt_h4tl_eif_io_event_notify(1);
    return true;
}

/* --- the silent start --- */

/* GPIO5 as a pulled-up input: nothing driven on the frame's RX */
static void tx_quiet(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_UART_TXD_OUT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

/* GPIO5 driven high as a plain GPIO: the idle level, before the UART gets it.
 * The level is set before the direction, so it never drives low. */
static void tx_idle_high(void)
{
    gpio_set_level(GPIO_UART_TXD_OUT, 1);
    gpio_set_direction(GPIO_UART_TXD_OUT, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(GPIO_UART_TXD_OUT, SIG_GPIO_OUT_IDX, false, false);
}

/* UART1 RX-only until the host's first HCI_Reset; returns the bytes dropped */
static uint32_t wait_for_host(void)
{
    uart_config_t cfg = {
        .baud_rate = CONFIG_C3HCI_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_HCI_NUM, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_HCI_NUM, &cfg));
    /* RX only: TX is not routed, GPIO5 stays the quiet input */
    ESP_ERROR_CHECK(uart_set_pin(UART_HCI_NUM, UART_PIN_NO_CHANGE, GPIO_UART_RXD_IN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_set_pull_mode(GPIO_UART_RXD_IN, GPIO_PULLUP_ONLY);

    uint8_t win[sizeof(HCI_RESET)] = {0}, b;
    uint32_t dropped = 0, n = 0;
    ESP_LOGI(TAG, "silent: waiting for the host's HCI_Reset on UART%d RX GPIO%d at %d baud",
             UART_HCI_NUM, GPIO_UART_RXD_IN, CONFIG_C3HCI_UART_BAUDRATE);
    for (;;) {
        if (uart_read_bytes(UART_HCI_NUM, &b, 1, portMAX_DELAY) != 1)
            continue;
        memmove(win, win + 1, sizeof(win) - 1);
        win[sizeof(win) - 1] = b;
        n++;
        if (n >= sizeof(win) && !memcmp(win, HCI_RESET, sizeof(win)))
            break;
        dropped++;
    }
    uart_driver_delete(UART_HCI_NUM);
    return dropped - (sizeof(HCI_RESET) - 1);
}

static void uhci_uart_install(void)
{
    periph_module_enable(PERIPH_UHCI0_MODULE);
    periph_module_reset(PERIPH_UHCI0_MODULE);

    periph_module_enable(PERIPH_UART1_MODULE);
    periph_module_reset(PERIPH_UART1_MODULE);

    uart_config_t uart_config = {
        .baud_rate = CONFIG_C3HCI_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,   /* the frame has no RTS/CTS */
        .rx_flow_ctrl_thresh = UART_RX_THRS,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_HCI_NUM, &uart_config));
    /* the UART is configured and idles high: now it may have the pin */
    ESP_ERROR_CHECK(uart_set_pin(UART_HCI_NUM, GPIO_UART_TXD_OUT, GPIO_UART_RXD_IN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    gdma_channel_alloc_config_t tx_channel_config = {
        .flags.reserve_sibling = 1,
        .direction = GDMA_CHANNEL_DIRECTION_TX,
    };
    ESP_ERROR_CHECK(gdma_new_ahb_channel(&tx_channel_config, &s_tx_channel));
    gdma_channel_alloc_config_t rx_channel_config = {
        .direction = GDMA_CHANNEL_DIRECTION_RX,
        .sibling_chan = s_tx_channel,
    };
    ESP_ERROR_CHECK(gdma_new_ahb_channel(&rx_channel_config, &s_rx_channel));

    gdma_connect(s_tx_channel, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_UHCI, 0));
    gdma_connect(s_rx_channel, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_UHCI, 0));

    gdma_strategy_config_t strategy_config = {
        .auto_update_desc = false,
        .owner_check = false
    };
    gdma_apply_strategy(s_tx_channel, &strategy_config);
    gdma_apply_strategy(s_rx_channel, &strategy_config);

    gdma_rx_event_callbacks_t rx_cbs = {
        .on_recv_eof = hci_uart_tl_rx_eof_callback
    };
    gdma_register_rx_event_callbacks(s_rx_channel, &rx_cbs, NULL);

    gdma_tx_event_callbacks_t tx_cbs = {
        .on_trans_eof = hci_uart_tl_tx_eof_callback
    };
    gdma_register_tx_event_callbacks(s_tx_channel, &tx_cbs, NULL);

    uhci_ll_init(s_uhci_hw);
    uhci_ll_rx_set_eof_mode(s_uhci_hw, UHCI_RX_LEN_EOF);
    s_uhci_hw->escape_conf.val = 0;   /* no software flow control */
    uhci_ll_attach_uart_port(s_uhci_hw, 1);
}

void app_main(void)
{
    esp_err_t ret;

    tx_quiet();

    /* NVS holds the PHY calibration */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    uint32_t dropped = wait_for_host();
    ESP_LOGI(TAG, "host spoke: HCI_Reset (%u bytes of noise dropped before it)", (unsigned) dropped);

    memcpy(s_replay, HCI_RESET, sizeof(HCI_RESET));
    s_replay_pos = 0;
    s_replay_len = sizeof(HCI_RESET);
    s_feed = xSemaphoreCreateBinary();
    xTaskCreate(feeder_task, "hci-replay", 2048, NULL, configMAX_PRIORITIES - 2, NULL);

    tx_idle_high();
    uhci_uart_install();

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.hci_tl_funcs = &s_hci_uart_tl_funcs;

    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "controller init failed: %s", esp_err_to_name(ret));
        return;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "controller enable failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "controller up: H4 on UART%d, TX GPIO%d, RX GPIO%d, %d baud, no flow control",
             UART_HCI_NUM, GPIO_UART_TXD_OUT, GPIO_UART_RXD_IN, CONFIG_C3HCI_UART_BAUDRATE);
}
