#ifndef GPIO_CONTROL_H
#define GPIO_CONTROL_H

void init_motor_control();
void cleanup_motor_control();
void stop_vehicle();
void control_vehicle(const char *direction, const char *turn, int speed_percent);

#endif // GPIO_CONTROL_H
