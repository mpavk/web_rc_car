#include "pwm_control.h"
#include <pigpio.h>
#include <glib.h>

// Апара́тний PWM на базі pigpio
#define PWM_ENABLE_A 13  // GPIO13 для двигуна A
#define PWM_ENABLE_B 12  // GPIO12 для двигуна B
#define PWM_FREQUENCY 1000  // 1 kHz

bool init_software_pwm() {
    if (gpioInitialise() < 0) {
        g_printerr("[ERROR] init_software_pwm: pigpio initialization failed\n");
        return false;
    }
    // Налаштування на апаратний PWM
    gpioSetMode(PWM_ENABLE_A, PI_OUTPUT);
    gpioSetMode(PWM_ENABLE_B, PI_OUTPUT);
    g_print("[LOG] init_software_pwm: pigpio initialized, hardware PWM ready\n");
    return true;
}

void cleanup_pwm() {
    // Зупинити PWM
    gpioHardwarePWM(PWM_ENABLE_A, 0, 0);
    gpioHardwarePWM(PWM_ENABLE_B, 0, 0);
    gpioTerminate();
    g_print("[LOG] cleanup_pwm: pigpio terminated\n");
}

bool is_pwm_initialized() {
    // Повертаємо true, якщо pigpio ініціалізувався
    return (gpioInitialise() >= 0);
}

void set_speed_A(int speed_percent) {
    if (speed_percent < 0) speed_percent = 0;
    if (speed_percent > 100) speed_percent = 100;
    // dutycycle у pigpio: 0-1e6
    gpioHardwarePWM(PWM_ENABLE_A, PWM_FREQUENCY, speed_percent * 10000);
    g_print("[DEBUG] PWM A (GPIO13) hardware speed set to %d%%\n", speed_percent);
}

void set_speed_B(int speed_percent) {
    if (speed_percent < 0) speed_percent = 0;
    if (speed_percent > 100) speed_percent = 100;
    gpioHardwarePWM(PWM_ENABLE_B, PWM_FREQUENCY, speed_percent * 10000);
    g_print("[DEBUG] PWM B (GPIO12) hardware speed set to %d%%\n", speed_percent);
}
