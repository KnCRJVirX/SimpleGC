#ifndef SIMPLEGC
#define SIMPLEGC

#include <cassert>
#include <type_traits>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <functional>

#include "ContextInfo.hpp"

namespace SimpleGC {

class Object;
class RefCollector;
class GC;
class AutoScanGC;

class Object {
    friend class GC;
public:
    using RefT = std::function<void(Object*)>;
    virtual void trace(RefT Ref) = 0;
    virtual ~Object() {}
};

class RefCollector {
    friend class GC;
    friend class AutoScanGC;
protected:
    std::unordered_set<Object*> refs;
public:
    template <typename T,
              typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
    T* addRef(T* obj) {
        refs.insert(obj);
        return obj;
    }

    template <typename T,
              typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
    T* delRef(T* obj) {
        if (refs.find(obj) != refs.end()) {
            refs.erase(obj);
        }
        return obj;
    }
};

class Manager {
    friend class GC;
    friend class AutoScanGC;
protected:
    std::unordered_set<Object*> allObjs;
    std::vector<RefCollector*> stackFrames;
    RefCollector* globalObjs;
public:
    Manager(): globalObjs(new RefCollector){
        newCallFrame();
    }
    ~Manager() {
        delete globalObjs;
        for (RefCollector* frame: stackFrames) {
            delete frame;
        }
        for (Object* obj: allObjs) {
            delete obj;
        }
    }

    static Manager& GlobalInstance() {
        static Manager inst;
        return inst;
    }

    RefCollector* newCallFrame() {
        RefCollector* nFrame = new RefCollector;
        stackFrames.push_back(nFrame);
        return nFrame;
    }
    RefCollector* curFrame() {
        return stackFrames.back();
    }
    RefCollector* retFrame() {
        RefCollector* cur = curFrame();
        stackFrames.pop_back();
        delete cur;
        return curFrame();
    }
    RefCollector* globalFrame() {
        return globalObjs;
    }

    template <typename T,
              typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
    T* addNewObject(T* obj) {
        allObjs.insert(obj);
        return obj;
    }

