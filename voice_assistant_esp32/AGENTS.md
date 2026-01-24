# AGENTS.md

This guide is for agentic coding agents contributing to this ESP32 Arduino project. It covers:
- Reliable build/upload/monitor commands (using arduino-cli)
- How to manage hardware dependencies and known incompatibilities
- Detailed code style guidelines (imports, formatting, types, naming, error handling)
- Best practices for maintainability and collaboration

## 1. BUILD / UPLOAD / MONITOR COMMANDS

**Environment:**
- Target board: `esp32:esp32:esp32s3` (ESP32-S3 core)
- Sketch: `voice_assistant_esp32.ino`

### Build firmware
- **Compile for ESP32-S3:**
  ```sh
  arduino-cli compile --fqbn esp32:esp32:esp32s3 voice_assistant_esp32.ino
  ```

### Upload to device
- **Upload (default port COM3):**
  ```sh
  arduino-cli upload -p COM3 --fqbn esp32:esp32:esp32s3 voice_assistant_esp32.ino
  ```
  *NOTE:* Change `COM3` to the correct port if necessary—use `arduino-cli board list` to find connected devices.

### Serial Monitor
- **Monitor Serial Output:**
  ```sh
  arduino-cli monitor -p COM3
  ```

### Library Management
- **Add library from Arduino registry:**
  ```sh
  arduino-cli lib install "<LibraryName>"
  ```
- *Manual libraries:* Some (Freenove, patched drivers, or Espressif) libraries may require manual `.zip` install or direct folder copy to Arduino `libraries/`. Always document any manual steps and confirm version compatibility.

### Testing
- **Unit/integration test support:**
  This codebase does *not* currently implement automated unit/integration tests. If adding tests:
  - Place them in a `test/` directory following Arduino test framework guidelines.
  - Run:  
    ```sh
    arduino-cli test -e esp32:esp32:esp32s3
    ```
  - *Note:* Arduino does not natively support granular per-function test execution. Use descriptive test file/method names and isolate tests by file/suite.

---

## 2. LIBRARY & HARDWARE COMPATIBILITY

- **Known Issue:** This project uses both Freenove and Espressif libraries, which can be incompatible due to API conflicts or different hardware assumptions.
- **Agent guidelines:**
  - NEVER blindly upgrade or mix Freenove/Espressif core or driver libraries.
  - Always document the version and known source of each non-Arduino registry library, especially if installing from `.zip` or GitHub/source.
  - If adding/changing a driver, ensure it works with *this* board (`esp32s3`) and document any nonstandard pinout/schematic.
  - Coordinate any hardware-dependent or cross-vendor changes with maintainers and document thoroughly in the sketch and README.

---

## 3. CODE STYLE & PRACTICES

### Imports & Includes
- Use `#include <...>` for official libraries/platforms.
- Use `#include "..."` for project-local headers.
- All includes go at top of source/header files.

### Formatting
- Indent with 2 or 4 spaces (match existing file style).
- Opening braces on same line as statement.
- Space after keywords, none before parentheses.
- Lines should fit <=100 chars.
- Single blank line to separate logical sections/functions.
- Trim trailing whitespace.

### Naming Conventions
- **Classes:** PascalCase (e.g. `Display`, `Button`)
- **Member variables:** lower_snake_case or trailing underscore (`foo_bar` or `fooBar_`)
- **Constants, Macros, Pins:** ALL_CAPS_SNAKE (e.g. `TFT_BL`, `BUTTON_PIN`)
- **Global drivers/hardware:** `extern YourType instance;` in .h, definition in one .cpp

### Typing & Qualifiers
- Use explicit types (`uint8_t`, `int16_t`, etc.)
- Prefer `const` for function arguments, variables, pointers that shouldn't change.
- Use `static`, `extern`, or anonymous namespaces for appropriate linkage.

### Functions
- Document in .h with purpose and parameters, especially for public/member API.
- In headers: Include full type for every argument.
- Class methods: `public:` then `protected:`, then `private:`

### Error Handling
- Use Arduino/embedded convention: fail softly or print error (`Serial.print`), or enter fatal loop (`while (1) delay(1000);`).
- Avoid C++ exceptions.
- Make all error/failure cases explicit in comments or docstring.

### Documentation
- Comment every new class, function, constant with Doxygen or Markdown-style block.
- Update README or GEMINI.md if hardware/build setup is changed.

### File & Macro Organization
- Each complex driver/class in its own .h/.cpp pair. Prefix internal headers with `_` or use internal/ folder if non-API.
- Use standard Arduino header guard format: `#ifndef DRIVER_BUTTON_H` ... `#endif`
- Place all hardware mapping macros at top of file.

### Best Practices
- Minimize stateful/global behavior. Use `extern` only for drivers needed project-wide.
- Avoid nonstandard/untested hardware routines unless absolutely required.
- Modularize unrelated features/drivers.
- When adding/changing drivers, prefer backward-compatible changes and document clearly.

---

## 4. GENERAL CONTRIBUTION ETIQUETTE
- Always:
  - Update relevant docs for code/hardware changes.
  - Minimize new dependencies and use only open-source, Arduino-compatible libraries (unless given prior approval).
  - Leave clear, concise and self-explanatory commits.
- If adding new hardware/board support, document pinouts and config changes in README and AGENTS.md.

---
Happy hacking, agents! Follow these best practices for clear, robust, and portable embedded code.
