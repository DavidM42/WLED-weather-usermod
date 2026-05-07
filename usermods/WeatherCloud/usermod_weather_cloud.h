#pragma once

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

// mode (effect) id for the weather mode which displays current weather
uint16_t WEATHER_EFFECT_ID = 187;

// Weather animations
enum WeatherAnimation {
  ANIM_NONE = 0,
  ANIM_CLEAR = 190,
  ANIM_PARTLY_CLOUDY = 191,
  ANIM_CLOUDY = 192,
  ANIM_RAIN = 193,
  ANIM_SNOW = 194,
  ANIM_NIGHT = 195,
  ANIM_WIND = 196,
  ANIM_THUNDERSTORM = 197,
  ANIM_FOG = 198,
  ANIM_HAIL = 199
};

WeatherAnimation currentWeatherAnimation = WeatherAnimation::ANIM_NONE;

// Should be done
uint16_t mode_clear_sky() {
  static uint16_t clearSunPosition = 0;
  static unsigned long clearLastUpdate = 0;

  // Update sun position based on speed
  if (millis() - clearLastUpdate > (255 - SEGMENT.speed) * 15) {
    clearSunPosition++;
    if (clearSunPosition >= SEGMENT.length()) {
      clearSunPosition = 0;
    }
    clearLastUpdate = millis();
  }

  // Fill the sky with blue color
  SEGMENT.fill(CRGB::DeepSkyBlue);

  // Tapered sun glow: golden-yellow centre, orange edges
  for (int i = 0; i < 20; i++) {
    int pos = (clearSunPosition + i) % SEGMENT.length();
    int dist = abs(i - 9); // 0 at centre, up to 9 at edges
    uint8_t t = (uint8_t)(255 - dist * 205 / 9); // 255 at centre -> ~50 at edge
    uint8_t g =
        (uint8_t)(100 + (uint16_t)t * 100 / 255);  // 100-200: golden yellow
    uint8_t b = (uint8_t)((uint16_t)t * 20 / 255); // 0-20: nearly no blue
    SEGMENT.setPixelColor(pos, ((uint32_t)255 << 16) | ((uint32_t)g << 8) | b);
  }

  return FRAMETIME;
}

// Should be done
uint16_t mode_partly_cloudy() {
  static uint16_t partlyCloudySunPosition = 0;
  static unsigned long partlyCloudyLastUpdate = 0;
  static uint16_t blueSpotPositions[10] = {0}; // Store positions of blue spots
  static uint8_t numBlueSpots = 0;
  static unsigned long spotUpdateTime = 0;

  // Update sun position based on speed
  if (millis() - partlyCloudyLastUpdate > (255 - SEGMENT.speed) * 15) {
    partlyCloudySunPosition++;
    if (partlyCloudySunPosition >= SEGMENT.length()) {
      partlyCloudySunPosition = 0;
    }
    partlyCloudyLastUpdate = millis();
  }

  // Fill the sky with grey clouds
  SEGMENT.fill(CRGB::Gray);

  // Update blue spots every 3 seconds
  if (millis() - spotUpdateTime > hw_random(15000, 20000)) {
    numBlueSpots = random8(2, 5); // Random number of spots
    for (int i = 0; i < numBlueSpots; i++) {
      blueSpotPositions[i] = random16(SEGMENT.length());
    }
    spotUpdateTime = millis();
  }

  // Tapered sun glow: golden-yellow centre, orange edges
  for (int i = 0; i < 20; i++) {
    int pos = (partlyCloudySunPosition + i) % SEGMENT.length();
    int dist = abs(i - 9);
    uint8_t t = (uint8_t)(255 - dist * 205 / 9);
    uint8_t g = (uint8_t)(100 + (uint16_t)t * 100 / 255);
    uint8_t b = (uint8_t)((uint16_t)t * 20 / 255);
    SEGMENT.setPixelColor(pos, ((uint32_t)255 << 16) | ((uint32_t)g << 8) | b);
  }

  // Draw blue spots with soft tapered edges to avoid hard cuts
  for (int i = 0; i < numBlueSpots; i++) {
    int spotSize = 7;
    for (int j = 0; j < spotSize; j++) {
      int pos = (blueSpotPositions[i] + j) % SEGMENT.length();
      int distFromCenter = abs(j - spotSize / 2);
      uint8_t alpha =
          (uint8_t)(255 - distFromCenter * 255 / (spotSize / 2 + 1));
      SEGMENT.setPixelColor(pos,
                            color_blend(CRGB::Gray, CRGB::DeepSkyBlue, alpha));
    }
  }

  return FRAMETIME;
}

