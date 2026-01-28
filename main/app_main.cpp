#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs_flash.h"

extern "C" {
#include "raop.h"
}

#include <FastLED.h>

static const char *TAG = "airplay_viz";

// ------------------- VU CONFIG (from Arduino version) -------------------
// Update cadence for VU processing (ms).
static constexpr int kMainLoopMs = 10;
// Non-linear response curve (higher = more punchy).
static constexpr float kExp = 1.4f;
// Background hue when signal is low.
static constexpr uint8_t kEmptyHue = 192;
// Background brightness when signal is low (0-255).
static constexpr uint8_t kEmptyBright = 12;
// If true, keep a dim background instead of full clear on low level.
static constexpr bool kKeepBackgroundOnLow = true;
// If true, show gentle rainbow flow when level is low.
static constexpr bool kLowLevelRainbow = true;
// Low-level rainbow animation speed (higher = faster).
static constexpr float kLowLevelRainbowSpeed = 0.2f;
// How long to keep "stream active" after last PCM activity.
static constexpr uint32_t kStreamTimeoutMs = 5000;
// Decay factor when no PCM frames arrive in this cycle.
static constexpr float kNoDataDecay = 0.98f;
// Overall smoothing for level response (0..1, higher = smoother).
static constexpr float kSmooth = 0.75f;
// Dynamic max scaling for level normalization.
static constexpr float kMaxCoef = 1.8f;
// Minimum level to draw bars.
static constexpr float kVuMinLevel = 1.2f;
// Separate smoothing for bar rise.
static constexpr float kLevelRise = 0.25f;
// Separate smoothing for bar fall (lower = longer decay).
static constexpr float kLevelFall = 0.03f;
// Base hue animation speed.
static constexpr float kHueSpeedBase = 0.3f;
// Additional hue speed based on level.
static constexpr float kHueSpeedScale = 2.0f;
// Threshold for sparkle effect.
static constexpr float kSparkleLevel = 0.6f;
// Sparkle probability (0-255).
static constexpr uint8_t kSparkleChance = 6;

// PCM tuning for 16-bit stereo
// Noise floor to ignore tiny values.
static constexpr uint16_t kPcmNoiseFloor = 150;
// Extra gain applied before normalization.
static constexpr float kPcmGain = 10.0f;
// Reference level after gain (higher = less sensitive).
static constexpr float kPcmRefLevel = 6000.0f;
// If true, use mono level (L=R).
static constexpr bool kMono = true;

static portMUX_TYPE g_pcm_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t g_pcm_abs_accum_l = 0;
static volatile uint32_t g_pcm_abs_accum_r = 0;
static volatile uint32_t g_pcm_frames = 0;
static volatile bool g_pcm_activity = false;
static uint32_t g_pcm_rx_frames = 0;

static bool g_streaming = false;
static portMUX_TYPE g_stream_lock = portMUX_INITIALIZER_UNLOCKED;

static CRGB g_leds[CONFIG_LED_COUNT];

static EventGroupHandle_t g_wifi_event_group;
static constexpr int kWifiConnectedBit = BIT0;

static raop_ctx_s *g_raop = nullptr;

static inline uint16_t abs16(int16_t v) {
	return (uint16_t)(v < 0 ? -((int32_t)v) : v);
}

