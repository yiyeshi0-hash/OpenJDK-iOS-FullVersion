# JDK8 代码缓存 RX 翻转 + SIGBUS 页翻转（air2）

给 `MirrorMappedCodeCache`（JIT26 双别名）补上"写 RX 页"的兜底路径，解决
`CodeCache: used=11MB / free=152MB` 却报
`compilation: disabled (not enough contiguous free space left)` 的问题。

| 项目 | 值 |
| --- | --- |
| 目标补丁 | `patches/jdk8u_ios_rx_toggle.diff` |
| 规模 | **527 行 / 5 文件 / 7 hunk**，`+419 −19` |
| 基底 | `corretto-8`(develop) + `jdk8u_ios.diff` + `jdk8u_ios_xattr.diff` + `jdk8u_ios_fix_clang.diff` + `jdk8u_ios_mirror_mapping.diff` |
| 应用顺序 | 第 5 个，`buildjdk.sh` 里排在 `mirror_mapping` 之后（已加） |
| 验证 | `git apply --check` **rc=0**；`patch -p1 --dry-run` 通过；改动块级 `-std=gnu++98` 语法检查 clang14 + g++11 均 0 error |
| 开关 | 惰性：仅在 `-XX:+MirrorMappedCodeCache` 且映射成功时生效；另加 `-XX:+A2InlineRxWrites`（默认 false）做 A/B |
| 编译 | **未编译整包、未上真机**（本机是 Windows/Linux，iOS SDK 在 Mac 上） |

---

## 1. 方案选择：为什么是 SIGBUS 页翻转，不是把 CodeHeap 挪到 RW 别名

任务给了三条路，逐条对照：

| 方案 | 结论 | 理由 |
| --- | --- | --- |
| (a) 只靠 `mirror_w()` 把写重定向到 RW 别名 | **不够** | 现网就是这样，真机 11MB 触顶；AAMC 成品同样症状 |
| (b) SIGBUS/SIGSEGV 逐页翻转（**本补丁**） | **采用** | 最小侵入、可失败降级、复用 jdk8 已有机制 |
| (c) CodeHeap 基址统一到 RW 别名，执行时 `mirror_x` 转 RX | **否决** | 见下 |

**(c) 为什么不行**

1. **执行地址必须是 RX。** nmethod 入口、relocation、vtable stub、解释器跳转里烘的
   都是代码缓存的规范地址，CPU 只能从 RX 取指。把 CodeHeap 基址改成 RW，等于要求
   *每次取指* 都做一次地址翻译——不是"改几行"，是要把 `mirror_*` 从偶尔调用变成
   取指路径上的常量开销，既做不对也无法验证。
2. **它不解决根因。** 写 RW 别名对 RX 别名是否可见，取决于 XNU 给共享映射加 `W`
   时有没有做 COW 分裂。若分裂了，读的一方（`HeapBlock::_length` / free list）
   看到的还是旧值，分配器照样错乱——**恰恰就是 11MB 那个形状**。换基址不改变可见性。
3. 影响面非常大：`CodeBlob` 的 `header_begin()`、`contains()`、`find_start()`、
   `segment_for()` 全部要重新定义"规范颜色"，任何一处漏掉就是随机崩溃。

**(b) 为什么可行**：jdk8 本来就有这条路径。`jdk8u_ios.diff` 给 `ReservedCodeSpace`
用的是"SIGBUS 逐页 RW↔RX 翻转"；`mirror_mapping` 因为"两个别名已经解决 W^X"把它
整块关掉了（`if (sig == SIGBUS && mirrored == NULL)`）。本补丁不是发明新机制，而是
把这条老路径改成**镜像感知**，并且复用同一个入口（`JVM_handle_bsd_signal`），不额外
`sigaction`、不抢 JVM 自己的信号处理。

失败可控也是选它的原因：所有 `mprotect` 失败只 `printf`，不 abort；有 8192 次翻转
上限，超限就把故障交回正常崩溃处理，不会把真 bug 变成静默。

---

## 2. 具体改了什么

### 2.1 窗口记录 + 页翻转 API（`os/bsd/vm/os_bsd.{hpp,cpp}`）

