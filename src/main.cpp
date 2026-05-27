#include "Game.h"

#include "raylib.h"

int main()
{
    Game game;
    if (!game.Initialize())
    {
        return 1;
    }

    while (!WindowShouldClose() && !game.ShouldClose())
    {
        game.HandleInput();
        game.Update(GetFrameTime());
        game.Render();
    }

    game.Shutdown();
    return 0;
}
