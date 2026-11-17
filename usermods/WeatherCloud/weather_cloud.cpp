
#include "wled.h"
#include <HTTPClient.h>
#include <WiFiClient.h>

// For global use use openweathermap
#ifndef OPENWEATHERMAP_WEATHER_API_URL
// but am using brightsky dwd wrapper now for more accuracy in germany
#define OPENWEATHERMAP_WEATHER_API_URL                                         \
  "http://api.openweathermap.org/data/2.5/weather"
#endif

#ifndef DWD_WEATHER_API_URL
#define DWD_WEATHER_API_URL "http://api.brightsky.dev/current_weather"
#endif

#ifndef UPDATE_INTERVAL
#define UPDATE_INTERVAL 600000UL // Update weather every 10 minutes
#endif

/**
 * Two options for APIs to use. In germany use accurate deutsche wetterdienst
 * wrapper (brightsky api) Internationally use openweathermap (which needs a
 * free api key)
 */
enum API_OPTIONS { DWD_BRIGHTSKY = 0, OPENWEATHERMAP = 1 };

// usermod id (kept local instead of in wled00/const.h to avoid core changes)
#ifndef USERMOD_ID_WEATHER_CLOUD
#define USERMOD_ID_WEATHER_CLOUD 200
#endif

// Weather animations (internal state only - these are NOT effect ids).
// Effect ids are assigned dynamically by strip.addEffect(255, ...) since
// WLED 16 ships with more built-in effects and fixed ids would collide.
enum WeatherAnimation {
  ANIM_NONE = 0,
  ANIM_CLEAR,
  ANIM_PARTLY_CLOUDY,
  ANIM_CLOUDY,
  ANIM_RAIN,
  ANIM_SNOW,
  ANIM_NIGHT,
  ANIM_WIND,
  ANIM_THUNDERSTORM,
  ANIM_FOG,
  ANIM_HAIL
};

static WeatherAnimation currentWeatherAnimation = WeatherAnimation::ANIM_NONE;

// ---------------------------------------------------------------------------
// Weather effects
//
// Design notes:
// - The lamp is a cotton cloud: the diffuser blurs neighbouring LEDs into a
//   few large soft zones. So the effects work with broad, slowly changing
//   areas (Perlin noise) and soft-edged particles instead of pixel detail.
// - Everything is time based (strip.now), so the look does not depend on the
//   frame rate. Rates are calibrated to how the effects looked when they were
//   tuned (the old firmware ran at ~10 fps because of a delay() in loop()).
// - WLED applies gamma correction on output (2.8 on this lamp), which turns
//   raw channel values below ~40-50 fully off. Background levels are chosen
//   to stay visible; the Ambient slider scales them.
//
// Sliders (all weather effects):
//   Speed   (sx) 128 = calibrated speed
//   Amount  (ix) 128 = original density (drops, flakes, strikes, cover, ...)
//   Ambient (c1) 128 = default background glow, 0 = dark background
// ---------------------------------------------------------------------------

#define WEATHER_FX_UI "@!,Amount,Ambient;;;01;sx=128,ix=128,c1=128"
static const char _data_FX_MODE_WEATHER[] PROGMEM = "Weather" WEATHER_FX_UI;
static const char _data_FX_MODE_WEATHER_CLEAR[] PROGMEM = "WeatherClearSky" WEATHER_FX_UI;
static const char _data_FX_MODE_WEATHER_PARTLY_CLOUDY[] PROGMEM = "WeatherPartlyCloudy" WEATHER_FX_UI;
static const char _data_FX_MODE_WEATHER_CLOUDY[] PROGMEM = "WeatherCloudy" WEATHER_FX_UI;
static const char _data_FX_MODE_WEATHER_RAIN[] PROGMEM = "WeatherRain" WEATHER_FX_UI;
static const char _data_FX_MODE_WEATHER_SNOW[] PROGMEM = "WeatherSnow" WEATHER_FX_UI;
static const char _data_FX_MODE_WEATHER_NIGHT[] PROGMEM = "WeatherNight" WEATHER_FX_UI;
static const char _data_FX_MODE_WEATHER_WIND[] PROGMEM = "WeatherWind" WEATHER_FX_UI;
static const char _data_FX_MODE_WEATHER_THUNDERSTORM[] PROGMEM = "WeatherThunderstorm" WEATHER_FX_UI;
static const char _data_FX_MODE_WEATHER_FOG[] PROGMEM = "WeatherFog" WEATHER_FX_UI;
static const char _data_FX_MODE_WEATHER_HAIL[] PROGMEM = "WeatherHail" WEATHER_FX_UI;

// --- helpers ------------------------------------------------------------------

// Seconds since this effect's previous frame, scaled by the speed slider.
static float weatherDelta(uint32_t &lastMs) {
  uint32_t now = strip.now;
  uint32_t dt = lastMs ? now - lastMs : 0;
  lastMs = now;
  if (dt > 200) dt = 200; // first frame after a switch or a stall: don't jump
  return dt * 0.001f * SEGMENT.speed * (1.0f / 128.0f);
}

