#ifndef __FPS_RENDERER__
#define __FPS_RENDERER__

class FOClient;

namespace FpsRenderer
{
    // Replaces FOClassic's isometric in-map renderer.
    // Menus and HUD still use the existing interface renderer.
    void DrawGame( FOClient& client );
}

#endif // __FPS_RENDERER__
