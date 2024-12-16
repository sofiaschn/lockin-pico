#define BTSTACK_FILE__ "bluetooth_handler.c"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include <string.h>

#include "btstack.h"

#define RFCOMM_SERVER_CHANNEL 1

static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static uint16_t rfcomm_channel_id;
static bool bluetooth_connected;
static uint8_t  spp_service_buffer[150];
static btstack_packet_callback_registration_t hci_event_callback_registration;
static bool char_available;
static char received_char;

static char lineBuffer[128] = { 0 };

int btstack_main(int argc, const char * argv[]);

void printf_bluetooth(char* string);
char getchar_bluetooth();
void wait_connection();
