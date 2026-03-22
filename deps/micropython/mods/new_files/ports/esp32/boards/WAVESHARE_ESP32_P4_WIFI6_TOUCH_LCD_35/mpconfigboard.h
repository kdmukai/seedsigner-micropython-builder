#define MICROPY_HW_BOARD_NAME               "Waveshare ESP32-P4 WiFi6 Touch LCD 3.5"
#define MICROPY_HW_MCU_NAME                 "ESP32P4"

// Keep UART REPL enabled for bring-up/fallback.
#define MICROPY_HW_ENABLE_UART_REPL         (1)

// Default I2C pins for MicroPython machine.I2C(0).
#define MICROPY_HW_I2C0_SCL                 (8)
#define MICROPY_HW_I2C0_SDA                 (7)

// Disable networking (P4 has no built-in WiFi/BT;
// external WiFi6 module is unused for SeedSigner).
#define MICROPY_PY_NETWORK                  (0)
#define MICROPY_PY_NETWORK_WLAN             (0)
#define MICROPY_PY_NETWORK_LAN              (0)
#define MICROPY_PY_NETWORK_PPP_LWIP         (0)
#define MICROPY_PY_SOCKET                   (0)

// Disable Bluetooth and ESP-NOW support.
#define MICROPY_PY_BLUETOOTH               (0)
#define MICROPY_BLUETOOTH_NIMBLE           (0)
#define MICROPY_PY_ESPNOW                  (0)

// Initialize display at C-level boot (before REPL).
extern void seedsigner_board_startup(void);
#define MICROPY_BOARD_STARTUP seedsigner_board_startup
