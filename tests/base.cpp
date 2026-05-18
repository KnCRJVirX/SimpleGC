#include <cstdlib>
#include <iostream>
#include <string>

#define SIMPLEGC_MACRO
#include "../SimpleGC.hpp"

using namespace SimpleGC;

namespace {

#if defined(_MSC_VER)
#define SIMPLEGC_TEST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define SIMPLEGC_TEST_NOINLINE __attribute__((noinline))
#else
#define SIMPLEGC_TEST_NOINLINE
#endif

struct Stats {
    int constructed = 0;
    int destroyed = 0;

    int alive() const {
        return constructed - destroyed;
    }

    void reset() {
        constructed = 0;
        destroyed = 0;
    }
};

Stats nodeStats;
Stats globalStats;

struct Node : SimpleGC::Object {
    int id;
    Node* left = nullptr;
    Node* right = nullptr;

    explicit Node(int id_ = 0) : id(id_) {
        ++nodeStats.constructed;
    }

    ~Node() override {
        ++nodeStats.destroyed;
    }

    void trace(RefT ref) override {
        ref(left);
        ref(right);
    }
};

struct GlobalNode : SimpleGC::Object {
    explicit GlobalNode() {
        ++globalStats.constructed;
    }

    ~GlobalNode() override {
        ++globalStats.destroyed;
    }

    void trace(RefT) override {}
};

struct TestRunner {
    int total = 0;
    int failed = 0;
    int warnings = 0;

    void expect(bool condition, const std::string& name) {
        ++total;
        if (condition) {
            std::cout << "[PASS] " << name << '\n';
        } else {
            ++failed;
            std::cout << "[FAIL] " << name << '\n';
        }
    }

