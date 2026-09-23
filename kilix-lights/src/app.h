/* app.h - interaction controller and full-color scene renderer. */
#ifndef KILIX_LIGHTS_APP_H
#define KILIX_LIGHTS_APP_H

#include "canvas.h"
#include "game.h"
#include "input.h"

typedef enum AppAction {
    APP_CONTINUE = 0,
    APP_QUIT
} AppAction;

typedef struct App {
    Game game;
    int hover_target;
    int captured_target;
    int mouse_x;
    int mouse_y;
    bool mouse_in_view;
} App;

void app_init(App *app);
AppAction app_handle(App *app, const input_event *event);
void app_update(App *app, double dt);
void app_draw(const App *app, Canvas *canvas);

/* Test-facing exact hit lookup. Cell targets are 0..48; controls >= 100. */
int app_target_at(const App *app, int x, int y);
bool app_interaction_selftest(void);

#endif
