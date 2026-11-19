# Theora 块边界滤波、解码重建与显示后处理的责任边界

本文基于本仓库（`libtheora`）源码与随附规范 `doc/spec/spec.tex`，追踪三条容易混淆的处理链：

1. **入环去块滤波（in-loop / loop filter）**：解码重建的一部分，其结果**写回参考帧**，因而影响后续帧预测。
2. **解码重建（reconstruction）**：逆变换 + 预测相加 + 未编码块拷贝，产出 `OC_FRAME_SELF` 缓冲区。
3. **出环显示后处理（out-of-loop post-processing / PP）**：默认关闭，只写入独立的显示缓冲区 `pp_frame_buf`，**不**回写参考帧。

报告的重点是划清责任边界：滤波强度从何而来、未编码块如何被滤波影响、哪些步骤写回参考、哪些步骤只影响显示，以及画面平滑 / 跨帧一致性 / 边缘保留三者的取舍；并给出定向测试证据。

---

## 1. 三段流水线的总体位置

解码入口 `th_decode_packetin()`（`lib/decode.c:2740`）在完成熵解码后，对每个 MCU 条带按平面依次执行：

```
oc_dec_pipeline_init()                     lib/decode.c:1329
  └─ oc_dec_postprocess_init()             lib/decode.c:1204   （决定本条带是否做 PP）
for each plane pli:
  oc_dec_dc_unpredict_mcu_plane()          lib/decode.c:1392
  oc_dec_frags_recon_mcu_plane()           lib/decode.c:1511   （重建 + 拷贝未编码块）
  if(pipe.loop_filter)
    oc_state_loop_filter_frag_rows()       lib/state.c:1055    （入环，写参考帧）
  oc_state_borders_fill_rows()             lib/state.c:770     （参考帧边界填充）
  if(pipe.pp_level >= DEBLOCKY+pp_offset)
    oc_dec_deblock_frag_rows()             lib/decode.c:1700   （出环去块，写 pp_frame_buf）
    if(pipe.pp_level >= DERINGY+pp_offset)
      oc_dec_dering_frag_rows()            lib/decode.c:1892   （出环去环，写 pp_frame_buf）
```

调用点集中在 `lib/decode.c:2854-2930`。源码注释（`lib/decode.c:2837-2852`）明确说明：DC 预测逆转、重建、未编码块拷贝按 1~2 个超级块行流水；随后是**尽量向前推进的入环滤波**、边界填充、**出环后处理**。所以三者的分界在源码与注释里都被显式表达。

关键的结构性事实：

* 入环滤波的目标缓冲由 `OC_FRAME_SELF` 指定（`lib/decode.c:2882-2884`，参数 `OC_FRAME_SELF`），而 `OC_FRAME_SELF` 在本帧末尾被提升为 `OC_FRAME_PREV`（`lib/decode.c:2957-2961`）。
* 出环去块/去环的目标是 `_dec->pp_frame_buf`，来源是 `_dec->state.ref_frame_bufs[refi]`（`lib/decode.c:2899-2908`），**从不写回** `ref_frame_bufs`。
* 公开输出 `th_decode_ycbcr_out()` 返回的是 `pp_frame_buf` 的翻转视图（`lib/decode.c:2988-2991`），即**显示输出永远来自 PP 缓冲，而不是参考帧本身**。

---

## 2. 滤波强度的来源

这里要区分两套"强度"，它们来自完全不同的地方，且服务对象不同。

### 2.1 入环滤波强度：来自码流 setup header 的 `LFLIMS`

* 位流解析：`oc_quant_params_unpack()` 先读 3 位 `nbits`，再读 64 个 `loop_filter_limits[qi]`（`lib/dequant.c:38-43`），写入 `th_quant_info::loop_filter_limits`（`include/theora/codec.h:416-417`）。
* 解码器初始化：`oc_dec_init()` 把该表复制进共享状态 `state.loop_filter_limits`（`lib/decode.c:410-411`）。
* 每帧选择强度：`oc_dec_pipeline_init()` 用**本帧第一个 qi**（`state.qis[0]`，即 DC 量化索引）索引该表：

  ```c
  flimit = _dec->state.loop_filter_limits[_dec->state.qis[0]];   // lib/decode.c:1369
  _pipe->loop_filter = flimit != 0;                              // lib/decode.c:1370
  if(flimit != 0) oc_loop_filter_init(&_dec->state,
      _pipe->bounding_values, flimit);                           // lib/decode.c:1371
  ```

  也就是说，**整帧、全部三个平面共用一个 `L`**；块级 qi（`frags[i].qii`）只影响 AC 反量化，不影响入环滤波强度。这一点在头文件里有明确说明：`include/theora/codec.h:402`（"The same limits are used for the loop filter in all color planes, despite potentially differing levels of quantization in each."），规范同样在 `doc/spec/spec.tex:7149-7154` 规定 `L = LFLIMS[QIS[0]]`。