static uint32_t millis() {
	return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int clamp_int(int v, int lo, int hi) {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static int map_int(float x, float in_min, float in_max, int out_min, int out_max) {
	if (in_max <= in_min) return out_min;
	float t = (x - in_min) / (in_max - in_min);
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	return out_min + (int)((out_max - out_min) * t);
}

static uint8_t rand8() {
	return (uint8_t)(esp_random() & 0xFF);
}

static float pcm_to_level(uint32_t sum, uint32_t frames) {
	if (frames == 0) return 0.0f;
	float avg = (float)sum / (float)frames;
	avg *= kPcmGain;
	if (avg <= kPcmNoiseFloor) return 0.0f;
	float scaled = (avg - kPcmNoiseFloor) / (float)(kPcmRefLevel - kPcmNoiseFloor);
	if (scaled < 0.0f) scaled = 0.0f;
	if (scaled > 1.0f) scaled = 1.0f;
	return scaled * 500.0f;
}

static void led_task(void *arg) {
	(void)arg;
	const int max_ch = CONFIG_LED_COUNT / 2;
	const float palette_scale = (max_ch > 0) ? (255.0f / (float)max_ch) : 0.0f;
	const float averK = 0.006f;

	bool stream_active = false;
	bool leds_blank = false;
	uint32_t log_timer = 0;
	uint32_t stream_timer = 0;

	float last_avg_l = 0.0f;
	float last_avg_r = 0.0f;
	float last_level_l = 0.0f;
	float last_level_r = 0.0f;

	float RsoundLevel_f = 0.0f;
	float LsoundLevel_f = 0.0f;
	float Rlength_f = 0.0f;
	float Llength_f = 0.0f;
	float averageLevel = 50.0f;
	int maxLevel = 100;
	float hue_accum = 0.0f;
	uint8_t hue_base = 0;
	float low_hue_accum = 0.0f;

	while (true) {
		bool streaming = false;
		portENTER_CRITICAL(&g_stream_lock);
		streaming = g_streaming;
		portEXIT_CRITICAL(&g_stream_lock);

		if (!streaming) {
			if (!leds_blank) {
				FastLED.clear(true);
				leds_blank = true;
			}
			stream_active = false;
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}

		if (max_ch <= 0) {
			FastLED.clear(true);
			vTaskDelay(pdMS_TO_TICKS(200));
			continue;
		}

		uint32_t sum_l = 0;
		uint32_t sum_r = 0;
		uint32_t frames = 0;
		bool activity = false;

		portENTER_CRITICAL(&g_pcm_mux);
		sum_l = g_pcm_abs_accum_l;
		sum_r = g_pcm_abs_accum_r;
		frames = g_pcm_frames;
		g_pcm_abs_accum_l = 0;
		g_pcm_abs_accum_r = 0;
		g_pcm_frames = 0;
		activity = g_pcm_activity;
		g_pcm_activity = false;
		portEXIT_CRITICAL(&g_pcm_mux);

		uint32_t now = millis();
		if (activity || frames > 0) {
			stream_timer = now;
			if (!stream_active) {
				stream_active = true;
				ESP_LOGI(TAG, "PCM stream active");
			}
		} else if (stream_active && (now - stream_timer > kStreamTimeoutMs)) {
			stream_active = false;
		}

		if (!stream_active) {
			if (!leds_blank) {
				FastLED.clear(true);
				leds_blank = true;
			}
			vTaskDelay(pdMS_TO_TICKS(20));
			continue;
		}

		if (frames > 0) {
			last_avg_r = (float)sum_r / (float)frames;
			last_avg_l = (float)sum_l / (float)frames;
			float RsoundLevel = pcm_to_level(sum_r, frames);
			float LsoundLevel = pcm_to_level(sum_l, frames);
			last_level_r = RsoundLevel;
			last_level_l = LsoundLevel;
		} else {
			last_avg_r *= kNoDataDecay;
			last_avg_l *= kNoDataDecay;
			last_level_r *= kNoDataDecay;
			last_level_l *= kNoDataDecay;
		}

		float RsoundLevel = last_level_r;
		float LsoundLevel = last_level_l;

		if (kMono) LsoundLevel = RsoundLevel;

		RsoundLevel = powf(RsoundLevel, kExp);
		LsoundLevel = powf(LsoundLevel, kExp);
		RsoundLevel_f = RsoundLevel * kSmooth + RsoundLevel_f * (1.0f - kSmooth);
		LsoundLevel_f = LsoundLevel * kSmooth + LsoundLevel_f * (1.0f - kSmooth);

		averageLevel = ((RsoundLevel_f + LsoundLevel_f) / 2.0f) * averK + averageLevel * (1.0f - averK);
		maxLevel = (int)(averageLevel * kMaxCoef);
		if (maxLevel < 1) maxLevel = 1;

		int Rlength = map_int(RsoundLevel_f, 0.0f, (float)maxLevel, 0, max_ch);
		int Llength = map_int(LsoundLevel_f, 0.0f, (float)maxLevel, 0, max_ch);
		Rlength = clamp_int(Rlength, 0, max_ch);
		Llength = clamp_int(Llength, 0, max_ch);

		if (Rlength > Rlength_f) Rlength_f += (Rlength - Rlength_f) * kLevelRise;
		else Rlength_f += (Rlength - Rlength_f) * kLevelFall;
		if (Llength > Llength_f) Llength_f += (Llength - Llength_f) * kLevelRise;
		else Llength_f += (Llength - Llength_f) * kLevelFall;
		Rlength = (int)Rlength_f;
		Llength = (int)Llength_f;

		if (RsoundLevel_f < kVuMinLevel && LsoundLevel_f < kVuMinLevel) {
			if (kLowLevelRainbow) {
				low_hue_accum += kLowLevelRainbowSpeed;
				uint8_t base = (uint8_t)low_hue_accum;
				for (int i = 0; i < CONFIG_LED_COUNT; ++i) {
					uint8_t hue = (uint8_t)(base + (i * 255 / CONFIG_LED_COUNT));
					g_leds[i] = CHSV(hue, 200, kEmptyBright);
				}
				FastLED.show();
				leds_blank = false;
			} else if (kKeepBackgroundOnLow) {
				for (int i = 0; i < CONFIG_LED_COUNT; ++i) {
					g_leds[i] = CHSV(kEmptyHue, 255, kEmptyBright);
				}
				FastLED.show();
				leds_blank = false;
			} else {
				FastLED.clear(true);
				leds_blank = true;
			}
			vTaskDelay(pdMS_TO_TICKS(kMainLoopMs));
			continue;
		}

		for (int i = 0; i < CONFIG_LED_COUNT; ++i) {
			g_leds[i] = CHSV(kEmptyHue, 255, kEmptyBright);
		}

		float level_norm = fmaxf(RsoundLevel_f, LsoundLevel_f) / (float)maxLevel;
		if (level_norm < 0.0f) level_norm = 0.0f;
		if (level_norm > 1.0f) level_norm = 1.0f;
		hue_accum += kHueSpeedBase + level_norm * kHueSpeedScale;
		hue_base = (uint8_t)hue_accum;

		int count = 0;
		for (int i = (max_ch - 1); i > ((max_ch - 1) - Rlength); --i) {
			uint8_t hue = (uint8_t)(count * palette_scale + hue_base);
			g_leds[i] = CHSV(hue, 255, 255);
			count++;
		}
		count = 0;
		for (int i = max_ch; i < (max_ch + Llength); ++i) {
			uint8_t hue = (uint8_t)(count * palette_scale + hue_base + 64);
			if (i >= 0 && i < CONFIG_LED_COUNT) {
				g_leds[i] = CHSV(hue, 255, 255);
			}
			count++;
		}

		if (level_norm > kSparkleLevel && rand8() < kSparkleChance) {
			int side = rand8() & 0x01;
			if (side == 0 && Rlength > 0) {
				int idx = (max_ch - 1) - (rand8() % Rlength);
				if (idx >= 0 && idx < CONFIG_LED_COUNT) {
					g_leds[idx] = CHSV((uint8_t)(hue_base + rand8() % 64), 200, 255);
				}
			} else if (Llength > 0) {
				int idx = max_ch + (rand8() % Llength);
				if (idx >= 0 && idx < CONFIG_LED_COUNT) {
					g_leds[idx] = CHSV((uint8_t)(hue_base + rand8() % 64), 200, 255);
				}
			}
		}

		FastLED.show();
		leds_blank = false;

		if (now - log_timer > 1000) {
			uint32_t rx_frames = 0;
			log_timer = now;
			portENTER_CRITICAL(&g_pcm_mux);
			rx_frames = g_pcm_rx_frames;
			g_pcm_rx_frames = 0;
			portEXIT_CRITICAL(&g_pcm_mux);
			ESP_LOGI(TAG, "PCM fps=%u, avg L/R=%.1f/%.1f, level L/R=%.1f/%.1f",
					 rx_frames, last_avg_l, last_avg_r, last_level_l, last_level_r);
		}

		vTaskDelay(pdMS_TO_TICKS(kMainLoopMs));
	}
}

static void raop_data_cb(const uint8_t *data, size_t len, u32_t playtime) {
	(void)playtime;
	if (!data || len < 4) return;

	size_t frames = len / 4;
	const int16_t *samples = (const int16_t *)data;

	uint32_t sum_l = 0;
	uint32_t sum_r = 0;
	for (size_t i = 0; i + 1 < frames * 2; i += 2) {
		sum_l += abs16(samples[i]);
		sum_r += abs16(samples[i + 1]);
	}

	portENTER_CRITICAL(&g_pcm_mux);
	g_pcm_abs_accum_l += sum_l;
	g_pcm_abs_accum_r += sum_r;
	g_pcm_frames += (uint32_t)frames;
	g_pcm_rx_frames += (uint32_t)frames;
	g_pcm_activity = true;
	portEXIT_CRITICAL(&g_pcm_mux);
}

static bool raop_cmd_cb(raop_event_t event, ...) {
	bool ok = true;
	va_list args;
	va_start(args, event);

	switch (event) {
	case RAOP_SETUP: {
		uint8_t **buffer = va_arg(args, uint8_t **);
		size_t *size = va_arg(args, size_t *);
		if (buffer) *buffer = nullptr;
		if (size) *size = 0;
		portENTER_CRITICAL(&g_stream_lock);
		g_streaming = false;
		portEXIT_CRITICAL(&g_stream_lock);
		portENTER_CRITICAL(&g_pcm_mux);
		g_pcm_abs_accum_l = 0;
		g_pcm_abs_accum_r = 0;
		g_pcm_frames = 0;
		g_pcm_rx_frames = 0;
		g_pcm_activity = false;
		portEXIT_CRITICAL(&g_pcm_mux);
		ESP_LOGI(TAG, "RAOP session setup");
		break;
	}
	case RAOP_STREAM:
		portENTER_CRITICAL(&g_stream_lock);
		g_streaming = true;
		portEXIT_CRITICAL(&g_stream_lock);
		ESP_LOGI(TAG, "RAOP stream started");
		break;
	case RAOP_PLAY:
		portENTER_CRITICAL(&g_stream_lock);
		g_streaming = true;
		portEXIT_CRITICAL(&g_stream_lock);
		ESP_LOGI(TAG, "RAOP play");
		break;
	case RAOP_FLUSH:
		portENTER_CRITICAL(&g_stream_lock);
		g_streaming = false;
		portEXIT_CRITICAL(&g_stream_lock);
		ESP_LOGI(TAG, "RAOP flush");
		break;
	case RAOP_STOP:
		portENTER_CRITICAL(&g_stream_lock);
		g_streaming = false;
		portEXIT_CRITICAL(&g_stream_lock);
		ESP_LOGI(TAG, "RAOP stop");
		break;
	case RAOP_PAUSE:
		portENTER_CRITICAL(&g_stream_lock);
		g_streaming = false;
		portEXIT_CRITICAL(&g_stream_lock);
		ESP_LOGI(TAG, "RAOP pause");
		break;
	default:
		break;
	}

	va_end(args);
	return ok;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
							   int32_t event_id, void *event_data) {
	(void)arg;
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		xEventGroupClearBits(g_wifi_event_group, kWifiConnectedBit);
		esp_wifi_connect();
		ESP_LOGW(TAG, "WiFi disconnected, retrying");
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		xEventGroupSetBits(g_wifi_event_group, kWifiConnectedBit);
		ESP_LOGI(TAG, "WiFi connected");
	}
}

static void wifi_init_sta() {
	g_wifi_event_group = xEventGroupCreate();

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	esp_event_handler_instance_t instance_any_id;
	esp_event_handler_instance_t instance_got_ip;
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

	wifi_config_t wifi_config = {};
	strncpy((char *)wifi_config.sta.ssid, CONFIG_WIFI_SSID, sizeof(wifi_config.sta.ssid));
	strncpy((char *)wifi_config.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
	wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
	wifi_config.sta.pmf_cfg.capable = true;
	wifi_config.sta.pmf_cfg.required = false;

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	ESP_ERROR_CHECK(esp_wifi_start());
}

static void raop_start_after_wifi() {
	xEventGroupWaitBits(g_wifi_event_group, kWifiConnectedBit, pdFALSE, pdTRUE, portMAX_DELAY);

	ESP_ERROR_CHECK(mdns_init());
	ESP_ERROR_CHECK(mdns_hostname_set(CONFIG_MDNS_HOSTNAME));
	ESP_ERROR_CHECK(mdns_instance_name_set(CONFIG_AIRPLAY_NAME));
	ESP_LOGI(TAG, "mDNS initialized");

	esp_netif_ip_info_t ip_info = {};
	esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	if (!netif) {
		ESP_LOGE(TAG, "No WiFi netif found");
		return;
	}
	ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &ip_info));

	uint8_t mac[6] = {0};
	ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));

	g_raop = raop_create(ip_info.ip.addr, (char *)CONFIG_AIRPLAY_NAME, mac, 44100, raop_cmd_cb, raop_data_cb);
	if (!g_raop) {
		ESP_LOGE(TAG, "RAOP create failed");
		return;
	}

	ESP_LOGI(TAG, "mDNS/Bonjour advertisement started");
}

extern "C" void app_main(void) {
	ESP_ERROR_CHECK(nvs_flash_init());
	wifi_init_sta();

	esp_err_t wdt_rc = esp_task_wdt_deinit();
	if (wdt_rc != ESP_OK && wdt_rc != ESP_ERR_INVALID_STATE) {
		ESP_LOGW(TAG, "task_wdt_deinit failed: %d", wdt_rc);
	}

	FastLED.addLeds<WS2812B, CONFIG_LED_PIN, GRB>(g_leds, CONFIG_LED_COUNT);
	FastLED.setBrightness(CONFIG_LED_BRIGHTNESS);
	FastLED.clear(true);

	xTaskCreatePinnedToCore(led_task, "led_task", 4096, nullptr, 1, nullptr, 1);

	raop_start_after_wifi();
}
