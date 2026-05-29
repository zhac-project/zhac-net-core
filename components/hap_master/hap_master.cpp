// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "hap_master.h"
#include "hap_protocol.h"
#include "hap_protocol_decode.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_rom_sys.h"
#include <cstdlib>
#include <cstring>

static const char* TAG = "hap_master";
static HapFrameCallback    s_cb;
static spi_device_handle_t s_dev;
static volatile TaskHandle_t s_task_handle = nullptr;
static SemaphoreHandle_t   s_spi_mutex;

// Pin assignments — S3 SPI master (TDD Section 2.3)
static constexpr gpio_num_t PIN_MOSI = GPIO_NUM_11;
static constexpr gpio_num_t PIN_MISO = GPIO_NUM_13;
static constexpr gpio_num_t PIN_SCLK = GPIO_NUM_12;
static constexpr gpio_num_t PIN_CS   = GPIO_NUM_10;
static constexpr gpio_num_t PIN_DRDY = GPIO_NUM_8;  // P4→S3 data-ready input

static constexpr size_t HAP_DMA_BUF_SIZE = ((HAP_MAX_FRAME_SIZE + 63) / 64) * 64;
static uint8_t* s_rx_buf = nullptr;
static uint8_t* s_tx_buf = nullptr;

static void IRAM_ATTR drdy_isr(void*) {
    BaseType_t woken = pdFALSE;
    TaskHandle_t h = (TaskHandle_t)s_task_handle;
    if (h) {
        vTaskNotifyGiveFromISR(h, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

void hap_master_set_task_handle(TaskHandle_t h) {
    s_task_handle = h;
}

void hap_master_init() {
    s_rx_buf = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        64, HAP_DMA_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    s_tx_buf = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        64, HAP_DMA_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    configASSERT(s_rx_buf && s_tx_buf);

    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = PIN_MISO,
        .sclk_io_num     = PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .data4_io_num    = -1,
        .data5_io_num    = -1,
        .data6_io_num    = -1,
        .data7_io_num    = -1,
        .data_io_default_level = false,
        .max_transfer_sz = HAP_MAX_FRAME_SIZE,
        .flags      = 0,
        .isr_cpu_id = {},
        .intr_flags = 0,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .command_bits     = 0,
        .address_bits     = 0,
        .dummy_bits       = 0,
        .mode             = 0,
        .clock_source     = SPI_CLK_SRC_DEFAULT,
        .duty_cycle_pos   = 0,
        .cs_ena_pretrans  = 0,
        .cs_ena_posttrans = 0,
        .clock_speed_hz   = 8 * 1000 * 1000,  // 8 MHz on 2-3 cm soldered copper on breadboard. 4 MHz proved stable (28 ms RTTs, no SPI mutex timeouts under burst); pushing higher to claim more headroom. If P4→S3 goes unidirectional-silent (the historical 5 MHz failure mode on the old 20 cm jumper rig), drop back to 4 or 6 MHz. 10 MHz remains the next candidate on a real PCB.
        .input_delay_ns   = 0,
        .sample_point     = {},
        .spics_io_num     = PIN_CS,
        .flags            = 0,
        .queue_size       = 4,
        .pre_cb           = nullptr,
        .post_cb          = nullptr,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_dev));

    gpio_config_t drdy_cfg = {
        .pin_bit_mask = 1ULL << PIN_DRDY,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type     = GPIO_INTR_POSEDGE,
#if SOC_GPIO_SUPPORT_PIN_HYS_FILTER
        .hys_ctrl_mode = GPIO_HYS_SOFT_DISABLE,
#endif
    };
    ESP_ERROR_CHECK(gpio_config(&drdy_cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_DRDY, drdy_isr, nullptr));

    s_spi_mutex = xSemaphoreCreateMutex();
    configASSERT(s_spi_mutex);
    ESP_LOGI(TAG, "hap_master init OK — SPI2 master 8 MHz, DRDY GPIO%d", PIN_DRDY);  // F48: matches clock_speed_hz (was a stale "10 MHz")
}

static void do_two_stage_exchange(const HapFrame& my_frame) {
    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        _METRIC_COUNTER_INC(METRIC_HAP_MUTEX_TIMEOUT, 1);
        ESP_LOGW(TAG, "SPI mutex timeout — driver may be stuck");
        return;
    }

    // ESP-IDF SPI master DMA path requires DMA-capable bufs; stack arrays
    // force a silent bounce per txn. Reuse the 64B-aligned heap bufs.
    // Length must also be 64B-aligned to satisfy slave-side DMA — clock
    // a full HAP_STAGE1_CLOCK_LEN slot, zeroing trailing bytes.
    memset(s_tx_buf, 0, HAP_STAGE1_CLOCK_LEN);
    hap_encode_stage1(my_frame, s_tx_buf);

    spi_transaction_t t1 = {};
    t1.length    = HAP_STAGE1_CLOCK_LEN * 8;
    t1.rxlength  = HAP_STAGE1_CLOCK_LEN * 8;
    t1.tx_buffer = s_tx_buf;
    t1.rx_buffer = s_rx_buf;
    // Same race fix as before stage-2 below: when this exchange follows
    // a previous one (recv → on_receive → ACK send), the slave is in its
    // ~30 µs inter-iter window (free pending → queue_receive → encode →
    // queue_trans). Without this wait, master clocks stage-1 onto an
    // empty slave HW FIFO; ACKs and back-to-back master sends silently
    // disappear. Standalone master sends with a parked slave pay the
    // same 80 µs but it's well below 0.5% overhead per exchange at 1 MHz.
    esp_rom_delay_us(80);
    if (spi_device_transmit(s_dev, &t1) != ESP_OK) {
        xSemaphoreGive(s_spi_mutex);
        ESP_LOGE(TAG, "stage1 spi_device_transmit failed");
        return;
    }

    HapFrame peer{};
    HapDecodeResult dr = hap_decode_stage1(s_rx_buf, peer);
    if (dr != HAP_DECODE_OK) {
        if (dr == HAP_DECODE_BAD_MAGIC) {
            _METRIC_COUNTER_INC(METRIC_HAP_BAD_MAGIC, 1);
        } else if (dr == HAP_DECODE_BAD_VERSION) {
            _METRIC_COUNTER_INC(METRIC_HAP_VERSION_MISMATCH, 1);
            ESP_LOGE(TAG, "RX HAP_VERSION mismatch (got 0x%02x, expected 0x%02x) — "
                          "one chip needs reflashing",
                     s_rx_buf[3], HAP_VERSION);
        } else if (dr == HAP_DECODE_BAD_HDR_CRC) {
            _METRIC_COUNTER_INC(METRIC_HAP_HDR_CRC_ERRORS, 1);
        } else if (dr == HAP_DECODE_OVERFLOW) {
            ESP_LOGW(TAG, "stage1 decode overflow");
        }
        peer.payload_len = 0;
    }

    size_t my_s2_bytes   = (my_frame.payload_len > 0) ? (size_t)my_frame.payload_len + 2 : 0;
    size_t peer_s2_bytes = (peer.payload_len > 0)     ? (size_t)peer.payload_len + 2     : 0;
    size_t s2_max        = (my_s2_bytes > peer_s2_bytes) ? my_s2_bytes : peer_s2_bytes;
    size_t s2_len        = (s2_max + (HAP_DMA_ALIGN - 1)) & ~(HAP_DMA_ALIGN - 1);

    bool dispatched = false;
    HapFrame dispatched_peer{};
    // Use a static dispatch buffer instead of malloc/free per frame.
    // Under high traffic (BULK_STATE_UPDATE bursts after a DEVICE_JOIN),
    // malloc churn fragmented internal RAM and tripped ESP-IDF SPI's
    // priv_trans cleanup on later transmits — crash inside
    // uninstall_priv_desc with a garbage memcpy length. The dispatch
    // buffer is exclusively owned by whichever do_two_stage_exchange
    // call holds s_spi_mutex; the callback runs synchronously between
    // mutex release and function return, after which the buffer is free
    // for the next exchange.
    static uint8_t s_dispatch_buf[HAP_MAX_PAYLOAD];

    if (s2_len > 0) {
        memset(s_tx_buf, 0, s2_len);
        memset(s_rx_buf, 0, s2_len);
        if (my_frame.payload_len > 0) {
            hap_encode_stage2(my_frame, s_tx_buf, HAP_DMA_BUF_SIZE);
        }

        // Give the slave a moment to queue its stage-2 descriptor.
        // Between stage-1 done and stage-2 queue_trans, the slave does
        // ~25-30 µs of CPU work (decode peer, memset/memcpy stage-2 bufs,
        // spi_slave_queue_trans). On a faster master + slower slave the
        // master can clock stage-2 before the slave's HW FIFO has the
        // descriptor — slave MISO outputs idle (0xFF), master CRC fails,
        // frame dropped silently. P4→S3 saw effectively 0% delivery
        // because heartbeats and DEVICE_LIST_RSP all carry stage-2
        // payload. 80 µs spin-wait costs <3% per exchange at 1 MHz and
        // covers the worst observed slave-prep window.
        esp_rom_delay_us(80);

        spi_transaction_t t2 = {};
        t2.length    = s2_len * 8;
        t2.rxlength  = s2_len * 8;
        t2.tx_buffer = s_tx_buf;
        t2.rx_buffer = s_rx_buf;
        if (spi_device_transmit(s_dev, &t2) != ESP_OK) {
            xSemaphoreGive(s_spi_mutex);
            ESP_LOGE(TAG, "stage2 spi_device_transmit failed");
            return;
        }

        if (peer.payload_len > 0) {
            if (hap_verify_stage2(s_rx_buf, peer.payload_len)) {
                if (peer.payload_len <= sizeof(s_dispatch_buf)) {
                    memcpy(s_dispatch_buf, s_rx_buf, peer.payload_len);
                    peer.payload    = s_dispatch_buf;
                    dispatched_peer = peer;
                    dispatched      = true;
                } else {
                    ESP_LOGW(TAG, "stage2 payload %u > dispatch buf %u — drop",
                             peer.payload_len, (unsigned)sizeof(s_dispatch_buf));
                }
            } else {
                _METRIC_COUNTER_INC(METRIC_HAP_CRC_ERRORS, 1);
            }
        }
    }

    if (!dispatched && dr == HAP_DECODE_OK && peer.payload_len == 0 &&
        peer.type != static_cast<HapMsgType>(0)) {
        dispatched_peer = peer;
        dispatched      = true;
    }

    xSemaphoreGive(s_spi_mutex);

    if (my_frame.type != static_cast<HapMsgType>(0)) {
        _METRIC_COUNTER_INC(METRIC_HAP_TX_FRAMES_TOTAL, 1);
    }

    if (dispatched && s_cb) s_cb(dispatched_peer);
}

void hap_master_send(const HapFrame& frame) {
    do_two_stage_exchange(frame);
}

void hap_master_recv() {
    HapFrame empty{};
    empty.type = static_cast<HapMsgType>(0);
    do_two_stage_exchange(empty);
}

void hap_master_set_callback(HapFrameCallback cb) {
    s_cb = std::move(cb);
}