* `flimit==0` 时整帧跳过入环滤波（`lib/decode.c:1370`）；这正是 VP3 默认表中高 qi 段全为 0 的原因（见附录 `doc/spec/spec.tex:8055-8071` 的硬编码表）。
* `flimit` 经 `oc_loop_filter_init_c()`（`lib/state.c:1036-1045`）预先展开成长度 256、以 127 为中心的查表 `bounding_values`，供 `loop_filter_h/v` 使用（`lib/state.c:1002-1031`）。

### 2.2 入环滤波的响应函数

`loop_filter_h/v` 内的注释（`lib/state.c:1007-1010`、`lib/state.c:1023-1026`）给出等价定义：

```
f = clamp(min(-2L - f, 0), f, max(2L - f, 0))   （以查表方式实现）
```

这对应规范 `doc/spec/spec.tex:7141-7154` 的分段 `lflim(R,L)`：`|R|>=2L` 归零、`L<=|R|<2L` 为 `±(|R| - 2L)` 的衰减余量、`|R|<L` 原样。**强的真实边缘（|R| 很大）被 2L 截断为 0，即不滤波**，这就是边缘保留的机制来源。

### 2.3 出环后处理强度：来自量化表的派生量，而非 `LFLIMS`

后处理在 `oc_dec_init()` 与 `oc_dequant_tables_init()` 中一次性派生（`lib/decode.c:397-408`、`lib/quant.c:84-85`）：

* `pp_dc_scale[qi] = qfac / 160`（`lib/quant.c:85`，`qfac = dc_scale[qi] * base[0]`），用于去块的 `qstep`。
* `pp_sharp_mod[qi] = -(qsum >> 11)`（`lib/decode.c:408`），其中 `qsum` 由每个 qi 的若干 AC/DC 反量化表项累加得到（`lib/decode.c:399-407`），用于去环的"锐化"负调制。

这说明出环后处理的强度**只由 setup header 的量化参数（`dc_scale`/`ac_scale`/基矩阵）决定**，与入环的 `LFLIMS` 无关；它也不随 `TH_DECCTL_SET_PPLEVEL` 改变"每档的强度"，而只改"启用哪些阶段"。

### 2.4 去块的实际判定与强度

`oc_dec_deblock_frag_rows()` 对每个块读取该块的 DC qi：`qstep = pp_dc_scale[*dc_qi]; flimit = (qstep*3)>>2;`（`lib/decode.c:1747-1748`、`1754-1755`）。`dc_qi` 本身来自 §3.2 的 `dc_qis` 跟踪数组。随后 `oc_filter_hedge()/oc_filter_vedge()`（`lib/decode.c:1610`、`1664`）在

```c
if(sum0<_flimit && sum1<_flimit && r[5]-r[4]<_qstep && r[4]-r[5]<_qstep)  // lib/decode.c:1638 / 1686
```

成立时才做 7 抽头平滑，否则原样拷贝。`sum0/sum1` 是边界两侧 4 像素的绝对差和（局部"平坦度"），`_qstep` 限制跨越边界的阶跃幅度。**所以出环去块只在边界本身已经比较平坦、级差不大时才平滑；强边缘（阶跃 > qstep 或局部方差大）保持原样。**

### 2.5 去环的强度分级

`oc_dec_dering_frag_rows()` 依据每块累计的方差 `variances[fragi]` 与阈值分档（`lib/decode.c:1887-1890` 定义阈值，`1931-1952` 分档）：

* `var > OC_DERING_THRESH1(384)`：做一次弱去环（`strong=0`）。
* `var > OC_DERING_THRESH2(1536)`：做一次强去环（`strong=1`）。
* 当 `pp_level >= SDERINGY/SDERINGC` 且 `var > OC_DERING_THRESH3(1920)`（亮度）/ `THRESH4(3840)`（色度）：强去环，并在相邻块方差也超 `THRESH4` 时重复调用 2 次（`lib/decode.c:1931-1943`）。
* `strong` 标志在每次调用内部改变系数钳位上限与位移（`OC_MOD_MAX[2]={24,32}`、`OC_MOD_SHIFT[2]={1,0}`，`lib/decode.c:1787-1788`）。

方差同时由 §3.2 的 `_pipe->variances` 累计。

---

## 3. 解码重建与"写回参考帧"的步骤

