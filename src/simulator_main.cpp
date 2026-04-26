#include <SDL2/SDL.h>
#include <unistd.h>

#include "HalDisplay.h"

extern void setup();
extern void loop();
extern HalDisplay display;

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  setup();
  while (!display.shouldQuit()) {
    loop();
    display.presentIfNeeded();
    SDL_Delay(1);
  }

  SDL_Quit();
  _exit(0);
}
