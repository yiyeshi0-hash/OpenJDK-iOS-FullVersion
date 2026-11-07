# JDK8 MirrorMappedCodeCache (JIT26) 移植说明

把 herbrine 的 **jre17** 镜像补丁
`patches/jre_17/ios/2_mirror_mapping.diff`（1509 行）移植成 **jdk8/corretto-8** 可用的
`patches/jdk8u_ios_mirror_mapping.diff`。

| 项目 | 值 |
| --- | --- |
| 目标补丁 | `patches/jdk8u_ios_mirror_mapping.diff` |
| 规模 | **1578 行 / 32 文件 / 117 hunk**，`+414 −139` |
| 基底 | `corretto-8`（= jdk8u）+ `jdk8u_ios.diff` + `jdk8u_ios_xattr.diff` + `jdk8u_ios_fix_clang.diff` + `jdk8u_ios_chunk_mmap.diff` |
| 应用顺序 | 第 5 个，`buildjdk.sh` 里排在 `chunk_mmap` 之后 |
| 验证 | `git apply --check` 干净通过（0 reject，见下）；**未编译**（构建机是 Linux，产不出 iOS JDK） |
| 默认开关 | `-XX:+MirrorMappedCodeCache`（默认 false ⇒ 不开启时补丁完全惰性） |

---

## 1. 机制（与 jre17 完全一致）

代码缓存被映射两次，两块虚拟地址指向**同一批物理页**：

```
RX 别名  PROT_READ|PROT_EXEC   ← os::pd_reserve_memory() 返回它，CodeCache/CodeHeap/汇编器
                                 都把它当规范地址；CPU 从这里执行
RW 别名  PROT_READ|PROT_WRITE  ← vm_remap() 出来的第二份映射；所有写代码缓存的操作都改到这里
```

* `mirror_w(p)`：把 RX 地址换成 RW 别名（非代码缓存指针原样返回）
* `mirror_x(p)`：把 RW 地址换回 RX 别名（非代码缓存指针原样返回）
* `mirror_w_set(lvalue)`：通过 RW 别名写入

两个方向都是**幂等**的，所以同一个指针不管哪种"颜色"传进来都安全。

RX 别名的来源有两条：

1. 普通 `mmap(PROT_READ|PROT_EXEC)`；
2. 设备带 TXM（iOS 19+）时，`brk #0x69` 陷进调试器（JIT26 loader），由调试器代劳映射 RX 区
   并把基址放在 x0 返回（`BreakGetJITMapping`）。

所以 `JIT26` 字符串、`[JIT26] Requesting .. MB for JIT mapping` 这些标志位都跟 jre17 一样保留。

## 2. jdk8 的接入点为什么和 jre17 不同