```cpp
static address debug_jit_rx_base;   // RX 别名基址（get_debug_jit_mapping 里记）
static size_t  debug_jit_rx_size;

static bool a2_rx_contains(address addr);
static bool a2_rx_make_writable(address addr, size_t len);
static void a2_rx_restore(address addr, size_t len);
static void a2_rx_restore_all();
static bool a2_rx_handle_fault(int sig, siginfo_t* info, void* ucVoid);
```

* 页对齐向上/向下取整，夹到窗口内；**按页记录原状态**：`RX / RW / RWX`，一页一字节
  （160MB / 16KB = 10240 页 ⇒ 10KB 表，`::malloc` 出来）。
* 翻转**优先 `RWX`**：保留 `PROT_EXEC`，正在执行同页代码的线程不会被踩空；iOS 拒绝
  W+X 时才退成 `RW`。
* 并发：`PTHREAD_MUTEX_RECURSIVE` + `trylock`。信号处理器里 `trylock` 失败也照做
  （`mprotect` 本身幂等，最坏是重复一次），不会因为抢锁死锁。
* 失败只打印，文案与 AAMC 成品对齐，方便按日志比对：
  ```
  [JIT26] Failed to make RX region writable: %d
  [JIT26] Failed to restore RX protection: %d
  ```

### 2.2 接上 SIGBUS / SIGSEGV（`os_cpu/bsd_aarch64/vm/os_bsd_aarch64.cpp`）

`JVM_handle_bsd_signal()` 顶部，mirror 开启时先把故障交给 `a2_rx_handle_fault()`：

* 取故障地址：`info->si_addr`，拿不到时用 `__DARWIN_ARM64` 的 `uc_mcontext->__es.__far`；
* **取指故障**（`pc` 在窗口内且 `far == pc`，即之前被迫剥了 EXEC）→ 把 EXEC 补回去，重试；
* **写故障**（`far` 在窗口内、`pc` 在窗口外）→ 该页翻成可写，内核返回时重执行那条 store；
* 不在窗口 → 返回 false，落回原有崩溃处理（真崩溃不受影响）。

arm64/iOS 对保护违规有时给 `SIGBUS`、有时给 `SIGSEGV`，两条都接。

### 2.3 什么时候恢复成 RX

翻页后页面保持可写，直到：

1. **下一个 safepoint 结束**——`SafepointSynchronize::end()` 里调 `a2_rx_restore_all()`
   （此时所有线程都停在 safepoint，没有线程在已被剥掉 EXEC 的页里跑）；
2. 或者显式 `a2_rx_restore(addr, len)`；
3. 若某页在恢复前被取指 → §2.2 的取指故障路径立刻把 EXEC 还回去再重试（**自愈**）。

`safepoint.cpp` 只多了一次 `int` 读（`dirty == 0` 立即返回），空闲时零成本。

### 2.4 翻译基准修正（很可能是 11MB 的真因之一）

`mirror_w()/mirror_x()` 原来拿 `CodeCache::low_bound()` 当 RX 基址算偏移。两个隐患：

* `CodeHeap::reserve()` 里已经在写 block header 时，`CodeCache::low_bound()` **还是 NULL**；
* 一旦 `ReservedSpace` 的对齐逻辑裁掉了预留区头部，`low_bound() ≠ 记录下来的 RX 基址`，
  于是**每个** `mirror_w()` 写整体偏移一个常量 → 记账静默错位，正好长成
  "used 11MB / free 152MB / 没有连续空间"。

现在以记录的 `debug_jit_rx_base` 为准，`low_bound()` 只作兜底；两者不一致时打一次警告，
delta 直接给出来：

```
[JIT26] WARNING: CodeCache::low_bound=0x132cd4000 != JIT26 RX base=0x134400000 (delta=...)
```

（真机日志里这两个数确实不等，所以这条警告大概会立刻出现。）

### 2.5 写穿透探针——本补丁最值钱的一行

`get_debug_jit_mapping()` 建好两个别名后立刻做一次往返：

```
*RW[0:4] = 0xA2C0DE01;  seen = *RX[0:4];  *RW[0:4] = 原值;
```

打印

```
[JIT26] alias write-through: OK (RW stores visible through RX)
[JIT26] alias write-through: BROKEN (RW stores NOT visible through RX, use -XX:+A2InlineRxWrites)
```

