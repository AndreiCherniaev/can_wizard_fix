#include "xvprintf.h"
#include <stdarg.h>
#include <stdio.h>
#include "stdbool.h"
#include "esp_log.h"

RingbufHandle_t can_messages;


void init_tx_ringbuf() {
    can_messages = xRingbufferCreate(1028, RINGBUF_TYPE_NOSPLIT);
    esp_log_set_vprintf(&vxprintf);
}

// This function will be called by the ESP log library every time ESP_LOG needs to be performed.
//      @important Do NOT use the ESP_LOG* macro's in this function ELSE recursive loop and stack overflow! So use printf() instead for debug messages.
int vxprintf(const char *fmt, va_list args) {
    char msg_to_send[300];
    size_t str_len;
    str_len = vsnprintf(msg_to_send, 299, fmt, args);
    xRingbufferSend(can_messages, msg_to_send, str_len + 1, pdMS_TO_TICKS(100));
    return str_len;
}

int xprintf(const char *fmt, ...) {
    va_list(args);
    va_start(args, fmt);
    return vxprintf(fmt, args);
}