### 3.1 重建写的是 `OC_FRAME_SELF`

`oc_state_frag_recon_c()`（`lib/state.c:959-997`）对每个编码块做逆变换并把结果写入 `ref_frame_data[OC_FRAME_SELF] + frag_buf_off`：

```c
dst = _state->ref_frame_data[OC_FRAME_SELF] + frag_buf_off;      // lib/state.c:985
if(refi == OC_FRAME_SELF) oc_frag_recon_intra(...);              // lib/state.c:986
else oc_frag_recon_inter(...) / oc_frag_recon_inter2(...);       // lib/state.c:993-996
```

`refi` 来自块的预测模式（`OC_FRAME_FOR_MODE`，`lib/state.h:215-217`）。重建阶段**已经**把结果写入了"当前帧缓冲"，但它还不是参考帧——参考帧的语义由 §3.4 的索引提升决定。

### 3.2 两个"副作用数组"：`dc_qis` 与 `variances`

这是入环/出环无耦合的关键证据：

* `dc_qis` 只在 PP 初始化中维护（`oc_dec_postprocess_init()`，`lib/decode.c:1204-1250`）。首帧（intra）用 `qis[0]` 填满整帧（`lib/decode.c:1227`）；后续帧只更新"本帧编码块"的条目为 `qis[0]`（`lib/decode.c:1230-1243`）。未编码块保留上一次被编码时的 qi。
* `variances` 由 `oc_filter_hedge/vedge` 在**出环去块时顺带累计**（`lib/decode.c:1634-1635`、`1684-1685`），供紧随其后的去环使用。由于累计发生在 PP 之间，`pp_level<2` 时 `variances` 不被使用（且会被释放，见 `lib/decode.c:1246-1251`）。
* **`dc_qis` 和 `variances` 都不参与入环滤波**；入环滤波只看 `frags[i].coded` 与固定的 `qis[0]`（`lib/state.c:1088-1099`）。这一点从 Decode 实验 1 得到确认：把入环滤波开关切换，`pp` 相关数组不改变参考像素（见 §7）。

### 3.3 未编码块如何影响后续预测（本报告的核心问题之一）

未编码块**不是**"什么都不做"，它走两条不同的路径：

**(a) 作为预测源被拷贝进 SELF。**
`oc_dec_frags_recon_mcu_plane()` 末尾（`lib/decode.c:1598-1606`）把本 MCU 内的未编码块从 `OC_FRAME_PREV` 拷到 `OC_FRAME_SELF`：

```c
if(_pipe->nuncoded_fragis[_pli] > 0){
  oc_frag_copy_list(&_dec->state,
      _dec->state.ref_frame_data[OC_FRAME_SELF],
      _dec->state.ref_frame_data[OC_FRAME_PREV], ...);  // lib/decode.c:1601-1605
}
```

拷贝内容 `PREV` 是**上一帧已经过入环滤波并填充边界**的缓冲区。因此：**一个本帧未编码的块，继承了上一帧入环滤波后的像素**。规范对此的表述见 `doc/spec/spec.tex:6922-6923`（"For uncoded blocks, the contents of the co-located block are copied from the previous frame..."）。

**(b) 作为滤波器的作用对象或触发者被间接修改。**
`oc_state_loop_filter_frag_rows_c()` 的判定（`lib/state.c:1078-1099`）：

```c
if(frags[fragi].coded){
  if(fragi>fragi0) loop_filter_h(ref,ystride,_bv);                 // 左边（同一行）
  if(fragi0>fragi_top) loop_filter_v(ref,ystride,_bv);             // 下边（上一行）
  if(fragi+1<fragi_end && !frags[fragi+1].coded) loop_filter_h(ref+8,...);  // 右边未编码
  if(fragi+nhfrags<fragi_bot && !frags[fragi+nhfrags].coded) loop_filter_v(ref+ystride*8,...); // 上边未编码
}
```

注意后两条：**编码块会主动对其相邻的未编码块的边界做滤波**（规范 `doc/spec/spec.tex:7498-7523` 的 "right/above neighbor is uncoded" 分支）。因此未编码块的像素会被写改动——这不是重建，而是滤波。由于它随后被拷入 SELF 并最终提升为 PREV，**一旦一个块出现"未编码、但与编码块相邻、且边界平坦"，它的某些像素就可能被改写；这个改写随后进入参考帧，影响下一帧的预测**。这就是"未编码块如何影响后续预测"的完整链条。

实验 4（§7）给出量化证据：第 1 帧仅 8/192 个亮度块编码，另有 184 个未编码；把入环滤波关闭后，**181 个未编码块中有 181 个像素不同**——证明滤波确实改写了未被编码的块，而不仅是重建块。