// Advance a 16.16 noise coordinate by `cellsPerSecond` over `dt` seconds.
static inline void advance(uint32_t &coord, float dt, float cellsPerSecond) {
  coord += (uint32_t)(dt * cellsPerSecond * 65536.0f);
}

// Background colour scaled by the Ambient slider (128 = as given).
static uint32_t ambientColor(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t a = SEGMENT.custom1;
  return RGBW32(min(255, (r * a) >> 7), min(255, (g * a) >> 7),
                min(255, (b * a) >> 7), 0);
}

// Smooth 0..255 texture. x/y are 16.16 noise coordinates. Contrast is
// stretched x2 because raw Perlin output mostly stays around the middle.
static uint8_t weatherNoise(uint32_t x, uint32_t y) {
  int32_t n = 128 + ((int32_t)perlin16(x, y) - 32768) / 128;
  return (uint8_t)constrain(n, 0, 255);
}

// Hann window: 1 at d=0, 0 at |d|>=1
static inline float hann(float d) {
  d = fabsf(d);
  return d >= 1.0f ? 0.0f : 0.5f + 0.5f * cos_approx(d * (float)M_PI);
}

// Blend (or add) a soft blob of colour c centred on fractional position pos.
// wrap: blob continues over the strip ends (for things circling the cloud).
static void drawBlob(float pos, float radius, uint32_t c, uint8_t amount,
                     bool additive = false, bool wrap = false) {
  const int len = SEGLEN;
  for (int i = 0; i < len; i++) {
    float d = fabsf(i - pos);
    if (wrap) d = fminf(d, len - d);
    if (d >= radius) continue;
    uint8_t w = (uint8_t)(hann(d / radius) * amount);
    if (!w) continue;
    uint32_t cur = SEGMENT.getPixelColor(i);
    SEGMENT.setPixelColor(i, additive ? color_add(cur, color_fade(c, w))
                                      : color_blend(cur, c, w));
  }
}

static inline float randf() { return hw_random16() * (1.0f / 65536.0f); }

// --- clear sky: blue sky with a warm sun travelling around the cloud ----------
static void drawSun(float &sunPos, float dt) {
  const int len = SEGLEN;
  sunPos += dt * 0.5f; // ~0.5 LED/s, as tuned (1 LED per ~1.9 s)
  if (sunPos >= len) sunPos -= len;
  float radius = 4.0f + SEGMENT.intensity * (6.0f / 128.0f); // 128 -> 10 (20 px)
  for (int i = 0; i < len; i++) {
    float d = fabsf(i - sunPos);
    d = fminf(d, len - d);
    float w = hann(d / radius);
    if (w <= 0.0f) continue;
    uint8_t core = (uint8_t)(w * 255);
    // warm yellow-white centre, yellow edge. Green is high on purpose: gamma 2.8
    // pulls yellow towards orange (raw G 200 shows as ~130 = sunset orange)
    uint32_t sun = color_blend(RGBW32(255, 225, 70, 0), RGBW32(255, 245, 130, 0), core);
    SEGMENT.setPixelColor(i, color_blend(SEGMENT.getPixelColor(i), sun,
                                         (uint8_t)min(255, core * 2)));
  }
}

// deep sky blue with a very gentle brightness texture
static void drawSky(uint32_t t) {
  const int len = SEGLEN;
  uint32_t sky = ambientColor(0, 191, 255);
  for (int i = 0; i < len; i++) {
    uint8_t n = weatherNoise(i * 4096, t);
    SEGMENT.setPixelColor(i, color_fade(sky, 192 + (n >> 2)));
  }
}

static void mode_clear_sky() {
  static uint32_t last = 0, skyT = 0;
  static float sunPos = 0;
  float dt = weatherDelta(last);
  advance(skyT, dt, 0.05f);
  drawSky(skyT);
  drawSun(sunPos, dt);
}

// --- partly cloudy: sun and blue sky, drifting clouds covering most of it ----
static void mode_partly_cloudy() {
  static uint32_t last = 0, skyT = 0, drift = 0, evolve = 0;
  static float sunPos = 0;
  float dt = weatherDelta(last);
  advance(skyT, dt, 0.05f);
  advance(drift, dt, 0.08f);  // clouds drift along the cloud
  advance(evolve, dt, 0.03f); // and slowly change shape
  drawSky(skyT);
  drawSun(sunPos, dt);

  // Amount = cloud cover; 128 ~ 70% covered (grey with blue gaps, as before)
  const int threshold = 233 - SEGMENT.intensity; // cloud where noise > threshold
  uint32_t cloud = ambientColor(150, 150, 160);
  for (unsigned i = 0; i < SEGLEN; i++) {
    int n = weatherNoise(i * 6554 + drift, evolve); // ~10 LEDs per noise cell
    int a = constrain((n - threshold) * 5 + 128, 0, 255); // soft cloud edges
    if (a)
      SEGMENT.setPixelColor(i, color_blend(SEGMENT.getPixelColor(i), cloud, a));
  }
}