    void observe(bool condition, const std::string& name, const std::string& warning) {
        ++total;
        if (condition) {
            std::cout << "[PASS] " << name << '\n';
        } else {
            ++warnings;
            std::cout << "[WARN] " << name << "：" << warning << '\n';
        }
    }
};

void collectManual() {
    SimpleGC::DoGC();
}

// Manual root mode: VARNEW registers a local root in the current StackFrame.
// The object must survive while the frame lives, then be collected afterward.
void runManualRootKeepsObject(TestRunner& t) {
    nodeStats.reset();

    {
        F_BEGIN
        Node* root = VARNEW(Node(1));
        collectManual();
        t.expect(nodeStats.destroyed == 0, "手动根在栈帧存活期间保留对象");
        (void)root;
    }

    collectManual();
    t.expect(nodeStats.constructed == 1 && nodeStats.destroyed == 1,
             "栈帧退出后手动根对象被回收");
}

// Object graph tracing: a root object should keep children alive through trace().
// Once the edge is removed, the detached child should become collectible.
void runTraceKeepsReachableChild(TestRunner& t) {
    nodeStats.reset();

    {
        F_BEGIN
        Node* root = VARNEW(Node(1));
        Node* child = GCNEW(Node(2));
        root->left = child;

        collectManual();
        t.expect(nodeStats.destroyed == 0, "trace 保留从根对象可达的子对象");

        root->left = nullptr;
        collectManual();
        t.expect(nodeStats.constructed == 2 && nodeStats.destroyed == 1,
                 "断开引用后的子对象被回收");
    }

    collectManual();
    t.expect(nodeStats.constructed == 2 && nodeStats.destroyed == 2,
             "栈帧退出后根对象被回收");
}

// Mark-sweep behavior: a cycle with no external root should still be reclaimed.
// This distinguishes the collector from plain reference counting.
void runCycleCollection(TestRunner& t) {
    nodeStats.reset();

    Node* a = GCNEW(Node(1));
    Node* b = GCNEW(Node(2));
    a->left = b;
    b->left = a;

    collectManual();
    t.expect(nodeStats.constructed == 2 && nodeStats.destroyed == 2,
             "不可达环引用会被标记清扫回收");
}

// Manual stack roots are counted, so duplicate AddStackRef calls require the
// same number of RemoveStackRef calls before the object becomes unreachable.
void runRefCountingInFrameTable(TestRunner& t) {
    nodeStats.reset();

    Node* node = GCNEW(Node(1));
    SimpleGC::AddStackRef(node);
    SimpleGC::AddStackRef(node);

    collectManual();
    t.expect(nodeStats.destroyed == 0, "多个栈引用会保留对象");

    SimpleGC::RemoveStackRef(node);
    collectManual();
    t.expect(nodeStats.destroyed == 0, "剩余一个栈引用时对象仍然存活");

    SimpleGC::RemoveStackRef(node);
    collectManual();
    t.expect(nodeStats.constructed == 1 && nodeStats.destroyed == 1,
             "移除最后一个栈引用后对象被回收");
}

// UniquePtr is a RAII wrapper for manual root mode: construction registers a
// stack root, moving transfers ownership, and destruction removes the root.
void runUniquePtrRoot(TestRunner& t) {
    nodeStats.reset();

    {
        F_BEGIN
        SimpleGC::UniquePtr<Node> ptr(new Node(1));
        collectManual();
        t.expect(nodeStats.destroyed == 0, "UniquePtr 会注册栈根");

        SimpleGC::UniquePtr<Node> moved(std::move(ptr));
        collectManual();
        t.expect(nodeStats.destroyed == 0, "移动后的 UniquePtr 仍保留根对象");
        (void)moved;
    }

    collectManual();
    t.expect(nodeStats.constructed == 1 && nodeStats.destroyed == 1,
             "UniquePtr 析构时移除栈根");
}

// Del should immediately delete a registered object, clear the caller's
// pointer, and remove it from the GC tables so later GC passes ignore it.
void runManualDel(TestRunner& t) {
    nodeStats.reset();

    Node* node = GCNEW(Node(1));
    SimpleGC::Del(node);

    t.expect(node == nullptr, "Del 会把调用方指针置空");
    t.expect(nodeStats.constructed == 1 && nodeStats.destroyed == 1,
             "Del 会立即删除已登记对象");

    collectManual();
    t.expect(nodeStats.destroyed == 1, "Del 会从 GC 表中移除对象");
}

// Make is the convenience factory for GC-managed allocation.
// This test checks that its result is registered in the manager tables.
void runMakeFactory(TestRunner& t) {
    nodeStats.reset();

    Node* node = SimpleGC::Make<Node>(1);
    t.expect(node != nullptr && nodeStats.constructed == 1,
             "Make 会构造并登记对象");

    SimpleGC::Del(node);
    t.expect(node == nullptr && nodeStats.destroyed == 1,
             "Make 返回的对象可以通过 GC 表删除");
}

// Global roots are intentionally never removed in this API, so the object
// should survive a normal manual GC pass.
void runGlobalRoot(TestRunner& t) {
    globalStats.reset();

    GlobalNode* node = GLOBALNEW(GlobalNode());
    collectManual();
    t.expect(globalStats.constructed == 1 && globalStats.destroyed == 0,
             "全局根会在手动 GC 后继续存活");
    (void)node;
}

#ifdef _WIN64
// Best-effort stack scrubbing before AutoScanGC tests. This reduces stale
// pointer values in this test file, but cannot make conservative scanning exact.
void scrubStack() {
    void* volatile scratch[256]{};
    for (int i = 0; i < 256; ++i) {
        scratch[i] = nullptr;
    }
    (void)scratch[0];
}

// Auto-scan mode: collect by scanning machine stack/registers instead of using
// F_BEGIN/VARNEW roots.
void collectAuto() {
    scrubStack();
    SimpleGC::AutoScanGC().gc();
}

// AutoScanGC should preserve a GCNew object while a live local variable still
// points to it on the stack or in captured registers.
void runAutoKeepsStackObject(TestRunner& t) {
    nodeStats.reset();

    Node* node = SimpleGC::GCNew<Node>(1);
    collectAuto();
    t.expect(nodeStats.destroyed == 0, "AutoScanGC 会保留栈上引用的 GCNew 对象");
    (void)node;
}

// Creates a GC-managed object without returning its pointer, so after the call
// there is no intentional C++ root left.
void makeAutoGarbage() {
    (void)SimpleGC::GCNew<Node>(1);
}

// AutoScanGC may collect this object after the creator frame returns. This is
// observed, not required, because conservative scanning can produce false roots.
void runAutoCollectsUnreferencedObject(TestRunner& t) {
    nodeStats.reset();

    makeAutoGarbage();
    collectAuto();
    t.observe(nodeStats.constructed == 1 && nodeStats.destroyed == 1,
              "AutoScanGC 会在栈引用消失后回收 GCNew 对象",
              "保守栈扫描可能把残留的类指针值误判为根");
}

// Creates a cyclic garbage graph in a separate frame for AutoScanGC.
// noinline keeps the test shape stable enough to exercise stack-frame return.
SIMPLEGC_TEST_NOINLINE void makeAutoCycleGarbage() {
    Node* a = SimpleGC::GCNew<Node>(1);
    Node* b = SimpleGC::GCNew<Node>(2);
    a->left = b;
    b->left = a;
}

// AutoScanGC can reclaim cycles when no pointer-like stack/register values keep
// them alive. This remains an observation due to conservative false positives.
void runAutoCollectsUnreachableCycle(TestRunner& t) {
    nodeStats.reset();

    makeAutoCycleGarbage();
    collectAuto();
    t.observe(nodeStats.constructed == 2 && nodeStats.destroyed == 2,
              "AutoScanGC 会回收不可达环引用",
              "保守栈扫描可能把残留的类指针值误判为根");
}
#endif

} // namespace

#ifdef _WIN32
#include <Windows.h>

void WinInit() {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
}
#endif

int main() {
    #ifdef _WIN32
    WinInit();
    #endif

    TestRunner t;

    runManualRootKeepsObject(t);
    runTraceKeepsReachableChild(t);
    runCycleCollection(t);
    runRefCountingInFrameTable(t);
    runUniquePtrRoot(t);
    runManualDel(t);
    runMakeFactory(t);

#ifdef _WIN64
    runAutoKeepsStackObject(t);
    runAutoCollectsUnreferencedObject(t);
    runAutoCollectsUnreachableCycle(t);
#else
    std::cout << "[跳过] AutoScanGC 测试需要 _WIN64\n";
#endif

    runGlobalRoot(t);

    std::cout << "\n总数：" << t.total
              << "，失败：" << t.failed
              << "，警告：" << t.warnings << '\n';
    return t.failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