// done
uint16_t mode_cloudy() {
  // Very slow, subtle brightness variation to keep the cloud feeling alive
  static uint16_t cloudPhase = 0;
  cloudPhase++;
  // One full cycle ~17 s at 30 fps (cloudPhase >> 1 advances phase by
  // 0.5/frame)
  uint8_t b = 70 + (uint8_t)((uint16_t)sin8(cloudPhase >> 1) * 90 /
                             255); // 70-160: visible breathing
  SEGMENT.fill(((uint32_t)b << 16) | ((uint32_t)b << 8) | b);
  return FRAMETIME;
}

// --- Persistent raindrop state -----------------------------------------------
#define NUM_RAINDROPS 6
enum DropState { DROP_FADEIN = 0, DROP_SUSTAIN = 1, DROP_FADEOUT = 2 };

struct Raindrop {
  uint16_t pos;
  uint8_t brightness;
  uint8_t peakBright;
  uint8_t fadeSpeed;
  uint8_t sustainLeft;
  DropState state;
};

static Raindrop raindrops[NUM_RAINDROPS];

static void spawnRaindrop(Raindrop &d, uint16_t numLeds, bool stagger) {
  d.pos = random16(numLeds);
  d.peakBright = 70 + random8(130); // 70-200
  d.fadeSpeed = 8 + random8(16);  // 8-24 per frame -> 270-830 ms fade at 30 fps
  d.sustainLeft = 2 + random8(8); // 2-10 frames at peak
  d.state = DROP_FADEIN;
  d.brightness = stagger ? random8(d.peakBright) : 0;
  if (stagger && d.brightness > d.peakBright / 2)
    d.state = DROP_FADEOUT;
}

// -----------------------------------------------------------------------------
uint16_t mode_raining() {
  static bool initialized = false;
  if (!initialized) {
    for (int i = 0; i < NUM_RAINDROPS; i++)
      spawnRaindrop(raindrops[i], SEGMENT.length(), true);
    initialized = true;
  }

  const uint32_t bgColor = 0x06060C;   // dark stormy background
  const uint32_t dropColor = 0x1040B0; // cool blue rain
  SEGMENT.fill(bgColor);

  for (int i = 0; i < NUM_RAINDROPS; i++) {
    Raindrop &d = raindrops[i];

    // Advance lifecycle
    switch (d.state) {
    case DROP_FADEIN:
      if ((uint16_t)d.brightness + d.fadeSpeed >= d.peakBright) {
        d.brightness = d.peakBright;
        d.state = DROP_SUSTAIN;
      } else {
        d.brightness += d.fadeSpeed;
      }
      break;

    case DROP_SUSTAIN:
      if (d.sustainLeft > 0)
        d.sustainLeft--;
      else
        d.state = DROP_FADEOUT;
      break;

    case DROP_FADEOUT:
      if (d.brightness <= d.fadeSpeed)
        spawnRaindrop(d, SEGMENT.length(), false);
      else
        d.brightness -= d.fadeSpeed;
      break;
    }

    // Render with soft 3-pixel spread centred on pos
    int c = (int)d.pos;
    uint8_t sideB = (uint8_t)((uint16_t)d.brightness * 35 / 100);
    if (c > 0)
      SEGMENT.setPixelColor(c - 1, color_blend(bgColor, dropColor, sideB));
    SEGMENT.setPixelColor(c, color_blend(bgColor, dropColor, d.brightness));
    if (c + 1 < (int)SEGMENT.length())
      SEGMENT.setPixelColor(c + 1, color_blend(bgColor, dropColor, sideB));
  }

  return FRAMETIME;
}

