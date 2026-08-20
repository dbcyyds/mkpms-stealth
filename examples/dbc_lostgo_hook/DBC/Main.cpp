#include <android/log.h>
#include <pthread.h>

import Engine;
import Hook;
import Logger;
static void* InitThread(void*) {
    Logger::Debug("InitThread: 启动");
    InitEngine();
    return nullptr;
}

__attribute__((constructor)) void DbcEntry() {
    Logger::Debug("DbcEntry: constructor");
    pthread_t t{};
    if (pthread_create(&t, nullptr, InitThread, nullptr) != 0)
        Logger::Debug("DbcEntry: pthread_create 失败");
    else
        pthread_detach(t);
}

__attribute__((destructor)) void DbcExit() {
    khook::cleanup();
}
