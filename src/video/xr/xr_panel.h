/* Übernommen aus XR-Settlers2.5 (extras/videoDrivers/OpenXR/PanelGeometry.h), dort auf der Quest abgenommen. */
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>
#include <initializer_list>

namespace vrPanel {
struct Vec3 { float x, y, z; };
inline Vec3 rotateYaw(Vec3 v, float yaw)
{ return {std::cos(yaw)*v.x+std::sin(yaw)*v.z, v.y, -std::sin(yaw)*v.x+std::cos(yaw)*v.z}; }
inline Vec3 worldPoint(Vec3 v, Vec3 origin, float yaw)
{ const auto p=rotateYaw(v,yaw); return {p.x+origin.x,p.y+origin.y,p.z+origin.z}; }
inline Vec3 localPoint(Vec3 v, Vec3 origin, float yaw)
{ return rotateYaw({v.x-origin.x,v.y-origin.y,v.z-origin.z},-yaw); }
inline Vec3 rotatePitch(Vec3 v, float pitch)
{ return {v.x, std::cos(pitch)*v.y-std::sin(pitch)*v.z, std::sin(pitch)*v.y+std::cos(pitch)*v.z}; }
struct Quaternion { float x, y, z, w; };
// Tilt around the center pixel, not around the viewer. The same pose drives
// the compositor and inverse ray mapping, including the cylinder's center.
struct Pose
{
    Vec3 origin;
    float yaw, centerZ, pitch, height;
    Vec3 toWorld(Vec3 v) const
    {
        const auto p = rotatePitch({v.x, v.y, v.z-centerZ}, pitch);
        return worldPoint({p.x, p.y+height, p.z+centerZ}, origin, yaw);
    }
    Vec3 toLocal(Vec3 v) const
    {
        const auto p = localPoint(v, origin, yaw);
        const auto t = rotatePitch({p.x, p.y-height, p.z-centerZ}, -pitch);
        return {t.x, t.y, t.z+centerZ};
    }
    Vec3 directionToLocal(Vec3 v) const { return rotatePitch(rotateYaw(v, -yaw), -pitch); }
    Quaternion orientation() const
    {
        const float sy=std::sin(yaw/2), cy=std::cos(yaw/2), sp=std::sin(pitch/2), cp=std::cos(pitch/2);
        return {cy*sp, sy*cp, -sy*sp, cy*cp}; // yaw * pitch
    }
};
struct Hit { float u = 0, v = 0; Vec3 point{}; };
// Use only the existing decorative outer rim; leave the bottom toolbar free.
inline bool moveFrameHit(float u,float v)
{
    return u>=0 && u<=1 && v>=0 && v<=1
      && (u<.015f || u>.985f || v<.02f || (v>.98f && (u<.35f || u>.65f)));
}
struct TranslationDrag
{
    Vec3 origin{}, anchor{};
    float distance=0;
    void begin(Vec3 panelOrigin,Vec3 hit,Vec3 hand) {
        origin=panelOrigin;anchor=hit;
        const float x=hit.x-hand.x,y=hit.y-hand.y,z=hit.z-hand.z;
        distance=std::sqrt(x*x+y*y+z*z);
    }
    Vec3 target(Vec3 hand,Vec3 direction) const
    { return {hand.x+direction.x*distance,hand.y+direction.y*distance,hand.z+direction.z*distance}; }
    Vec3 translated(Vec3 hand,Vec3 direction) const {
        const auto p=target(hand,direction);
        return {origin.x+p.x-anchor.x,origin.y+p.y-anchor.y,origin.z+p.z-anchor.z};
    }
};
inline float angle(int curvature) { return curvature == 1 ? 0.785398163f : curvature == 2 ? 1.308996939f : 0.f; }

// width is arc length; the center pixel stays at z when switching shape.
inline bool intersect(Vec3 origin, Vec3 direction, float width, float height, float z, float arc, Hit& hit)
{
    auto accept = [&](float t, float u) {
        const float v = 0.5f - (origin.y + t * direction.y) / height;
        if(t <= 0.f || u < 0.f || u > 1.f || v < 0.f || v > 1.f) return false;
        hit = {u, v, {origin.x + t * direction.x, origin.y + t * direction.y, origin.z + t * direction.z}};
        return true;
    };
    if(arc == 0.f)
    {
        if(direction.z >= -1e-5f) return false;
        const float t = (z - origin.z) / direction.z;
        return accept(t, 0.5f + (origin.x + t * direction.x) / width);
    }
    const float radius = width / arc;
    const float centerZ = z + radius;
    const float oz = origin.z - centerZ;
    const float a = direction.x * direction.x + direction.z * direction.z;
    if(a < 1e-8f) return false;
    const float b = 2.f * (origin.x * direction.x + oz * direction.z);
    const float c = origin.x * origin.x + oz * oz - radius * radius;
    const float discriminant = b * b - 4.f * a * c;
    if(discriminant < 0.f) return false;
    for(float sign : {-1.f, 1.f})
    {
        const float t = (-b + sign * std::sqrt(discriminant)) / (2.f * a);
        const float x = origin.x + t * direction.x;
        const float rz = oz + t * direction.z;
        // Only the inner, front-facing surface is interactive.
        if(direction.x * x + direction.z * rz <= 0.f) continue;
        const float theta = std::atan2(x, -rz);
        if(accept(t, 0.5f + theta / arc)) return true;
    }
    return false;
}
} // namespace vrPanel