### 3.4 写回参考帧的确切时刻

真实"写回"只发生在帧末的索引提升（`lib/decode.c:2946-2962`）：

```c
if(_dec->state.frame_type == OC_INTRA_FRAME){
  ref_frame_idx[OC_FRAME_GOLD] = ref_frame_idx[OC_FRAME_PREV] = ref_frame_idx[OC_FRAME_SELF]; // 2949-2952
}else{
  ref_frame_idx[OC_FRAME_PREV] = ref_frame_idx[OC_FRAME_SELF];                                  // 2957-2959
}
```

在此之前，`SELF` 已经历：重建 → 未编码块拷贝 → 入环滤波 → 边界填充（`lib/decode.c:2880-2892`）。**顺序不可交换**：规范 `doc/spec/spec.tex:7545-7556` 专门指出，Theora 相对 VP3 的修正是"统一在填充边界之前做入环滤波"，否则入环预测会因边界像素与滤波次序不一致而产生跨帧漂移。仓库实现遵循此约定（`oc_state_borders_fill_rows` 在 `oc_state_loop_filter_frag_rows` **之后**，`lib/decode.c:2882-2892`）。

### 3.5 整帧被丢弃时的特殊路径

若一帧 `ntotal_coded_fragis <= 0`（含 0 字节包），`th_decode_packetin()` 只更新 granule 并返回 `TH_DUPFRAME`（`lib/decode.c:2764-2772`），**不重建、不滤波、不更新参考索引**。`pp_frame_buf` 也保持不变，因此 `th_decode_ycbcr_out()` 仍返回上一帧内容（实验 5 确认输出不变）。这说明：**"重复帧"既不进参考环，也不经过任何后处理**——它完全沿用上一次的 SELF=当前状态。

---

## 4. 只影响显示的步骤：出环后处理

### 4.1 缓冲分离与"永不回写"的静态证据

* `pp_frame_buf` 在 `pp_level < DEBLOCKC` 时只配置亮度，色度直接引用参考帧（`lib/decode.c:1284-1290`、`1319-1323`）；在 `pp_level >= DEBLOCKC` 时指向独立分配的 `pp_frame_data`（`lib/decode.c:1291-1316`）。
* 后处理函数 `oc_dec_deblock_frag_rows` / `oc_dec_dering_frag_rows` 只写 `_dec->pp_frame_buf`（`lib/decode.c:2899`、`2906`），源是 `ref_frame_bufs[refi]`；解码器的 `lib/decode.c` 中，对 `ref_frame_data[OC_FRAME_SELF] =` 的赋值只有 `lib/decode.c:2067`（dummy frame）与 `lib/decode.c:2793`（新帧选 buffer，编码器/共享状态的同类赋值见 `lib/encode.c:1263,1749`、`lib/state.c:668` 初始化 NULL），后处理路径没有对它的写入。
* 参考索引提升发生在 PP 循环**之后**，且提升的是 `SELF`（重建+入环滤波后的结果），因此 PP 缓冲的内容无论怎样都不会经由索引进入参考环。
* 当 PP 不可用（例如 `pp_level` 被再次禁用、或首个可用帧为 intra 却已在后续调用）时，`oc_dec_pipeline_init()` 直接把 `pp_frame_buf` 指向参考帧（`lib/decode.c:1379-1382`），此时"显示"与"参考"当然重合——但这是**未做后处理**的情况。

### 4.2 关闭 / 不同强度 / 参考环变化的结果区分

| 操作 | 显示输出 `ycbcr_out` | 参考环 `PREV/GOLD` | 后续帧影响 |
|---|---|---|---|
| 后处理关闭（默认 `pp_level=0`） | = 参考帧（同一缓冲，`pp_frame_buf` 指向 ref） | 由入环滤波决定 | 无 |
| `pp_level=1`（TRACKDCQI） | 与 0 完全相同（只维护 `dc_qis`） | 不变 | 无 |
| `pp_level=2`（亮度去块） | 亮度变化；**色度=参考帧副本** | 不变 | 无 |
| `pp_level=3..4`（±亮度去环/强去环） | 亮度进一步平滑 | 不变 | 无 |
| `pp_level=5..7`（+色度） | 三平面独立于参考帧 | 不变 | 无 |
| 入环滤波开/关（改 `LFLIMS` 或 `qis[0]`） | 显示随之变化 | **变化** | **改变后续解码结果** |

后处理档位的语义严格对应 `lib/decode.c:32-48` 的宏定义，并且**档位是"是否启用某阶段"的阶梯**，不是"平滑程度"：