| 关注点 | jre17 | jdk8（本补丁） |
| --- | --- | --- |
| 宏定义位置 | `os_bsd.hpp` + `decltype` | `runtime/os.hpp` **顶部**，用 **C++98 函数模板**（hotspot 8 用 `-std=gnu++98`，`decltype` 编译不过） |
| 递归包含风险 | 无 | `os.hpp → atomic.hpp → bytes.hpp → bytes_aarch64.hpp` 会在 `os_bsd.hpp` 之前用到 `mirror_w()`，因此宏必须放在 os.hpp 顶部；`os_bsd.hpp` 只保留 `Bsd` 的静态成员 |
| CodeBlob 边界 | `CodeBlobLayout` 类，逐字段 `mirror_x(...)` | jdk8 **没有** `CodeBlobLayout`：`header_end/content_*/code_*/data_*` 全部由 `header_begin()` 派生 ⇒ 只改 `header_begin()` 一行就覆盖全部边界 |
| `this` 的颜色 | `operator new` 返回 RW，`this` 是 RW | 同 jre17：`operator new` 返回 `mirror_w(CodeCache::allocate(...))`，构造函数里的普通字段赋值因此不用逐个改 |
| 代码缓存预留 | `os::pd_reserve_memory(bytes, exec)` | `os::pd_reserve_memory(bytes, requested_addr, alignment_hint, executable)`（jdk8 已有 exec 参数），钩子加 `requested_addr == NULL` 判定 |
| `CodeHeap` | `low()/high()` | `low_boundary()/high()`；另加 `segment_for()` 别名归一化（见 §4） |
| nmethod 存指针还是存偏移 | 存绝对指针（`_scopes_data_begin` 等） | **存偏移**，指针由 `this + offset` 现算 ⇒ jre17 那批 `_deopt_handler_begin = mirror_x(this) + ...` 在 jdk8 **不需要**（自动随 `header_begin()` 变 RX） |
| `CompiledMethod` | 独立类 | jdk8 **不存在**（nmethod 直接继承 CodeBlob），相应 setter 落到 `nmethod.hpp` |
| `Access` API（oop 存储） | `oops/accessBackend.hpp` 统一钩 `store_internal` | jdk8 没有 Access 层 ⇒ 改在 `nmethod::oop_addr_at()` 一处返回 RW 别名，GC 写嵌入 oop 就自动落 RW |
| `Atomic` | `Atomic::Platform{Store,Add,Cmpxchg}` 结构体 | jdk8 是 `atomic_bsd_aarch64.inline.hpp` 里的**自由函数**，逐个包 `mirror_w(dest)` |
| `isRWXJITAvailable()` | 改它来关掉 W^X 翻转 | jdk8 没这个 API；等价物是既有 ios 补丁的 **SIGBUS 逐页翻转**，镜像开启时直接不用它（见 §4） |
| 既有 ios 代码缓存策略 | 无 | jdk8 的 `ReservedCodeSpace` 会把整个代码缓存 `mprotect(RW)` 并靠 SIGBUS 翻转；镜像模式下必须跳过，否则 RX 别名丢执行权限 |

## 3. 移植清单（按层）

### 3.1 OS 层（`os/bsd/vm/os_bsd.{hpp,cpp}`）

* `os::Bsd::debug_jit_mapping_mirrored`、`mirrored_swap_wx/find_rw/find_rx`
* `BreakGetJITMapping`（`naked,noinline,optnone` + `brk #0x69`）、`DeviceRequiresTXMWorkaround()`、
  `get_debug_jit_mapping()`（RX 申请 + `vm_remap` + `vm_protect`，printf 文案与 jre17 逐字一致）
* `os_mirror_find_rw/rx`：给 os.hpp 模板用的 C 入口
* `os::pd_reserve_memory`：`executable && requested_addr == NULL` 时走镜像
* `os::pd_commit_memory`：镜像已存在时 `exec` 直接返回 true（**不能** mprotect，否则 RX 别名丢 EXEC）
* 新增 include：`<dirent.h>`、`<limits.h>`、`code/codeCache.hpp`

### 3.2 代码堆（`share/vm/memory/heap.{hpp,cpp}`）

* `CodeHeap::contains()` 接受 RW/RX 两种指针（`contains_internal` + `mirrored_swap_wx`）
* `segment_for()` 先用 `mirror_x()` 归一化（jre17 没做，见 §6）
* `HeapBlock::initialize/set_used/set_free`、`FreeBlock::set_length/set_link`
* 3 处 `memset(allocated_space(), badCodeHeap*Val, ...)`（ASSERT 路径）

### 3.3 CodeBlob / nmethod

* `CodeBlob::header_begin()` → `mirror_x(this)`（**关键一行**，全部边界随之变 RX）
* `set_name/set_frame_size/set_unpack_with_exception_in_tls_offset`、`set_oop_maps/flush`
* `BufferBlob/RuntimeStub/SingletonBlob::operator new` → `mirror_w(...)`；各 `create()` → `mirror_x(blob)`
* `nmethod::operator new` → `mirror_w(...)`；`new_nmethod/new_native_nmethod` 返回 `mirror_x(nm)`
* `register_nmethod/unregister_nmethod/CodeCache::commit` → `mirror_x(this)`
* `PcDescCache::reset_to/add_pc_desc`、`copy_scopes_pcs/copy_scopes_data`、`copy_values`、
  `initialize_immediate_oop`、`_oops_are_stale`、`_oops_do_mark_link`、`_state`、`_entry_bci`
