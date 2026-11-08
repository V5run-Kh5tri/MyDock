#include "App.h"

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    MyDock::App app(instance);
    return app.Run();
}

