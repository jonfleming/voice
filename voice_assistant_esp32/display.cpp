#include <TFT_eSPI.h>

#include "display.h"

#define BUTTON_PIN 19

lv_indev_t *indev_keypad; // External declaration of the keypad input device

// Define screen dimensions
#ifdef FNK0102A_1P14_135x240_ST7789
static const uint16_t screenWidth = 135;
static const uint16_t screenHeight = 240;
#elif defined FNK0102B_3P5_320x480_ST7796
static const uint16_t screenWidth = 320;
static const uint16_t screenHeight = 480;
#endif

// Buffer for drawing
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * screenHeight / 5];

// TFT instance
TFT_eSPI tft = TFT_eSPI(screenWidth, screenHeight);

// Button instance
Button button(BUTTON_PIN);

// Display instance
Display display;

#if LV_USE_LOG != 0
/* Serial debugging */
void my_print(const char *buf)
{
  Serial.printf(buf); // Print the buffer to the serial monitor
  Serial.flush();     // Ensure all data is sent
}
#endif

// Display flush function
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
  uint32_t w = (area->x2 - area->x1 + 1); // Calculate width of the area to flush
  uint32_t h = (area->y2 - area->y1 + 1); // Calculate height of the area to flush

  tft.startWrite();                                        // Start writing to the TFT
  tft.setAddrWindow(area->x1, area->y1, w, h);             // Set the address window for writing
  tft.pushColors((uint16_t *)&color_p->full, w * h, true); // Push colors to the TFT
  tft.endWrite();                                          // End writing to the TFT
  lv_disp_flush_ready(disp);                               // Inform LVGL that flushing is complete
}

// Keypad read function
void my_keypad_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  static int last_key = 0; // Static variable to store the last key value

  button.key_scan();                           // Scan the button state
  int buttonState = button.get_button_state(); // Get the current button state
  int act_key = button.get_button_key_value(); // Get the current button key value

  // Update the state based on the button state
  switch (buttonState)
  {
  case Button::KEY_STATE_PRESSED:
    data->state = LV_INDEV_STATE_PR; // Button is pressed
    break;
  case Button::KEY_STATE_RELEASED:
    data->state = LV_INDEV_STATE_REL; // Button is released
    break;
  }

  // Map button key values to LVGL key codes
  switch (act_key)
  {
  case 1:
    act_key = LV_KEY_ENTER; // Map to Enter key
    break;
  case 2:
    if (display.getTftShowDirection() == 0)
      act_key = LV_KEY_PREV;
    else if (display.getTftShowDirection() == 1)
      act_key = LV_KEY_LEFT;
    else if (display.getTftShowDirection() == 2)
      act_key = LV_KEY_NEXT;
    else if (display.getTftShowDirection() == 3)
      act_key = LV_KEY_RIGHT;
    break;
  case 3:
    if (display.getTftShowDirection() == 0)
      act_key = LV_KEY_NEXT;
    else if (display.getTftShowDirection() == 1)
      act_key = LV_KEY_RIGHT;
    else if (display.getTftShowDirection() == 2)
      act_key = LV_KEY_PREV;
    else if (display.getTftShowDirection() == 3)
      act_key = LV_KEY_LEFT;
    break;
  case 4:
    if (display.getTftShowDirection() == 0)
      act_key = LV_KEY_LEFT;
    else if (display.getTftShowDirection() == 1)
      act_key = LV_KEY_NEXT;
    else if (display.getTftShowDirection() == 2)
      act_key = LV_KEY_RIGHT;
    else if (display.getTftShowDirection() == 3)
      act_key = LV_KEY_PREV;
    break;
  case 5:
    if (display.getTftShowDirection() == 0)
      act_key = LV_KEY_RIGHT;
    else if (display.getTftShowDirection() == 1)
      act_key = LV_KEY_PREV;
    else if (display.getTftShowDirection() == 2)
      act_key = LV_KEY_LEFT;
    else if (display.getTftShowDirection() == 3)
      act_key = LV_KEY_NEXT;
    break;
  default:
    break;
  }
  last_key = act_key;   // Update the last key value
  data->key = last_key; // Set the key value in the input device data
}

void tftRst(void) {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);
  delay(50);
  digitalWrite(TFT_BL, HIGH);
  delay(50);
}

// Setup the TFT display
void setupTFT(int direction)
{
  tftRst();
  display.setTftShowDirection(direction);
  tft.begin();                                    // Initialize the TFT
  tft.setRotation(display.getTftShowDirection()); // Set the rotation of the TFT using the tft_show_dirction macro
}

