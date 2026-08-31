#include "Core.h"

#include <cmath>

#include "Client.h"
#include "FpsRenderer.h"
#include "GameOptions.h"
#include "Keyboard.h"
#include "SpriteManager.h"
#include "Timer.h"

namespace
{
    const float FPS_PI = 3.14159265358979323846f;
    const float FPS_TWO_PI = FPS_PI * 2.0f;
    const float FPS_FOV = 70.0f * FPS_PI / 180.0f;
    const float FPS_TURN_SPEED = 2.25f; // radians / second
    const float FPS_MAX_VIEW_DISTANCE = 64.0f;
    const int   FPS_COLUMN_WIDTH = 2;

    struct FpsCameraState
    {
        bool   Initialized;
        float  Yaw;
        uint   LastTick;
        ushort LastHexX;
        ushort LastHexY;

        FpsCameraState() : Initialized( false ), Yaw( 0.0f ), LastTick( 0 ), LastHexX( 0 ), LastHexY( 0 ) {}
    };

    FpsCameraState Camera;

    float NormalizeAngle( float angle )
    {
        while( angle < 0.0f )
            angle += FPS_TWO_PI;
        while( angle >= FPS_TWO_PI )
            angle -= FPS_TWO_PI;
        return angle;
    }

    bool IsBlockingCell( HexManager& map, int hx, int hy )
    {
        if( hx < 0 || hy < 0 || hx >= (int)map.GetMaxHexX() || hy >= (int)map.GetMaxHexY() )
            return true;

        Field& field = map.GetField( (ushort)hx, (ushort)hy );
        if( field.IsWall )
            return true;
        if( !field.IsNotPassed )
            return false;

        // Field::IsNotPassed also includes critter occupancy. Critters will be
        // rendered as billboards, not as floor-to-ceiling map walls. Keep
        // blocking map items and block-line cells solid.
        for( auto it = field.Items.begin(), end = field.Items.end(); it != end; ++it )
        {
            ItemHex* item = *it;
            if( item && !item->IsPassed() )
                return true;
        }

        if( field.Crit || field.IsMultihex )
            return false;

        // No critter and no local blocking item means this is normally a block
        // line projected into the cell by the map collision cache.
        return true;
    }

    uint WallColor( float distance, bool side, bool scenery )
    {
        int light = 224 - (int)(distance * 7.0f);
        light = CLAMP( light, 42, 224 );

        if( side )
            light = light * 4 / 5;

        if( scenery )
            return COLOR_ARGB( 255, light * 4 / 5, light, light * 3 / 5 );

        return COLOR_ARGB( 255, light, light * 9 / 10, light * 3 / 4 );
    }

    void AddRect( PointVec& points, int left, int top, int right, int bottom, uint color )
    {
        if( right <= left || bottom <= top )
            return;

        SprMngr.PrepareSquare( points, Rect( left, top, right, bottom ), color );
    }

    void DrawCrosshair( PointVec& points, int width, int height )
    {
        const int cx = width / 2;
        const int cy = height / 2;
        const uint color = COLOR_ARGB( 220, 235, 235, 220 );

        AddRect( points, cx - 8, cy - 1, cx - 2, cy + 1, color );
        AddRect( points, cx + 2, cy - 1, cx + 8, cy + 1, color );
        AddRect( points, cx - 1, cy - 8, cx + 1, cy - 2, color );
        AddRect( points, cx - 1, cy + 2, cx + 1, cy + 8, color );
    }
}