```
0 DISABLED  1 TRACKDCQI  2 DEBLOCKY  3 DERINGY  4 SDERINGY
5 DEBLOCKC  6 DERINGC  7 SDERINGC                              lib/decode.c:32-48
```

实验 1/6 显示：入环滤波开启时显示差异集中在块边界 ±1 像素内（`d0/d1` 直方图，§7.3）；而 `pp=7` 相对 `pp=0` 的最大差值仅 3（`maxdelta=3`），且差异同样受边缘条件约束——**后处理的"平滑"是渐进的、局部的，不会像入环滤波那样改变参考像素的幅度**。

> 注意一个实现层面的坑：`strong` 在 `oc_dec_dering_frag_rows()` 中读取 `_dec->pp_level`（`lib/decode.c:1915`），而不是 `_pipe->pp_level`。后者是 `oc_dec_pipeline_init()` 在条带开始时"冻结"的、用于防止回调中途改值的副本（`lib/decode.c:1375-1382`）。这意味着**在解码回调中修改 `pp_level` 会让同一帧内不同条带对 SDERING 档位的判断不一致**；而 `_pipe->pp_level` 字段（`lib/decint.h:111`）与注释（`lib/decode.c:1373-1375`）正是为此目的。这是一条值得定向测试的边界（见 §8）。

---

## 5. 画面平滑、跨帧一致性与边缘保留

### 5.1 画面平滑

* **入环去块**：只在块边界做 4 抽头水平/垂直滤波，每侧只改 2 个像素（`lib/state.c:1005-1030`），且只在 `|R|` 落入 `2L` 窗口时生效。它平滑的是**重建带来的块效应**，而不是图像内容。
* **出环去块**：7 抽头的平滑只在 `sum0/sum1 < flimit` 且边界级差 `< qstep` 时启用（`lib/decode.c:1638`、`1686`），本质是"只抹平已经很平的地方的轻微不连续"。
* **去环（dering）**：按块方差分档，用 3×3 邻域加权重建每个像素（`oc_dering_block()`，`lib/decode.c:1786-1885`），抑制振铃。强档（`SDERING*`）会把系数钳位上限从 24 提到 32 并改变位移（`lib/decode.c:1787-1788`），产生更强的平滑。

### 5.2 跨帧一致性

跨帧一致性的来源是**参考帧必须是"同一个东西"**。有两条相关约束：

1. **入环滤波必须在填边界之前**（§3.4）：保证 PREV/GOLD 的边界像素与主体像素来自同一处理链，避免"参考帧内部已滤波、边界未滤波"的混合。规范 `doc/spec/spec.tex:7541-7556` 解释这是 Theora 相对 VP3 的修正；VP3 编码器根本不做入环滤波，导致近静止场景的预测环失配。
2. **后处理绝不能进入参考环**：否则解码器之间会因为 `pp_level` 不同而累积不同的参考像素，造成**不可恢复的漂移**。源码通过 §4.1 的缓冲分离保证这一点。实验 2 验证：只在关键帧之后开启 `pp=7`，最终输出与全程 `pp=0` **逐像素相同**——因为 PP 从未触及参考帧，而"延迟开启"不会补做历史后处理（后处理是无状态显示步骤）。

一个推论：**`pp_level` 可以逐帧任意改变而不会破坏解码一致性**；而 **`LFLIMS`（或 `qis[0]`）的任何改变都会改变解码轨迹**。这不是可配置项，而是码流属性。

### 5.3 边缘保留

边缘保留由两道"闸门"实现：

* 入环：`lflim(R,L)` 在 `|R| >= 2L` 时返回 0（规范 `doc/spec/spec.tex:7141-7146`；实现查表 `lib/state.c:1036-1045`）。强边缘（大 `R`）被完全跳过。
* 出环：去块的 `sum0/sum1 < flimit` 且级差 `< qstep`（`lib/decode.c:1638`）；去环只在方差超过阈值时才动作（`lib/decode.c:1931-1952`）。

因此 **`L`/`pp_dc_scale` 越大，"平坦度"的门槛越宽，越容易把真实边缘也当作块效应来平滑**；这是平滑与边缘保留的权衡旋钮。注意 `L` 是**全帧统一**的（§2.1），出环的 `qstep` 则是**按块 qi** 的（§2.4），所以"逐块自适应强度"是出环后处理与入环滤波的一个重要区别。

实验 7.3（Probe B/C）给出量化边界：入环滤波开启/关闭的差异**全部落在块边界 ±1 像素内**（`d0=929, d1=748, d2=0, d>=4=0`），且恰好位于"真实边缘所在块缝"的像素只有 66/444 被改动，而普通块缝改动 881/2631——说明滤波器确实优先保留有内容支撑的真实边缘。

