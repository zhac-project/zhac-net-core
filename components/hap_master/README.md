# hap_master — S3-side SPI Master for HAP

The ESP32-S3 half of the SPI link to the ESP32-P4. Owns SPI2 in master mode,
the DRDY interrupt, and the only RX/TX byte buffers on the S3 side. Every
HAP frame that the S3 emits — REST request → P4, OTA chunk, MQTT publish
forwarded back from P4, etc. — passes through this driver.

This component is **S3-only**. The CMakeLists.txt early-returns an empty
component on `esp32p4` to avoid pulling S3 GPIOs into the P4 build. The P4
counterpart is `hap_slave`.

## Where it sits

```
[S3 task: task_hap (or any caller)]
        │
        ▼
hap_session_send  ──────► HapSendFn callback ────►  hap_master_send  ◀── this component
                                                        │
                                                        │ encode + SPI2 transaction
                                                        ▼
                                               ┌──── SPI2 (HSPI) ────┐
                                               │  MOSI / MISO / SCLK │
                                               │  CS / DRDY (input)  │
                                               └─────────────────────┘
                                                        │
[P4 hap_slave] ─────────────────────────────────────────┘
        │
        │ on TX-ready: pulses DRDY high
        ▼
[S3 GPIO8 ISR drdy_isr] ── xTaskNotifyFromISR (s_task_handle, eNoAction)
        │
        ▼
TaskNotifyTake in caller (typically task_hap waiting for a reply)
```

## Dependencies (CMakeLists.txt)

`REQUIRES hap_protocol esp_driver_spi esp_driver_gpio freertos`. Built only
when `IDF_TARGET == esp32s3`.

## Hardware

### Pin assignment (S3 master)

| Signal | GPIO   | Direction | Notes                                  |
|--------|--------|-----------|----------------------------------------|
| MOSI   | GPIO11 | S3 → P4   | `hap_master.cpp:19` (`PIN_MOSI`)       |
| MISO   | GPIO13 | P4 → S3   | `hap_master.cpp:20` (`PIN_MISO`)       |
| SCLK   | GPIO12 | S3 → P4   | `hap_master.cpp:21` (`PIN_SCLK`)       |
| CS     | GPIO10 | S3 → P4   | `hap_master.cpp:22` (`PIN_CS`)         |
| DRDY   | GPIO8  | P4 → S3   | input, posedge ISR (`drdy_isr`)        |

These are compile-time constants in `hap_master.cpp:19-23`. They line up with
the P4-side pins documented in `components/hap_slave/README.md` (P4 uses a
different GPIO bank — GPIO18..22 — wired to the S3 pins above).

### SPI configuration

| Setting     | Value                  | Source                        |
|-------------|------------------------|-------------------------------|
| Bus         | `SPI2_HOST` (HSPI)     | `spi_bus_initialize`          |
| Clock       | 8 MHz                  | `spi_device_interface_config` |
| Mode        | 0 (CPOL=0, CPHA=0)     | default                       |
| DMA channel | `SPI_DMA_CH_AUTO`      | always-DMA                    |
| Buffer cap  | `HAP_DMA_BUF_SIZE`     | matches `HAP_MAX_FRAME_SIZE`  |

8 MHz is the current operating speed on a 2–3 cm soldered-copper
breadboard rig (4 MHz proved stable, 8 MHz remains stable; 10 MHz is
the next candidate on a real PCB). 20 MHz does not survive the
inter-board jumper wiring. Documented in `hap_master.cpp:81`.

## Public API (`include/hap_master.h`)

```cpp
using HapFrameCallback = std::function<void(const HapFrame&)>;

// One-time init: claim SPI2, configure GPIOs, install drdy_isr, create
// the SPI mutex. Idempotent — safe to call once at boot only. Logs
//   I hap_master init OK — SPI2 master 8 MHz, DRDY GPIO8
void hap_master_init();

// Encode `frame` and clock it out. Acquires s_spi_mutex (blocking,
// portMAX_DELAY) so concurrent callers serialize. The transaction is
// always HAP_DMA_BUF_SIZE bytes wide — the slave is configured the
// same way, so we read the slave's TX in the same transaction.
// Decoded RX (if any) is dispatched to the callback installed via
// hap_master_set_callback, on the caller's task.
void hap_master_send(const HapFrame& frame);

// Pull-mode read: starts an empty (zeroed) TX transaction so the slave
// can clock out queued data. Used by task_hap when DRDY edge missed
// or after a reset. Same dispatch path as hap_master_send.
void hap_master_recv();

// Install the RX dispatcher. Typically:
//   hap_master_set_callback([](const HapFrame& f){ hap_session_on_receive(f); });
// Called from hap_master_send / hap_master_recv on the caller's task,
// after the SPI mutex has been released.
void hap_master_set_callback(HapFrameCallback cb);

// Register the FreeRTOS task that should be unblocked from drdy_isr
// via xTaskNotifyFromISR. Set this once, from the task that will read
// DRDY notifications (typically task_hap). Without this, DRDY edges
// have no effect — recv must be polled.
void hap_master_set_task_handle(TaskHandle_t h);
```

