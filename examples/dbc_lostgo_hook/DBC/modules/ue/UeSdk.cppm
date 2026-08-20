module;

#include "include/ue/Types.h"

export module UeSdk;

import GlobalConfig;
import UeContainers;
import UeObject;

export namespace UC {

class FBaseMoeGameCharacter;

struct FNR3E3MySubTaskStruct {
    int32_t SubTaskIndex;
    FVector SubTaskPositions;
    bool SubTaskIsComplete;
    char pad[3]{};
};

struct FNR3E3MyTaskStruct {
    int32_t TaskType;
    int32_t TaskID;
    FVector TaskPosition;
    TArray<FNR3E3MySubTaskStruct> TaskSub;
    bool bIsComplete;
    bool bIsUrgent;
    char pad1[2]{};
    int32_t UrgentRandomSubCount;
    TArray<FString> TaskName;
    TArray<FString> TaskDescription;
    bool UrgentTaskIsRunning;
    char pad2[3]{};
    float UrgentTaskDefaultCDTime;
    float UrgentTaskCDTime;
    float UrgentTaskBuffModifyCDTime;
    int32_t UrgentTask_Failure_Time;
    int64_t UrgentTask_Failure_RemainingTime;
    int32_t UrgentTaskRemainingCDTime;
    float CurrentUrgentTaskAutoFinishTime;
    float UrgentTaskAutoFinishTime;
    int32_t TaskProgression;
    int32_t TaskPriority;
    bool InterruptTask;
    unsigned char UrgentTaskType;
    bool IsIndividualCompletion;
    char pad3;
    int32_t IndividualCompletionTaskCount;
    bool isEmergencyTaskTriggeredCooldown;
    char pad4[3];
    int32_t CurrentMaxCD;
    bool bShouldAIOverallUpload;
    char pad5[3];
    uint8_t IgnoresTags[0x20]{};
    uint8_t AutoDeleteTags[0x20]{};
    FBaseMoeGameCharacter* UrgentTaskTriggerCharacter{};
};
static_assert(sizeof(FNR3E3MyTaskStruct) == 0xE0);

enum class ENR3E3TaskCompletionMethod : uint8_t { Default = 0, GM = 1 };
enum class ENR3E3TaskCompleteReason : uint8_t { Default = 0 };

class FMoeNR3E3GameStateBase : public UObject {};

class ActorComponent : public UObject {
public:
    static ActorComponent* StaticClass() {
        static ActorComponent* cls = nullptr;
        if (!cls) cls = FindObject<ActorComponent>(u"ActorComponent");
        return cls;
    }
};

class Actor : public UObject {
public:
    static UClass* StaticClass() {
        return FindObject<UClass>(u"/Script/Engine.Actor");
    }
    ActorComponent* GetComponentByClass(UClass* cls) const;
    FVector K2_GetActorLocation() const;
    bool K2_SetActorLocation(const FVector& newLoc, bool bSweep, bool bTeleport);
    bool K2_TeleportTo(const FVector& destLocation, const FRotator& destRotation);
};

class FBaseMoeGameCharacter : public Actor {
public:
    // SDK: MoeCharacterMovement @ 0x6F0
    static constexpr size_t kMoeCharacterMovementOffset = 0x6F0;
};

class FMoeGameCharacter : public FBaseMoeGameCharacter {
public:
    static UClass* StaticClass() {
        return FindObject<UClass>(u"/Script/MoeGame.MoeGameCharacter");
    }
};

// 官方 Teleport 最终会走这里写胶囊体+Mesh
class FMoeExCharacterMovementComponent : public ActorComponent {
public:
    static UClass* StaticClass() {
        static UClass* cls = nullptr;
        if (!cls)
            cls = FindObject<UClass>(u"/Script/MoeGameCore.MoeExCharacterMovementComponent");
        if (!cls)
            cls = FindObject<UClass>(u"MoeExCharacterMovementComponent");
        return cls;
    }
    static UClass* MainStaticClass() {
        static UClass* cls = nullptr;
        if (!cls)
            cls = FindObject<UClass>(u"/Script/MoeGameCore.MainCharacterMovementComponent");
        if (!cls)
            cls = FindObject<UClass>(u"MainCharacterMovementComponent");
        return cls;
    }
    void SetCapsuleAndMeshLocation(const FVector& targetLocation);
    void SetCapsuleAndMeshTransform(const FTransform& targetTransform);
};

class FSPGameLibrary : public UObject {
public:
    static FSPGameLibrary* StaticClass() {
        static FSPGameLibrary* obj = nullptr;
        if (!obj)
            obj = FindObject<FSPGameLibrary>(u"Default__SPGameLibrary");
        if (!obj)
            obj = FindObject<FSPGameLibrary>(u"/Script/Feature_SP.Default__SPGameLibrary");
        return obj;
    }
    // bForceSetPos=true 时强制写位置
    static bool TeleportCharacterUsingLocation(FBaseMoeGameCharacter* character,
                                               const FVector& targetLocation,
                                               bool bForceSetPos);
};

class UWorld : public UObject {
public:
    uint8_t pad[0x8];
    void* PersistentLevel{};