### 5.4 流水线延迟带来的"局部边界"

由于对某一行块的上边做垂直滤波时会用到上一片段行的 2 个像素，`th_decode_packetin()` 用 `sdelay/edelay` 把每条带可用的行推迟一行（`lib/decode.c:2878-2893`；边界填充的两像素延迟注释见 `lib/decode.c:2886-2889`）。这就是 `th_stripe_callback` 文档中"可用片段行数随像素格式和后处理档位变化"的来源（`include/theora/theoradec.h:118-121`）。注意 `pp_level >= DEBLOCKY` 与"仅入环"两条分支都会增加延迟（`lib/decode.c:2897-2914`），因此**条带回调的区域大小依赖 PP 档位**。但参考环的像素内容不依赖 PP；这一点与回调内容要分开看（见下）。

> 这里有一个需要谨慎表述的点：`stripe_buf` 在 `lib/decode.c:2855` 由 `pp_frame_buf` 翻转而来，所以**回调看到的是后处理之后的数据**（若 PP 开启）。因此 PP 虽然不影响参考环，但确实影响"应用回调拿到的像素"。区分点是：**参考环** vs **显示/回调视图**，而不是"全部显示相关"。

---

## 6. 责任边界的精确总结

```
                 ┌───────────────────────────────────────────────┐
 entropy decode  │ 码流 → 模式/MV/qi/token                       │
                 └───────────────────┬───────────────────────────┘
                                     ▼
                 ┌───────────────────────────────────────────────┐
 reconstruction  │ DC 预测逆转 → 逆变换 → 预测相加               │
 (写 SELF)       │ 未编码块: 从 PREV 拷入 SELF                    │
                 └───────────────────┬───────────────────────────┘
                                     ▼
                 ┌───────────────────────────────────────────────┐
 in-loop filter  │ 按 qis[0] 选 LFLIMS；只处理"至少一侧编码"的边 │──► 写 SELF
 (写 SELF)       │ 编码块会改写相邻未编码块的边界像素             │
                 └───────────────────┬───────────────────────────┘
                                     ▼
                 ┌───────────────────────────────────────────────┐
 border padding  │ 左右行填充 + 上下 cap 填充                    │──► 写 SELF 边界
                 └───────────────────┬───────────────────────────┘
                                     ▼
                 ┌───────────────────────────────────────────────┐
 reference 提升  │ intra: SELF→GOLD+PREV ; inter: SELF→PREV       │──► 影响后续预测
                 └───────────────────────────────────────────────┘

     (!) 出环 PP 是旁路分支，只写 pp_frame_buf，不进入以上任何一步
         ┌───────────────────────────────────────────────┐
         │ deblock(pp≥2) → dering(pp≥3/4) → chroma(pp≥5) │──► 写 pp_frame_buf
         └───────────────────────────────────────────────┘
```

* **写回参考图像的步骤**：重建（写 SELF）、未编码块拷贝（写 SELF）、入环滤波（写 SELF）、边界填充（写 SELF 边界）、帧末索引提升。
* **只影响显示的步骤**：出环去块、出环去环、色度 PP；它们写 `pp_frame_buf`，被 `th_decode_ycbcr_out()` 与 stripe 回调读取，但从不进入参考环。
* **两者共享的输入**：`state.qis[0]`（决定 `L`）与块级 `qii`（决定出环 `qstep`）——但用于不同目的，**不可混淆**。

---

## 7. 定向测试证据

所有实验基于 `libtheora` 的真实编码器/解码器（本仓库源码），使用最小 `ogg` 兼容层（仅 `oggpack*` 写入函数，测试目的），直接链接 `lib/*.c` 并读取解码器内部状态以区分参考帧与显示帧。测试代码放在仓库外的诊断目录，未修改仓库源码。

### 7.1 后处理是显示专用的（Experiment 1 / Probe D）

128×96、4:2:0、quality=32、5 帧（首帧关键帧，其余仅 8/192 亮度块编码）：

```
frame coded_frags out(vspp0)Y outCb outCr selfY-same selfhash
    0         288       5668   672  1280         0 identical
    1           8       5360   672  1280         0 identical
    2           8       5571   672  1280         0 identical
    3           8       5355   672  1280         0 identical
    4           8       5562   672  1280         0 identical
pp=7 max|luma delta| frame0=3 ; pp=2 luma diff=5668 chroma diff=0
```