* `nmethod::oop_addr_at()`：返回 RW 别名（GC 原地改写嵌入 oop）
* `nmethod.hpp` 全部字段 setter：hotness / rtm_state / unload_reported / has_flushed_dependencies /
  has_unsafe_access / has_method_handle_invokes / has_wide_vectors / lazy_critical_native /
  stack_traversal_mark / osr_link / method / exception_cache / scavenge_root_state
* `pcDesc.hpp`：`set_flag/set_pc_offset/set_scope_decode_offset/set_obj_decode_offset`

### 3.4 汇编层

* `asm/codeBuffer.hpp`：`CodeSection::initialize`（`start = mirror_x(start)`）、8 个 `emit_*`、
  `CodeBuffer::initialize`
* `asm/codeBuffer.cpp`：`copy_relocations_to` ×3、`relocate_code_to` ×3、`expand` 的 debug 填充
* `asm/assembler.cpp`：`_code_section` ×2、`start_a_const` 的 padding 填充
* `cpu/aarch64/vm/assembler_aarch64.hpp`：`Instruction_aarch64::patch/spatch`（`a = mirror_w(a)`）
* `cpu/aarch64/vm/bytes_aarch64.hpp`：`Bytes::put_native_u2/u4/u8`
* `cpu/aarch64/vm/nativeInst_aarch64.hpp`：`addr_rw_at()` + 5 个 `set_*_at`
* `cpu/aarch64/vm/nativeInst_aarch64.cpp`：`NativeMovConstReg::set_data`、`NativeMovRegMem::set_offset`、
  `NativeIllegalInstruction::insert`、`NativeJump::patch_verified_entry`
* `cpu/aarch64/vm/vtableStubs_aarch64.cpp`：两个 `create_*` 之后 `s = mirror_x(s)`

### 3.5 其它

* `code/stubs.cpp`（`_stub_buffer`）、`code/icBuffer.{hpp,cpp}`、`code/vtableStubs.{hpp,cpp}`、
  `code/dependencies.cpp`、`code/exceptionHandlerTable.cpp`
* `interpreter/interpreter.{hpp,cpp}`（`CodeletMark` 用 `mirror_w(_clet)`）、`interpreterRuntime.cpp`
  （`SignatureHandlerLibrary`）
* `os_cpu/bsd_aarch64/vm/atomic_bsd_aarch64.inline.hpp`：store/store_ptr/add/add_ptr/xchg/xchg_ptr/
  cmpxchg/cmpxchg_ptr 全部 `mirror_w(dest)`
* `runtime/globals.hpp`：`product(bool, MirrorMappedCodeCache, false, ...)`

## 4. jdk8 特有的三处"必需改动"（jre17 没有对应物）

1. **`virtualspace.cpp` `ReservedCodeSpace` 跳过 `protect_memory(MEM_PROT_RW)`**
   既有 ios 补丁会无条件把整块代码缓存 mprotect 成 RW；镜像模式下这块是 RX 别名，
   改成 RW 就直接丢掉执行权限。改成 `protect_rw` 布尔 + `debug_jit_mapping_mirrored != NULL` 时关闭。
2. **`os_cpu/bsd_aarch64/vm/os_bsd_aarch64.cpp` 的 SIGBUS W^X 翻转加守卫**
   镜像模式下不需要逐页翻转；真实 SIGBUS 必须落到正常崩溃处理（而不是被 `mprotect(RW)` 吞掉）。
   条件写成 `sig == SIGBUS && os::Bsd::debug_jit_mapping_mirrored == NULL`。
3. **`os::pd_uncommit_memory` 加守卫**（jre17 没有）
   镜像模式下对 exec 区 `uncommit`（`MADV_FREE`+`PROT_NONE`）会让 RX 别名失去支撑/执行权限，直接返回 true。

## 5. 简化 / 偏差（有意为之）

