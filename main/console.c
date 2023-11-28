#include "console.h"
#include <fcntl.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_console.h"
#include "esp_vfs_dev.h"
#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "freertos/portmacro.h"
#include "freertos/projdefs.h"
#include "linenoise/linenoise.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_vfs_usb_serial_jtag.h"
#include "driver/usb_serial_jtag.h"
#include "cmd_system.h"
#include "cmd_can.h"
#include "cmd_utils.h"
#include "can.h"
#include "fs.h"
#include "xvprintf.h"

#if CONFIG_LOG_COLORS
static const bool use_colors = true;
#else
static const bool use_colors = true;
#endif

static const char* TAG = "console task";
char prompt_buf[60];
esp_console_config_t console_config;
struct linenoiseState ls;

SemaphoreHandle_t console_taken_sem;

static void update_prompt() {
    char text[45];
    int text_len;
    static char* prompt_color;
    text[0] = '\0';
    switch (curr_can_state.state) {
        case CAN_NOT_INSTALLED:
            strcat(text, "not installed");
            prompt_color = LOG_COLOR(LOG_COLOR_RED);
            break;
        case CAN_STOPPED:
            strcat(text, "stopped");
            prompt_color = LOG_COLOR(LOG_COLOR_RED);
            break;
        case CAN_ERROR_ACTIVE:
            strcat(text, "error active");
            prompt_color = LOG_COLOR(LOG_COLOR_GREEN);
            break;
        case CAN_ERROR_PASSIVE:
            strcat(text, "error passive");
            prompt_color = LOG_COLOR(LOG_COLOR_BROWN);
            break;
        case CAN_BUF_OFF:
            strcat(text, "bus off");
            prompt_color = LOG_COLOR(LOG_COLOR_RED);
            break;
        case CAN_RECOVERING:
            strcat(text, "recovering");
            prompt_color = LOG_COLOR(LOG_COLOR_RED);
            break;
    }
    if (curr_can_state.state != CAN_NOT_INSTALLED) {
        char tec_rec[25];
        snprintf(tec_rec, 24, " [TEC: %" PRIu32 "][REC: %" PRIu32 "]", curr_can_state.tx_error_counter, curr_can_state.rx_error_counter);
        strcat(text, tec_rec);
    }
    strcat(text, " > ");
    prompt_buf[0] = '\0';
    if (use_colors) {
        strcat(prompt_buf, prompt_color);
        strcat(prompt_buf, text);
        strcat(prompt_buf, LOG_RESET_COLOR);
    } else {
        strcat(prompt_buf, text);
    }
    text_len = strlen(text);
    ls.prompt = prompt_buf;
    ls.plen = text_len;
}

void console_task_tx(void* arg) {
    static const TickType_t prompt_timeout = pdMS_TO_TICKS(200);
    // static const TickType_t prompt_timeout = portMAX_DELAY;
    const int fd = fileno(stdout);
    char *msg_to_print;
    size_t msg_to_print_size;
    while(1) {
        msg_to_print = (char *)xRingbufferReceive(can_messages, &msg_to_print_size, prompt_timeout);
        update_prompt();
        xSemaphoreTake(console_taken_sem, portMAX_DELAY);
        xSemaphoreTake(stdout_taken_sem, portMAX_DELAY);
        linenoiseHide(&ls);
        if (msg_to_print != NULL) {
            // if zero-length string - just refresh prompt. used for updating prompt
            if(msg_to_print[0] != '\0') {
                write(fd, msg_to_print, msg_to_print_size);
                flushWrite();
            }
            vRingbufferReturnItem(can_messages, (void *) msg_to_print);
        }
        linenoiseShow(&ls);
        xSemaphoreGive(stdout_taken_sem);
        xSemaphoreGive(console_taken_sem); 
    }
}

