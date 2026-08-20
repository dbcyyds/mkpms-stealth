#pragma once

#include <cstdint>

struct FVector {
    float X{};
    float Y{};
    float Z{};

    FVector() = default;
    FVector(float x, float y, float z) : X(x), Y(y), Z(z) {}

    FVector operator+(const FVector& o) const { return {X + o.X, Y + o.Y, Z + o.Z}; }
    FVector operator-(const FVector& o) const { return {X - o.X, Y - o.Y, Z - o.Z}; }
};

struct FRotator {
    float Pitch{};
    float Yaw{};
    float Roll{};
};

// UE FTransform 简化占位（本项目传送主用 Location）
struct FTransform {
    float Rotation[4]{}; // quat
    FVector Translation{};
    float pad0{};
    FVector Scale3D{1.f, 1.f, 1.f};
    float pad1{};
};