| 差异 | 原因 |
| --- | --- |
| 宏放在 `runtime/os.hpp` 而非 `os_bsd.hpp` | 避免 `os.hpp → atomic.hpp → bytes.hpp → bytes_aarch64.hpp` 的包含环导致 `mirror_w` 未定义 |
| 用函数模板 `mirror_find_rw/rx` 代替 `decltype` 宏 | jdk8 hotspot 是 C++98 |
| `mirror_w_set(x)` = `*mirror_find_rw(&(x))` | 同上，且两种别名都接受 |
| `mirror_w/x` 增加了"镜像未就绪就直接返回"的早退 | jdk8 的 `CodeCache::_heap` 在 `CodeCache::initialize()` 之前为 NULL，避免提前解引用 |
| `emit_int8/emit_int16` 也镜像了 | jre17 漏了这两个（同一模式），jdk8 一并补上 |
| `unregister_nmethod` 也用 `mirror_x` | 与 `register_nmethod` 的地址形态保持一致 |
| nmethod 构造函数里 `copy_values_to(this)` / `copy_code_and_locs_to(this)` **没有**改成传 `mirror_x(this)` | jdk8 里 `this` 本来就是 RW 别名，而 `CodeBuffer` 内部写已经 `mirror_w`，传哪种都等价 |
| `mmap` 后把 `MAP_FAILED` 归一成 0 | jre17 直接 `if(!buf_rx)`，`MAP_FAILED` 会漏过；行为等价但更严谨 |
| `CodeHeap::segment_for()` 归一化指针 | jre17 的 `contains()` 接受两种别名，但 `segment_for()` 用裸减法，RW 指针会算出垃圾索引；jdk8 里 `find_start/deallocate` 依赖它，所以补上 |

## 6. 没有移植的点（清单）

| jre17 的改动 | 处理 |
| --- | --- |
| `share/code/compiledMethod.{hpp,cpp}`（`_gc_data`、`set_has_unsafe_access`、`_exception_cache` 等） | jdk8 无此文件，等价 setter 已并入 `nmethod.hpp`（`_gc_data` 在 jdk8 不存在，跳过） |
| `share/oops/accessBackend.hpp::store_internal` | jdk8 无 Access API；改用 `nmethod::oop_addr_at()` 一处覆盖 GC 写嵌入 oop |
| `CodeBlobLayout` 类的 4 个构造/字段改动 | jdk8 无此结构，由 `header_begin()` 一行覆盖 |
| `CodeBlob::adjust_size()` | jdk8 无 `CodeCache::free_unused_tail()`，无对应物 |
| `DeoptimizationBlob::set{uncommon_trap,implicit_exception_uncommon_trap}_offset` | `INCLUDE_JVMCI` 字段，jdk8 没有（`_unpack_with_exception_in_tls` 已处理） |
| `os::Bsd::isRWXJITAvailable()` | jdk8 无此 API；由 SIGBUS 守卫取代（§4.2） |
| `CompiledMethod::mark_for_deoptimization` 的 `_mark_for_deoptimization_status` | jdk8 nmethod 无该字段 |
| `CodeBuffer::experimental`/`CodeSection::expand_locs` 等 jdk9+ 结构 | jdk8 无 |
| `nmethod` 构造里的 `_deopt_handler_begin/_deopt_mh_handler_begin/_scopes_data_begin = mirror_x(this)+off` | jdk8 存偏移、指针现算，随 `header_begin()` 自动变 RX |
| `InterpreterCodelet::initialize` 里的 `DEBUG_ONLY(::new(&_strings) CodeStrings())` | 未镜像（placement new 写进 RX 区）；**release 构建不受影响**，fastdebug/debug 构建的 interpreter 生成可能 fault |
| `VMStructs` / Serviceability Agent 的字段元数据 | 未更新（不影响运行） |
| ICache flush 用哪个别名 | 未改：`ICache::invalidate_range(cb->content_begin())` 仍是 RX 地址，依赖 AArch64 指令缓存按物理地址维护（与 jre17 相同的前提） |

## 7. 如何验证

### 7.1 补丁能干净应用（可在任意 Linux/CI 机器上做）

```bash
git clone --depth 1 https://github.com/corretto/corretto-8 openjdk && cd openjdk
git init -q . && git add -A && git -c user.email=a@b -c user.name=c commit -qm base   # 可选，仅便于回滚
for p in jdk8u_ios.diff jdk8u_ios_xattr.diff jdk8u_ios_fix_clang.diff jdk8u_ios_chunk_mmap.diff; do
  git apply --reject --whitespace=fix ../patches/$p
done
git apply --check -v ../patches/jdk8u_ios_mirror_mapping.diff && echo OK
```

