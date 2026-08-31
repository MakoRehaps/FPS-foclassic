#include "Core.h"

#include <cmath>

#include "Client.h"
#include "FpsRenderer.h"
#include "GameOptions.h"
#include "Keyboard.h"
#include "SpriteManager.h"
#include "Timer.h"
#include "Window.h"

namespace
{
    const float FPS_PI = 3.14159265358979323846f;
    const float FPS_TWO_PI = FPS_PI * 2.0f;
    const float FPS_FOV_NORMAL = 70.0f * FPS_PI / 180.0f;
    const float FPS_FOV_AIM = 50.0f * FPS_PI / 180.0f;
    const float FPS_MOUSE_SENS = 0.0027f;
    const float FPS_MOUSE_AIM_SCALE = 0.65f;
    const float FPS_MAX_PITCH = 0.52f;
    const float FPS_MAX_VIEW_DISTANCE = 64.0f;
    const int   FPS_COLUMN_WIDTH = 2;
    const uint  FPS_MOVE_REPEAT_MS = 65;
    const uint  FPS_FIRE_REPEAT_MS = 220;

    struct FpsCameraState
    {
        bool   Initialized;
        bool   MouseCaptured;
        bool   Aiming;
        bool   FireHeld;
        float  Yaw;
        float  Pitch;
        uint   LastTick;
        uint   NextMoveTick;
        uint   NextFireTick;
        ushort LastHexX;
        ushort LastHexY;

        FpsCameraState() : Initialized( false ), MouseCaptured( false ), Aiming( false ), FireHeld( false ),
            Yaw( 0.0f ), Pitch( 0.0f ), LastTick( 0 ), NextMoveTick( 0 ), NextFireTick( 0 ), LastHexX( 0 ), LastHexY( 0 ) {}
    };

    struct RayHit
    {
        bool Hit;
        bool Side;
        bool Scenery;
        float Distance;

        RayHit() : Hit( false ), Side( false ), Scenery( false ), Distance( FPS_MAX_VIEW_DISTANCE ) {}
    };

    struct Billboard
    {
        uint  SpriteId;
        float Forward;
        float Side;
        float Scale;
        uint  Color;

        Billboard( uint sprite_id, float forward, float side, float scale, uint color ) :
            SpriteId( sprite_id ), Forward( forward ), Side( side ), Scale( scale ), Color( color ) {}
    };

    FpsCameraState Camera;

    bool BillboardFarther( const Billboard& a, const Billboard& b )
    {
        return a.Forward > b.Forward;
    }

    float NormalizeAngle( float angle )
    {
        while( angle < 0.0f )
            angle += FPS_TWO_PI;
        while( angle >= FPS_TWO_PI )
            angle -= FPS_TWO_PI;
        return angle;
    }

    float CurrentFov()
    {
        return Camera.Aiming ? FPS_FOV_AIM : FPS_FOV_NORMAL;
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

        // Field::IsNotPassed also contains critter occupancy. Critters are
        // vertical billboards in FPS, not floor-to-ceiling map walls.
        for( auto it = field.Items.begin(), end = field.Items.end(); it != end; ++it )
        {
            ItemHex* item = *it;
            if( item && !item->IsPassed() )
                return true;
        }

        if( field.Crit || field.IsMultihex )
            return false;

        // A non-passed cell with no local item/critter is normally a projected
        // block-line cell from a scenery prototype.
        return true;
    }

