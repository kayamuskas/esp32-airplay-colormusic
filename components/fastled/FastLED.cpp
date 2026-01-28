#include "FastLED.h"

#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "fastled_shim";

const CRGB CRGB::White = CRGB(255, 255, 255);

static led_strip_handle_t g_strip;
static CRGB *g_leds = nullptr;
static int g_led_count = 0;
static uint8_t g_brightness = 255;

static CRGB hsv_to_rgb(const CHSV &hsv) {
	uint8_t region = hsv.h / 43;
	uint8_t remainder = (hsv.h - (region * 43)) * 6;

	uint8_t p = (uint8_t)((hsv.v * (255 - hsv.s)) >> 8);
	uint8_t q = (uint8_t)((hsv.v * (255 - ((hsv.s * remainder) >> 8))) >> 8);
	uint8_t t = (uint8_t)((hsv.v * (255 - ((hsv.s * (255 - remainder)) >> 8))) >> 8);

	switch (region) {
	case 0:
		return CRGB(hsv.v, t, p);
	case 1:
		return CRGB(q, hsv.v, p);
	case 2:
		return CRGB(p, hsv.v, t);
	case 3:
		return CRGB(p, q, hsv.v);
	case 4:
		return CRGB(t, p, hsv.v);
	default:
		return CRGB(hsv.v, p, q);
	}
}

CRGB &CRGB::operator=(const CHSV &hsv) {
	CRGB rgb = hsv_to_rgb(hsv);
	r = rgb.r;
	g = rgb.g;
	b = rgb.b;
	return *this;
}

void CRGB::fadeToBlackBy(uint8_t amount) {
	r = (uint8_t)((r * (255 - amount)) / 255);
	g = (uint8_t)((g * (255 - amount)) / 255);
	b = (uint8_t)((b * (255 - amount)) / 255);
}

void CFastLED::addLedsInternal(int pin, CRGB *leds, int count) {
	g_leds = leds;
	g_led_count = count;

	led_strip_config_t strip_config = {};
	strip_config.strip_gpio_num = pin;
	strip_config.max_leds = count;
	strip_config.led_pixel_format = LED_PIXEL_FORMAT_GRB;
	strip_config.led_model = LED_MODEL_WS2812;
	strip_config.flags.invert_out = false;

	led_strip_rmt_config_t rmt_config = {};
	rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
	rmt_config.resolution_hz = 10 * 1000 * 1000;
	rmt_config.mem_block_symbols = 64;
	rmt_config.flags.with_dma = false;

	esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &g_strip);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to init led_strip: %d", err);
	}
}

void CFastLED::setBrightness(uint8_t brightness) {
	g_brightness = brightness;
}

void CFastLED::show() {
	if (!g_strip || !g_leds) return;

	for (int i = 0; i < g_led_count; ++i) {
		uint8_t r = (uint8_t)((g_leds[i].r * g_brightness) / 255);
		uint8_t g = (uint8_t)((g_leds[i].g * g_brightness) / 255);
		uint8_t b = (uint8_t)((g_leds[i].b * g_brightness) / 255);
		led_strip_set_pixel(g_strip, i, r, g, b);
	}
	led_strip_refresh(g_strip);
}

void CFastLED::clear(bool write) {
	if (!g_leds) return;
	for (int i = 0; i < g_led_count; ++i) {
		g_leds[i] = CRGB(0, 0, 0);
	}
	if (write) {
		show();
	}
}

CFastLED FastLED;
