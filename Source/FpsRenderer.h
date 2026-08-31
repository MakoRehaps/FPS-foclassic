#ifndef __FPS_RENDERER__
#define __FPS_RENDERER__

class FOClient;

namespace FpsRenderer
{
    // True while the player is in the unobstructed in-map FPS view.
    bool IsCaptured( FOClient& client );

    // Intercepts gameplay-only keys/clicks. Returning true prevents the
    // original isometric cursor action from also running.
    bool KeyDown( FOClient& client, unsigned char dik );
    bool MouseButton( FOClient& client, int button, bool down );

    // Replaces FOClassic's isometric in-map renderer and also advances the
    // continuous first-person input state once per rendered game frame.
    void DrawGame( FOClient& client );
}

#endif // __FPS_RENDERER__
