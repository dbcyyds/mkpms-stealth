module;

#include <chrono>
#include <cstdio>
#include <cstring>

export module Task;

import Logger;
import UeResolve;
import UeSdk;

using namespace UC;

namespace {

struct TaskDoneInfo {
    bool valid = false;
    bool success = false;
    int taskId = 0;
    int taskType = 0;
    int taskIndex = 0;
    bool urgent = false;
    int subIndex = 0;
    char name[64]{};
    int pendingAfter = -1;
};

int32_t PickSubTask(const FNR3E3MyTaskStruct& task) {
    if (task.TaskSub.Num() == 0) return 0;
    for (int i = 0; i < task.TaskSub.Num(); ++i) {
        if (!task.TaskSub[i].SubTaskIsComplete)
            return task.TaskSub[i].SubTaskIndex;
    }
    return task.TaskSub[0].SubTaskIndex;
}

void CopyTaskName(const FNR3E3MyTaskStruct& task, char* out, int outLen) {
    if (!out || outLen <= 0)
        return;
    out[0] = '\0';
    if (task.TaskName.Num() > 0) {
        const std::string name = task.TaskName[0].ToUtf8();
        if (!name.empty()) {
            std::strncpy(out, name.c_str(), static_cast<size_t>(outLen - 1));
            out[outLen - 1] = '\0';
            return;
        }
    }
    std::snprintf(out, static_cast<size_t>(outLen), "任务#%d", task.TaskID);
}

void RecordDoneTask(TaskDoneInfo& info, const FNR3E3MyTaskStruct& task, int idx, bool urgent) {
    info.valid = true;
    info.success = true;
    info.taskId = task.TaskID;
    info.taskType = task.TaskType;
    info.taskIndex = idx;
    info.urgent = urgent;
    info.subIndex = PickSubTask(task);
    CopyTaskName(task, info.name, sizeof(info.name));
}

bool CompleteOne(
    FMoeNR3E3TaskComponentBase* taskComp,
    FMoeNR3E3CharBaseComponent* charComp,
    const FNR3E3MyTaskStruct& task,
    int urgentIdx,
    int taskIndex,
    bool urgent,
    TaskDoneInfo* doneOut)
{
    if (task.bIsComplete) return false;
    if (urgent && !task.UrgentTaskIsRunning) {
        if (urgentIdx < 0) return false;
        taskComp->TriggerUrgentTask(urgentIdx);
        taskComp->TriggerUrgentTaskOnServer(urgentIdx);
    }
    const int32_t sub = PickSubTask(task);
    if (doneOut)
        RecordDoneTask(*doneOut, task, taskIndex, urgent);
    if (urgent) {
        if (urgentIdx >= 0)
            taskComp->K2_CompleteUrgentTask(urgentIdx, sub, ENR3E3TaskCompleteReason::Default);
        charComp->CompleteUrgentTask(task.TaskID, ENR3E3TaskCompleteReason::Default);
        charComp->CompleteTask(task.TaskID);
        taskComp->CompleteTask(task.TaskID);
    }
    taskComp->CompleteTaskByGM(task.TaskID, sub);
    taskComp->CompleteTaskOnServer(task.TaskID, sub, ENR3E3TaskCompletionMethod::Default);
    return true;
}

bool g_lastTriggerOk = false;
int g_lastTriggerPending = -1;
TaskDoneInfo g_lastDone{};

} // namespace

export bool TryCompleteOneTask() {
    auto* charComp = UeResolve::GetCharComponent();
    if (!charComp) {
        static int s_noChar;
        if ((++s_noChar % 20) == 1)
            Logger::Info("做任务: 无 CharComponent（未进局/角色未就绪）");
        return false;
    }
    auto* taskComp = charComp->GetTaskComponent();
    if (!taskComp) {
        static int s_noTask;
        if ((++s_noTask % 20) == 1)
            Logger::Info("做任务: 有角色但无 TaskComponent");
        return false;
    }

    taskComp->ModifyNextTaskProgressByGM(200);
    taskComp->ModifyNextTaskProgressByGMOnServer(200);

    const auto urgent = taskComp->GetUrgentTaskArray();
    const auto normal = taskComp->GetTaskArray();
    bool done = false;

    for (int i = 0; i < urgent.Num() && !done; ++i) {
        const auto& t = urgent[i];
        if (t.bIsComplete) continue;
        done = CompleteOne(taskComp, charComp, t, i, i, true, &g_lastDone);
    }
    for (int i = 0; i < normal.Num() && !done; ++i) {
        const auto& t = normal[i];
        if (t.bIsComplete || !t.bIsUrgent) continue;
        int urgentIdx = -1;
        for (int j = 0; j < urgent.Num(); ++j) {
            if (urgent[j].TaskID == t.TaskID) { urgentIdx = j; break; }
        }
        done = CompleteOne(taskComp, charComp, t, urgentIdx, i, true, &g_lastDone);
    }
    for (int i = 0; i < normal.Num() && !done; ++i) {
        const auto& t = normal[i];
        if (t.bIsComplete || t.bIsUrgent) continue;
        done = CompleteOne(taskComp, charComp, t, -1, i, false, &g_lastDone);
    }
    return done;
}

export int CountPendingTasks() {
    auto* charComp = UeResolve::GetCharComponent();
    if (!charComp) return -1;
    auto* taskComp = charComp->GetTaskComponent();
    if (!taskComp) return -1;

    int pending = 0;
    const auto urgent = taskComp->GetUrgentTaskArray();
    for (int i = 0; i < urgent.Num(); ++i)
        if (!urgent[i].bIsComplete) ++pending;
    const auto normal = taskComp->GetTaskArray();
    for (int i = 0; i < normal.Num(); ++i)
        if (!normal[i].bIsComplete) ++pending;
    return pending;
}

namespace {

bool ExecuteTaskTrigger() {
    static auto lastClick = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - lastClick < std::chrono::milliseconds(500))
        return false;
    lastClick = now;

    g_lastDone = {};
    const bool ok = TryCompleteOneTask();
    g_lastTriggerOk = ok;
    g_lastTriggerPending = CountPendingTasks();
    if (!ok) {
        g_lastDone.valid = true;
        g_lastDone.success = false;
        g_lastDone.pendingAfter = g_lastTriggerPending;
        return false;
    }
    g_lastDone.pendingAfter = g_lastTriggerPending;
    Logger::Info("做任务: 完成 [{}] id={} name={} 剩余={}",
                 g_lastDone.urgent ? "紧急" : "普通",
                 g_lastDone.taskId,
                 g_lastDone.name,
                 g_lastDone.pendingAfter);
    return true;
}

} // namespace

export bool TriggerTaskButton() {
    return ExecuteTaskTrigger();
}

export bool GetLastTaskTriggerResult(int* pendingAfter) {
    if (pendingAfter)
        *pendingAfter = g_lastTriggerPending;
    return g_lastTriggerOk;
}
