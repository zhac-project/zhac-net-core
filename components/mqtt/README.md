# mqtt — ESP-MQTT Client Library (upstream Espressif)

Upstream Espressif ESP-MQTT client library providing full MQTT 3.1.1/5.0 client with TLS and WebSocket support.

## Overview

This component is the official Espressif `esp-mqtt` library, providing a complete MQTT client implementation with support for MQTT 3.1.1 and 5.0 protocols, TLS encryption, WebSocket transport, and last-will-testament (LWT) messages. Used by `mqtt_gw` on the S3 side for broker communication.

## Dependencies

- `esp_event`
- `tcp_transport`
- `esp_timer` (private)
- `http_parser` (private)
- `esp_hw_support` (private)
- `heap` (private)

## Features

- MQTT 3.1.1 and 5.0 protocol support
- TLS support (with certificate verification)
- WebSocket transport (ws:// and wss://)
- Last Will and Testament (LWT)
- QoS 0, 1, 2 support
- Session persistence (clean session flag)
- Automatic reconnection with exponential backoff
- Outbox for message reliability

## Key Types

### Client Handle

```c
typedef struct esp_mqtt_client* esp_mqtt_client_handle_t;
```

### Configuration

```c
typedef struct {
    struct {
        struct {
            const char* uri;              // Broker URI (e.g., "mqtt://host:1883")
            const char* host;             // Broker hostname
            int port;                      // Broker port
            const char* username;         // Authentication username
            const char* password;         // Authentication password
            const char* client_id;        // Client identifier
        } broker;
        struct {
            bool disable_clean_session;   // Use persistent session
            int keepalive;                // Keepalive interval (seconds)
        } session;
        struct {
            const char* cert_pem;         // Server certificate (PEM)
            const char* clientcert_pem;   // Client certificate (PEM)
            const char* clientkey_pem;    // Client private key (PEM)
        } credentials;
        struct {
            const char* alpn_protos;      // ALPN protocols for TLS
        } network;
        struct {
            int out_buffer_size;          // Outbox buffer size
        } buffer;
    } broker;
    struct {
        const char* topic;               // LWT topic
        const char* msg;                 // LWT message
        int qos;                          // LWT QoS
        int retain;                       // LWT retain flag
    } lwt;
} esp_mqtt_client_config_t;
```

### Event Types

```c
typedef enum {
    MQTT_EVENT_BEFORE_CONNECT,       // Client initialized, before connect
    MQTT_EVENT_CONNECTED,            // Successfully connected
    MQTT_EVENT_DISCONNECTED,         // Disconnected from broker
    MQTT_EVENT_SUBSCRIBED,           // Subscription acknowledged
    MQTT_EVENT_UNSUBSCRIBED,         // Unsubscription acknowledged
    MQTT_EVENT_PUBLISHED,            // Publish acknowledged
    MQTT_EVENT_DATA,                 // Incoming message data
    MQTT_EVENT_ERROR,                // Error occurred
} esp_mqtt_event_id_t;
```

### Event Data

```c
typedef struct {
    esp_mqtt_event_id_t event_id;    // Event type
    esp_mqtt_client_handle_t client; // Client handle
    char* data;                      // Message data
    char* topic;                     // Message topic
    int total_data_len;              // Total message length (for fragmented)
    int current_data_offset;         // Offset in fragmented message
    int msg_id;                      // Message ID (for QoS 1/2)
    int msg_outbox_size;             // Outbox size
} esp_mqtt_event_t;
```

## Public API

### Client Lifecycle

```c
// Initialize MQTT client with configuration
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t* config);

// Start the client (connect to broker)
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client);

// Stop the client (disconnect gracefully)
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client);

// Destroy the client and free resources
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client);
```

### Publishing

```c
// Publish a message
esp_err_t esp_mqtt_client_publish(esp_mqtt_client_handle_t client,
                                   const char* topic,
                                   const char* data,
                                   int len,
                                   int qos,
                                   int retain,
                                   int* msg_id);
```

### Subscriptions

```c
// Subscribe to a topic
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client,
                               const char* topic,
                               int qos);

// Unsubscribe from a topic
esp_err_t esp_mqtt_client_unsubscribe(esp_mqtt_client_handle_t client,
                                       const char* topic);
```

### Connection Management

```c
// Reconnect to broker
esp_err_t esp_mqtt_client_reconnect(esp_mqtt_client_handle_t client);

// Disconnect from broker
esp_err_t esp_mqtt_client_disconnect(esp_mqtt_client_handle_t client);

// Set broker URI (alternative to init config)
esp_err_t esp_mqtt_client_set_uri(esp_mqtt_client_handle_t client,
                                   const char* uri);
```

### Event Handling

```c
typedef esp_err_t (*event_event_cb_t)(void* handler_args, esp_event_base_t base,
                                       int32_t event_id, void* event_data);

// Register event handler
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client,
                                          esp_mqtt_event_id_t event,
                                          event_event_cb_t event_handler,
                                          void* handler_arg);

// Unregister event handler
esp_err_t esp_mqtt_client_unregister_event(esp_mqtt_client_handle_t client,
                                            esp_mqtt_event_id_t event,
                                            event_event_cb_t event_handler);
```

## Usage Examples

### Basic MQTT Client

```c
#include "mqtt_client.h"

static esp_mqtt_client_handle_t mqtt_client;

static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                                int32_t event_id, void* event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            printf("MQTT connected\n");
            esp_mqtt_client_subscribe(mqtt_client, "zhac/+/+/set", 1);
            break;

        case MQTT_EVENT_DISCONNECTED:
            printf("MQTT disconnected\n");
            break;

        case MQTT_EVENT_DATA:
            printf("Message on %s: %.*s\n", event->topic, event->data_len, event->data);
            break;

        case MQTT_EVENT_ERROR:
            printf("MQTT error\n");
            break;
    }
}

void app_main(void) {
    esp_mqtt_client_config_t config = {
        .broker = {
            .address = {
                .uri = "mqtt://broker.local:1883",
            },
        },
    };

    mqtt_client = esp_mqtt_client_init(&config);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}
```

### Publishing with QoS

```c
int msg_id;
esp_mqtt_client_publish(mqtt_client,
                        "zhac/devices/0x00124B0012345678/attributes/state",
                        "on",
                        2,          // Length
                        1,          // QoS 1 (at least once)
                        0,          // Not retained
                        &msg_id);
```

### TLS Connection

```c
esp_mqtt_client_config_t config = {
    .broker = {
        .address = {
            .uri = "mqtts://broker.local:8883",
        },
        .verification = {
            .certificate = server_cert_pem,  // PEM-encoded CA certificate
        },
    },
};
```

### Last Will and Testament

```c
esp_mqtt_client_config_t config = {
    .session = {
        .last_will = {
            .topic = "zhac/devices/bridge/availability",
            .msg = "offline",
            .qos = 1,
            .retain = true,
        },
    },
};
```

## Transport Protocols

| URI Scheme | Protocol | Port Default |
|------------|----------|--------------|
| `mqtt://` | TCP | 1883 |
| `mqtts://` | TCP + TLS | 8883 |
| `ws://` | WebSocket | 80 |
| `wss://` | WebSocket + TLS | 443 |

## QoS Levels

| QoS | Description | Guarantee |
|-----|-------------|-----------|
| 0 | At most once | May lose messages |
| 1 | At least once | May duplicate messages |
| 2 | Exactly once | No loss, no duplicates |

## Implementation Details

- **Reconnection**: Exponential backoff with configurable max delay
- **Outbox**: Stores unacknowledged QoS 1/2 messages for redelivery
- **Clean session**: When enabled, broker discards session on disconnect
- **Keepalive**: PINGREQ/PINGRESP to detect connection loss
- **Event-driven**: All operations signaled via event handler

## Related Components

- **mqtt_gw** — Wraps esp-mqtt with simplified API
- **S3 firmware** (in `firmware/s3_core/`) — Uses mqtt_gw for MQTT broker communication