这一行直接判定"双别名到底共不共享物理页"。AAMC 成品也对不上时，这是唯一能一次性
说清根因的证据；下次真机日志就能收敛（不需要再猜 COW 还是 mprotect）。

### 2.6 A/B 开关 `-XX:+A2InlineRxWrites`（默认 false）

探针报 `BROKEN` 时打开：`mirror_w()` 不再返回 RW 别名，而是把 RX 那一页翻成可写、
**就地写**，靠 safepoint 恢复。写的人和读的人是同一批页，不存在可见性问题。

默认关闭，因为热路径多一次页表查表，且会周期性剥掉 EXEC（多几次信号）。

---

## 3. 影响面

| 维度 | 影响 |
| --- | --- |
| 生效条件 | `__APPLE__` + `-XX:+MirrorMappedCodeCache` + `get_debug_jit_mapping()` 成功；否则所有新函数首行即返回 |
| 非代码缓存地址 | `mirror_w/mirror_x/mirrored_swap_wx` 仍原样返回（`swap_wx` 现在两个方向都判窗口，不再盲目平移） |
| 写路径 | 不碰 nmethod / CodeBuffer / 汇编器的任何一处（mirror 补丁一行未改） |
| 信号处理 | 只在 mirror 开启时先试水；不在窗口内的故障行为与打补丁前完全一致 |
| 内存 | 页状态表 `size/page` 字节（160MB ⇒ 10KB） |
| 新增 flag | `globals.hpp` 一行 product flag（`A2InlineRxWrites`，默认 false） |
| 老路径 | mirror 关闭时 `ReservedCodeSpace` 的 SIGBUS 逐页翻转**保持原样**（含它 `return !mprotect(...)` 的返回值语义，本次没动） |

---

## 4. 未验证项 / 风险

1. **没编译、没上真机**。这是本补丁最大的未验证点；只有块级语法检查（§6）。
2. **`mprotect` 语义未验证**：iOS 26 上 "RX → RWX" 与 "RX → RW" 到底哪个被允许，
   只能看设备日志。两条路都写了，哪条生效未知。
3. **恢复窗口期的取指**：若 safepoint 恢复前有线程恰好从被剥 EXEC 的页取指，靠
   取指故障自愈；自愈本身要进一次信号，若那时再失败就是真崩溃（概率低，不是零）。
4. **TXM 设备**：若 RX 区是调试器（`brk #0x69`）给的，该区 `max_prot` 可能只有 RX，
   翻页会失败 → 日志出 `Failed to make RX region writable`，此时自动退回 RW 别名写。
5. **11MB 的确切触发点仍是推断**（commit/expand 失败？block header 读到旧值？）。
   补丁加的三条日志就是为了把它钉死，而不是声称已经证明。
6. 三个旧补丁在**当前** corretto-8 tip 上各有 1 个 hunk reject，与本补丁无关，但会
   一起出现在构建日志里，别误判成本补丁的锅。逐个核实过：
   * `dependencies.hpp` / `cpCache.hpp`：上游已改成 `(~0u) << ...`，reject 是**无操作**
     （补丁想改的东西上游已经改了）。
   * `hotspot/make/bsd/makefiles/gcc.make`：上游把那段重构成 `MAC_FLAGS = -mmacosx-version-min=...`，
     所以 iOS 补丁里 `-mmacosx-version-min` → `-miphoneos-version-min` 的交换落不了地。
     从新 clone 构建时要看眼：需要改成 `MAC_FLAGS = -miphoneos-version-min=$(MACOSX_VERSION_MIN)`
     （现有构建能过，说明 CFLAGS 实际由 configure/buildjdk.sh 主导，但这个 hunk 是死的）。

---

## 5. 真机判据

启动即应看到（stdout，launcher 会收）：

```
[JIT26] Requesting 160 MB for JIT mapping
[JIT26] Got JIT mapping 0x...            (TXM 设备) 或直接
[JIT26] mapping at RW=0x..., RX=0x...
[JIT26] RX toggle armed: base=0x... size=160MB page=16KB
[JIT26] alias write-through: OK|BROKEN
```

然后按顺序看：