// Setup the button
void setupButton()
{
  button.init(); // Initialize the button
}

// Setup LVGL
void setupLVGL()
{
#if LV_USE_LOG != 0
  lv_log_register_print_cb(my_print); // Register the print function for debugging
#endif
  lv_init(); // Initialize LVGL

  // Initialize the display buffer
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * screenHeight / 5);

  // Initialize the display driver
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);

  // Set the resolution based on the TFT direction
  switch (display.getTftShowDirection())
  {
  case 0: // Normal orientation
  case 2: // 180 degree rotation
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    break;
  case 1: // 90 degree rotation
  case 3: // 270 degree rotation
    disp_drv.hor_res = screenHeight;
    disp_drv.ver_res = screenWidth;
    break;
  default:
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    break;
  }

  disp_drv.flush_cb = my_disp_flush; // Set the flush callback
  disp_drv.draw_buf = &draw_buf;     // Set the draw buffer
  lv_disp_drv_register(&disp_drv);   // Register the display driver

  // Initialize the input device driver
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_KEYPAD;            // Set the input device type to keypad
  indev_drv.read_cb = my_keypad_read;               // Set the read callback
  indev_keypad = lv_indev_drv_register(&indev_drv); // Register the input device driver
}

// Initialize the display
void Display::init(int screenDir)
{
  setupTFT(screenDir); // Setup the TFT display
  setupButton();       // Setup the button
  setupLVGL();         // Setup LVGL
}

// Create a small instruction label at the top of the screen.
// Uses LVGL so the label is part of LV's object tree and will persist
// until you remove or hide it. Call after `Display::init()`.
void Display::showBootInstructions(const char* text)
{
  // If a boot label already exists, update text and make sure it's visible
  if (boot_label) {
    lv_label_set_text(boot_label, text);
    lv_obj_clear_flag(boot_label, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  // Create a label on the active screen and save the pointer
  boot_label = lv_label_create(lv_scr_act());
  lv_label_set_text(boot_label, text);
  lv_obj_set_width(boot_label, screenWidth);
  lv_obj_set_style_text_align(boot_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(boot_label, LV_ALIGN_TOP_MID, 0, 6);
}

void Display::hideBootInstructions()
{
  if (!boot_label) return;
  lv_obj_del(boot_label);
  boot_label = nullptr;
}

void Display::displayLine1(const char* text)
{
  // Remove existing transcription if present
  if (line1_label) {
    lv_label_set_text(line1_label, text);
    return;
  }

  // Create label, enable wrapping, and position below the boot label (or near top)
  line1_label = lv_label_create(lv_scr_act());
  lv_label_set_long_mode(line1_label, LV_LABEL_LONG_WRAP);
  // Allow some horizontal margin
  int margin = 12;
  lv_obj_set_width(line1_label, screenWidth - margin * 2);
  lv_label_set_text(line1_label, text);

  if (boot_label) {
    lv_obj_align_to(line1_label, boot_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
  } else {
    lv_obj_align(line1_label, LV_ALIGN_TOP_MID, 0, 36);
  }
}

void Display::displayLine2(const char* text)
{
  // Remove existing transcription if present
  if (line2_label) {
    lv_label_set_text(line2_label, text);
    // re-align because the height may have changed after updating text
    if (line1_label) lv_obj_align_to(line2_label, line1_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    return;
  }

  // Create label, enable wrapping, and position below line1_label (or near top)
  line2_label = lv_label_create(lv_scr_act());
  lv_label_set_long_mode(line2_label, LV_LABEL_LONG_WRAP);
  int margin = 12;
  lv_obj_set_width(line2_label, screenWidth - margin * 2);
  lv_label_set_text(line2_label, text);

  if (line1_label) {
    // place line2 just below line1 with 6px spacing
    lv_obj_align_to(line2_label, line1_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
  } else {
    // fallback position if line1 doesn't exist
    lv_obj_align(line2_label, LV_ALIGN_TOP_MID, 0, 60);
  }
}

void Display::clearLines()
{
  if (line1_label) {
    lv_obj_del(line1_label);
    line1_label = nullptr;
  }

  if (line2_label) {
    lv_obj_del(line2_label);
    line2_label = nullptr;
  }
}

// Handle routine display tasks
void Display::routine(void)
{
  lv_task_handler(); // Handle LVGL tasks
}