#define NUM_SNOWFLAKES 5

struct Snowflake {
  float pos;
  float speed;
};

Snowflake snowflakes[NUM_SNOWFLAKES];

void initSnowflakes(uint16_t numLeds) {
  for (int i = 0; i < NUM_SNOWFLAKES; i++) {
    snowflakes[i].pos = random(numLeds);
    snowflakes[i].speed =
        0.01f + 0.04f * (random8() / 255.0f); // Speed between 0.01 and 0.05
  }
}

uint16_t mode_snow(void) {
  static bool initialized = false;
  if (!initialized) {
    initSnowflakes(SEGMENT.length());
    initialized = true;
  }

  // 1. Fill all LEDs with a very dim gray (darker cloud background)
  for (uint16_t i = 0; i < SEGMENT.length(); i++) {
    SEGMENT.setPixelColor(i, 0x050508); // Much dimmer bluish-gray
  }

  // 2. Move and draw snowflakes horizontally (draw OVER the background)
  for (int i = 0; i < NUM_SNOWFLAKES; i++) {
    // Sub-pixel 5-pixel Hann-window snowflake for smooth movement
    float fpos = snowflakes[i].pos;
    int centre = (int)fpos;
    float frac = fpos - (float)centre;
    const int hw = 2; // half-width: 5 pixels total
    for (int w = -hw; w <= (hw + 1); w++) {
      int led = centre + w;
      if (led < 0 || led >= (int)SEGMENT.length())
        continue;
      float dist = fabsf((float)w - frac);
      if (dist >= (float)hw)
        continue;
      uint8_t angle = (uint8_t)(dist * 128.0f / (float)hw);
      uint8_t t = cos8(angle);
      // Cold blue-white
      uint8_t bl = (uint8_t)min(255, (int)t + (int)t / 5);
      SEGMENT.setPixelColor(led, ((uint32_t)t << 16) | ((uint32_t)t << 8) |
                                     (uint32_t)bl);
    }

    snowflakes[i].pos += snowflakes[i].speed;
    if (snowflakes[i].pos >= SEGMENT.length()) {
      snowflakes[i].pos = 0;
      snowflakes[i].speed = 0.01f + 0.04f * (random8() / 255.0f);
    }
  }

  return FRAMETIME;
}

struct LightningBolt {
  uint32_t lastTime;
  uint16_t interval;
  uint16_t start;
  uint16_t len;
  uint8_t fade;
};

LightningBolt lightningBolt;

