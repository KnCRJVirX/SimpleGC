#ifndef CONTEXTINFO_H
#define CONTEXTINFO_H

#ifdef _WIN64

#include <Windows.h>

static inline CONTEXT GetCurrentContext() {
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    return ctx;
}

static inline void* GetStackBase() {
    return ((NT_TIB*)NtCurrentTeb())->StackBase;
}

static inline void* GetStackCurTop() {
    void* rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

#elif defined(_WIN32)

static inline CONTEXT GetCurrentContext() {
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    return ctx;
}

static inline void* GetStackBase() {
    return ((NT_TIB*)NtCurrentTeb())->StackBase;
}

static inline void* GetStackCurTop() {
    void* esp;
    asm volatile("mov %%esp, %0" : "=r"(esp));
    return esp;
}

#endif

#endif