    static UWorld* GetWorld();
};

class UGameInstance : public UObject {
public:
    uint8_t pad[0x10];
    TArray<UObject*> LocalPlayers{};
};

class UGameViewportClient : public UObject {
public:
    uint8_t pad[0x48];
    UWorld* World{};
    UGameInstance* GameInstance{};
};

class UEngine : public UObject {
public:
    uint8_t pad[0x7D0];
    UGameViewportClient* GameViewport{};

    static UEngine* GetEngine();
};

class UMoeGameLibrary : public UObject {
public:
    static UMoeGameLibrary* StaticClass() {
        return FindObject<UMoeGameLibrary>(u"Default__MoeGameLibrary");
    }
    FBaseMoeGameCharacter* GetLocalCharacter();
};

class UGameplayStatics : public UObject {
public:
    static UGameplayStatics* StaticClass() {
        return FindObject<UGameplayStatics>(u"Default__GameplayStatics");
    }
    FBaseMoeGameCharacter* GetLocalCharacter() const;
};

class FMoeNR3E3TaskComponentBase : public ActorComponent {
public:
    static UClass* StaticClass() {
        return FindObject<UClass>(u"/Script/Feature_NR3E.MoeNR3E3TaskComponentBase");
    }
    void ModifyNextTaskProgressByGM(int32_t nextProgress);
    void ModifyNextTaskProgressByGMOnServer(int32_t nextProgress);
    TArray<FNR3E3MyTaskStruct> GetTaskArray();
    TArray<FNR3E3MyTaskStruct> GetUrgentTaskArray();
    void CompleteTask(int32_t taskId);
    void CompleteTaskByGM(int32_t taskId, int32_t subIdx);
    void CompleteTaskOnServer(int32_t taskId, int32_t subIdx, ENR3E3TaskCompletionMethod method);
    void K2_CompleteUrgentTask(int32_t taskIndex, int32_t subTaskId, ENR3E3TaskCompleteReason reason);
    void TriggerUrgentTask(int32_t taskIndex);
    void TriggerUrgentTaskOnServer(int32_t taskIndex);
};

class FMoeNR3E3CharBaseComponent : public ActorComponent {
public:
    static UClass* StaticClass() {
        return FindObject<UClass>(u"/Script/Feature_NR3E.MoeNR3E3CharBaseComponent");
    }
    FMoeNR3E3GameStateBase* GetE3GameState();
    FMoeNR3E3TaskComponentBase* GetTaskComponent();
    void CompleteUrgentTask(int32_t taskId, ENR3E3TaskCompleteReason reason);
    void CompleteTask(int32_t taskId);
};

// 传送组件 /Script/MoeGameCore.MoeCharTeleportComponent
class FMoeCharTeleportComponent : public ActorComponent {
public:
    static UClass* StaticClass() {
        // 优先完整路径，失败再短名
        static UClass* cls = nullptr;
        if (!cls)
            cls = FindObject<UClass>(u"/Script/MoeGameCore.MoeCharTeleportComponent");
        if (!cls)
            cls = FindObject<UClass>(u"MoeCharTeleportComponent");
        return cls;
    }
    static UClass* MainStaticClass() {
        static UClass* cls = nullptr;
        if (!cls)
            cls = FindObject<UClass>(u"/Script/MoeGameCore.MainCharTeleportComponent");
        if (!cls)
            cls = FindObject<UClass>(u"MainCharTeleportComponent");
        return cls;
    }