uint16_t mode_thunderstorm() {
  static uint8_t lightningCooldown = 0;
  static uint8_t globalFlash = 0;
  static uint32_t frame = 0;
  frame++;

  // 1. Fill with dark stormy blue
  for (uint16_t i = 0; i < SEGMENT.length(); i++) {
    uint8_t flicker = random8((uint16_t)(i * 31 + frame * 17)) %
                      10; // pseudo-random per pixel per frame
    SEGMENT.setPixelColor(i, ((0x00 + flicker) << 16) |
                                 ((0x00 + flicker) << 8) |
                                 (0x30 + flicker)); // dark blue
  }

  // 2. Occasionally trigger a global flash (big lightning)
  if (random8() < 3 && lightningCooldown == 0) {
    globalFlash = 255;
    lightningCooldown = random8(10, 40); // Cooldown before next big flash
  }

  // 3. Fade global flash
  if (globalFlash > 0) {
    for (uint16_t i = 0; i < SEGMENT.length(); i++) {
      SEGMENT.setPixelColor(
          i, color_blend(SEGMENT.getPixelColor(i), 0xFFFFFF, globalFlash));
    }
    globalFlash = (globalFlash > 40) ? globalFlash - 40 : 0;
  }

  // 4. Occasionally trigger a lightning bolt
  static uint8_t boltStrikesLeft = 0;
  static bool boltPause = false; // true during pause between positions

  uint32_t now = millis();
  if (now - lightningBolt.lastTime > lightningBolt.interval) {
    if (boltPause) {
      // End pause, prepare for next position
      boltPause = false;
      boltStrikesLeft = 0; // Force new position on next strike
      lightningBolt.interval =
          400 + random16(1200); // Normal interval for next strike (0.4-1.6s)
    } else if (boltStrikesLeft == 0) {
      // New position, but add a pause before starting
      lightningBolt.start = random16(SEGMENT.length() - 6);
      lightningBolt.len = 3 + (random8() % 6); // Length 3-8
      boltStrikesLeft = 2 + (random8() % 3);   // 2-4 strikes at this position
      boltPause = true;
      lightningBolt.interval =
          800 + random16(2000); // Pause between positions (0.8-2.8s)
    } else {
      // Continue striking at current position
      lightningBolt.fade = 0;
      lightningBolt.lastTime = millis();
      lightningBolt.interval =
          400 + random16(1200); // Normal interval for next strike (0.4-1.6s)
      boltStrikesLeft--;
    }
    lightningBolt.lastTime = millis();
  }

  // Draw and fade the bolt if active
  if (lightningBolt.fade < 255 && !boltPause) {
    for (uint16_t i = lightningBolt.start;
         i < lightningBolt.start + lightningBolt.len && i < SEGMENT.length();
         i++) {
      SEGMENT.setPixelColor(i, color_blend(SEGMENT.getPixelColor(i), 0xFFFFCC,
                                           255 - lightningBolt.fade));
      if (i > 0)
        SEGMENT.setPixelColor(i - 1, color_blend(SEGMENT.getPixelColor(i - 1),
                                                 0xA0A0FF,
                                                 128 - lightningBolt.fade / 2));
      if (i + 1 < SEGMENT.length())
        SEGMENT.setPixelColor(i + 1, color_blend(SEGMENT.getPixelColor(i + 1),
                                                 0xA0A0FF,
                                                 128 - lightningBolt.fade / 2));
    }
    lightningBolt.fade += 40;
    if (lightningBolt.fade > 255)
      lightningBolt.fade = 255;
  }

  // 5. Cooldown timer for global flash
  if (lightningCooldown > 0)
    lightningCooldown--;

  return hw_random(40, 80); // Fast update for flicker and flashes
}

#define NUM_WIND_SPOTS 3

struct WindSpot {
  float pos;
  float speed;
  uint8_t width;
};

static WindSpot windSpots[NUM_WIND_SPOTS];

// TODO wind still kinda bad and needs finetuning
void initWindSpots(uint16_t numLeds) {
  for (int i = 0; i < NUM_WIND_SPOTS; i++) {
    // Spread spots evenly so the whole cloud is lit
    windSpots[i].pos = (float)(numLeds / NUM_WIND_SPOTS * i) +
                       random8(numLeds / NUM_WIND_SPOTS);
    windSpots[i].speed =
        0.10f + 0.10f * (random8() / 255.0f); // 0.10-0.20 per frame
    windSpots[i].width = 8 + (random8() % 5); // Half-width 8-12
  }
}