    RayHit CastRay( HexManager& map, float pos_x, float pos_y, float ray_angle, float max_distance )
    {
        RayHit result;
        result.Distance = max_distance;

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

        for( int steps = 0; steps < 256; ++steps )
        {
            if( side_x < side_y )
            {
                result.Distance = side_x;
                side_x += delta_x;
                map_x += step_x;
                result.Side = false;
            }
            else
            {
                result.Distance = side_y;
                side_y += delta_y;
                map_y += step_y;
                result.Side = true;
            }

            if( result.Distance > max_distance )
            {
                result.Distance = max_distance;
                return result;
            }

            if( IsBlockingCell( map, map_x, map_y ) )
            {
                result.Hit = true;
                if( map_x >= 0 && map_y >= 0 && map_x < (int)map.GetMaxHexX() && map_y < (int)map.GetMaxHexY() )
                    result.Scenery = map.GetField( (ushort)map_x, (ushort)map_y ).IsScen;
                return result;
            }
        }

        result.Distance = max_distance;
        return result;
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

    void DrawCrosshair( PointVec& points, int width, int height, bool target )
    {
        const int cx = width / 2;
        const int cy = height / 2;
        const uint color = target ? COLOR_ARGB( 235, 255, 105, 90 ) : COLOR_ARGB( 220, 235, 235, 220 );
        const int spread = Camera.Aiming ? 1 : 3;

        AddRect( points, cx - 9, cy - 1, cx - 2 - spread, cy + 1, color );
        AddRect( points, cx + 2 + spread, cy - 1, cx + 9, cy + 1, color );
        AddRect( points, cx - 1, cy - 9, cx + 1, cy - 2 - spread, color );
        AddRect( points, cx - 1, cy + 2 + spread, cx + 1, cy + 9, color );
    }

    void InitializeCameraFromChosen( CritterCl* chosen )
    {
        Camera.Initialized = true;
        Camera.LastTick = Timer::GameTick();
        Camera.LastHexX = chosen->GetHexX();
        Camera.LastHexY = chosen->GetHexY();

        int tx = (int)chosen->GetHexX();
        int ty = (int)chosen->GetHexY();
        MoveHexByDirUnsafe( tx, ty, chosen->GetDir() );
        const float dx = (float)tx - (float)chosen->GetHexX();
        const float dy = (float)ty - (float)chosen->GetHexY();
        Camera.Yaw = NormalizeAngle( (float)atan2( (double)dy, (double)dx ) );
        Camera.Pitch = 0.0f;
    }

    int DirectionFromVector( CritterCl* chosen, float vx, float vy )
    {
        float best_dot = -1.0e30f;
        int best_dir = chosen->GetDir();
        const int ox = (int)chosen->GetHexX();
        const int oy = (int)chosen->GetHexY();

        for( int dir = 0; dir < DIRS_COUNT; ++dir )
        {
            int nx = ox;
            int ny = oy;
            MoveHexByDirUnsafe( nx, ny, (uchar)dir );
            float dx = (float)(nx - ox);
            float dy = (float)(ny - oy);
            float len = (float)sqrt( (double)(dx * dx + dy * dy) );
            if( len <= 0.0f )
                continue;
            dx /= len;
            dy /= len;
            const float dot = dx * vx + dy * vy;
            if( dot > best_dot )
            {
                best_dot = dot;
                best_dir = dir;
            }
        }
        return best_dir;
    }

    void RecenterMouse()
    {
        #ifdef FO_WINDOWS
        if( MainWindow && MainWindow->Focused )
        {
            const int sx = MainWindow->x() + MainWindow->w() / 2;
            const int sy = MainWindow->y() + MainWindow->h() / 2;
            ::SetCursorPos( sx, sy );
        }
        #endif
    }

    void UpdateMouseLook( FOClient& client )
    {
        if( !FpsRenderer::IsCaptured( client ) )
        {
            Camera.MouseCaptured = false;
            Camera.FireHeld = false;
            Camera.Aiming = false;
            return;
        }

        const int cx = MODE_WIDTH / 2;
        const int cy = MODE_HEIGHT / 2;
        if( !Camera.MouseCaptured )
        {
            Camera.MouseCaptured = true;
            RecenterMouse();
            GameOpt.MouseX = cx;
            GameOpt.MouseY = cy;
            return;
        }

        const int dx = GameOpt.MouseX - cx;
        const int dy = GameOpt.MouseY - cy;
        const float sensitivity = FPS_MOUSE_SENS * (Camera.Aiming ? FPS_MOUSE_AIM_SCALE : 1.0f);
        Camera.Yaw = NormalizeAngle( Camera.Yaw + (float)dx * sensitivity );
        Camera.Pitch += (float)dy * sensitivity;
        Camera.Pitch = CLAMP( Camera.Pitch, -FPS_MAX_PITCH, FPS_MAX_PITCH );

        #ifdef FO_WINDOWS
        RecenterMouse();
        GameOpt.MouseX = cx;
        GameOpt.MouseY = cy;
        #endif
    }

    bool TargetInCrosshair( float dx, float dy, float& forward, float& side, float tolerance )
    {
        const float cyaw = (float)cos( (double)Camera.Yaw );
        const float syaw = (float)sin( (double)Camera.Yaw );
        forward = dx * cyaw + dy * syaw;
        side = -dx * syaw + dy * cyaw;
        if( forward <= 0.05f )
            return false;
        return (float)fabs( (double)(side / forward) ) <= tolerance;
    }

    bool TargetVisible( FOClient& client, float pos_x, float pos_y, float target_x, float target_y )
    {
        const float dx = target_x - pos_x;
        const float dy = target_y - pos_y;
        const float distance = (float)sqrt( (double)(dx * dx + dy * dy) );
        if( distance <= 0.01f )
            return true;
        const float angle = (float)atan2( (double)dy, (double)dx );
        RayHit wall = CastRay( client.HexMngr, pos_x, pos_y, angle, distance + 0.75f );
        return !wall.Hit || wall.Distance + 0.75f >= distance;
    }

    CritterCl* FindCrosshairCritter( FOClient& client, float tolerance )
    {
        CritterCl* chosen = client.Chosen;
        if( !chosen )
            return NULL;

        const float pos_x = (float)chosen->GetHexX() + 0.5f;
        const float pos_y = (float)chosen->GetHexY() + 0.5f;
        CritterCl* best = NULL;
        float best_forward = 1.0e30f;

        CritMap& critters = client.HexMngr.GetCritters();
        for( auto it = critters.begin(), end = critters.end(); it != end; ++it )
        {
            CritterCl* cr = it->second;
            if( !cr || cr == chosen || cr->IsFinishing() || !cr->Visible )
                continue;

            const float tx = (float)cr->GetHexX() + 0.5f;
            const float ty = (float)cr->GetHexY() + 0.5f;
            float forward = 0.0f;
            float side = 0.0f;
            if( !TargetInCrosshair( tx - pos_x, ty - pos_y, forward, side, tolerance ) )
                continue;
            if( forward >= best_forward )
                continue;
            if( !TargetVisible( client, pos_x, pos_y, tx, ty ) )
                continue;

            best = cr;
            best_forward = forward;
        }
        return best;
    }

    ItemHex* FindCrosshairItem( FOClient& client, float tolerance, float max_distance )
    {
        CritterCl* chosen = client.Chosen;
        if( !chosen )
            return NULL;

        const float pos_x = (float)chosen->GetHexX() + 0.5f;
        const float pos_y = (float)chosen->GetHexY() + 0.5f;
        ItemHex* best = NULL;
        float best_forward = 1.0e30f;

        ItemHexVec& items = client.HexMngr.GetItems();
        for( auto it = items.begin(), end = items.end(); it != end; ++it )
        {
            ItemHex* item = *it;
            if( !item || item->IsFinishing() || item->IsHidden() || item->IsFullyTransparent() )
                continue;

            const float tx = (float)item->GetHexX() + 0.5f;
            const float ty = (float)item->GetHexY() + 0.5f;
            float forward = 0.0f;
            float side = 0.0f;
            if( !TargetInCrosshair( tx - pos_x, ty - pos_y, forward, side, tolerance ) )
                continue;
            if( forward > max_distance || forward >= best_forward )
                continue;
            if( !TargetVisible( client, pos_x, pos_y, tx, ty ) )
                continue;

            best = item;
            best_forward = forward;
        }
        return best;
    }

    void TryFire( FOClient& client )
    {
        CritterCl* chosen = client.Chosen;
        if( !chosen || !chosen->ItemSlotMain || !chosen->IsFree() )
            return;
        if( !client.ChosenAction.empty() )
            return;

        CritterCl* target = FindCrosshairCritter( client, Camera.Aiming ? 0.045f : 0.085f );
        if( !target )
            return;

        client.SetAction( CHOSEN_USE_ITEM,
                          chosen->ItemSlotMain->GetId(),
                          chosen->ItemSlotMain->GetProtoId(),
                          TARGET_CRITTER,
                          target->GetId(),
                          chosen->GetFullRate() );
    }

    void TryInteract( FOClient& client )
    {
        CritterCl* chosen = client.Chosen;
        if( !chosen || !chosen->IsFree() || !client.ChosenAction.empty() )
            return;

        const float use_dist = (float)max( 2u, chosen->GetUseDist() );
        CritterCl* cr = FindCrosshairCritter( client, 0.13f );
        if( cr && DistGame( chosen->GetHexX(), chosen->GetHexY(), cr->GetHexX(), cr->GetHexY() ) <= (uint)use_dist )
        {
            if( cr->IsCanTalk() )
                client.SetAction( CHOSEN_TALK_NPC, cr->GetId() );
            else
                client.SetAction( CHOSEN_PICK_CRITTER, cr->GetId(), 0 );
            return;
        }

        ItemHex* item = FindCrosshairItem( client, 0.16f, use_dist + 0.75f );
        if( !item )
            return;

        if( item->IsCanPickUp() )
        {
            client.SetAction( CHOSEN_PICK_ITEM, item->GetProtoId(), item->GetHexX(), item->GetHexY() );
        }
        else if( item->IsUsable() && chosen->ItemSlotMain )
        {
            client.SetAction( CHOSEN_USE_ITEM,
                              chosen->ItemSlotMain->GetId(),
                              chosen->ItemSlotMain->GetProtoId(),
                              item->IsItem() ? TARGET_ITEM : TARGET_SCENERY,
                              item->GetId(),
                              USE_USE );
        }
    }

    void ProcessMovement( FOClient& client )
    {
        CritterCl* chosen = client.Chosen;
        if( !chosen || !chosen->IsFree() || !client.ChosenAction.empty() )
            return;

        float forward_input = 0.0f;
        float strafe_input = 0.0f;
        if( Keyb::KeyPressed[DIK_W] ) forward_input += 1.0f;
        if( Keyb::KeyPressed[DIK_S] ) forward_input -= 1.0f;
        if( Keyb::KeyPressed[DIK_D] ) strafe_input += 1.0f;
        if( Keyb::KeyPressed[DIK_A] ) strafe_input -= 1.0f;
        if( forward_input == 0.0f && strafe_input == 0.0f )
            return;

        const uint now = Timer::FastTick();
        if( now < Camera.NextMoveTick )
            return;
        Camera.NextMoveTick = now + FPS_MOVE_REPEAT_MS;

        const float fx = (float)cos( (double)Camera.Yaw );
        const float fy = (float)sin( (double)Camera.Yaw );
        const float rx = -fy;
        const float ry = fx;
        float vx = fx * forward_input + rx * strafe_input;
        float vy = fy * forward_input + ry * strafe_input;
        const float len = (float)sqrt( (double)(vx * vx + vy * vy) );
        if( len <= 0.0f )
            return;
        vx /= len;
        vy /= len;

        const int dir = DirectionFromVector( chosen, vx, vy );
        ushort hx = chosen->GetHexX();
        ushort hy = chosen->GetHexY();
        if( !MoveHexByDir( hx, hy, (uchar)dir, client.HexMngr.GetMaxHexX(), client.HexMngr.GetMaxHexY() ) )
            return;

        client.SetAction( CHOSEN_MOVE, hx, hy, Keyb::ShiftDwn ? 1 : 0, 0, 0, Timer::FastTick() );
    }

    void ProcessContinuousInput( FOClient& client )
    {
        UpdateMouseLook( client );
        if( !FpsRenderer::IsCaptured( client ) )
            return;

        ProcessMovement( client );

        const uint now = Timer::FastTick();
        if( Camera.FireHeld && now >= Camera.NextFireTick )
        {
            Camera.NextFireTick = now + FPS_FIRE_REPEAT_MS;
            TryFire( client );
        }
    }

    void CollectBillboards( FOClient& client, FloatVec& wall_depth, int screen_width, int screen_height, int horizon, float fov )
    {
        CritterCl* chosen = client.Chosen;
        if( !chosen )
            return;

        const float pos_x = (float)chosen->GetHexX() + 0.5f;
        const float pos_y = (float)chosen->GetHexY() + 0.5f;
        const float cyaw = (float)cos( (double)Camera.Yaw );
        const float syaw = (float)sin( (double)Camera.Yaw );
        vector<Billboard> billboards;

        CritMap& critters = client.HexMngr.GetCritters();
        for( auto it = critters.begin(), end = critters.end(); it != end; ++it )
        {
            CritterCl* cr = it->second;
            if( !cr || cr == chosen || cr->IsFinishing() || !cr->Visible || !cr->SprId )
                continue;
            const float dx = (float)cr->GetHexX() + 0.5f - pos_x;
            const float dy = (float)cr->GetHexY() + 0.5f - pos_y;
            const float forward = dx * cyaw + dy * syaw;
            const float side = -dx * syaw + dy * cyaw;
            if( forward > 0.10f && forward < FPS_MAX_VIEW_DISTANCE )
                billboards.push_back( Billboard( cr->SprId, forward, side, 0.90f, 0 ) );
        }

        ItemHexVec& items = client.HexMngr.GetItems();
        for( auto it = items.begin(), end = items.end(); it != end; ++it )
        {
            ItemHex* item = *it;
            if( !item || item->IsFinishing() || item->IsHidden() || item->IsFullyTransparent() || !item->SprId )
                continue;
            const float dx = (float)item->GetHexX() + 0.5f - pos_x;
            const float dy = (float)item->GetHexY() + 0.5f - pos_y;
            const float forward = dx * cyaw + dy * syaw;
            const float side = -dx * syaw + dy * cyaw;
            if( forward <= 0.10f || forward >= FPS_MAX_VIEW_DISTANCE )
                continue;

            float scale = 0.34f;
            if( item->IsScenOrGrid() || item->IsWall() || item->IsContainer() || item->IsDoor() )
                scale = 0.78f;
            billboards.push_back( Billboard( item->SprId, forward, side, scale, 0 ) );
        }

        sort( billboards.begin(), billboards.end(), BillboardFarther );
        const float projection = (float)screen_width * 0.5f / (float)tan( (double)(fov * 0.5f) );

        for( auto it = billboards.begin(), end = billboards.end(); it != end; ++it )
        {
            Billboard& bb = *it;
            const float screen_xf = (float)screen_width * 0.5f + bb.Side / bb.Forward * projection;
            if( screen_xf < -(float)screen_width || screen_xf > (float)screen_width * 2.0f )
                continue;

            const int depth_x = CLAMP( (int)screen_xf, 0, screen_width - 1 );
            if( bb.Forward > wall_depth[depth_x] + 0.25f )
                continue;

            if( bb.SpriteId >= SprMngr.GetSpritesInfo().size() )
                continue;
            SpriteInfo* si = SprMngr.GetSpriteInfo( bb.SpriteId );
            if( !si || si->Width <= 0 || si->Height <= 0 )
                continue;

            float draw_h = (float)screen_height * bb.Scale / max( bb.Forward, 0.15f );
            draw_h = CLAMP( draw_h, 8.0f, (float)screen_height * 3.0f );
            float draw_w = draw_h * (float)si->Width / (float)si->Height;
            draw_w = min( draw_w, (float)screen_width * 2.5f );

            const int x = (int)(screen_xf - draw_w * 0.5f);
            const int y = (int)((float)horizon - draw_h * 0.5f);
            SprMngr.DrawSpriteSize( bb.SpriteId, x, y, draw_w, draw_h, true, true, bb.Color );
        }
    }
}

namespace FpsRenderer
{
    bool IsCaptured( FOClient& client )
    {
        return MainWindow && MainWindow->Focused &&
               client.IsMainScreen( CLIENT_MAIN_SCREEN_GAME ) &&
               client.HexMngr.IsMapLoaded() &&
               client.GetActiveScreen() == CLIENT_SCREEN_NONE &&
               !FOClient::ConsoleActive;
    }