本仓库实测（corretto-8 `develop`，8.512.09.1）：

```
$ git apply --check -v patches/jdk8u_ios_mirror_mapping.diff
Checking patch hotspot/src/share/vm/runtime/globals.hpp...
Checking patch hotspot/src/share/vm/runtime/os.hpp...
Checking patch hotspot/src/share/vm/runtime/virtualspace.cpp...
$ echo $?
0
```

> 注：前 4 个补丁本身在最新 corretto-8 上就有 6~8 个 hunk 被 reject（`(-1)` → `(~0u)` 之类的
> clang 兼容性小改），这是既有状态，与本次镜像补丁无关；本补丁在**这种 reject 后的树**上依然干净应用。

### 7.2 二进制里有 JIT26 符号（构建出 libjvm.dylib 之后）

```bash
strings libjvm.dylib | grep JIT26
# [JIT26] Requesting %zu MB for JIT mapping
# [JIT26] Got JIT mapping %p from debugger
# [JIT26] mapping at RW=%p, RX=%p
# [JIT26] Failed to allocate RX region
# [JIT26] Failed to remap RX region %d
# [JIT26] Failed to set RW protection %d
strings libjvm.dylib | grep -c MirrorMappedCodeCache   # 非 0
```

### 7.3 启动日志（真机）

启动参数需带 `-XX:+MirrorMappedCodeCache`，然后：

* **带 TXM 的设备（iOS 19+，`/private/preboot/*/.../Ap,TrustedExecutionMonitor.img4` 存在）**：
  日志出现 `[JIT26] Requesting <N> MB for JIT mapping` → `[JIT26] Got JIT mapping 0x... from debugger`
  → `[JIT26] mapping at RW=0x..., RX=0x...`
* **不带 TXM 的设备**：走 `mmap(PROT_READ|PROT_EXEC)` 分支，**只会**出现
  `[JIT26] mapping at RW=..., RX=...`，看不到 `Requesting`。（jre17 也是这个行为，
  `Requesting` 那行只在调试器路径打印。）
* 失败会打印 `Failed to allocate RX region` / `Failed to remap RX region <kern_return>` /
  `Failed to set RW protection <kern_return>`，并且 VM 回退到原有（SIGBUS 翻转）路径继续启动。

可以额外确认开关生效：`-XX:+PrintFlagsFinal` 里 `MirrorMappedCodeCache = true`。

## 8. 已知风险（未编译，按可能性排序）

1. **没有真正编译过**。最可能出问题的地方：`mirror_w` 模板对 `volatile` 指针的类型推导、
   个别 TU 没拿到 `runtime/os.hpp`、`CodeBlob::header_begin() const` 的 const 转换。
2. **代码缓存内的写路径可能仍有遗漏**。已覆盖：汇编发射、reloc 拷贝、nmethod 字段、
   oop 槽、堆块头、ICStub/InterpreterCodelet/VtableStub。未覆盖的边界写法会以硬 SIGBUS 形式暴露
   （注意：镜像模式下 SIGBUS 翻转被禁用，不会像旧路径那样"自动修好"，所以这类 crash 是**确定性的**，
   反而更容易定位）。
3. `CodeCache::find_blob()` 等用 RW 指针查询的路径：`contains()` + `segment_for()` 已兼容，
   但其它做裸指针算术的地方没有逐一审查。
4. 整个 `ReservedCodeCacheSize` 在启动时即双别名映射（两倍虚拟地址、同样大小）；物理页仍是懒分配。
5. fastdebug/debug 构建下 interpreter 生成的 `CodeStrings` placement-new 会写 RX 区（见 §6）。

## 9. 相关文件

* `patches/jdk8u_ios_mirror_mapping.diff` —— 本补丁
* `buildjdk.sh` —— 在 `jdk8u_ios_chunk_mmap.diff` 之后应用
* 参考：`https://github.com/herbrine8403/OpenJDK-iOS-FullVersion/blob/buildjre17-21/patches/jre_17/ios/2_mirror_mapping.diff`
