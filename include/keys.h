#ifndef KEYS_H
#define KEYS_H

#include <string>

enum Key { K_NONE = 0, K_OK, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ADJ };

// Key source: individual GPIO keys (active high, key between pin and 3.3V, 10k pull-down)
// or one SARADC channel with a resistor ladder.
//   order of pins: OK, UP, DOWN, LEFT, RIGHT, ADJ (-1 = not fitted)
bool keysStartGpio(const int pins[6]);
bool keysStartAdc(int channel);
void keysStop();

// Queue shared by the hardware scanner and the browser simulator.
void keysPush(Key k);
bool keysPop(Key& k);
bool keysPushByName(const std::string& name);  // "ok", "up", "down", "left", "right", "adj"

#endif // KEYS_H
