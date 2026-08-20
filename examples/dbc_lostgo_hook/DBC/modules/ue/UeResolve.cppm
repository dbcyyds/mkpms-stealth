module;

#include <cstdint>

export module UeResolve;

import UeObject;
import UeSdk;

export namespace UeResolve {

using namespace UC;

[[nodiscard]] FMoeGameCharacter* GetLocalCharacter() {
    if (auto* lib = UMoeGameLibrary::StaticClass()) {
        if (auto* c = lib->GetLocalCharacter())
            return reinterpret_cast<FMoeGameCharacter*>(c);
    }
    if (auto* world = UWorld::GetWorld()) {
        if (auto* gs = UGameplayStatics::StaticClass()) {
            if (auto* c = gs->GetLocalCharacter())
                return reinterpret_cast<FMoeGameCharacter*>(c);
        }
    }
    return nullptr;
}

[[nodiscard]] FMoeNR3E3CharBaseComponent* GetCharComponent() {
    auto* actor = GetLocalCharacter();
    if (!actor)
        return nullptr;
    return reinterpret_cast<FMoeNR3E3CharBaseComponent*>(
        actor->GetComponentByClass(FMoeNR3E3CharBaseComponent::StaticClass()));
}

[[nodiscard]] FMoeCharTeleportComponent* GetTeleportComponent() {
    auto* actor = GetLocalCharacter();
    if (!actor)
        return nullptr;
    if (auto* cls = FMoeCharTeleportComponent::MainStaticClass()) {
        if (auto* c = actor->GetComponentByClass(cls))
            return reinterpret_cast<FMoeCharTeleportComponent*>(c);
    }
    if (auto* cls = FMoeCharTeleportComponent::StaticClass()) {
        if (auto* c = actor->GetComponentByClass(cls))
            return reinterpret_cast<FMoeCharTeleportComponent*>(c);
    }
    return nullptr;
}

} // namespace UeResolve
