/* Übernommen aus XR-Settlers2.5 (libs/driver/include/driver/VrInput.h), dort auf der Quest abgenommen. */
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>
#include <cstdlib>
namespace vrInput {
struct Axes { float x,y; };
inline Axes mapAxes(float x,float y,bool invertX,bool invertY,bool swap)
{ x*=invertX ? -1.f : 1.f; y*=invertY ? -1.f : 1.f; return swap ? Axes{y,x} : Axes{x,y}; }
struct ZoomRepeat {
    int held=0; unsigned long next=0;
    int sample(float axis,bool enabled,unsigned long now) {
        const int direction=!enabled || std::fabs(axis)<.55f ? 0 : axis>0 ? 1 : -1;
        if(!direction) { held=0;next=0;return 0; }
        if(direction!=held) { held=direction;next=now+450;return direction; }
        if(now>=next) { next=now+250;return direction; }
        return 0;
    }
};
/// Trigger held on the map: a short tap stays a click, moving the ray grabs the map and moving the hand
/// towards/away from the panel zooms in/out one step per 6 cm. depth grows when the hand moves towards the panel.
struct MapGrab {
    bool pending=false, active=false;
    int startX=0, startY=0;
    float startDepth=0, depthRef=0;
    void begin(int x,int y,float depth) { pending=true;active=false;startX=x;startY=y;startDepth=depthRef=depth; }
    bool engaged() const { return pending || active; }
    int sample(int x,int y,float depth) {
        if(pending && (std::abs(x-startX)+std::abs(y-startY)>12 || std::fabs(depth-startDepth)>.03f)) { pending=false;active=true; }
        if(!active) return 0;
        const float d=depth-depthRef;
        if(d>=.06f) { depthRef+=.06f;return 1; }
        if(d<=-.06f) { depthRef-=.06f;return -1; }
        return 0;
    }
    /// Returns true if the trigger was only tapped, i.e. the deferred click must be sent now.
    bool end() { const bool tap=pending; pending=active=false; return tap; }
};
inline float smoothingAlpha(int strength, float seconds)
{ return strength == 0 ? 1.f : 1.f - std::exp(-seconds / (strength == 1 ? 0.025f : 0.055f)); }
inline unsigned confirmationSeconds(unsigned started, unsigned now)
{
    const unsigned elapsed=now-started; // unsigned subtraction also handles tick counter rollover
    return elapsed>=15000 ? 0 : (15000-elapsed+999)/1000;
}
struct StickTurbo
{
    int heldDirection=0, lastDirection=0;
    unsigned long releasedAt=0;
    bool boosted=false;
    bool sample(float x, float y, unsigned long now)
    {
        const int direction = std::fabs(x)<.5f && std::fabs(y)<.5f ? 0
          : std::fabs(x)>std::fabs(y) ? (x>0 ? 1 : 2) : (y>0 ? 3 : 4);
        if(!direction) {
            if(heldDirection) { lastDirection=heldDirection; releasedAt=now; }
            heldDirection=0; boosted=false;
        } else if(direction!=heldDirection) {
            boosted=!heldDirection && lastDirection==direction && now-releasedAt<=300;
            heldDirection=direction;
        }
        return boosted;
    }
};
inline float stickAxis(float v)
{ return std::fabs(v)<=.25f ? 0.f : std::copysign((std::fabs(v)-.25f)/.75f,v); }
struct PanAccumulator
{
    float x=0, y=0;
    void sample(float sx,float sy,float dt,int speed,bool turbo,int& dx,int& dy)
    {
        float vx=stickAxis(sx), vy=stickAxis(sy);
        const float length=std::sqrt(vx*vx+vy*vy);
        if(length>1) { vx/=length;vy/=length; }
        const float rate=200.f*(1<<speed)*(turbo ? 3.f : 1.f);
        x+=vx*dt*rate; y+=vy*dt*rate;
        dx=static_cast<int>(x);dy=static_cast<int>(y);x-=dx;y-=dy;
        if(vx==0.f) x=0;
        if(vy==0.f) y=0;
    }
};
struct ButtonGesture
{
    bool held=false, fired=false;
    unsigned long since=0;
    int sample(bool down, bool active, unsigned long now, int shortAction, int longAction)
    {
        if(!active) { held=false; fired=false; return 0; }
        int action=0;
        if(down && !held) { since=now; fired=false; if(!longAction) action=shortAction; }
        if(down && longAction && !fired && now-since>=600) { action=longAction; fired=true; }
        if(!down && held && longAction && !fired) action=shortAction;
        held=down;
        return action;
    }
};
}
