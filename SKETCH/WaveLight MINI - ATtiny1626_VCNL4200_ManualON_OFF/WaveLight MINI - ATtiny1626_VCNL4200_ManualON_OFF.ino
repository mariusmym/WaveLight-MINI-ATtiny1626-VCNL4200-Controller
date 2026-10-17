#include <tinyNeoPixel_Static.h>
#include <Vishay_VCNL4200.h>

#define NUMLEDS 44                // Number of LEDs in the strip
#define LED_PIN PIN_PA7
#define PROXIMITY_THRESHOLD 30    // 30cm ~ 180 ; 40cm ~ 120 ; 50cm ~ 70; 60cm ~ 40 ; 70cm ~ 26; 80cm ~ 20; 90cm ~ 15; 100cm ~ 12. 
#define TOUCH_THRESHOLD 1000      // Threshold for close proximity (a few mm)
#define AMBIENT_LIGHT_THRESHOLD 2 // Ambient light threshold | celling lights on ~ 11000+ ; no lights, TV on ~ 13; minimum value = 2
#define OVERRIDE_DURATION 60000   // 1 minute manual override in milliseconds

// Reducing the buffer size by storing only one color channel
byte pixels[NUMLEDS * 3];         // Use 3 bytes per pixel for WS2812B (RGB)
tinyNeoPixel leds = tinyNeoPixel(NUMLEDS, LED_PIN, NEO_GRB + NEO_KHZ800, pixels);

uint16_t prx;
uint16_t lux;                     // Using integer instead of float to save memory
bool ledsOn = false;              // Tracks if LEDs are currently on
bool manualOverride = false;      // Tracks if manual override mode is active
unsigned long lastTriggerTime = 0;
unsigned long overrideStartTime = 0;
const unsigned long timeoutDuration = 60000; // 1 minute in milliseconds
byte brightness = 100;            //brightness level for LEDs

void setup() {
  pinMode(LED_PIN, OUTPUT);
  vcnl4200.begin();
  leds.begin();
  delay(1000);
}

void loop() {
  // Read proximity sensor value
  vcnl4200.read_PRX(&prx);


  // Serial.print("PRX: "); Serial.println(prx);
  // Serial.print("LUX: "); Serial.println(lux);
  // Serial.print("LEDs On: "); Serial.println(ledsOn);

  // Handle manual override mode
  if (manualOverride) {
    if (millis() - overrideStartTime > OVERRIDE_DURATION) {
      manualOverride = false; // Exit manual override after 1 minute
    } else {
      // In manual override, toggle LEDs only with close proximity touches
      if (prx > TOUCH_THRESHOLD) {
        if (ledsOn) {
          turnOffLEDs();
          ledsOn = false;
        } else {
          turnOnLEDs();
          ledsOn = true;
        }
        delay(500); // Debounce delay for touch
      }
      return; // Skip the rest of the loop during manual override
    }
  }

  // Normal operation
  if (!ledsOn) {
    lux = (uint16_t)vcnl4200.get_lux(); // Using integer for lux
    if (lux < AMBIENT_LIGHT_THRESHOLD && prx > PROXIMITY_THRESHOLD) {
      turnOnLEDs();
      ledsOn = true;
      lastTriggerTime = millis(); // Start timing the LED on duration
    }
  }

  if (ledsOn) {
    if (millis() - lastTriggerTime > timeoutDuration) {
      turnOffLEDs();
      ledsOn = false;
    } else if (prx > TOUCH_THRESHOLD) {
      // Manual override if user touches the sensor while LEDs are on
      manualOverride = true;
      overrideStartTime = millis();
      turnOffLEDs();
      ledsOn = false;
      delay(500); // Stabilization delay
    }
  }

  delay(50); // Delay to avoid rapid checking
}

// Function to turn on the LED strip
void turnOnLEDs() {
  for (int i = 0; i < NUMLEDS; i++) {
    leds.setPixelColor(i, brightness, brightness, brightness); // Set LED color to white
  }
  leds.show();
}

// Function to turn off the LED strip
void turnOffLEDs() {
  for (int i = 0; i < NUMLEDS; i++) {
    leds.setPixelColor(i, 0, 0, 0); // Turning off the LEDs
  }
  leds.show();
}