* `selfY-same=0` 与 `selfhash=identical`：把 `pp_level` 从 0 换到 7，**每一帧解码后的 `OC_FRAME_SELF` 像素完全一致**。
* `pp=2` 时色度差异为 0：亮度去块档不处理色度，色度直接引用参考帧（`lib/decode.c:1319-1323`）。
* `pp=7` 相对 `pp=0` 的最大差值只有 3：后处理是渐进平滑，不会像入环滤波那样显著改变像素。

Probe D（4 帧）：
```
D: pp7 vs pp0 luma differing pixels=4804 ; display buf==ref buf? no ; pp_frame_data=allocated
```
显示缓冲与参考缓冲**不是同一块内存**，确认 §4.1 的分离。

### 7.2 延迟开启后处理不影响最终结果（Experiment 2 / Probe E）

```
frame 0 late-pp output equals pp0: luma=yes chroma=yes
...
frame 4 late-pp output equals pp0: luma=yes chroma=yes
```

Probe E（只在 inter 帧请求 `pp=7`）：
```
E: frame 0 requested pp=0, pipe.pp_level=0, dc_qis=NULL, disp==ref? YES
E: frame 1 requested pp=7, pipe.pp_level=0, dc_qis=NULL, disp==ref? YES
...
E: final output vs pp0 differing pixels=0
```

这直接验证了 `oc_dec_postprocess_init()` 的行为：一旦首个可用帧不是 intra，就无法"从零开始"跟踪 `dc_qis`，于是永久拒绝后处理并回退到参考帧视图（`lib/decode.c:1220-1224`、`1379-1382`）。**后处理是不可追溯的：中途开启不会补偿历史。**

### 7.3 入环滤波改变参考环（Experiment 3 / 3b + Probe B/C）

```
=== Experiment 3: in-loop filter changes the reference loop ===
frame out_diff_vs_filtered refY_diff_vs_filtered maxdelta
    0                 2752                   2752       21
    1                 2726                   2726       36
    2                 2814                   2814       21
    3                 2726                   2726       41
    4                 2814                   2814       25
frame0 differing pixels: within 1px of a block boundary=2752, elsewhere=0
```

* 关闭入环滤波后，`out` 与 `refY` 的差异数量**完全相等**：说明在 PP 关闭时，差异全部来自参考帧本身。
* 差异像素 100% 位于块边界 ±1 像素内，与 4 抽头滤波的作用范围一致。
* 第 1 帧（只有 8 个块编码）差异反而增大（maxdelta=36）：因为大量未编码块把上一帧滤波后的结果带了下来（见 §3.3）。

Experiment 3b（只对关键帧关闭入环滤波，其余帧正常）：差异持续到最后一帧（`2736` 像素），**证明一次参考帧污染会沿帧链传播**——这正是"参考环路变化"与"显示后处理变化"的本质区别。

边缘直方图（Experiment B/C 输出）：
```
B: in-loop on/off differing pixels by min distance to 8px seam: d0=929 d1=748 d2=0 d3=0 d>=4=0
B: genuine-edge pixels=444 changed=66 ; other seam pixels=2631 changed=881
C: pp7 vs pp0 differing=4363 maxdelta=3 ; genuine-edge changed=130/444 ; interior-seam changed=954/2631
```
入环滤波的改动严格限制在块边界 0~1 像素；真实内容边缘（恰好落在块缝上）只有 66/444 被改动（15%），普通块缝 881/2631 被改动（33%）。这量化了 §5.3 的边缘保留效果。

### 7.4 未编码块被滤波改写并继承（Experiment 4）

```
frame1 coded fragments=8 of 192
frame1 uncoded luma blocks=184 ; of these 181 differ when the in-loop filter is disabled
```

第 1 帧只编码 8 个块，但关闭入环滤波后，**184 个未编码块中有 181 个像素不同**。源码解释：编码块会改写其右侧/上侧相邻未编码块的边界（`lib/state.c:1094-1098`），这些未编码块随后被拷入 SELF 并成为下一帧的 PREV（`lib/decode.c:1599-1605`）。这是"未编码块如何被滤波影响、又如何影响后续预测"的直接证据。

### 7.5 显示后处理的无损性（Probe C / F）

```
C: pp1 vs pp0 luma differing pixels=0 ; dc_qis tracked=yes
F: pp2 vs pp0 luma diffs=4804 chroma diffs=0 ; chroma planed copied from ref
```
* `pp=1` 与 `pp=0` 输出逐像素相同：TRACKDCQI 档只维护 `dc_qis`，不产生任何显示差异。
* `pp=2` 色度与参考帧逐像素相同：亮度去块不触及色度平面。

### 7.6 响应表与规范一致（Experiment 6）

