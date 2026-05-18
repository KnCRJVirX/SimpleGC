# SimpleGC

SimpleGC 是一个用于学习和实验的 C++ 简易垃圾回收器。项目核心是一个头文件 `SimpleGC.hpp`，实现了基于对象图遍历的 mark-sweep GC，并额外提供一个 Windows x64 下的保守栈扫描版本 `AutoScanGC`。

## 文件结构

```text
.
├── SimpleGC.hpp       # GC 主实现
├── ContextInfo.hpp    # Windows 下获取线程上下文和栈边界
├── tests/
│   └── base.cpp       # 覆盖手动根模式和 AutoScanGC 的测试
└── README.md
```

## 基本设计

所有希望被 GC 管理的对象都需要继承 `SimpleGC::Object`，并实现：

```cpp
void trace(RefT ref) override;
```

`trace()` 的职责是把当前对象持有的其他 GC 对象指针交给 `ref`。GC 从根对象出发调用 `trace()`，递归标记所有可达对象，最后删除不可达对象。

示例：

```cpp
struct Node : SimpleGC::Object {
    Node* left = nullptr;
    Node* right = nullptr;

    void trace(RefT ref) override {
        ref(left);
        ref(right);
    }
};
```

## 两种使用模式

SimpleGC 目前有两套使用方式，建议不要混用。

### 1. 手动根模式

手动根模式通过 `F_BEGIN` 创建函数栈帧，通过 `VARNEW` 把局部变量登记为根对象，然后使用 `DoGC()` 触发回收。

```cpp
#define SIMPLEGC_MACRO
#include "SimpleGC.hpp"

using namespace SimpleGC;

void foo() {
    F_BEGIN

    Node* root = VARNEW(Node());
    root->left = GCNEW(Node());

    DoGC();
}
```

常用接口：

- `VARNEW(T(...))`：创建对象，并登记为当前栈帧根对象
- `GCNEW(T(...))`：创建对象，只加入 GC 管理集合，不登记为根
- `GLOBALNEW(T(...))`：创建对象，并登记为全局根
- `DoGC()`：执行普通 mark-sweep GC
- `F_BEGIN`：进入一个手动 GC 栈帧

手动根模式是相对确定的：只要根集合和 `trace()` 写对，就能比较稳定地判断对象是否应该被回收。

### 2. 自动扫描模式

自动扫描模式使用 `AutoScanGC` 扫描机器栈和寄存器，不需要 `F_BEGIN` / `VARNEW`。但是对象仍然必须通过 GC 的分配入口登记，例如 `GCNew` 或 `Make`。

```cpp
#include "SimpleGC.hpp"

using namespace SimpleGC;

void foo() {
    Node* root = GCNew<Node>();
    root->left = GCNew<Node>();

    AutoScanGC().gc();
}
```

注意：下面这种普通 `new` 不会自动进入 GC 管理集合：

```cpp
Node* node = new Node(); // SimpleGC 不知道这个对象存在
```

如果要让 `AutoScanGC` 管理对象，至少需要：

```cpp
Node* node = GCNew<Node>();
// 或
Node* node = Make<Node>();
// 或
Node* node = GCRef(new Node());
```

`AutoScanGC` 当前只在 Windows 下启用，策略是保守栈扫描。

## API 速览

### 对象管理

- `GCNew<T>(args...)`：创建并登记一个 GC 对象
- `VarNew<T>(args...)`：创建、登记并加入当前栈帧根集合
- `GlobalNew<T>(args...)`：创建、登记并加入全局根集合
- `Make<T>(args...)`：`GCNew` 的简化工厂
- `GCRef(ptr)`：把已有对象指针登记到 GC 管理器
- `VarRef(ptr)`：登记对象，并加入当前栈帧根集合
- `GlobalRef(ptr)`：登记对象，并加入全局根集合
- `Del(ptr)`：立即删除已登记对象，并把调用方指针置空

### 根集合

- `AddStackRef(ptr)`：增加当前栈帧中的根引用计数
- `RemoveStackRef(ptr)`：减少当前栈帧中的根引用计数
- `CallFunction()` / `ReturnFuntion()`：手动创建和退出栈帧
- `StackFrame`：RAII 栈帧对象，宏 `F_BEGIN` 基于它实现

### 回收器

- `DoGC()`：执行手动根模式的 mark-sweep GC
- `GC().gc()`：等价于直接创建普通 GC 并运行
- `AutoScanGC().gc()`：执行 Windows x64 保守栈扫描 GC

## 测试

当前测试位于 `tests/base.cpp`，覆盖内容包括：

- 手动根对象保活和栈帧退出后回收
- `trace()` 保活子对象
- 不可达环引用回收
- `AddStackRef` / `RemoveStackRef` 引用计数
- `UniquePtr` 的 RAII 根管理
- `Del` 删除并移出 GC 表
- `Make` 工厂函数
- 全局根保活
- Windows x64 下 `AutoScanGC` 的基本行为

使用 MinGW g++ 编译：

```powershell
g++ -std=c++20 -Wall -Wextra -pedantic -g tests\base.cpp -o tests\base.exe
.\tests\base.exe
```

如果你有独立的 `tests/test.cpp`，也可以类似编译：

```powershell
g++ -std=c++20 -Wall -Wextra -pedantic -g tests\test.cpp -o tests\test.exe
.\tests\test.exe
```

## 已知限制

- 手动根模式需要显式使用 `F_BEGIN`、`VARNEW` 或相关根登记接口。
- `AutoScanGC` 是保守扫描，可能因为栈上残留值导致对象被误保活。
- `AutoScanGC` 不能管理普通 `new` 出来的对象，除非先通过 `GCRef` / `GCNew` / `Make` 登记。
- 当前实现不是线程安全的。
- 当前实现没有分代、增量、并发、移动压缩等高级 GC 特性。
- `GLOBALNEW` 创建的全局根当前没有对应的移除接口。
