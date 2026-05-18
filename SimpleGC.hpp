#ifndef SIMPLEGC
#define SIMPLEGC

#include <cassert>
#include <type_traits>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <functional>
#include <utility>

#include "ContextInfo.hpp"

namespace SimpleGC {

class Object;
class RefCollector;
class GC;
class AutoScanGC;

template <typename K, typename V, typename F>
void __SafeEraseUMap(std::unordered_map<K, V, F>& umap, const K& key) {
    if (umap.find(key) != umap.end()) {
        umap.erase(key);
    }
}

template <typename E, typename F>
void __SafeEraseUSet(std::unordered_set<E, F>& uset, const E& elem) {
    if (uset.find(elem) != uset.end()) {
        uset.erase(elem);
    }
}

template <typename K, typename V, typename F>
V __SafeGetUMap(const std::unordered_map<K, V, F>& umap, const K& key) {
    if (umap.find(key) != umap.end()) {
        return umap.at(key);
    }
    return V{};
}

class Object {
public:
    using RefT = std::function<void(Object*)>;
    virtual void trace(RefT Ref) = 0;
    virtual ~Object() {}
};

class RefCollector {
    friend class GC;
    friend class AutoScanGC;
protected:
    std::unordered_map<Object*, size_t> refCounts;

    void delRefPtr(Object* obj) {
        __SafeEraseUMap(refCounts, obj);
    }
public:
    template <typename T,
              typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
    T* incRef(T* obj) {
        ++refCounts[obj];
        return obj;
    }

    template <typename T,
              typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
    T* decRef(T* obj) {
        if (refCounts.find(obj) != refCounts.end()) {
            --refCounts[obj];
            if (refCounts[obj] == 0) {
                delRefPtr(obj);
                refCounts.erase(obj);
            }
        }
        return obj;
    }
};

class Manager {
    friend class GC;
    friend class AutoScanGC;
protected:
    std::unordered_set<Object*> allObjs;
    std::unordered_map<void*, Object*> originPtrIndex;
    std::unordered_map<Object*, void*> objectPtrIndex;
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

        void* originPtr = (void*)obj;
        Object* objPtr = obj;
        originPtrIndex.insert(std::make_pair(originPtr, objPtr));
        objectPtrIndex.insert(std::make_pair(objPtr, originPtr));

        return obj;
    }

    template <typename T,
              typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
    void delObject(T* obj) {
        Object* pObj = obj;
        void* origin = __SafeGetUMap(objectPtrIndex, pObj);
        
        __SafeEraseUSet(allObjs, pObj);
        __SafeEraseUMap(originPtrIndex, origin);
        __SafeEraseUMap(objectPtrIndex, pObj);
        
        delete obj;
    }

    void delObject(void* ptr) {
        Object* obj = __SafeGetUMap(originPtrIndex, ptr);
        
        if (obj) {
            __SafeEraseUSet(allObjs, obj);
            __SafeEraseUMap(originPtrIndex, ptr);
            __SafeEraseUMap(objectPtrIndex, obj);

            delete obj;
        }
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
        for (auto& [obj, _]: mgrRef.globalObjs->refCounts) {
            f(obj);
        }
        
        // Stack references
        for (RefCollector* frame: mgrRef.stackFrames) {
            for (auto& [obj, _]: frame->refCounts) {
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
        std::unordered_map<void*, Object*> objs = mgrRef.originPtrIndex;

        std::function<void(void*)> f;
        f = [&f, &objs](void* ptr) {
            Object* obj = __SafeGetUMap(objs, ptr);
            if (obj) {
                __SafeEraseUMap(objs, ptr);
                obj->trace(f);
            }
        };

        // Global references
        for (auto& [obj, _]: mgrRef.globalObjs->refCounts) {
            f(obj);
        }
        
        // Stack references
        void** base = (void**)GetStackBase();
        void** top = (void**)stackTop;
        for (void** p = top; p < base; ++p) {
            f((Object*)*p);
        }
        
        // Register references
        f((void*)ctx.Rax);
        f((void*)ctx.Rbx);
        f((void*)ctx.Rcx);
        f((void*)ctx.Rdx);
        f((void*)ctx.Rdi);
        f((void*)ctx.Rsi);
        f((void*)ctx.Rsp);
        f((void*)ctx.Rbp);
        f((void*)ctx.R8);
        f((void*)ctx.R9);
        f((void*)ctx.R10);
        f((void*)ctx.R11);
        f((void*)ctx.R12);
        f((void*)ctx.R13);
        f((void*)ctx.R14);
        f((void*)ctx.R15);

        // Collect references
        for (auto& [originPtr, objPtr]: objs) {
            mgrRef.delObject(originPtr);
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
    return GlobalManager().curFrame()->incRef(obj);
}

template <typename T,
          typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
static inline T* AddGlobalRef(T* obj) {
    return GlobalManager().globalFrame()->incRef(obj);
}

template <typename T,
          typename = std::enable_if_t<std::is_base_of_v<Object, T>>>
static inline T* RemoveStackRef(T* obj) {
    return GlobalManager().curFrame()->decRef(obj);
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

template <typename T, typename... Args>
T* Make(Args&&... args) {
    return GCRef(new T(std::forward<Args>(args)...));
}

#ifdef SIMPLEGC_MACRO
#define VARNEW(InitExpr) VarRef(new InitExpr)
#define GCNEW(InitExpr) GCRef(new InitExpr)
#define GLOBALNEW(InitExpr) GlobalRef(new InitExpr)
#define F_BEGIN volatile StackFrame __sframe = StackFrame();
#define ASGC() AutoScanGC().gc()
#endif

} // namespace SimpleGC

#endif
