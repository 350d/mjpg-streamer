#ifndef INPUT_AVF_H
#define INPUT_AVF_H

#include "../../mjpg_streamer.h"

// Function declarations
int input_init(input_parameter *param, int id);
void input_cleanup(int id);

#endif /* INPUT_AVF_H */