    bool KeyDown( FOClient& client, unsigned char dik )
    {
        if( !IsCaptured( client ) )
            return false;

        switch( dik )
        {
            case DIK_W:
            case DIK_A:
            case DIK_S:
            case DIK_D:
                return true;
            case DIK_E:
                TryInteract( client );
                return true;
            default:
                return false;
        }
    }

    bool MouseButton( FOClient& client, int button, bool down )
    {
        if( !IsCaptured( client ) )
            return false;

        if( button == 0 )
        {
            Camera.FireHeld = down;
            if( down )
            {
                Camera.NextFireTick = Timer::FastTick() + FPS_FIRE_REPEAT_MS;
                TryFire( client );
            }
            return true;
        }
        if( button == 1 )
        {
            Camera.Aiming = down;
            return true;
        }
        return false;
    }

    void DrawGame( FOClient& client )
    {
        const int screen_width = MODE_WIDTH;
        const int screen_height = MODE_HEIGHT;
        if( screen_width <= 0 || screen_height <= 0 )
            return;

        CritterCl* chosen = client.Chosen;
        if( chosen && client.HexMngr.IsMapLoaded() && !Camera.Initialized )
            InitializeCameraFromChosen( chosen );

        ProcessContinuousInput( client );

        PointVec geometry;
        geometry.reserve( (screen_width / FPS_COLUMN_WIDTH + 16) * 6 );

        const float fov = CurrentFov();
        const float projection = (float)screen_width * 0.5f / (float)tan( (double)(fov * 0.5f) );
        int horizon = screen_height / 2 - (int)( (float)tan( (double)Camera.Pitch ) * projection );
        horizon = CLAMP( horizon, -screen_height, screen_height * 2 );

        AddRect( geometry, 0, 0, screen_width, CLAMP( horizon, 0, screen_height ), COLOR_ARGB( 255, 48, 52, 58 ) );
        AddRect( geometry, 0, CLAMP( horizon, 0, screen_height ), screen_width, screen_height, COLOR_ARGB( 255, 64, 57, 48 ) );

        if( !chosen || !client.HexMngr.IsMapLoaded() )
        {
            DrawCrosshair( geometry, screen_width, screen_height, false );
            SprMngr.DrawPoints( geometry, DRAW_PRIMITIVE_TRIANGLELIST );
            return;
        }

        const uint now = Timer::GameTick();
        float dt = (float)(now - Camera.LastTick) / 1000.0f;
        Camera.LastTick = now;
        dt = CLAMP( dt, 0.0f, 0.10f );

        // Arrow keys remain a keyboard turning fallback for mouse-less setups.
        if( Keyb::KeyPressed[DIK_LEFT] )
            Camera.Yaw -= 2.25f * dt;
        if( Keyb::KeyPressed[DIK_RIGHT] )
            Camera.Yaw += 2.25f * dt;
        Camera.Yaw = NormalizeAngle( Camera.Yaw );

        Camera.LastHexX = chosen->GetHexX();
        Camera.LastHexY = chosen->GetHexY();
        const float pos_x = (float)Camera.LastHexX + 0.5f;
        const float pos_y = (float)Camera.LastHexY + 0.5f;
        FloatVec wall_depth( screen_width, FPS_MAX_VIEW_DISTANCE );

        for( int column = 0; column < screen_width; column += FPS_COLUMN_WIDTH )
        {
            const float camera_x = ( (float)column + 0.5f ) / (float)screen_width;
            const float ray_angle = Camera.Yaw - fov * 0.5f + camera_x * fov;
            RayHit hit = CastRay( client.HexMngr, pos_x, pos_y, ray_angle, FPS_MAX_VIEW_DISTANCE );
            if( !hit.Hit )
                continue;

            float distance = hit.Distance * (float)cos( (double)(ray_angle - Camera.Yaw) );
            distance = max( distance, 0.05f );
            for( int x = column; x < min( column + FPS_COLUMN_WIDTH, screen_width ); ++x )
                wall_depth[x] = distance;

            int wall_height = (int)( (float)screen_height * 0.90f / distance );
            wall_height = min( wall_height, screen_height * 4 );
            int top = horizon - wall_height / 2;
            int bottom = horizon + wall_height / 2;
            top = max( top, 0 );
            bottom = min( bottom, screen_height );
            AddRect( geometry, column, top, min( column + FPS_COLUMN_WIDTH, screen_width ), bottom,
                     WallColor( distance, hit.Side, hit.Scenery ) );
        }

        SprMngr.DrawPoints( geometry, DRAW_PRIMITIVE_TRIANGLELIST );
        CollectBillboards( client, wall_depth, screen_width, screen_height, horizon, fov );

        PointVec crosshair;
        const bool has_target = FindCrosshairCritter( client, Camera.Aiming ? 0.045f : 0.085f ) != NULL;
        DrawCrosshair( crosshair, screen_width, screen_height, has_target );
        SprMngr.DrawPoints( crosshair, DRAW_PRIMITIVE_TRIANGLELIST );
    }
}