| # | 判据 | 含义 |
| --- | --- | --- |
| 1（主） | `CodeCache: size=163840Kb used=...` 的 **used 随游玩增长**（越过 11MB，逐步 20/40/80MB），且 `compilation:` **不再**变成 `disabled (not enough contiguous free space left)` | 修好了 |
| 2 | 出现 `[JIT26] RX toggle: write fault at 0x... made writable` | 确实有绕过 `mirror_w()` 的写，方案命中 |
| 3 | 若是 `alias write-through: BROKEN`，且判据 1 仍不成立 | 加 `-XX:+A2InlineRxWrites` 重跑，对比 used 曲线；仍不动 ⇒ 问题不在可见性 |
| 4 | `WARNING: CodeCache::low_bound=... != JIT26 RX base=...` | §2.4 的裁剪假设成立，delta 就是偏移量 |
| 5（反例） | 大量 `Failed to make RX region writable: 1/13` | 设备不允许翻页 ⇒ 本方案对该机无效，要换 `MAP_JIT` + `pthread_jit_write_protect_np` 路线 |
| 6 | `-XX:+PrintCompilation` 有持续输出；MC 1.12 从 20 秒/帧回到正常 | 端到端判据 |

判据 1 是唯一算数的：**used 增长 + `compilation: enabled` 保持**。其余都是辅助定位。

---

## 6. 复现与验证记录

**应用检查（本机做过）**

```bash
# 1) 取基底（只要 hotspot，稀疏 + blob 过滤，~87MB）
git clone --filter=blob:none --no-checkout --depth 1 https://github.com/corretto/corretto-8.git corretto8
cd corretto8 && git sparse-checkout init --cone && git sparse-checkout set hotspot
git checkout develop && git config core.autocrlf false

# 2) 依次应用前四个补丁（热点部分）
for p in jdk8u_ios jdk8u_ios_xattr jdk8u_ios_fix_clang jdk8u_ios_mirror_mapping; do
  git apply --reject --include='hotspot/*' --whitespace=fix ../patches/$p.diff
done

# 3) 本补丁
git apply --check patches/jdk8u_ios_rx_toggle.diff     # rc=0
patch -p1 --dry-run < patches/jdk8u_ios_rx_toggle.diff # 全部 checking file 通过
```

> **行尾说明**：仓库里各补丁的 blob 都是 LF，macOS 上 `git clone` 出来就是 LF，
> 直接 `git apply` 即可，**不需要**任何转换。只有在 Windows 上（`core.autocrlf=true`）
> 检出的工作区里，补丁和源码会同时变成 CRLF；此时若混用 LF/CRLF 会看到
> `patch does not apply`。本机验证时踩过一次这个坑：把补丁 blob 用
> `git show HEAD:patches/... > x.diff` 取出来（LF）再对 LF 源码 `git apply` →
> 5 个文件全部 `Applied ... cleanly`。

**最终验证（用推上去的那个分支重做，不是本地副本）**

```
git clone -b air2-rx-toggle → git show HEAD:patches/jdk8u_ios_rx_toggle.diff (sha256 0cad2a85…)
worktree at base commit → git apply -v → Applied patch ... cleanly ×5, rc=0
```

**块级语法检查（本机做过）**：把 `os_bsd.cpp` 里新增的两段代码（`debug_jit_rx_base`
到 `a2_rx_handle_fault` 结束、`a2_mirror_rx_lo` 到 `mirrored_find_rx` 结束）**原样**切
到一个 TU 里，配上与 hotspot 同形的桩声明（`address = unsigned char*`、`os::Bsd` 嵌套类、
`MirrorMappedCodeCache/A2InlineRxWrites`、`CodeCache::low_bound/high_bound`），然后：

```
clang++ 14  -std=gnu++98 -fsyntax-only -Wall -Wextra   → 0 error
g++ 11      -std=gnu++98 -fsyntax-only -Wall           → 0 error
```

工具已随分支入库，任何有 clang/g++ 的机器都能重跑（不需要 iOS SDK）：

```bash
tools/rx-syntaxcheck/run_check.sh /path/to/openjdk   # 已应用本补丁的树
# → region A/B 行号 + clang++ ok + g++ ok
```

**没做的检查**：全文件 `-fsyntax-only`。缺 iOS SDK 的 `<mach/mach.h>` `<mach-o/dyld.h>`
`<objc/objc-auto.h>`、缺 Darwin 的 `__builtin_available` 目标 ⇒ 无法检查，不是"检查通过"。