// --- cloudy: grey with slowly drifting lighter and darker areas ---------------
static void mode_cloudy() {
  static uint32_t last = 0, drift = 0, evolve = 0;
  float dt = weatherDelta(last);
  advance(drift, dt, 0.06f);
  advance(evolve, dt, 0.03f);
  // Amount = contrast between light and dark areas
  const int contrast = SEGMENT.intensity; // 128 -> roughly 70..170
  for (unsigned i = 0; i < SEGLEN; i++) {
    int n = weatherNoise(i * 5461 + drift, evolve) - 128;
    int lvl = constrain(130 + n * contrast / 200, 75, 225); // floor stays visible after gamma
    SEGMENT.setPixelColor(i, ambientColor(lvl, lvl, lvl + 8));
  }
}

// --- rain: blue-grey cloud with soft blue drops glowing up and fading ---------
#define MAX_RAINDROPS 16
struct Raindrop {
  float pos;
  float bri;       // current brightness 0..255
  float peak;      // peak brightness
  float fadeRate;  // brightness units per second
  float sustain;   // seconds left at peak
  uint8_t state;   // 0 fade in, 1 sustain, 2 fade out
};

static void spawnRaindrop(Raindrop &d, bool stagger) {
  d.pos = randf() * (SEGLEN - 1);
  d.peak = 70 + hw_random8(130);           // 70-200
  d.fadeRate = 80.0f + hw_random8(160);    // ~0.3-2.5 s fades, as tuned
  d.sustain = 0.2f + randf() * 0.8f;       // 0.2-1.0 s at peak
  d.state = 0;
  d.bri = stagger ? randf() * d.peak : 0;
}

static void mode_raining() {
  static uint32_t last = 0, drift = 0;
  static Raindrop drops[MAX_RAINDROPS];
  static uint16_t initLen = 0;
  if (initLen != SEGLEN) {
    for (auto &d : drops) spawnRaindrop(d, true);
    initLen = SEGLEN;
  }
  float dt = weatherDelta(last);
  advance(drift, dt, 0.25f);

  // moving darker/lighter rain curtains in the background
  for (unsigned i = 0; i < SEGLEN; i++) {
    uint8_t n = weatherNoise(i * 8192 + drift, 0);
    SEGMENT.setPixelColor(i, color_fade(ambientColor(30, 45, 100), 128 + (n >> 1)));
  }

  const int count = max(1, SEGMENT.intensity * 12 / 255); // 128 -> 6 drops
  const uint32_t dropColor = RGBW32(0x10, 0x40, 0xB0, 0);
  for (int i = 0; i < count && i < MAX_RAINDROPS; i++) {
    Raindrop &d = drops[i];
    switch (d.state) {
    case 0:
      d.bri += d.fadeRate * dt;
      if (d.bri >= d.peak) { d.bri = d.peak; d.state = 1; }
      break;
    case 1:
      d.sustain -= dt;
      if (d.sustain <= 0) d.state = 2;
      break;
    default:
      d.bri -= d.fadeRate * dt;
      if (d.bri <= 0) spawnRaindrop(d, false);
      break;
    }
    drawBlob(d.pos, 2.0f, dropColor, (uint8_t)d.bri);
  }
}

// --- snow: soft white cloud, flakes drifting slowly and fading in/out ---------
#define MAX_SNOWFLAKES 12
struct Snowflake {
  float pos, speed, age, life;
};

static void spawnSnowflake(Snowflake &f, bool stagger) {
  f.pos = randf() * (SEGLEN - 1);
  f.speed = 0.1f + randf() * 0.4f; // 0.1-0.5 LED/s, as tuned
  f.life = 4.0f + randf() * 6.0f;  // visible for 4-10 s
  f.age = stagger ? randf() * f.life : 0;
}

static void mode_snow() {
  static uint32_t last = 0, drift = 0;
  static Snowflake flakes[MAX_SNOWFLAKES];
  static uint16_t initLen = 0;
  if (initLen != SEGLEN) {
    for (auto &f : flakes) spawnSnowflake(f, true);
    initLen = SEGLEN;
  }
  float dt = weatherDelta(last);
  advance(drift, dt, 0.04f);

  for (unsigned i = 0; i < SEGLEN; i++) {
    uint8_t n = weatherNoise(i * 5461 + drift, 0);
    SEGMENT.setPixelColor(i, color_fade(ambientColor(68, 72, 95), 170 + n / 3));
  }

  const int count = max(1, SEGMENT.intensity * 10 / 255); // 128 -> 5 flakes
  for (int i = 0; i < count && i < MAX_SNOWFLAKES; i++) {
    Snowflake &f = flakes[i];
    f.age += dt;
    f.pos += f.speed * dt;
    if (f.age >= f.life || f.pos >= SEGLEN) spawnSnowflake(f, false);
    // smooth fade in and out over the flake's life: no popping
    uint8_t bri = (uint8_t)(255.0f * sin_approx(f.age / f.life * (float)M_PI));
    drawBlob(f.pos, 2.5f, RGBW32(200, 200, 240, 0), bri, true);
  }
}

