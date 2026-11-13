// MQTT client (docs/FSD.md §5).
//
// Topics live under sensact/<device-id>/:
//   status            "online" / "offline" (retained; "offline" is the last will)
//   info              JSON: firmware, IP, RSSI, uptime, free heap (retained, every 60 s)
//   actor/<aid>/set   commands, routed to the handler set with sa_mqtt_set_command_handler()
// With discovery on, Home-Assistant-style config messages make the device appear in
// openHAB (MQTT binding) or Home Assistant. For now these cover the diagnostics; sensors
// and actors follow.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SA_MQTT_BASE "sensact"

// Builds "sensact/<id>/<sub>". Returns false if it does not fit.
bool sa_mqtt_topic(char *buf, size_t len, const char *device_id, const char *sub);

// Extracts <aid> from "sensact/<id>/actor/<aid>/set". Returns false if the topic does not
// match or the ID is not a valid short ID.
bool sa_mqtt_parse_actor_set(const char *topic, size_t topic_len, const char *device_id,
                             char *aid, size_t aid_len);

typedef struct {
    bool configured;          // broker set in the settings
    bool connected;
    char broker[80];          // "host:port"
    uint32_t connects;        // successful connections since boot
    uint32_t published;       // messages handed to the client since boot
    uint32_t disconnected_s;  // seconds since the connection was lost (0 when connected)
    char last_error[64];
} sa_mqtt_status_t;

// Starts the client if a broker is configured. Needs the network stack to be running.
esp_err_t sa_mqtt_start(void);

void sa_mqtt_get_status(sa_mqtt_status_t *out);

// Publishes to sensact/<id>/<sub>. Non-blocking; dropped if not connected.
bool sa_mqtt_publish(const char *sub, const char *payload, bool retain);

// Called for each command on sensact/<id>/actor/<aid>/set (MQTT task context).
// Return false if the actor is unknown or inactive (the command is then logged).
typedef bool (*sa_mqtt_command_cb_t)(const char *aid, const char *payload);
void sa_mqtt_set_command_handler(sa_mqtt_command_cb_t cb);

#ifdef __cplusplus
}
#endif