uint16_t mode_wind(void) {
  static bool initialized = false;
  if (!initialized) {
    initWindSpots(SEGMENT.length());
    initialized = true;
  }

  // Dim stormy sky base
  SEGMENT.fill(0x060810);

  // Move and draw wind gusts with smooth Hann-window (cos8) profile
  for (int s = 0; s < NUM_WIND_SPOTS; s++) {
    windSpots[s].pos += windSpots[s].speed;
    if (windSpots[s].pos >= SEGMENT.length())
      windSpots[s].pos -= SEGMENT.length();

    int hw = (int)windSpots[s].width; // half-width
    int centre = (int)windSpots[s].pos;
    for (int w = -hw; w <= hw; w++) {
      int led = (centre + w + SEGMENT.length()) % SEGMENT.length();
      // cos8: 255 at centre (angle=0), 0 at edge (angle=128)
      uint8_t angle = (uint8_t)((uint32_t)abs(w) * 128 / hw);
      uint8_t b = (uint8_t)((uint32_t)cos8(angle) * 180 / 255);
      // Additive blend so overlapping spots combine naturally
      uint32_t existing = SEGMENT.getPixelColor(led);
      uint8_t er = (existing >> 16) & 0xFF;
      uint8_t eg = (existing >> 8) & 0xFF;
      uint8_t eb = existing & 0xFF;
      SEGMENT.setPixelColor(led,
                            ((uint32_t)min(255, (int)er + (int)b) << 16) |
                                ((uint32_t)min(255, (int)eg + (int)b) << 8) |
                                (uint32_t)min(255, (int)eb + (int)b));
    }
  }

  return FRAMETIME;
}

#define NUM_STARS 8 // Adjust as needed

struct Star {
  uint16_t pos;
  uint8_t phase;
  uint8_t speed;
};

Star stars[NUM_STARS];

void initStars(uint16_t numLeds) {
  for (int i = 0; i < NUM_STARS; i++) {
    // Spread stars evenly so they cover the whole cloud
    uint16_t segLen = numLeds / NUM_STARS;
    stars[i].pos = (uint16_t)(segLen * i + (segLen > 1 ? random8(segLen) : 0));
    // Stagger initial phases so stars don't all brighten/dim together
    stars[i].phase = (uint8_t)(i * 256 / NUM_STARS);
    stars[i].speed =
        1 + random8(3); // 1-3 per frame -> ~4-12 s full cycle at 30 fps
  }
}

uint16_t mode_night(void) {
  static bool initialized = false;
  if (!initialized) {
    initStars(SEGMENT.length());
    initialized = true;
  }

  // Deep night sky - very dim blue backdrop
  SEGMENT.fill(0x000008);

  for (int i = 0; i < NUM_STARS; i++) {
    stars[i].phase += stars[i].speed;
    // sin8 returns 0..255; map to [20, 180] so stars are always faintly present
    uint8_t b = 20 + (uint8_t)((uint16_t)sin8(stars[i].phase) * 160 / 255);
    int c = (int)stars[i].pos;
    // Soft 5-pixel gaussian-like spread: centre full, +-1 at 50%, +-2 at 12.5%
    // Blend additively so overlapping stars combine gracefully
    for (int w = -2; w <= 2; w++) {
      int led = c + w;
      if (led < 0 || led >= (int)SEGMENT.length())
        continue;
      uint8_t intensity;
      switch (abs(w)) {
      case 0:
        intensity = b;
        break; // centre
      case 1:
        intensity = b >> 1;
        break; // 50 %
      default:
        intensity = b >> 3;
        break; // 12.5 %
      }
      // Slightly blue-tinted white for a starlight feel
      uint32_t existing = SEGMENT.getPixelColor(led);
      uint8_t er = (existing >> 16) & 0xFF;
      uint8_t eg = (existing >> 8) & 0xFF;
      uint8_t eb = existing & 0xFF;
      uint8_t bCh = (uint8_t)min(255, (int)intensity + intensity / 4);
      SEGMENT.setPixelColor(led,
                            ((uint32_t)min(255, (int)er + intensity) << 16) |
                                ((uint32_t)min(255, (int)eg + intensity) << 8) |
                                (uint32_t)min(255, (int)eb + (int)bCh));
    }
  }

  return FRAMETIME;
}