// --- thunderstorm: dark storm cloud, lightning lights up regions of it ---------
static void mode_thunderstorm() {
  static uint32_t last = 0, drift = 0;
  static uint32_t nextEvent = 0;  // time of next stroke on/off change
  static uint8_t strokesLeft = 0; // remaining strokes of the current strike
  static bool strokeOn = false;
  static float centre = 0, radius = 0, flash = 0, strokeBri = 0;
  static uint32_t lastFlashMs = 0;
  float dt = weatherDelta(last);
  const uint32_t now = strip.now;
  const float realDt = min(200u, now - lastFlashMs) * 0.001f; // lightning timing ignores the speed slider
  lastFlashMs = now;
  advance(drift, dt, 0.3f);
  const int len = SEGLEN;

  // dark, slowly churning storm cloud
  for (int i = 0; i < len; i++) {
    uint8_t n = weatherNoise(i * 6554 + drift, drift >> 2);
    SEGMENT.setPixelColor(i, color_fade(ambientColor(40, 45, 125), 120 + n / 2));
  }

  // Strike = 2-5 quick strokes at one place, like real lightning.
  // Amount = how often it strikes (128 -> every ~1.5-6 s, like before).
  if ((int32_t)(now - nextEvent) >= 0) {
    if (strokeOn) { // end of a stroke
      strokeOn = false;
      if (--strokesLeft > 0) {
        nextEvent = now + 40 + hw_random16(120); // gap between strokes
      } else {
        uint32_t pause = 1500 + hw_random16(4500);
        nextEvent = now + pause * 128 / max(8, (int)SEGMENT.intensity);
      }
    } else { // start of a stroke
      if (strokesLeft == 0) { // new strike
        strokesLeft = 2 + hw_random8(4);
        strokeBri = 255;
        if (hw_random8() < 64) { // sheet lightning: whole cloud
          centre = len / 2.0f;
          radius = len;
        } else {
          centre = randf() * (len - 1);
          radius = 6 + hw_random8(13); // 6-18 LEDs
        }
      } else { // later strokes: slightly weaker, branch a little
        strokeBri *= 0.55f + randf() * 0.4f;
        centre = constrain(centre + (randf() - 0.5f) * 6.0f, 0.0f, (float)(len - 1));
      }
      strokeOn = true;
      flash = strokeBri;
      nextEvent = now + 30 + hw_random16(90); // stroke duration
    }
  }
  // afterglow: fade quickly after each stroke (independent of speed slider)
  if (!strokeOn) flash = fmaxf(0.0f, flash - 1500.0f * realDt);

  if (flash > 1.0f) {
    for (int i = 0; i < len; i++) {
      float w = (radius >= len) ? 1.0f : hann((i - centre) / radius);
      if (w <= 0.0f) continue;
      uint8_t core = (uint8_t)(w * 255);
      // warm white core, blue-violet halo
      uint32_t c = color_blend(RGBW32(0xA0, 0xA0, 0xFF, 0), RGBW32(0xFF, 0xFF, 0xE0, 0), core);
      SEGMENT.setPixelColor(i, color_blend(SEGMENT.getPixelColor(i), c,
                                           (uint8_t)(w * flash)));
    }
  }
}

// --- wind: pale streaks rushing along the cloud in gusts ----------------------
static void mode_wind() {
  static uint32_t last = 0, flow = 0, gustT = 0, evolve = 0;
  float dt = weatherDelta(last);
  advance(gustT, dt, 0.2f);
  advance(evolve, dt, 0.2f);
  // gust strength 0..1 changes slowly; Amount = how gusty it is
  float gust = weatherNoise(gustT, 0x5A5A0000) / 255.0f;
  float gustiness = SEGMENT.intensity / 128.0f;
  float strength = constrain(0.4f + (gust - 0.5f) * 1.6f * gustiness, 0.15f, 1.0f);
  advance(flow, dt, 0.5f * (0.5f + 1.5f * strength)); // ~3-16 LED/s depending on gusts

  uint32_t base = ambientColor(45, 55, 80);
  for (unsigned i = 0; i < SEGLEN; i++) {
    uint8_t n = weatherNoise(i * 8192 + flow, evolve);
    // only the upper part of the noise forms bright streaks
    int streak = constrain((n - 120) * 3, 0, 255);
    uint8_t lvl = (uint8_t)(streak * (0.3f + 0.7f * strength) * (180.0f / 255.0f));
    SEGMENT.setPixelColor(i, color_add(base, RGBW32(lvl, lvl, lvl + lvl / 8, 0)));
  }
}

// --- night: deep blue cloud with softly twinkling stars ------------------------
#define MAX_STARS 16
struct Star {
  float pos, phase, speed;
};

