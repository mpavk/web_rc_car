#ifndef PWM_CONTROL_H
#define PWM_CONTROL_H

#include <gpiod.h>

// Ініціалізація і зупинка PWM
bool init_software_pwm();
void cleanup_pwm();

// Перевірка статусу PWM
bool is_pwm_initialized();

// Установити швидкість для каналів A і B (0–100%)
void set_speed_A(int speed_percent);
void set_speed_B(int speed_percent);

#endif // PWM_CONTROL_H