#define NUM_FOG_BANKS 3

struct FogBank {
  float pos;
  float speed;
  uint8_t width;
};

static FogBank fogBanks[NUM_FOG_BANKS];

// TODO improve this effect
void initFogBanks(uint16_t numLeds) {
  for (int i = 0; i < NUM_FOG_BANKS; i++) {
    fogBanks[i].pos = random(numLeds);
    fogBanks[i].speed = 0.01 + 0.03 * (random8() / 255.0); // Very slow
    fogBanks[i].width = 8 + (random8() % 8);               // Wide banks
  }
}

uint16_t mode_fog(void) {
  static bool initialized = false;
  if (!initialized) {
    initFogBanks(SEGMENT.length());
    initialized = true;
  }

  // 1. Fill all LEDs with a very dim grayish base
  for (uint16_t i = 0; i < SEGMENT.length(); i++) {
    // SEGMENT.setPixelColor(i, 0x101012); // Very dim blue-gray
    SEGMENT.setPixelColor(i, BLACK);
  }

  // 2. Overlay drifting fog banks
  for (int f = 0; f < NUM_FOG_BANKS; f++) {
    fogBanks[f].pos += fogBanks[f].speed;
    if (fogBanks[f].pos >= SEGMENT.length())
      fogBanks[f].pos -= SEGMENT.length();

    for (int w = -fogBanks[f].width; w <= fogBanks[f].width; w++) {
      int led =
          ((int)fogBanks[f].pos + w + SEGMENT.length()) % SEGMENT.length();
      // Very soft edge: center is 80, edges fade out
      uint8_t brightness = 80 - abs(w) * (80 / (fogBanks[f].width + 1));
      uint32_t col = ((uint32_t)brightness << 16) |
                     ((uint32_t)brightness << 8) | brightness; // soft gray
      SEGMENT.setPixelColor(
          led, color_blend(SEGMENT.getPixelColor(led), col, brightness));
    }
  }

  return FRAMETIME;
}

#define NUM_HAILSTONES 3

struct Hailstone {
  float pos;
  float speed;
  uint8_t width;
};

static Hailstone hailstones[NUM_HAILSTONES];

void initHailstones(uint16_t numLeds) {
  for (int i = 0; i < NUM_HAILSTONES; i++) {
    hailstones[i].pos = random(numLeds);
    hailstones[i].speed =
        0.12f + 0.16f * (random8() / 255.0f); // 0.12-0.28 per frame
    hailstones[i].width = 2;
  }
}