static void mode_night() {
  static uint32_t last = 0;
  static Star stars[MAX_STARS];
  static uint16_t initLen = 0;
  static uint8_t initCount = 0;
  const int count = max(1, SEGMENT.intensity * 16 / 255); // 128 -> 8 stars
  if (initLen != SEGLEN || initCount != count) {
    // spread stars evenly so they cover the whole cloud
    float spacing = (float)SEGLEN / count;
    for (int i = 0; i < count; i++) {
      stars[i].pos = spacing * i + randf() * spacing;
      stars[i].phase = i * 256.0f / count; // staggered
      stars[i].speed = 10.0f + randf() * 20.0f; // ~8-25 s twinkle, as tuned
    }
    initLen = SEGLEN;
    initCount = count;
  }
  float dt = weatherDelta(last);

  SEGMENT.fill(ambientColor(0, 6, 90));
  for (int i = 0; i < count; i++) {
    Star &s = stars[i];
    s.phase += s.speed * dt;
    if (s.phase >= 256.0f) s.phase -= 256.0f;
    // always faintly present: 20..180
    uint8_t b = 20 + (uint16_t)sin8_t((uint8_t)s.phase) * 160 / 255;
    drawBlob(s.pos, 2.5f, RGBW32(255, 255, 255, 0), b, true);
  }
}

// --- fog: pale, low contrast, with wisps wafting through ----------------------
static void mode_fog() {
  static uint32_t last = 0, drift = 0, evolve = 0, waft = 0, waftEvolve = 0;
  static float swayPhase = 0;
  float dt = weatherDelta(last);
  advance(drift, dt, 0.04f);
  advance(evolve, dt, 0.02f);
  // wisps: a finer layer that glides along (~1 LED/s) and sways back and
  // forth (~+-5 LEDs over ~12 s), like fog wafting past
  advance(waft, dt, 0.12f);
  advance(waftEvolve, dt, 0.06f);
  swayPhase += dt * 0.5f;
  if (swayPhase > 2.0f * (float)M_PI) swayPhase -= 2.0f * (float)M_PI;
  const uint32_t sway = (uint32_t)((sin_approx(swayPhase) + 1.0f) * 0.6f * 65536.0f);

  // Amount = density: denser fog is brighter and more uniform
  const int centre = 90 + SEGMENT.intensity * 50 / 255;     // 128 -> 115
  const int spread = 45 - SEGMENT.intensity * 25 / 255;     // 128 -> ~33
  for (unsigned i = 0; i < SEGLEN; i++) {
    int n = weatherNoise(i * 4096 + drift, evolve) - 128;
    int wisp = weatherNoise(i * 7282 + waft + sway, waftEvolve + 0x40000000);
    int lvl = centre + n * spread / 128 + max(0, wisp - 140) * 45 / 115;
    lvl = constrain(lvl, 30, 230);
    SEGMENT.setPixelColor(i, ambientColor(lvl - 4, lvl, lvl + 4));
  }
}

// --- hail: grey storm cloud pelted by short, bright icy flashes ----------------
#define MAX_HAILSTONES 16
struct Hailstone {
  float pos, bri;
};

static void mode_hail() {
  static uint32_t last = 0, drift = 0;
  static Hailstone stones[MAX_HAILSTONES];
  static float spawnBudget = 0;
  float dt = weatherDelta(last);
  advance(drift, dt, 0.3f);

  for (unsigned i = 0; i < SEGLEN; i++) {
    uint8_t n = weatherNoise(i * 6554 + drift, 0);
    SEGMENT.setPixelColor(i, color_fade(ambientColor(75, 77, 95), 150 + n / 3));
  }

  // Amount = hail rate: 128 -> ~5 stones per second
  spawnBudget += dt * 5.0f * SEGMENT.intensity / 128.0f;
  for (auto &h : stones) {
    if (h.bri <= 0 && spawnBudget >= 1.0f) {
      h.pos = randf() * (SEGLEN - 1);
      h.bri = 180 + hw_random8(76);
      spawnBudget -= 1.0f;
    }
    if (h.bri > 0) {
      drawBlob(h.pos, 1.8f, RGBW32(52, 153, 255, 0), (uint8_t)h.bri, true);
      h.bri -= 1100.0f * dt; // ~0.2 s flash
    }
  }
  if (spawnBudget > 3.0f) spawnBudget = 3.0f;
}

/**
 * The all encompassing weather effect mode that picks the current weather on
 * itself
 */
static void mode_weather() {
  switch (currentWeatherAnimation) {
  case ANIM_CLEAR:         return mode_clear_sky();
  case ANIM_PARTLY_CLOUDY: return mode_partly_cloudy();
  case ANIM_CLOUDY:        return mode_cloudy();
  case ANIM_RAIN:          return mode_raining();
  case ANIM_SNOW:          return mode_snow();
  case ANIM_THUNDERSTORM:  return mode_thunderstorm();
  case ANIM_FOG:           return mode_fog();
  case ANIM_WIND:          return mode_wind();
  case ANIM_NIGHT:         return mode_night();
  case ANIM_HAIL:          return mode_hail();
  case ANIM_NONE:
  default:
    // no (valid) weather data: red error cloud
    SEGMENT.fill(RED);
    break;
  }
}

class WeatherCloudMod : public Usermod {
private:
  bool usermodEnabled = true;
  const char *_usermodConfigName = "Weather Cloud";

  /**
   * 0 for DWD brightsky api in germany without api key
   * 1 for openweathermap api
   */
  API_OPTIONS API_USED = API_OPTIONS::DWD_BRIGHTSKY;