void console_task_interactive(void* arg) {
    console_taken_sem = xSemaphoreCreateMutex();
    stdout_taken_sem = xSemaphoreCreateMutex();
    char *buf = calloc(1, console_config.max_cmdline_length);
    char *line;
    /* Figure out if the terminal supports escape sequences */
    printf("Testing your console...\n");
    int probe_status = linenoiseProbe();
    // int probe_status = 1;
    if (probe_status) { /* zero indicates success */
        printf("\n"
               "Your terminal application does not support escape sequences.\n"
               "Line editing and history features are disabled.\n"
               "On Windows, try using Putty instead.\n"
               "Your terminal is in Dumb Mode from now! \n");
        linenoiseSetDumbMode(1);
    }
    printf("\n"
       "Type 'help' to get the list of commands.\n"
       "Use UP/DOWN arrows to navigate through command history.\n"
       "Press TAB when typing command name to auto-complete.\n"
       "Ctrl+C will terminate the console environment.\n");
    ls.buflen = console_config.max_cmdline_length;
    ls.buf = buf;
    update_prompt();
    linenoiseEditStart(&ls);
    xTaskCreate(console_task_tx, "console tsk tx", 5000, NULL, CONFIG_CONSOLE_TX_PRIORITY, NULL);
    esp_log_set_vprintf(&vxprintf);
    while (true) {
        line = linenoiseEditFeed(&ls);
        if (line == linenoiseEditMore) continue;
        xSemaphoreTake(console_taken_sem, portMAX_DELAY);
        linenoiseEditStop(&ls);
        if (line == NULL) { /* Break on EOF or error */
            ESP_LOGE(TAG, "Ctrl+C???");
            break;
        }

        /* Add the command to the history if not empty*/
        if ((strlen(line) > 0) && (line[0] != '^')) {
            linenoiseHistoryAdd(line);
            /* Save command history to filesystem */
            linenoiseHistorySave(HISTORY_PATH);
        }

        int ret;
        esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            printf("Unrecognized command\n");
        } else if (err == ESP_ERR_INVALID_ARG) {
            // command was empty
        } else if (err == ESP_OK && ret != ESP_OK) {
            printf("Command returned non-zero error code: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        } else if (err != ESP_OK) {
            printf("Internal error: %s\n", esp_err_to_name(err));
        }
        /* linenoise allocates line buffer on the heap, so need to free it */
        linenoiseFree(line);
        linenoiseEditStart(&ls);
        xSemaphoreGive(console_taken_sem);
    }

    ESP_LOGE(TAG, "Restarting...");
    esp_console_deinit();
    esp_restart();
    while (1) {
        vTaskDelay(100);
    }
}

void initialize_console(void) {
    /* Disable buffering on stdin */
    setvbuf(stdin, NULL, _IONBF, 0);
    /* Minicom, screen, idf_monitor send CR when ENTER key is pressed */
    esp_vfs_dev_usb_serial_jtag_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    /* Move the caret to the beginning of the next line on '\n' */
    esp_vfs_dev_usb_serial_jtag_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    /* Enable non-blocking mode on stdin and stdout */
    fcntl(fileno(stdout), F_SETFL, 0);
    fcntl(fileno(stdin), F_SETFL, 0);
    usb_serial_jtag_driver_config_t usb_serial_jtag_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    /* Install USB-SERIAL-JTAG driver for interrupt-driven reads and writes */
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_serial_jtag_config));
    /* Tell vfs to use usb-serial-jtag driver */
    esp_vfs_usb_serial_jtag_use_driver();
    console_config.max_cmdline_args = 8;
    console_config.max_cmdline_length = 256;
    if (use_colors) console_config.hint_color = atoi(LOG_COLOR_CYAN);
    ESP_ERROR_CHECK(esp_console_init(&console_config));
    // linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(&esp_console_get_completion);
    linenoiseSetHintsCallback((linenoiseHintsCallback*) &esp_console_get_hint);
    linenoiseHistorySetMaxLen(30);
    linenoiseSetMaxLineLen(console_config.max_cmdline_length);
    linenoiseHistoryLoad(HISTORY_PATH);
    /* Register commands */
    esp_console_register_help_command();
    register_system();
    register_can_commands();
    register_utils_commands();
}
