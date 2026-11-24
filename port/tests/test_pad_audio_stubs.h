/* Pad fixtures have no controller speaker. Let SDL handle rumble and LEDs. */
int runtime_audio_bluetooth_rumble(uint32_t id, uint16_t low, uint16_t high, uint32_t ms) {
    (void)id; (void)low; (void)high; (void)ms;
    return 0;
}

int runtime_audio_bluetooth_led(uint32_t id, const uint8_t rgb[3]) {
    (void)id; (void)rgb;
    return 0;
}