  // lat and long of location
  float LAT = 0.00;
  float LON = 0.00;
  // API Key to set in settings to use openweathermap
  String OPENWEATHERMAP_API_KEY = "";

  // Global variables for weather data
  String currentWeather = "clear";
  unsigned long lastUpdate = 0;
  uint8_t weatherEffectId = 255;

  // Function to fetch weather data
  void updateWeather() {
    if (lastUpdate && (millis() - lastUpdate) < UPDATE_INTERVAL)
      return;

    if (!WLED_CONNECTED) {
      // DEBUG_PRINTLN(F("Wifi not connected so not updating weather"));
      return;
    }

    WiFiClient client;
    HTTPClient http;
    DynamicJsonDocument doc(1024);
    int httpCode;
    String payload;
    String weather;
    // the fetch blocks the main loop - keep it short so LEDs/audio sync don't stall long
    http.setTimeout(3000);

    DEBUG_PRINT(F("Updating weather "));
    if (API_USED == API_OPTIONS::OPENWEATHERMAP) {
      DEBUG_PRINTLN(F("via OpenWeatherMap..."));
      String owmUrl = String(OPENWEATHERMAP_WEATHER_API_URL) + "?lat=" + LAT +
                      "&lon=" + LON + "&appid=" + OPENWEATHERMAP_API_KEY +
                      "&units=metric";
      http.begin(client, owmUrl);
      httpCode = http.GET();

      if (httpCode == 200) {
        payload = http.getString();
        deserializeJson(doc, payload);

        weather = doc["weather"][0]["main"].as<String>();
        currentWeather = weather;
        DEBUG_PRINTLN(F("Should show weather now"));
        DEBUG_PRINTLN(weather);
        int32_t windSpeed = doc["wind"]["speed"].as<int32_t>();
        bool isNight = false;
        // int32_t mockDt = 1737639430;
        DEBUG_PRINTLN(doc["dt"].as<int32_t>());
        DEBUG_PRINTLN(doc["sys"]["sunset"].as<int32_t>());
        DEBUG_PRINTLN(doc["dt"].as<int32_t>() >
                      doc["sys"]["sunset"].as<int32_t>());
        DEBUG_PRINTLN(doc["dt"].as<int32_t>() <
                      doc["sys"]["sunrise"].as<int32_t>());
        DEBUG_PRINTLN(F("Should have shown all that stuff"));
        if (doc["dt"].as<int32_t>() > doc["sys"]["sunset"].as<int32_t>() ||
            doc["dt"].as<int32_t>() < doc["sys"]["sunrise"].as<int32_t>()) {
          isNight = true;
        }
        int32_t cloudiness = doc["clouds"]["all"].as<int32_t>();
        DEBUG_PRINT(F("OpenWeathermap returned: "));
        DEBUG_PRINTLN(currentWeather);
        setAnimationFromOpenWeatherData(currentWeather, windSpeed, isNight,
                                        cloudiness);
        DEBUG_PRINT(F("Showing animation: "));
        DEBUG_PRINTLN(currentWeatherAnimation);
      }
    } else if (API_USED == API_OPTIONS::DWD_BRIGHTSKY) {
      DEBUG_PRINTLN(F("via BrightSky (DWD)..."));
      String brightskyUrl =
          String(DWD_WEATHER_API_URL) + "?lat=" + LAT + "&lon=" + LON;
      http.begin(client, brightskyUrl);
      httpCode = http.GET();

      if (httpCode == 200) {
        payload = http.getString();
        deserializeJson(doc, payload);

        weather = doc["weather"]["icon"].as<String>();
        currentWeather = weather;
        DEBUG_PRINT(F("Brightsky returned: "));
        DEBUG_PRINTLN(currentWeather);
        setAnimationDWDWeatherData(currentWeather);
        DEBUG_PRINT(F("Showing animation: "));
        DEBUG_PRINTLN(currentWeatherAnimation);
      }
    }

    http.end();
    lastUpdate = millis();
  }

  // Map weather conditions to animation
  void setAnimationDWDWeatherData(String weather) {
    if (weather.indexOf("night") != -1) {
      currentWeatherAnimation = ANIM_NIGHT;
    } else if (weather == "clear-day") {
      currentWeatherAnimation = ANIM_CLEAR;
    } else if (weather == "partly-cloudy-day") {
      currentWeatherAnimation = ANIM_PARTLY_CLOUDY;
    } else if (weather == "cloudy") {
      currentWeatherAnimation = ANIM_CLOUDY;
    } else if (weather == "fog") {
      currentWeatherAnimation = ANIM_FOG;
    } else if (weather == "wind") {
      currentWeatherAnimation = ANIM_WIND;
    } else if (weather == "rain") {
      currentWeatherAnimation = ANIM_RAIN;
    } else if (weather == "sleet") {
      // Could possibly do schneeregen animation but not worth the effort
      currentWeatherAnimation = ANIM_RAIN;
    } else if (weather == "snow") {
      currentWeatherAnimation = ANIM_SNOW;
    } else if (weather == "hail") {
      currentWeatherAnimation = ANIM_HAIL;
    } else if (weather == "thunderstorm") {
      currentWeatherAnimation = ANIM_THUNDERSTORM;
    } else {
      currentWeatherAnimation = ANIM_NONE; // Default to none
    }
  }