uint16_t mode_hail(void) {
  static bool initialized = false;
  if (!initialized) {
    initHailstones(SEGMENT.length());
    initialized = true;
  }

  // 1. Fill all LEDs with dark grey (stormy sky)
  for (uint16_t i = 0; i < SEGMENT.length(); i++) {
    SEGMENT.setPixelColor(i, 0x101014); // Darker grey
  }

  // 2. Move and draw hailstones
  for (int h = 0; h < NUM_HAILSTONES; h++) {
    hailstones[h].pos += hailstones[h].speed;
    if (hailstones[h].pos >= SEGMENT.length()) {
      hailstones[h].pos = 0;
      hailstones[h].speed = 0.12f + 0.16f * (random8() / 255.0f);
      hailstones[h].width = 2;
    }
    // Sub-pixel 5-pixel Hann-window hailstone for smooth movement
    float fpos = hailstones[h].pos;
    int centre = (int)fpos;
    float frac = fpos - (float)centre;
    const int hw = 2; // half-width: 5 pixels total
    for (int w = -hw; w <= (hw + 1); w++) {
      int led = centre + w;
      if (led < 0 || led >= (int)SEGMENT.length())
        continue;
      float dist = fabsf((float)w - frac);
      if (dist >= (float)hw)
        continue;
      uint8_t angle = (uint8_t)(dist * 128.0f / (float)hw);
      uint8_t t = cos8(angle);
      // Icy blue colour scaled by brightness
      uint8_t r = (uint8_t)((uint32_t)0x21 * t / 255);
      uint8_t g = (uint8_t)((uint32_t)0x62 * t / 255);
      uint8_t b = (uint8_t)((uint32_t)0xA3 * t / 255);
      SEGMENT.setPixelColor(led, ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
    }
  }

  return FRAMETIME;
}

// TODO move all weather thingys into seperate files?

// TODO commmit to my git and update fork afterwards

/**
 * The all encompassing weather effect mode that picks the current weather on
 * itself
 */
uint16_t mode_weather() {

  // seg.setOption(SEG_OPTION_ON, true);

  switch (currentWeatherAnimation) {
  case ANIM_CLEAR:
    return mode_clear_sky();
    break;
  case ANIM_PARTLY_CLOUDY:
    return mode_partly_cloudy();
    break;
  case ANIM_CLOUDY:
    return mode_cloudy();
    break;
  case ANIM_RAIN:
    return mode_raining();
    break;
  case ANIM_SNOW:
    return mode_snow();
    break;
  case ANIM_THUNDERSTORM:
    return mode_thunderstorm();
    break;
  case ANIM_FOG:
    return mode_fog();
    break;
  case ANIM_WIND:
    return mode_wind();
    break;
  case ANIM_NIGHT:
    return mode_night();
    break;
  // none and default defaulting to full red error cloud
  case ANIM_NONE:
  default:
    for (unsigned i = 0; i < strip.getSegmentsNum(); i++) {
      Segment &seg = strip.getSegment(i);
      if (!seg.isActive())
        continue;
      // DEBUG_PRINT(F("No fitting mode found. Defaulting to error state!"));
      seg.fill_solid((uint32_t)0xFF0000);
      // seg.setColor(0, (uint32_t)0xFF0000);
      // seg.setMode(FX_MODE_STATIC);
    }

    break;
  }
  // DEBUG_PRINT(F("New mode id: "));
  // DEBUG_PRINTLN(seg.currentMode());

  return FRAMETIME;
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
        DynamicJsonDocument doc(1024);
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
    strip.addEffect(WEATHER_EFFECT_ID, &mode_weather, "Weather@!;<empty>;;01");
    strip.addEffect(WeatherAnimation::ANIM_CLEAR, &mode_clear_sky,
                    "WeatherClearSky@!;<empty>;;01");
    strip.addEffect(WeatherAnimation::ANIM_PARTLY_CLOUDY, &mode_partly_cloudy,
                    "WeatherPartlyCloudy@!;<empty>;;01");
    strip.addEffect(WeatherAnimation::ANIM_CLOUDY, &mode_cloudy,
                    "WeatherCloudy@!;<empty>;;01");
    strip.addEffect(WeatherAnimation::ANIM_RAIN, &mode_raining,
                    "WeatherRain@!;<empty>;;01");
    strip.addEffect(WeatherAnimation::ANIM_SNOW, &mode_snow,
                    "WeatherSnow@!;<empty>;;01");
    strip.addEffect(WeatherAnimation::ANIM_NIGHT, &mode_night,
                    "WeatherNight@!;<empty>;;01");
    strip.addEffect(WeatherAnimation::ANIM_WIND, &mode_wind,
                    "WeatherWind@!;<empty>;;01");
    strip.addEffect(WeatherAnimation::ANIM_THUNDERSTORM, &mode_thunderstorm,
                    "WeatherThunderstorm@!;<empty>;;01");
    strip.addEffect(WeatherAnimation::ANIM_FOG, &mode_fog,
                    "WeatherFog@!;<empty>;;01");
    strip.addEffect(WeatherAnimation::ANIM_HAIL, &mode_hail,
                    "WeatherHail@!;<empty>;;01");

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

      updateWeather();
      // showAnimation();
      delay(100); // Slow down updates to the LEDs
    }
  }

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
