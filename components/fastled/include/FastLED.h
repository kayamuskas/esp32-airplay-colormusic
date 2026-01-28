#pragma once

#include <stdint.h>

enum EOrder {
	GRB = 0,
};

enum EChipset {
	WS2812B = 0,
};

struct CHSV {
	uint8_t h;
	uint8_t s;
	uint8_t v;

	CHSV(uint8_t hue, uint8_t sat, uint8_t val) : h(hue), s(sat), v(val) {}
};

struct CRGB {
	uint8_t r;
	uint8_t g;
	uint8_t b;

	static const CRGB White;

	CRGB() : r(0), g(0), b(0) {}
	CRGB(uint8_t red, uint8_t green, uint8_t blue) : r(red), g(green), b(blue) {}

	CRGB &operator=(const CHSV &hsv);
	void fadeToBlackBy(uint8_t amount);
};

class CFastLED {
public:
	template<EChipset CHIPSET, uint8_t PIN, EOrder ORDER>
	void addLeds(CRGB *leds, int count) {
		addLedsInternal(PIN, leds, count);
	}

	void setBrightness(uint8_t brightness);
	void show();
	void clear(bool write);

private:
	void addLedsInternal(int pin, CRGB *leds, int count);
};

extern CFastLED FastLED;
