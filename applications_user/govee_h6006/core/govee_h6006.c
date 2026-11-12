#include "govee_h6006.h"
#include <string.h>
#include <stdint.h>

uint8_t govee_calculate_checksum(const uint8_t* packet) {
    uint8_t checksum = 0;
    for(int i = 0; i < GOVEE_PACKET_SIZE - 1; i++) {
        checksum ^= packet[i];
    }
    return checksum;
}

bool govee_validate_packet(const uint8_t* packet) {
    return packet[GOVEE_PACKET_SIZE - 1] == govee_calculate_checksum(packet);
}

void govee_h6006_create_power_packet(uint8_t* packet, bool on) {
    memset(packet, 0, GOVEE_PACKET_SIZE);
    packet[0] = 0x33;
    packet[1] = GOVEE_CMD_POWER;
    packet[2] = on ? 0x01 : 0x00;
    packet[19] = govee_calculate_checksum(packet);
}

void govee_h6006_create_brightness_packet(uint8_t* packet, uint8_t brightness) {
    memset(packet, 0, GOVEE_PACKET_SIZE);
    packet[0] = 0x33;
    packet[1] = GOVEE_CMD_BRIGHTNESS;
    // Clamp to verified H6006 0-100 scale
    packet[2] = brightness > 100 ? 100 : brightness;
    packet[19] = govee_calculate_checksum(packet);
}

void govee_h6006_create_color_packet(uint8_t* packet, uint8_t r, uint8_t g, uint8_t b) {
    memset(packet, 0, GOVEE_PACKET_SIZE);
    packet[0] = 0x33;
    packet[1] = GOVEE_CMD_COLOR;
    packet[2] = 0x0D; // RGB mode (verified against H6006 hardware)
    packet[3] = r;
    packet[4] = g;
    packet[5] = b;
    packet[19] = govee_calculate_checksum(packet);
}

void govee_h6006_create_white_packet(uint8_t* packet, uint16_t temperature) {
    // Verified H6006 CT range: 2700-6500K
    if(temperature < 2700) temperature = 2700;
    if(temperature > 6500) temperature = 6500;

    memset(packet, 0, GOVEE_PACKET_SIZE);
    packet[0] = 0x33;
    packet[1] = GOVEE_CMD_COLOR;
    packet[2] = 0x0D; // Same mode byte as RGB (verified)
    packet[3] = 0x00; // Zero RGB
    packet[4] = 0x00;
    packet[5] = 0x00;
    packet[6] = (temperature >> 8) & 0xFF; // Big-endian kelvin
    packet[7] = temperature & 0xFF;
    packet[19] = govee_calculate_checksum(packet);
}

void govee_h6006_create_keepalive_packet(uint8_t* packet) {
    memset(packet, 0, GOVEE_PACKET_SIZE);
    packet[0] = 0xAA;
    packet[1] = 0x01;
    packet[2] = 0x00;
    packet[19] = govee_calculate_checksum(packet);
}
