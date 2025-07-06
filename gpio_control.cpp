#include "gpio_control.h"
#include "pwm_control.h"
#include <gpiod.h>
#include <glib.h>
#include <cstring>

#define MOTOR_IN1_BACK  23
#define MOTOR_IN2_FWD   18
#define MOTOR_IN3_LEFT  25
#define MOTOR_IN4_RIGHT 24

static struct gpiod_chip *chip = nullptr;
static struct gpiod_line *line_IN1 = nullptr;
static struct gpiod_line *line_IN2 = nullptr;
static struct gpiod_line *line_IN3 = nullptr;
static struct gpiod_line *line_IN4 = nullptr;
static const char *CONSUMER = "vehicle_control";

// Helper to set line with debug
static void set_line(struct gpiod_line *line, int value, const char* name) {
    int ret = gpiod_line_set_value(line, value);
    if (ret < 0) {
        g_printerr("[ERROR] Setting %s to %d failed: %d\n", name, value, ret);
    } else {
        g_print("[DEBUG] %s set to %d\n", name, value);
    }
}

void init_motor_control() {
    chip = gpiod_chip_open_by_name("gpiochip0");
    if (!chip) {
        g_printerr("[ERROR] init_motor_control: Cannot open gpiochip0\n");
        return;
    }
    line_IN1 = gpiod_chip_get_line(chip, MOTOR_IN1_BACK);
    line_IN2 = gpiod_chip_get_line(chip, MOTOR_IN2_FWD);
    line_IN3 = gpiod_chip_get_line(chip, MOTOR_IN3_LEFT);
    line_IN4 = gpiod_chip_get_line(chip, MOTOR_IN4_RIGHT);

    if (!line_IN1 || !line_IN2 || !line_IN3 || !line_IN4) {
        g_printerr("[ERROR] init_motor_control: Cannot get GPIO lines\n");
        return;
    }

    if (gpiod_line_request_output(line_IN1, CONSUMER, 0) < 0 ||
        gpiod_line_request_output(line_IN2, CONSUMER, 0) < 0 ||
        gpiod_line_request_output(line_IN3, CONSUMER, 0) < 0 ||
        gpiod_line_request_output(line_IN4, CONSUMER, 0) < 0) {
        g_printerr("[ERROR] init_motor_control: Failed to request GPIO lines as output\n");
        return;
    }

    // Ensure PWM subsystem is initialized
    if (!init_software_pwm()) {
        g_printerr("[ERROR] init_motor_control: PWM init failed\n");
        return;
    }

    g_print("[LOG] init_motor_control: Initialization complete\n");
}

void cleanup_motor_control() {
    if (!chip) return;
    stop_vehicle();
    gpiod_line_release(line_IN1);
    gpiod_line_release(line_IN2);
    gpiod_line_release(line_IN3);
    gpiod_line_release(line_IN4);
    gpiod_chip_close(chip);
    chip = nullptr;
    cleanup_pwm();
    g_print("[LOG] cleanup_motor_control: GPIO and PWM cleaned up\n");
}

void stop_vehicle() {
    if (!chip) return;
    set_line(line_IN1, 0, "IN1_BACK");
    set_line(line_IN2, 0, "IN2_FWD");
    set_line(line_IN3, 0, "IN3_LEFT");
    set_line(line_IN4, 0, "IN4_RIGHT");
    set_speed_A(0);
    set_speed_B(0);
    g_print("[LOG] stop_vehicle: Vehicle stopped\n");
}

void control_vehicle(const char *direction, const char *turn, int speed_percent) {
    if (!chip) {
        g_printerr("[WARN] control_vehicle: GPIO not initialized\n");
        return;
    }

    // --- Нова, надійна логіка ---

    // 1. Визначаємо, чи є команди на рух
    bool is_forward  = direction && std::strcmp(direction, "forward")  == 0;
    bool is_backward = direction && std::strcmp(direction, "backward") == 0;
    bool is_left     = turn      && std::strcmp(turn,      "left")      == 0;
    bool is_right    = turn      && std::strcmp(turn,      "right")     == 0;

    // 2. Розраховуємо фінальний стан для кожного мотора

    // Мотор A (вперед/назад)
    if (is_forward) {
        set_line(line_IN2, 1, "IN2_FWD");
        set_line(line_IN1, 0, "IN1_BACK");
        set_speed_A(speed_percent);
    } else if (is_backward) {
        set_line(line_IN1, 1, "IN1_BACK");
        set_line(line_IN2, 0, "IN2_FWD");
        set_speed_A(speed_percent);
    } else {
        // Якщо немає команди на рух вперед/назад, зупиняємо мотор А
        set_line(line_IN1, 0, "IN1_BACK");
        set_line(line_IN2, 0, "IN2_FWD");
        set_speed_A(0);
    }

    // Мотор B (вліво/вправо)
    if (is_left) {
        set_line(line_IN3, 1, "IN3_LEFT");
        set_line(line_IN4, 0, "IN4_RIGHT");
        set_speed_B(100); // Поворот завжди на повній швидкості
    } else if (is_right) {
        set_line(line_IN4, 1, "IN4_RIGHT");
        set_line(line_IN3, 0, "IN3_LEFT");
        set_speed_B(100);
    } else {
        // Якщо немає команди на поворот, зупиняємо мотор B
        set_line(line_IN3, 0, "IN3_LEFT");
        set_line(line_IN4, 0, "IN4_RIGHT");
        set_speed_B(0);
    }

    g_print("[LOG] control_vehicle: Fwd:%d, Bwd:%d, Left:%d, Right:%d -> SpeedA:%d, SpeedB:%d\n",
            is_forward, is_backward, is_left, is_right,
            (is_forward || is_backward) ? speed_percent : 0,
            (is_left || is_right) ? 100 : 0);
}
