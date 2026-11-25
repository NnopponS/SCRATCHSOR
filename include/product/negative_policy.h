#pragma once
namespace negative_policy {
constexpr float upperLimit=-0.15f;
inline bool candidate(float y,float spread){return y+2.5f*spread<upperLimit;}
}