对 `L=1..30`，用 `(raw+4)>>3` 的索引方式穷举每个索引对应的 8 个原始 `R` 值，与规范 `lflim(R,L)` 的分段定义逐点比较：

```
response table checked=61440 mismatches=0
L=10: bv[127+9]=9 (R=9), bv[127+10]=10 (R=10), bv[127+19]=1 (R=19), bv[127+21]=0 (R=21), bv[127-21]=0 (R=-21)
```

`oc_loop_filter_init_c()` 的查表与 `doc/spec/spec.tex:7141-7154` 的二分衰减、`|R|>=2L` 归零完全吻合；`R=19` 得 `1`（`|R|-2L = -1` 取补的余量）而 `R=21` 得 `0`，确认 `2L` 截断。

### 7.7 重复帧不经过任何处理（Experiment 5）

```
ret=1 (TH_DUPFRAME=1) ycbcr_out unchanged=yes
```
0 字节包（丢帧）只更新 granule 并返回 `TH_DUPFRAME`（`lib/decode.c:2764-2772`），显示输出与上一帧逐像素相同，参考索引不变。

---

## 8. 建议的定向测试与静态检查

对仓库本身（非本次诊断脚本）值得补充的测试：

1. **PP 缓冲不可达参考帧的静态断言**：在 `oc_dec_deblock_frag_rows`/`oc_dec_dering_frag_rows` 前后，对 `ref_frame_bufs[SELF]` 做校验和，验证后处理调用前后不变。本次诊断脚本用外部 memcmp 完成，适合固化为单元测试。
2. **`pp_level` 冻结语义**：在 `TH_DECCTL_SET_STRIPE_CB` 回调中修改 `TH_DECCTL_SET_PPLEVEL`，断言同一帧内各条带的 `_pipe->pp_level` 保持一致。当前 `oc_dec_dering_frag_rows()` 读取 `_dec->pp_level`（`lib/decode.c:1915`）而非 `_pipe->pp_level`，存在帧内不一致的隐患，值得专门测试。
3. **入环滤波跨帧传播**：用 §7.3 的"只污染关键帧"方法，断言差异在后续帧持续存在（不应衰减为 0）。
4. **未编码块滤波改写**：构造"编码块 + 相邻未编码平坦块"的最小流，断言未编码块边界像素被改写，而内部像素不变（§7.4 的精确化）。
5. **`LFLIMS` 边界**：`flimit=0` 时整帧应跳过入环滤波（`lib/decode.c:1370`）；可用于回归 VP3 兼容表的第 48-63 项全 0 行为。
6. **响应表参考实现**：把 `oc_loop_filter_init_c()` 与规范公式的逐点比较（§7.6）固化为测试，防止未来优化破坏 `2L` 截断。

---

## 9. 结论

* **滤波强度有两个互不相干的来源**：入环用 setup header 的 `LFLIMS[qis[0]]`（全帧共用，`lib/decode.c:1369-1371`）；出环用 `qfac/160` 与 `qsum>>11` 派生的 `pp_dc_scale/pp_sharp_mod`（`lib/quant.c:85`、`lib/decode.c:408`）。前者受码流约束、影响参考环；后者是解码器可选的显示增强。
* **未编码块不是"跳过"**：它被从 `PREV` 拷入 `SELF`（`lib/decode.c:1599-1605`），并会被相邻编码块触发的入环滤波改写（`lib/state.c:1094-1098`），最终经索引提升进入下一帧参考——这是未编码块影响后续预测的完整机制。
* **写回参考的步骤**是重建、未编码拷贝、入环滤波、边界填充、索引提升；**只影响显示的步骤**是出环去块/去环/色度 PP，它们写独立的 `pp_frame_buf`，被 `th_decode_ycbcr_out()` 与 stripe 回调读取，但从不回写参考。
* **关闭 / 不同强度 / 参考环变化的结果可区分**：PP 档位只切换显示阶段（0/1 无差异，2 起亮度变化，5 起色度独立），且中途开启不会追溯；入环变化会立即改变参考帧并沿帧链传播。实验 1/2/3/7 分别验证了这三点。
* **平滑 / 跨帧一致 / 边缘保留的边界**：入环在 `2L` 截断处保留强边缘、只作用于块缝 ±1 像素（§7.3）；出环在"平坦度 + 级差"双门下只抹平已接近平坦的不连续；跨帧一致性要求入环滤波先于边界填充、后处理永不入环——这两条在源码（`lib/decode.c:2882-2892`、`2946-2962`）与规范（`doc/spec/spec.tex:7545-7556`）中都有明确约束，且由本次定向测试实证。