  // Map weather conditions from openweathermap to animations
  void setAnimationFromOpenWeatherData(String weather, float windSpeed,
                                       bool isNight, int cloudiness) {
    // wind speed over 50 is strong winds
    if (windSpeed > 50) {
      currentWeatherAnimation = ANIM_WIND;
      return;
    }

    if (isNight) {
      currentWeatherAnimation = ANIM_NIGHT;
      return;
    }

    if (weather == "Clear")
      currentWeatherAnimation = ANIM_CLEAR;
    else if (weather == "Clouds" || weather == "Fog" || weather == "Mist")
      if (cloudiness < 50) {
        currentWeatherAnimation = ANIM_PARTLY_CLOUDY;
      } else {
        if (weather == "Fog") {
          currentWeatherAnimation = ANIM_FOG;
        } else {
          currentWeatherAnimation = ANIM_CLOUDY;
        }
      }
    else if (weather == "Rain" || weather == "Drizzle")
      currentWeatherAnimation = ANIM_RAIN;
    else if (weather == "Snow")
      currentWeatherAnimation = ANIM_SNOW;
    else if (weather == "Thunderstorm")
      currentWeatherAnimation = ANIM_THUNDERSTORM;
    else
      currentWeatherAnimation = ANIM_NONE; // Default to none
  }

public:
  // Initialization
  void setup() {
    // TODO add speed and intensity to all of them
    // id 255 = take the next free slot; ids are appended after built-in effects.
    // Effects are referenced by name in the UI; presets store the numeric id,
    // so re-save presets if the number of built-in effects changes.
    weatherEffectId = strip.addEffect(255, &mode_weather, _data_FX_MODE_WEATHER);
    strip.addEffect(255, &mode_clear_sky, _data_FX_MODE_WEATHER_CLEAR);
    strip.addEffect(255, &mode_partly_cloudy, _data_FX_MODE_WEATHER_PARTLY_CLOUDY);
    strip.addEffect(255, &mode_cloudy, _data_FX_MODE_WEATHER_CLOUDY);
    strip.addEffect(255, &mode_raining, _data_FX_MODE_WEATHER_RAIN);
    strip.addEffect(255, &mode_snow, _data_FX_MODE_WEATHER_SNOW);
    strip.addEffect(255, &mode_night, _data_FX_MODE_WEATHER_NIGHT);
    strip.addEffect(255, &mode_wind, _data_FX_MODE_WEATHER_WIND);
    strip.addEffect(255, &mode_thunderstorm, _data_FX_MODE_WEATHER_THUNDERSTORM);
    strip.addEffect(255, &mode_fog, _data_FX_MODE_WEATHER_FOG);
    strip.addEffect(255, &mode_hail, _data_FX_MODE_WEATHER_HAIL);
    DEBUG_PRINTF_P(PSTR("Weather effect registered with id %u\n"), weatherEffectId);

    // Turn on all section
    for (unsigned i = 0; i < strip.getSegmentsNum(); i++) {
      Segment &seg = strip.getSegment(i);
      if (!seg.isActive())
        continue;

      seg.setOption(SEG_OPTION_ON, true);
    }
  }

  // Main loop
  void loop() {
    if (usermodEnabled) {

      // updateWeather() rate-limits itself to UPDATE_INTERVAL.
      // No delay() here: blocking the main loop throttles LED output and
      // audio sync (UDP) reception for the audioreactive usermod.
      updateWeather();
    }
  }

  uint16_t getId() override { return USERMOD_ID_WEATHER_CLOUD; }

  /****** Config page stuff */
  // from
  // https://github.com/Aircoookie/WLED/blob/main/usermods/EXAMPLE_v2/usermod_v2_example.h

