#ifndef __DISPLAY_H
#define __DISPLAY_H

#include "lvgl.h"
#include "TFT_eSPI.h"
#include "driver_button.h"

#define TFT_BL 20
#define BUTTON_PIN 19
#define TFT_DIRECTION 1           // Define the direction of the TFT display
extern lv_indev_t* indev_keypad;  // External declaration of the keypad input device
// Class to handle display operations
class Display {
private:
    int tft_show_dirction;  // Non-static member variable for display direction

public:
    // Function to initialize the display
    // Pointers to LVGL objects we create so we can update/remove them later
    lv_obj_t* boot_label = nullptr;
    lv_obj_t* transcription_label = nullptr;
    void init(int screenDir);

    // Function to handle routine display tasks
    void routine();

    // Show a small instruction label at the top of the screen
    void showBootInstructions(const char* text);

    // Show/hide the small boot instruction banner
    void hideBootInstructions();

    // Display transcription text with word-wrapping (no scrolling).
    void showTranscription(const char* text);
    void clearTranscription();

    // Getter for tft_show_dirction
    int getTftShowDirection() const { return tft_show_dirction; }

    // Setter for tft_show_dirction
    void setTftShowDirection(int direction) { tft_show_dirction = direction; }
};
// Global display instance (defined in display.cpp)
extern Display display;

#endif