## Threading and concurrency

- **`s_spi_mutex`** (one mutex) serializes every SPI transaction. Both
  `hap_master_send` and `hap_master_recv` take it. Held across the
  encode + `spi_device_transmit` + decode + callback dispatch.
- **`drdy_isr`** is `IRAM_ATTR`. It does exactly one thing: notify
  `s_task_handle` (set via `hap_master_set_task_handle`). No allocation,
  no logging.
- **Callback context**: `HapFrameCallback` runs on the calling task —
  always after `s_spi_mutex` is released (so the callback can safely call
  `hap_master_send` again, e.g. for chained protocols).

Caller responsibility: only one task should call `hap_master_recv`. Multiple
concurrent `hap_master_send` callers are fine — they serialize on the mutex.

## Error and failure modes

| Log line                                                | Meaning                                                  |
|---------------------------------------------------------|----------------------------------------------------------|
| `E hap_encode overflow`                                 | Caller's frame > `HAP_DMA_BUF_SIZE`. Frame dropped, mutex released. |
| `E spi_device_transmit: ESP_ERR_…`                      | SPI driver returned an error (rare). Frame dropped.       |
| `W RX copy malloc fail — drop`                          | Out of heap when copying decoded payload into a private buffer for the callback. Frame dropped silently after this. |
| `W RX CRC mismatch (signal integrity?)`                 | One frame lost. Counter, not metric. Stream resyncs via `hap_decode_stream`. |
| `W RX truncated frame`                                  | DMA returned fewer bytes than the LEN field claims.       |
| `E RX HAP_VERSION mismatch (got 0x.., expected 0x..) — drop` | Mixed-firmware S3 / P4. Reflash both. |
| `W recv bad magic 0xXXYY`                               | Pull-mode read hit garbage; expected when slave has no TX. |

There is no metrics counter for RX drops — diagnose from `idf.py monitor`.

## Integration example

Wired up alongside `hap_session`. The session layer is what callers should
talk to; `hap_master_send` is only invoked indirectly through `HapSendFn`.

```cpp
#include "hap_master.h"
#include "hap_session.h"

void hap_init() {
    hap_master_init();
    hap_master_set_task_handle(task_hap_handle);              // for DRDY wake
    hap_master_set_callback([](const HapFrame& f) {
        hap_session_on_receive(f);                            // upstream into session
    });

    HapSessionCfg cfg{};
    cfg.send = [](const HapFrame& f) { hap_master_send(f); }; // session → us
    cfg.on_frame     = on_frame;
    cfg.on_sync      = on_sync;
    cfg.on_link_dead = on_link_dead;
    hap_session_init(cfg);
}

// task_hap loop body
for (;;) {
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10))) {
        hap_master_recv();    // DRDY fired → drain whatever P4 has for us
    }
    hap_session_tick();        // 10 ms cadence for retransmit timer
}
```

## Cross-references

- `components/hap_slave/README.md` — P4-side counterpart and pin map
- `components/hap_session/README.md` — sliding-window layer above this driver
- `components/hap_protocol/README.md` — wire format
- `docs/FINDINGS.md` — SPI signal-integrity notes (8 MHz operating point rationale)

## Recent changes

- The transaction is now **always** sized to `HAP_DMA_BUF_SIZE` regardless
  of the encoded frame length. The P4 slave does the same, so we read its
  outbound queue in the same SPI transaction the master uses to send. This
  removes the previous round-trip race where the master would re-transact
  to read the slave's response.
- A version-mismatch log line was added so a stale binary on either side
  fails loudly instead of silently CRC-erroring.