    void delObject(Object* obj) {
        if (allObjs.find(obj) == allObjs.end()) {
            return;
        }
        allObjs.erase(obj);
        delete obj;
    }
};

static inline Manager& GlobalManager() {
    return Manager::GlobalInstance();
}

class GC {
protected:
    Manager& mgrRef;
public:
    GC(Manager& mgr = GlobalManager()): mgrRef(mgr){}
    void gc() {
        std::unordered_set<Object*> objs = mgrRef.allObjs;

        std::function<void(Object*)> f;
        f = [&f, &objs](Object* obj) {
            if (objs.find(obj) == objs.end()) {
                return;
            }

            objs.erase(obj);
            obj->trace(f);
        };
        
        // Global references
        for (Object* obj: mgrRef.globalObjs->refs) {
            f(obj);
        }
        
        // Stack references
        for (RefCollector* frame: mgrRef.stackFrames) {
            for (Object* obj: frame->refs) {
                f(obj);
            }
        }

        // Collect references
        for (Object* obj: objs) {
            mgrRef.delObject(obj);
        }
    }
};

#ifdef _WIN64
class AutoScanGC {
protected:
    Manager& mgrRef;
    void* stackTop;
    CONTEXT ctx;
public:
    AutoScanGC(Manager& mgr = GlobalManager(), void* sTop = GetStackCurTop(), const CONTEXT& _ctx = GetCurrentContext()): mgrRef(mgr), stackTop(sTop), ctx(_ctx) {}
    void gc() {
        std::unordered_set<Object*> objs = mgrRef.allObjs;

        std::function<void(Object*)> f;
        f = [&f, &objs](Object* obj) {
            if (objs.find(obj) == objs.end()) {
                return;
            }

            objs.erase(obj);
            obj->trace(f);
        };

        // Global references
        for (Object* obj: mgrRef.globalObjs->refs) {
            f(obj);
        }
        
        // Stack references
        void** base = (void**)GetStackBase();
        void** top = (void**)stackTop;
        for (void** p = top; p < base; ++p) {
            f((Object*)*p);
        }
        
        // Register references
        f((Object*)ctx.Rax);
        f((Object*)ctx.Rbx);
        f((Object*)ctx.Rcx);
        f((Object*)ctx.Rdx);
        f((Object*)ctx.Rdi);
        f((Object*)ctx.Rsi);
        f((Object*)ctx.Rsp);
        f((Object*)ctx.Rbp);
        f((Object*)ctx.R8);
        f((Object*)ctx.R9);
        f((Object*)ctx.R10);
        f((Object*)ctx.R11);
        f((Object*)ctx.R12);
        f((Object*)ctx.R13);
        f((Object*)ctx.R14);
        f((Object*)ctx.R15);

        // Collect references
        for (Object* obj: objs) {
            mgrRef.delObject(obj);
        }
    }
};
#endif

template <typename T,
          typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
static inline T* AddObjectIntoGC(T* obj) {
    return GlobalManager().addNewObject(obj);
}

template <typename T,
          typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
static inline T* AddStackRef(T* obj) {
    return GlobalManager().curFrame()->addRef(obj);
}

template <typename T,
          typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
static inline T* AddGlobalRef(T* obj) {
    return GlobalManager().globalFrame()->addRef(obj);
}

template <typename T,
          typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
static inline T* RemoveStackRef(T* obj) {
    return GlobalManager().curFrame()->delRef(obj);
}

template <typename T,
          typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
static inline T* GCRef(T* obj) {
    AddObjectIntoGC(obj);
    return obj;
}

template <typename T,
          typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
static inline T* VarRef(T* obj) {
    GCRef(obj);
    AddStackRef(obj);
    return obj;
}

template <typename T,
          typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
static inline T* VarDeRef(T* obj) {
    RemoveStackRef(obj);
    return obj;
}

template <typename T,
          typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
static inline T* GlobalRef(T* obj) {
    GCRef(obj);
    AddGlobalRef(obj);
    return obj;
}

template <typename T,
          typename = std::enable_if_t<std::is_base_of_v<Object, T>>,
          typename... Args>
static inline T* GCNew(Args&&... args) {
    T* obj = new T(std::forward<Args>(args)...);
    return GCRef(obj);
}

template <typename T,
          typename = std::enable_if_t<std::is_base_of_v<Object, T>>,
          typename... Args>
static inline T* VarNew(Args&&... args) {
    T* obj = new T(std::forward<Args>(args)...);
    return VarRef(obj);
}

template <typename T,
          typename = std::enable_if_t<std::is_base_of_v<Object, T>>,
          typename... Args>
static inline T* GlobalNew(Args&&... args) {
    T* obj = new T(std::forward<Args>(args)...);
    return GlobalRef(obj);
}

template <typename T,
          typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
static inline void Del(T*& obj) {
    if (obj) {
        GlobalManager().delObject(obj);
        obj = nullptr;
    }
}

static inline RefCollector* CallFunction() {
    return GlobalManager().newCallFrame();
}

static inline RefCollector* ReturnFuntion() {
    return GlobalManager().retFrame();
}

static inline void DoGC() {
    GC().gc();
}

template <typename T,
          typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
class UniquePtr {
protected:
    T* p;
public:
    UniquePtr(T* _ptr): p(_ptr) {
        VarRef(p);
    };
    UniquePtr(const UniquePtr&) = delete;
    UniquePtr(UniquePtr&& rhs): p(rhs.p) {
        rhs.p = nullptr;
    }
    ~UniquePtr() {
        VarDeRef(p);
    };

    UniquePtr& operator=(const UniquePtr&) = delete;
    UniquePtr& operator=(UniquePtr&& rhs) {
        VarDeRef(p);
        p = rhs.p;
        rhs.p = nullptr;
        return *this;
    }

    T& operator*() const {
        return *p;
    }
    T* operator->() const {
        return p;
    }
    T* get() const {
        return p;
    }
};

class StackFrame {
protected:
    RefCollector* rc;
public:
    StackFrame(): rc(CallFunction()) {}
    ~StackFrame() {
        assert(rc == GlobalManager().curFrame());
        ReturnFuntion();
    }
};

// #define SIMPLEGC_NEEDMACRO
#ifdef SIMPLEGC_MACRO
#define VARNEW(InitExpr) VarRef(new InitExpr)
#define GCNEW(InitExpr) GCRef(new InitExpr)
#define GLOBALNEW(InitExpr) GlobalRef(new InitExpr)
#define F_BEGIN StackFrame __sframe = StackFrame();
#define ASGC() AutoScanGC().gc()
#endif

} // namespace SimpleGC

#endif