    void TeleportCharacterUsingLocation(const FVector& targetLocation);
    void ServerTeleportCharacterUsingLocation(const FVector& targetLocation);
    void CallServerTeleportCharacter(const FVector& targetLocation);
    void TeleportCharacterUsingTransform(const FTransform& targetTransform);
    bool CheckTeleportTargetLocation(const FVector& targetLocation);
    bool IsValidDistanceToTeleport(const FVector& targetLocation);
};

UEngine* UEngine::GetEngine() {
    static UEngine* engine = nullptr;
    if (engine) return engine;
    if (!Core::GEngineAddress) return nullptr;
    engine = reinterpret_cast<UEngine*>(*reinterpret_cast<int64_t*>(Core::GEngineAddress));
    return engine;
}

UWorld* UWorld::GetWorld() {
    if (auto* engine = UEngine::GetEngine()) {
        if (!engine->GameViewport) return nullptr;
        return engine->GameViewport->World;
    }
    return nullptr;
}

ActorComponent* Actor::GetComponentByClass(UClass* componentClass) const {
    static UObject* fn = FindObject<UObject>(u"Engine.Actor.GetComponentByClass");
    if (!fn)
        fn = FindObject<UObject>(u"/Script/Engine.Actor.GetComponentByClass");
    struct { UClass* ComponentClass; ActorComponent* ReturnValue; } p{componentClass, nullptr};
    if (fn) ProcessEvent(fn, &p);
    return p.ReturnValue;
}

FVector Actor::K2_GetActorLocation() const {
    static UObject* fn = FindObject<UObject>(u"/Script/Engine.Actor.K2_GetActorLocation");
    if (!fn)
        fn = FindObject<UObject>(u"Engine.Actor.K2_GetActorLocation");
    if (!fn)
        fn = FindObject<UObject>(u"K2_GetActorLocation");
    struct { FVector ReturnValue; } p{};
    if (fn) ProcessEvent(fn, &p);
    return p.ReturnValue;
}

bool Actor::K2_SetActorLocation(const FVector& newLoc, bool bSweep, bool bTeleport) {
    static UObject* fn = FindObject<UObject>(u"/Script/Engine.Actor.K2_SetActorLocation");
    if (!fn)
        fn = FindObject<UObject>(u"Engine.Actor.K2_SetActorLocation");
    if (!fn)
        fn = FindObject<UObject>(u"K2_SetActorLocation");
    // FHitResult ~0x8C，预留 0x90
    struct {
        FVector NewLocation;
        bool bSweep;
        uint8_t SweepHitResult[0x90]{};
        bool bTeleport;
        bool ReturnValue;
    } p{newLoc, bSweep, {}, bTeleport, false};
    if (fn) ProcessEvent(fn, &p);
    return p.ReturnValue;
}

bool Actor::K2_TeleportTo(const FVector& destLocation, const FRotator& destRotation) {
    static UObject* fn = FindObject<UObject>(u"/Script/Engine.Actor.K2_TeleportTo");
    if (!fn)
        fn = FindObject<UObject>(u"Engine.Actor.K2_TeleportTo");
    if (!fn)
        fn = FindObject<UObject>(u"K2_TeleportTo");
    struct {
        FVector DestLocation;
        FRotator DestRotation;
        bool ReturnValue;
    } p{destLocation, destRotation, false};
    if (fn) ProcessEvent(fn, &p);
    return p.ReturnValue;
}

void FMoeExCharacterMovementComponent::SetCapsuleAndMeshLocation(const FVector& targetLocation) {
    static UObject* fn = FindObject<UObject>(
        u"/Script/MoeGameCore.MoeExCharacterMovementComponent.SetCapsuleAndMeshLocation");
    if (!fn)
        fn = FindObject<UObject>(u"MoeExCharacterMovementComponent.SetCapsuleAndMeshLocation");
    if (!fn)
        fn = FindObject<UObject>(u"SetCapsuleAndMeshLocation");
    struct { FVector TargetLocation; } p{targetLocation};
    if (fn) ProcessEvent(fn, &p);
}

void FMoeExCharacterMovementComponent::SetCapsuleAndMeshTransform(const FTransform& targetTransform) {
    static UObject* fn = FindObject<UObject>(
        u"/Script/MoeGameCore.MoeExCharacterMovementComponent.SetCapsuleAndMeshTransform");
    if (!fn)
        fn = FindObject<UObject>(u"SetCapsuleAndMeshTransform");
    struct { FTransform TargetTransform; } p{targetTransform};
    if (fn) ProcessEvent(fn, &p);
}

bool FSPGameLibrary::TeleportCharacterUsingLocation(FBaseMoeGameCharacter* character,
                                                    const FVector& targetLocation,
                                                    bool bForceSetPos) {
    static UObject* fn = FindObject<UObject>(
        u"/Script/Feature_SP.SPGameLibrary.TeleportCharacterUsingLocation");
    if (!fn)
        fn = FindObject<UObject>(u"SPGameLibrary.TeleportCharacterUsingLocation");
    auto* def = StaticClass();
    if (!fn || !def)
        return false;
    struct {
        FBaseMoeGameCharacter* Character;
        FVector TargetLocation;
        bool bForceSetPos;
        bool ReturnValue;
    } p{character, targetLocation, bForceSetPos, false};
    def->ProcessEvent(fn, &p);
    return p.ReturnValue;
}

FBaseMoeGameCharacter* UMoeGameLibrary::GetLocalCharacter() {
    static UObject* fn = FindObject<UObject>(u"/Script/MoeGame.MoeGameLibrary.GetLocalCharacter");
    struct { FBaseMoeGameCharacter* ReturnValue; } p{};
    if (fn) ProcessEvent(fn, &p);
    return p.ReturnValue;
}

FBaseMoeGameCharacter* UGameplayStatics::GetLocalCharacter() const {
    static UObject* fn = FindObject<UObject>(u"GetLocalCharacter");
    struct { FBaseMoeGameCharacter* ReturnValue; } p{};
    if (fn) ProcessEvent(fn, &p);
    return p.ReturnValue;
}

void FMoeNR3E3TaskComponentBase::ModifyNextTaskProgressByGM(int32_t v) {
    static UObject* fn = FindObject<UObject>(u"/Script/Feature_NR3E.MoeNR3E3TaskComponentBase.ModifyNextTaskProgressByGM");
    struct { int32_t NextProgress; } p{v};
    if (fn) ProcessEvent(fn, &p);
}

void FMoeNR3E3TaskComponentBase::ModifyNextTaskProgressByGMOnServer(int32_t v) {
    static UObject* fn = FindObject<UObject>(u"/Script/Feature_NR3E.MoeNR3E3TaskComponentBase.ModifyNextTaskProgressByGMOnServer");
    struct { int32_t NextProgress; } p{v};
    if (fn) ProcessEvent(fn, &p);
}

TArray<FNR3E3MyTaskStruct> FMoeNR3E3TaskComponentBase::GetTaskArray() {
    static UObject* fn = FindObject<UObject>(u"/Script/Feature_NR3E.MoeNR3E3TaskComponentBase.GetTaskArray");
    struct { TArray<FNR3E3MyTaskStruct> OutArray; } p{};
    if (fn) ProcessEvent(fn, &p);
    return p.OutArray;
}

TArray<FNR3E3MyTaskStruct> FMoeNR3E3TaskComponentBase::GetUrgentTaskArray() {
    static UObject* fn = FindObject<UObject>(u"/Script/Feature_NR3E.MoeNR3E3TaskComponentBase.GetUrgentTaskArray");
    struct { TArray<FNR3E3MyTaskStruct> OutArray; } p{};
    if (fn) ProcessEvent(fn, &p);
    return p.OutArray;
}

void FMoeNR3E3TaskComponentBase::K2_CompleteUrgentTask(int32_t idx, int32_t sub, ENR3E3TaskCompleteReason r) {
    static UObject* fn = FindObject<UObject>(u"/Script/Feature_NR3E.MoeNR3E3TaskComponentBase.K2_CompleteUrgentTask");
    struct { int32_t TaskIndex; int32_t SubTaskId; ENR3E3TaskCompleteReason Reason; } p{idx, sub, r};
    if (fn) ProcessEvent(fn, &p);
}

void FMoeNR3E3TaskComponentBase::CompleteTask(int32_t taskId) {
    static UObject* fn = FindObject<UObject>(u"/Script/Feature_NR3E.MoeNR3E3TaskComponentBase.CompleteTask");
    struct { int32_t TaskID; int32_t SubTaskId; ENR3E3TaskCompletionMethod Method; } p{taskId, 0, ENR3E3TaskCompletionMethod::Default};
    if (fn) ProcessEvent(fn, &p);
}

void FMoeNR3E3TaskComponentBase::CompleteTaskByGM(int32_t taskId, int32_t subIdx) {
    static UObject* fn = FindObject<UObject>(u"/Script/Feature_NR3E.MoeNR3E3TaskComponentBase.CompleteTaskByGM");
    struct { int32_t TaskID; int32_t SubTaskId; } p{taskId, subIdx};
    if (fn) ProcessEvent(fn, &p);
}

void FMoeNR3E3TaskComponentBase::CompleteTaskOnServer(int32_t taskId, int32_t subIdx, ENR3E3TaskCompletionMethod m) {
    static UObject* fn = FindObject<UObject>(u"/Script/Feature_NR3E.MoeNR3E3TaskComponentBase.CompleteTaskOnServer");
    struct { int32_t TaskID; int32_t SubTaskId; ENR3E3TaskCompletionMethod Method; } p{taskId, subIdx, m};
    if (fn) ProcessEvent(fn, &p);
}

FMoeNR3E3GameStateBase* FMoeNR3E3CharBaseComponent::GetE3GameState() {
    static UObject* fn = FindObject<UObject>(u"/Script/Feature_NR3E.MoeNR3E3CharBaseComponent.GetE3GameState");
    struct { FMoeNR3E3GameStateBase* ReturnValue; } p{};
    if (fn) ProcessEvent(fn, &p);
    return p.ReturnValue;
}

FMoeNR3E3TaskComponentBase* FMoeNR3E3CharBaseComponent::GetTaskComponent() {
    static UObject* fn = FindObject<UObject>(u"/Script/Feature_NR3E.MoeNR3E3CharBaseComponent.GetTaskComponent");
    struct { FMoeNR3E3TaskComponentBase* ReturnValue; } p{};
    if (fn) ProcessEvent(fn, &p);
    return p.ReturnValue;
}

void FMoeNR3E3CharBaseComponent::CompleteTask(int32_t taskId) {
    static UObject* fn = FindObject<UObject>(u"/Script/Feature_NR3E.MoeNR3E3CharBaseComponent.CompleteTask");
    struct { int32_t TaskID; } p{taskId};
    if (fn) ProcessEvent(fn, &p);
}

void FMoeNR3E3CharBaseComponent::CompleteUrgentTask(int32_t taskId, ENR3E3TaskCompleteReason r) {
    static UObject* fn = FindObject<UObject>(u"/Script/Feature_NR3E.MoeNR3E3CharBaseComponent.CompleteUrgentTask");
    struct { int32_t TaskID; ENR3E3TaskCompleteReason Reason; } p{taskId, r};
    if (fn) ProcessEvent(fn, &p);
}

void FMoeNR3E3TaskComponentBase::TriggerUrgentTask(int32_t idx) {
    static UObject* fn = FindObject<UObject>(u"/Script/Feature_NR3E.MoeNR3E3TaskComponentBase.TriggerUrgentTask");
    struct { int32_t TaskIndex; } p{idx};
    if (fn) ProcessEvent(fn, &p);
}

void FMoeNR3E3TaskComponentBase::TriggerUrgentTaskOnServer(int32_t idx) {
    static UObject* fn = FindObject<UObject>(u"/Script/Feature_NR3E.MoeNR3E3TaskComponentBase.TriggerUrgentTaskOnServer");
    struct { int32_t TaskIndex; } p{idx};
    if (fn) ProcessEvent(fn, &p);
}

void FMoeCharTeleportComponent::TeleportCharacterUsingLocation(const FVector& targetLocation) {
    static UObject* fn = FindObject<UObject>(
        u"/Script/MoeGameCore.MoeCharTeleportComponent.TeleportCharacterUsingLocation");
    if (!fn)
        fn = FindObject<UObject>(u"MoeCharTeleportComponent.TeleportCharacterUsingLocation");
    if (!fn)
        fn = FindObject<UObject>(u"TeleportCharacterUsingLocation");
    struct { FVector TargetLocation; } p{targetLocation};
    if (fn) ProcessEvent(fn, &p);
}

void FMoeCharTeleportComponent::ServerTeleportCharacterUsingLocation(const FVector& targetLocation) {
    static UObject* fn = FindObject<UObject>(
        u"/Script/MoeGameCore.MoeCharTeleportComponent.ServerTeleportCharacterUsingLocation");
    if (!fn)
        fn = FindObject<UObject>(u"ServerTeleportCharacterUsingLocation");
    struct { FVector TargetLocation; } p{targetLocation};
    if (fn) ProcessEvent(fn, &p);
}

void FMoeCharTeleportComponent::CallServerTeleportCharacter(const FVector& targetLocation) {
    static UObject* fn = FindObject<UObject>(
        u"/Script/MoeGameCore.MoeCharTeleportComponent.CallServerTeleportCharacter");
    if (!fn)
        fn = FindObject<UObject>(u"CallServerTeleportCharacter");
    struct { FVector TargetLocation; } p{targetLocation};
    if (fn) ProcessEvent(fn, &p);
}

void FMoeCharTeleportComponent::TeleportCharacterUsingTransform(const FTransform& targetTransform) {
    static UObject* fn = FindObject<UObject>(
        u"/Script/MoeGameCore.MoeCharTeleportComponent.TeleportCharacterUsingTransform");
    if (!fn)
        fn = FindObject<UObject>(u"TeleportCharacterUsingTransform");
    struct { FTransform TargetTransform; } p{targetTransform};
    if (fn) ProcessEvent(fn, &p);
}

bool FMoeCharTeleportComponent::CheckTeleportTargetLocation(const FVector& targetLocation) {
    static UObject* fn = FindObject<UObject>(
        u"/Script/MoeGameCore.MoeCharTeleportComponent.CheckTeleportTargetLocation");
    if (!fn)
        fn = FindObject<UObject>(u"CheckTeleportTargetLocation");
    struct { FVector TargetLocation; bool ReturnValue; } p{targetLocation, false};
    if (fn) ProcessEvent(fn, &p);
    return p.ReturnValue;
}

bool FMoeCharTeleportComponent::IsValidDistanceToTeleport(const FVector& targetLocation) {
    static UObject* fn = FindObject<UObject>(
        u"/Script/MoeGameCore.MoeCharTeleportComponent.IsValidDistanceToTeleport");
    if (!fn)
        fn = FindObject<UObject>(u"IsValidDistanceToTeleport");
    struct { FVector TargetLocation; bool ReturnValue; } p{targetLocation, false};
    if (fn) ProcessEvent(fn, &p);
    return p.ReturnValue;
}

} // namespace UC