namespace FpsRenderer
{
    void DrawGame( FOClient& client )
    {
        const int screen_width = MODE_WIDTH;
        const int screen_height = MODE_HEIGHT;
        if( screen_width <= 0 || screen_height <= 0 )
            return;

        PointVec geometry;
        geometry.reserve( (screen_width / FPS_COLUMN_WIDTH + 8) * 6 );

        // Doom-style fixed horizon: no isometric map is drawn underneath this.
        const int horizon = screen_height / 2;
        AddRect( geometry, 0, 0, screen_width, horizon, COLOR_ARGB( 255, 48, 52, 58 ) );
        AddRect( geometry, 0, horizon, screen_width, screen_height, COLOR_ARGB( 255, 64, 57, 48 ) );

        CritterCl* chosen = client.Chosen;
        if( !chosen || !client.HexMngr.IsMapLoaded() )
        {
            DrawCrosshair( geometry, screen_width, screen_height );
            SprMngr.DrawPoints( geometry, DRAW_PRIMITIVE_TRIANGLELIST );
            return;
        }

        const uint now = Timer::GameTick();
        if( !Camera.Initialized )
        {
            Camera.Initialized = true;
            Camera.LastTick = now;
            Camera.LastHexX = chosen->GetHexX();
            Camera.LastHexY = chosen->GetHexY();

            const int dirs = max( 1, DIRS_COUNT );
            Camera.Yaw = NormalizeAngle( (float)chosen->GetDir() * FPS_TWO_PI / (float)dirs );
        }

        float dt = (float)(now - Camera.LastTick) / 1000.0f;
        Camera.LastTick = now;
        dt = CLAMP( dt, 0.0f, 0.10f );

        // Classic Doom keyboard turning for the camera pass. Keeping A/D out
        // of this first step avoids colliding with FOClassic's existing game
        // hotkeys until the FPS input layer replaces them wholesale.
        if( Keyb::KeyPressed[DIK_LEFT] )
            Camera.Yaw -= FPS_TURN_SPEED * dt;
        if( Keyb::KeyPressed[DIK_RIGHT] )
            Camera.Yaw += FPS_TURN_SPEED * dt;
        Camera.Yaw = NormalizeAngle( Camera.Yaw );

        // Camera always follows the authoritative chosen critter position.
        Camera.LastHexX = chosen->GetHexX();
        Camera.LastHexY = chosen->GetHexY();
        const float pos_x = (float)Camera.LastHexX + 0.5f;
        const float pos_y = (float)Camera.LastHexY + 0.5f;

        for( int column = 0; column < screen_width; column += FPS_COLUMN_WIDTH )
        {
            const float camera_x = ( (float)column + 0.5f ) / (float)screen_width;
            const float ray_angle = Camera.Yaw - FPS_FOV * 0.5f + camera_x * FPS_FOV;
            const float ray_dir_x = (float)cos( (double)ray_angle );
            const float ray_dir_y = (float)sin( (double)ray_angle );

            int map_x = (int)floor( (double)pos_x );
            int map_y = (int)floor( (double)pos_y );

            const float huge = 1.0e30f;
            const float abs_ray_x = (float)fabs( (double)ray_dir_x );
            const float abs_ray_y = (float)fabs( (double)ray_dir_y );
            const float delta_x = abs_ray_x < 0.00001f ? huge : (float)fabs( 1.0 / (double)ray_dir_x );
            const float delta_y = abs_ray_y < 0.00001f ? huge : (float)fabs( 1.0 / (double)ray_dir_y );

            const int step_x = ray_dir_x < 0.0f ? -1 : 1;
            const int step_y = ray_dir_y < 0.0f ? -1 : 1;

            float side_x = ray_dir_x < 0.0f ? (pos_x - (float)map_x) * delta_x : ((float)map_x + 1.0f - pos_x) * delta_x;
            float side_y = ray_dir_y < 0.0f ? (pos_y - (float)map_y) * delta_y : ((float)map_y + 1.0f - pos_y) * delta_y;

            bool hit = false;
            bool side = false;
            bool scenery = false;
            float distance = FPS_MAX_VIEW_DISTANCE;

            for( int steps = 0; steps < 256; ++steps )
            {
                if( side_x < side_y )
                {
                    distance = side_x;
                    side_x += delta_x;
                    map_x += step_x;
                    side = false;
                }
                else
                {
                    distance = side_y;
                    side_y += delta_y;
                    map_y += step_y;
                    side = true;
                }

                if( distance > FPS_MAX_VIEW_DISTANCE )
                    break;

                if( IsBlockingCell( client.HexMngr, map_x, map_y ) )
                {
                    hit = true;
                    if( map_x >= 0 && map_y >= 0 && map_x < (int)client.HexMngr.GetMaxHexX() && map_y < (int)client.HexMngr.GetMaxHexY() )
                        scenery = client.HexMngr.GetField( (ushort)map_x, (ushort)map_y ).IsScen;
                    break;
                }
            }

            if( !hit )
                continue;

            // Correct fish-eye distortion caused by measuring along the ray.
            distance *= (float)cos( (double)(ray_angle - Camera.Yaw) );
            distance = max( distance, 0.05f );

            int wall_height = (int)( (float)screen_height * 0.90f / distance );
            wall_height = min( wall_height, screen_height * 4 );

            int top = horizon - wall_height / 2;
            int bottom = horizon + wall_height / 2;
            top = max( top, 0 );
            bottom = min( bottom, screen_height );

            AddRect( geometry, column, top, min( column + FPS_COLUMN_WIDTH, screen_width ), bottom, WallColor( distance, side, scenery ) );
        }

        DrawCrosshair( geometry, screen_width, screen_height );
        SprMngr.DrawPoints( geometry, DRAW_PRIMITIVE_TRIANGLELIST );
    }
}