  /*
   * addToConfig() can be used to add custom persistent settings to the cfg.json
   * file in the "um" (usermod) object. It will be called by WLED when settings
   * are actually saved (for example, LED settings are saved) If you want to
   * force saving the current state, use serializeConfig() in your loop().
   *
   * CAUTION: serializeConfig() will initiate a filesystem write operation.
   * It might cause the LEDs to stutter and will cause flash wear if called too
   * often. Use it sparingly and always in the loop, never in network callbacks!
   *
   * addToConfig() will make your settings editable through the Usermod Settings
   * page automatically.
   *
   * Usermod Settings Overview:
   * - Numeric values are treated as floats in the browser.
   *   - If the numeric value entered into the browser contains a decimal point,
   * it will be parsed as a C float before being returned to the Usermod.  The
   * float data type has only 6-7 decimal digits of precision, and doubles are
   * not supported, numbers will be rounded to the nearest float value when
   * being parsed. The range accepted by the input field is +/- 1.175494351e-38
   * to +/- 3.402823466e+38.
   *   - If the numeric value entered into the browser doesn't contain a decimal
   * point, it will be parsed as a C int32_t (range: -2147483648 to 2147483647)
   * before being returned to the usermod. Overflows or underflows are truncated
   * to the max/min value for an int32_t, and again truncated to the type used
   * in the Usermod when reading the value from ArduinoJson.
   * - Pin values can be treated differently from an integer value by using the
   * key name "pin"
   *   - "pin" can contain a single or array of integer values
   *   - On the Usermod Settings page there is simple checking for pin conflicts
   * and warnings for special pins
   *     - Red color indicates a conflict.  Yellow color indicates a pin with a
   * warning (e.g. an input-only pin)
   *   - Tip: use int8_t to store the pin value in the Usermod, so a -1 value
   * (pin not set) can be used
   *
   * See usermod_v2_auto_save.h for an example that saves Flash space by reusing
   * ArduinoJson key name strings
   *
   * If you need a dedicated settings page with custom layout for your Usermod,
   * that takes a lot more work. You will have to add the setting to the HTML,
   * xml.cpp and set.cpp manually. See the WLED Soundreactive fork (code and
   * wiki) for reference.  https://github.com/atuline/WLED
   *
   * I highly recommend checking out the basics of ArduinoJson serialization and
   * deserialization in order to use custom settings!
   */
  void addToConfig(JsonObject &root) override {
    JsonObject top = root.createNestedObject(FPSTR(_usermodConfigName));
    top["enabled"] = usermodEnabled;
    top["latitude"] = LAT;
    top["longitude"] = LON;
    top["API"] = API_USED;
    top["openWeatherMap_ApiKey"] = OPENWEATHERMAP_API_KEY;
  }

  /*
   * readFromConfig() can be used to read back the custom settings you added
   * with addToConfig(). This is called by WLED when settings are loaded
   * (currently this only happens immediately after boot, or after saving on the
   * Usermod Settings page)
   *
   * readFromConfig() is called BEFORE setup(). This means you can use your
   * persistent values in setup() (e.g. pin assignments, buffer sizes), but also
   * that if you want to write persistent values to a dynamic buffer, you'd need
   * to allocate it here instead of in setup. If you don't know what that is,
   * don't fret. It most likely doesn't affect your use case :)
   *
   * Return true in case the config values returned from Usermod Settings were
   * complete, or false if you'd like WLED to save your defaults to disk (so any
   * missing values are editable in Usermod Settings)
   *
   * getJsonValue() returns false if the value is missing, or copies the value
   * into the variable provided and returns true if the value is present The
   * configComplete variable is true only if the "exampleUsermod" object and all
   * values are present.  If any values are missing, WLED will know to call
   * addToConfig() to save them
   *
   * This function is guaranteed to be called on boot, but could also be called
   * every time settings are updated
   */
  bool readFromConfig(JsonObject &root) override {
    // default settings values could be set here (or below using the 3-argument
    // getJsonValue()) instead of in the class definition or constructor setting
    // them inside readFromConfig() is slightly more robust, handling the rare
    // but plausible use case of single value being missing after boot (e.g. if
    // the cfg.json was manually edited and a value was removed)

    JsonObject top = root[FPSTR(_usermodConfigName)];

    bool configComplete = !top.isNull();

    configComplete &= getJsonValue(top["enabled"], usermodEnabled);
    configComplete &= getJsonValue(top["latitude"], LAT);
    configComplete &= getJsonValue(top["longitude"], LON);
    configComplete &= getJsonValue(top["API"], API_USED);
    configComplete &=
        getJsonValue(top["openWeatherMap_ApiKey"], OPENWEATHERMAP_API_KEY);

    // 10s after saving force one update again
    if (millis() - lastUpdate > 10000) {
      lastUpdate = 0;
    }
    return configComplete;
  }

  /*
   * appendConfigData() is called when user enters usermod settings page
   * it may add additional metadata for certain entry fields (adding drop down
   * is possible) be careful not to add too much as oappend() buffer is limited
   * to 3k
   */
  void appendConfigData() override {
    oappend(F("addInfo('"));
    oappend(String(FPSTR(_usermodConfigName)).c_str());
    oappend(F(":latitude"));
    oappend(F("',1,'<i>e.g. use https://www.gps-coordinates.net/</i>');"));

    oappend(F("addInfo('"));
    oappend(String(FPSTR(_usermodConfigName)).c_str());
    oappend(F(":longitude"));
    oappend(F("',1,'<i></i>');"));

    oappend(F("dd=addDropdown('"));
    oappend(String(FPSTR(_usermodConfigName)).c_str());
    oappend(F("','API');"));
    oappend(F("addOption(dd,'Brightsky (DWD data for Germany)',0);"));
    oappend(F("addOption(dd,'Openweathermap (Global)',1);"));

    oappend(F("addInfo('"));
    oappend(String(FPSTR(_usermodConfigName)).c_str());
    oappend(F(":openWeatherMap_ApiKey"));
    oappend(F("',1,'<i>only needed for OpenWeatherMap</i>');"));

    // TODO how to show current weather here?
    // oappend(F("addInfo('"));
    // oappend(currentWeather.c_str());
  }
};

static WeatherCloudMod weather_cloud;
REGISTER_USERMOD(weather_cloud);
