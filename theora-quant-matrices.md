# Theora 量化参数：打包、解析与反量化表生成

本文分析 libtheora 中量化参数（`th_quant_info`）如何从编码器经由 setup header 传递到解码器，
以及如何展开成 64×3×2 张反量化表。内容包括：

* qi 区间的共享方式（比特流层与内存层，四种机制）；
* Y'/Cb/Cr 与 intra/inter 如何选表；
* 合法矩阵的格式约束；
* 量化系数与重建值的取值范围；
* 把结论分成**参数兼容**、**数值稳定**、**感知质量**三类；
* 注明哪些结论来自源码或实测，哪些只是待实测的判断。

文中 `路径:行号` 均指本仓库。实测结论标注为「**实测**」，其余为源码/规范结论。

---

## 0. 结论摘要

| 主题 | 结论 |
| --- | --- |
| 量化参数载体 | 只存在于 setup header（`0x82` 包）前段，`oc_quant_params_pack` / `oc_quant_params_unpack`（`lib/enquant.c:85`、`lib/dequant.c:24`） |
| 区间共享 | 4 层：端点复用、基矩阵内容去重（9 bit 全局索引）、6 组区间集合的拷贝标志（`NEWQR`/`RPQR`）、解码器最终表的指针别名 |
| 表项选择 | `dequant_tables[qi][pli][qti]`；DC 恒用 `qis[0]`，AC 用每块 `qii`；`qti = (mb_mode != INTRA)` |
| 合法矩阵 | 基矩阵 8 bit；scale 按 1..16 bit 读；NBMS ≤ 384（规范 MUST，库实际接受 ≤512）；区间大小为正且和为 63；`bmi < NBMS`；loop filter 限值 7 bit（0..127） |
| 量化器范围 | 表项被夹到 `[QMIN, 4096]`，QMIN = DC intra 16 / AC intra 8 / DC inter 32 / AC inter 16（`lib/quant.c:32-33`） |
| 系数范围 | 比特流 token 幅值 ≤ 580（`lib/huffman.h:25`）；**实测**最小量化器下编码器最坏输出 510（DC 残差 508/510），与 `lib/quant.c:24-28` 的注释一致 |
| 重建范围 | 反量化乘积可达 580×4096 = 2,375,680，规范要求截断为 int16；iDCT 中间量 int32、输出 int16，最终像素夹到 0..255 |
| 参数兼容风险 | 见 §8：编码器 API 不校验区间和；loop filter ≥128 会让整个量化段错位（实测 24530/24576 表项改变）；`TH_DEF_QUANT_INFO` 违反 `codec.h` 自述的单调性；DC 替换与指针别名存在潜在冲突 |
| 感知质量 | 矩阵形状/插值/双 scale/色度 RD scale 属启发式；其收益必须在本文未做的编码实验中验证（§10、§12） |

---

## 1. 数据流总览

```
应用 th_quant_info
   │  TH_ENCCTL_SET_QUANT_PARAMS / TH_VP31_QUANT_INFO / TH_DEF_QUANT_INFO
   ▼
oc_enc_set_quant_params ── oc_quant_params_clone ──► _enc->qinfo
   │                              (深拷贝 + 共享指针去重)
   │  oc_enc_quant_params_updated
   │      ├─ oc_enc_enquant_tables_init ─► oc_dequant_tables_init ─► state.dequant_tables
   │      │                                                       └► _enc->dequant_dc[qi][pli][qti]
   │      └─ oc_enquant_qavg_init ─► log_qavg / log_plq / chroma_rd_scale
   ▼
oc_quant_params_pack（写 setup header）
   ▼
────────── 比特流 ──────────
   ▼
oc_quant_params_unpack ─► decoder state.qinfo
   ▼
oc_dequant_tables_init ─► state.dequant_tables[64][3][2]（zig-zag 顺序）
   ▼
逐帧：state.qis[]、每块 qii、每块 mb_mode → 选取一张表
```

关键点：**编码器与解码器都用同一个 `oc_dequant_tables_init` 生成表**，
因此编码端量化与解码端反量化在数学上是同一套值（`lib/encode.c:1063-1099`、`lib/decode.c:392-397`）。
打包/解析只负责传 `qinfo`，不传表。

---

## 2. 比特流语法（setup header 内的量化段）

规范：`doc/spec/spec.tex`，`sec:setupheader`（约 2605 行）给出顺序：

```
loop filter 分辨率 (3 bit) → loop filter 表 (64×NBITS)
AC scale 分辨率 (4 bit) → AC scale 表 (64×NBITS)
DC scale 分辨率 (4 bit) → DC scale 表 (64×NBITS)
base matrix 个数 NBMS (9 bit) → NBMS×64 个 8 bit 基矩阵
6 组“区间集合”描述（intra/inter × Y'/Cb/Cr）
```

C 实现逐项对照：

| 字段 | 写（`lib/enquant.c`） | 读（`lib/dequant.c`） |
| --- | --- | --- |
| loop filter 分辨率 | `nbits=OC_ILOG_32(max)`，写 3 bit（`:100-106`） | 读 3 bit（`:38-43`） |
| AC scale | `nbits=OC_ILOGNZ_32(max)`，写 `nbits-1` 4 bit（`:109-112`） | `val+1`（`:44-49`） |
| DC scale | 同上（`:115-118`） | `val+1`（`:50-55`） |
| NBMS | `nbase_mats-1` 写 9 bit（`:140`） | `val+1`（`:56-57`） |
| 基矩阵 | 每个 8 bit ×64（`:141-143`） | 8 bit ×64（`:60-65`） |
| bmi 位宽 | `OC_ILOG_32(nbase_mats-1)`（`:147`） | `oc_ilog(nbase_mats-1)`（`:66`） |
| 区间集合 | 拷贝标志 + 区间/索引（`:148-182`） | 拷贝标志 + 区间/索引（`:66-141`） |

注意两点：

1. 三种「分辨率」字段的语义不同：
   * loop filter 的 3 bit 字段**就是**位宽（可以是 0，此时 64 个限值全为 0）；
   * AC/DC scale 的 4 bit 字段是「位宽−1」，所以读侧位宽 ∈ **1..16**，最小 1 bit，
     不可能出现「0 bit 的 scale」。
   `oc_pack_read(_opb,0)` 直接返回 0（`lib/bitpack.c:79`），这是 loop filter 位宽为 0 时
   能安全工作的原因。
2. 6 组集合以 `i = 3*qti + pli` 的**行优先**顺序出现，这对拷贝标志的语义至关重要（§3.3）。

---

## 3. 区间（qi range）共享：四种机制

Theora 允许 384 张矩阵（64 qi × 2 qti × 3 pli），但规范只允许**稀疏**地存端点，
其余线性插值；并且允许在多个层次上复用。四个层次从内到外：

### 3.1 端点复用（相邻区间的端点共享）

一组区间由 `nranges` 个长度 + `nranges+1` 个基矩阵构成：
`base_matrices[i]` 是第 i 个区间的左端点，同时也是第 i−1 个区间的右端点
（`include/theora/codec.h:349-351`；规范 2768-2772）。

因此「n 个区间」只存 n+1 张矩阵，而不是 2n 张。`oc_dequant_tables_init` 的循环
（`lib/quant.c:59-124`）就是按这个约定：`memcpy(base, base_matrices[qri])`，
然后对 `qi ∈ [qistart, qiend)` 逐级插值走向 `base_matrices[qri+1]`。

最后一组 `qri == nranges` 时 `qi_end = qi+1`（`lib/quant.c:67`），
只生成 qi=63 一张表，用的是最后一个端点。

### 3.2 基矩阵按内容去重（9 bit 全局索引）

打包时，`(qti, pli, qri)` 的每张端点矩阵都会在**全局** `base_mats[]` 里查重，
内容相同则复用同一索引（`lib/enquant.c:119-138`）。所以：

* 同一矩阵出现多次（例如 VP3 的 inter 三平面共用一张）只写一次；
* 索引位宽 = `ilog(NBMS-1)`；当 NBMS=1 时为 0 bit，即所有区间强制指向唯一矩阵。

这带来两个可测的极端：

* **实测**：唯一一张基矩阵、6 组集合完全相同 → 量化段仅 **277 字节**；
* **实测**：63 个大小 1 的区间 × 6 组，配 64 张互不相同矩阵（NBMS=64）
  → **4315 字节**，打包/解析后 384 张表逐项一致。

为什么规范上限是 384：每个 `(qti,pli)` 最多 63 个区间 → 64 个端点；
6 组 → 6×64 = 384，正是所有端点都不复用时能达到的最大值。

解析端只做范围检查 `bmi >= nbase_mats → TH_EBADHEADER`（`lib/dequant.c:134-137`），
不做跨组一致性检查（共享是自然的，因为索引指向同一份 `base_mats` 拷贝）。

### 3.3 整组区间集合的拷贝标志（NEWQR / RPQR）

对 `i>0` 的集合，先读 1 bit `NEWQR`（`lib/dequant.c:74-97`）：

* `NEWQR=0`：复制一份已有集合，不写任何 sizes/bmi。若 `qti>0` 再读 1 bit `RPQR`：
  * `RPQR=1` → 复制**同一 pli、上一个 qti**（intra→inter 共享，用于「帧间沿用帧内矩阵」）；
  * `RPQR=0` → 复制**行优先顺序上的前一组**（`(i-1)/3`, `(i-1)%3`），
    典型用于 Cb/Cr 相同。
* `NEWQR=1`：显式写区间。

规范写法 `qtj=(3*qti+pli-1)//3, plj=(pli+2)%3`（spec 2848-2850）与 C 的
`qtj=(i-1)/3, plj=(i-1)%3` 等价，因为 `i=3*qti+pli`。

打包端反向判断（`lib/enquant.c:148-181`）：

* `qti>0` 且与 intra 同 pli 的 sizes 与 **indices** 完全相同 → 写 2 bit `1`；
* 与行优先前一组完全相同 → 写 `0`（1 或 2 bit）；
* 否则写 `1` 再写显式区间。

注意比较的是**索引数组**而不是矩阵内容：内容相同的矩阵已在 §3.2 合并成同一索引，
因此这层判断能在「矩阵不同、但索引布局一致」时也命中。

**实测**（`/tmp` 下的 pack→unpack 往返，见 §11）：VP3 默认参数 376 字节、
libtheora 默认参数 959 字节、inter==intra 的合成参数 399 字节，
三者在解析后 384 张反量化表与原表**逐项 0 差异**；指针共享关系也符合预期。

### 3.4 解码器的最终表指针别名

`oc_dequant_tables_init` 在生成每张表后，把它与**同一 qi 内**已经生成的
`(plj, qtj)` 表比较（`lib/quant.c:96-114`），内容相同就把指针改成指向同一份存储。
注释给出的动机是缓存局部性（`"This simple check helps us improve cache coherency"`）。

两个必须知道的性质：

* 比较范围是 `qtj ∈ [0, qti]`、`plj < (qtj<qti ? 3 : pli)`，即**只在同一个 qi 内**；
  **不同 qi 的表永远不会别名**。所以逐帧切换 qi 不会因别名出现串扰。
* `th_quant_info` 的存储仍在 `oc_theora_state` / `oc_enc_ctx` 里固定保留
  （`dequant_table_data[64][3][2]`，`lib/state.h:465`），别名只影响指针。

**实测**（用真实 `lib/quant.c` 编译的 harness）：
两套内置参数在**每一个** qi 上都是同一种别名形态：
`{Y-intra}`、`{Cb-intra, Cr-intra}`、`{Y-inter, Cb-inter, Cr-inter}`
——即 6 张表只占 3 份存储。384 个指针槽合计只有 192 个不同目标；
内容去重后 DEF 有 190 种、VP31 有 192 种不同矩阵。
**没有任何一组别名跨 qi**（内容相同的表若在不同 qi，指针仍是分开的）。

---

## 4. 生成一张反量化表

### 4.1 区间定位与插值

对 `(qti, pli)` 的一组区间，qi 落在 `[Σ_{j<qri} sizes[j], Σ_{j≤qri} sizes[j]]`；
端点上的 qi 取哪个区间结果相同（spec 之 quant-mat 步骤 1 的说明）。
C 实现不是查找而是顺序推进（`lib/quant.c:59-124`），逻辑等价。

区间内插值（`lib/quant.c:117-123`）：

```c
base[ci] = (2*((qi_end-qi)*bm[qri][ci] + (qi-qi_start)*bm[qri+1][ci])
            + sizes[qri]) / (2*sizes[qri]);
```

也就是四舍五入、平局取较大值（分子加 `sizes`，再除以 `2*sizes`；所有量非负），
与规范 3019-3032 的公式一致。插值结果是 `unsigned char`，由于端点在 0..255
且插值是凸组合（权重和为 `2*sizes`），结果天然不超过 255。

### 4.2 scale、钳位与 zig-zag

```c
qfac = dc_scale[qi] * base[0];
q    = (qfac/100) << 2;                     // DC
q    = ((ac_scale[qi] * base[OC_FZIG_ZAG[zzi]])/100) << 2;  // AC
q    = OC_CLAMPI(QMIN[qti][!zzi], q, OC_QUANT_MAX);
```

（`lib/quant.c:83-94`）。三个要点：

* **`base` 按 zig-zag 顺序存放**（`OC_FZIG_ZAG[zzi]`）：表本身也是 zig-zag 序
  （`lib/state.h:462-463` 明确写了 "stored in zig-zag order"），解码器用
  `ac_quant[zzi]` 直接索引（`lib/decode.c:1574`），而 `oc_enquant_qavg_init`
  需要先做自然序→zig-zag 的映射（`lib/enquant.c:346` 的 `OC_IZIG_ZAG`）。
  规范里的 `QMAT` 是自然序，二者只差一个置换。
* `<<2` 是给整数 DCT 的 4 倍尺度补偿（spec 2984-2991）。
* `QMIN = {DC,AC}×{intra,inter} = {16,8}/{32,16}`，`OC_QUANT_MAX = 4096`
  （`lib/quant.h:27` 与 `lib/quant.c:32-33`；spec 的 Table qmin）。

`_pp_dc_scale[qi] = qfac/160`（`lib/quant.c:85`）**不是**反量化值，
只供解码器去块/去振铃后处理使用（`lib/decode.c:1747,1933`）。

### 4.3 两套内置参数

`TH_VP31_QUANT_INFO`（`lib/encode.c:92-135`）：
每组 1 个区间、长度 63；基矩阵是 JPEG 亮度表、JPEG 色度表、和一张平坦 inter 表；
Cb 与 Cr 共用同一个数组 `OC_VP31_BASES_INTRA_C`，inter 三平面共用 `OC_VP31_BASES_INTER`
（`lib/encode.c:125-132`）；每个数组的两个端点内容完全相同
（`base_matrices[0] == base_matrices[1]`），所以整段 qi 上矩阵不变，插值退化为常数。

`TH_DEF_QUANT_INFO`（`lib/encode.c:266-309`，当前默认）：
每组 3 个区间 `{32,16,15}`、4 个端点。Y intra 从全 15 平台 → 粗斜坡 → JPEG 亮度表；
C 从 19 平台 → 斜坡 → JPEG 色度表；inter 从 21 平台 → 三档斜坡 → 平坦 inter 表。
scale 表 dc=ac，从 365 降到 10。

**实测**（用真实 `lib/quant.c`/`lib/dequant.c` 编译的 harness，并与独立 Python 模型逐项比对）：

| 指标 | `TH_DEF_QUANT_INFO` | `TH_VP31_QUANT_INFO` |
| --- | --- | --- |
| 384 张表中不同内容数 | 190 | 192 |
| 6 组集合各自的不同 qi 表数 | 63 或 64 | 64 |
| 被夹到 QMIN 的表项 | 310 / 24576（1.3%） | 561 / 24576（2.3%） |
| 被夹到 4096 的表项 | 0 | 0 |
| 表项最大值 | 304（qi0、Y、inter 的整张表都是 304，基矩阵为常数 21） | 2560（qi0、Y、inter、zzi=61） |
| Y-intra DC 量化器 qi=0/16/32/48/63 | 216/100/48/24/16 | 140/68/36/16/16 |
| 沿 qi 增大时量化器**变大**的点数 | **1105**（最大 +8） | 0 |

---

## 5. 表项选择：qi、pli、qti、qii

### 5.1 四个索引

```
表 = state.dequant_tables[ qi ][ pli ][ qti ]      // lib/state.h:464
```

* `pli ∈ {0,1,2}` = Y', Cb, Cr。像素格式（4:2:0/4:2:2/4:4:4）只改变每平面的块数，
  **不改变表项的选择规则**，也不改变矩阵数值；它只通过 `OC_PCD[4][3]` 的单平面能量占比
  影响编码器的 `log_qavg` 与 `chroma_rd_scale`（`lib/enquant.c:300-311,349-358`，
  由 `lib/encode.c:1108-1109` 把 `pixel_fmt` 传入 `oc_enquant_qavg_init`）。
* `qti = (mb_mode != OC_MODE_INTRA)`：
  * 解码器 `lib/decode.c:1537-1538`；
  * 编码器 `lib/analyze.c:776`。
  所以 intra 帧里所有块 qti=0；inter 帧里被选为 intra 编码的块仍用 qti=0，
  其余 7 种模式（inter nomv/mv/last/last2、golden nomv/mv、inter 4MV）都用 qti=1
  （模式枚举见 `lib/state.h:188-210`）。**golden 预测也走 inter 表。**
* `qi` 有帧级（≤3 个）与块级（qii）两层；`qii` 只作用于 AC。
  * 帧头读 `qis[0..2]`，6 bit 每个（`lib/decode.c:450-463`；spec 3280-3292）；
  * 每个编码块读 `qii`（`lib/decode.c:902-981`；spec 4360-4400），
    用与 superblock coded-flag 相同的二元 RLE 两遍编码；
  * **DC 恒用 `qis[0]`**：解码器 `lib/decode.c:1530` 用 `dequant[_pli][0][qti][0]`，
    AC 用 `dequant[_pli][frags[fragi].qii][qti]`（`:1538`）。
    规范明确理由是 DC 预测在量化域进行（spec 1002-1008、4380-4384）。
  * 编码器还做了一次显式替换：把每个 `qi` 的 DC 都改写为帧首 qi 的 DC
    （`lib/analyze.c:549-560`），以便「量化域 DC 预测」与解码端一致。

### 5.2 编码器的 `qii` 选择

`oc_enc_calc_lambda`（`lib/rate.c:151-202`）在低码率端（`log_qavg < 7.0`）
用 K-means 结论选最多 3 个邻近量化器；随后在 `oc_analyze_*_mb_luma`
（`lib/analyze.c:1405-1512`）里按 R-D 代价动态规划选每块的 qii，
并把 RLE 码率计入代价（`oc_qii_state_advance`，`lib/analyze.c:463-503`）。
VP3 兼容模式或快速分析模式下强制 `nqis=1`（`lib/rate.c:191-193`）。

### 5.3 编码器每帧的「压缩表」

编码器不是直接索引 384 张表，而是按本帧 `qis[]` 生成 `enquant[pli][qii][qti]`
（最多 3×3×2 份），并把每张表的 DC 项替换成帧首 qi 的 DC
（`lib/analyze.c:546-560`）；`oc_enc_enquant_table_fixup` 再把 DC 的乘子/移位
在所有 qii 上强制一致（`lib/enquant.c:210-218`）。SIMD 版本同理
（`lib/x86/x86enquant.c:30-56`、`lib/arm/armenquant.s:27-47`）。

---

## 6. 合法矩阵

「合法」分两层：**规范/比特流合法**与**库实际接受**。

### 6.1 比特流层（规范 MUST）

| 约束 | 出处 | libtheora 行为 |
| --- | --- | --- |
| 基矩阵元素为 8 bit 无符号 | spec 2818-2819；`lib/dequant.c:60-65` | 一致 |
| NBMS ≤ 384 | spec 2809-2810 | **不检查上限**：9 bit 读 +1 后只检查 `bmi < NBMS`；**实测** NBMS=384/385/512 的完整流均被接受（ret=0） |
| 区间大小之和恰为 63 | spec 2775-2783、2873-2891 | 解析端通过 `qi<63` 循环结构强制成立：**实测**一个只声明「一个 62」的流被解释成 `sizes={62,1}` 且 `ret=0`（第二个 size 字段宽度 `ilog(62-62)=0`，隐式为 1） |

> 推论：**解析器产出的 `qinfo` 永远覆盖 qi 0..63**，不会让 `oc_dequant_tables_init`
> 留下未写入的表项。未初始化的表只会由 `TH_ENCCTL_SET_QUANT_PARAMS` 传入非法
> `sizes`（§6.2）后产生——**实测** `sizes[0]=62` 的单区间参数下，qi=63 的存储
> 保持初始值 `0xAAAA` 未被写入。
| 区间大小 ≥ 1 | `sizes[qri]=val+1`（`lib/dequant.c:102`） | 一致 |
| `bmi < NBMS` | spec；`lib/dequant.c:134-137` | 一致（**实测** bmi==NBMS → `TH_EBADHEADER=-20`） |
| scale 值 ≤ 65535 | 4 bit 位宽字段 +1 | 一致 |
| loop filter 限值 7 bit（0..127） | spec 2646-2652（`LFLIMS` 7 bit） | 读侧最多 0..127（3 bit 分辨率字段 ≤7）；写侧对 ≥128 会截断分辨率字段，见 §8 |
| 矩阵最终值 ∈ [QMIN,4096] | spec Table qmin；`lib/quant.c:88,93` | 一致 |

### 6.2 库 API 层（`TH_ENCCTL_SET_QUANT_PARAMS`）

`oc_enc_set_quant_params`（`lib/encode.c:1119-1135`）只检查「非 NULL 时 `buf_sz` 正确」
和「尚未写 setup header」，**完全不校验** `nranges`、`sizes` 和、base 指针有效性。
`oc_quant_params_clone`（`lib/enquant.c:22-83`）也只负责拷贝与指针去重。

后果（均**实测**）：

* `sizes` 和为 64 或 70：`oc_dequant_tables_init` 在 `qi` 超过 63 后继续写
  `_dequant[qi]`，越界写（ASAN 在 `lib/quant.c:89` 报 SEGV）。
* `sizes` 和小于 63（如 62）：`oc_quant_params_pack` 会在
  `lib/enquant.c:177` 读过 `sizes` 数组末尾（ASAN: global-buffer-overflow）。
* 区间/矩阵端点指针指向栈上临时数组：clone 会深拷贝，因此**指针内容**必须在调用期间有效，
  这一点 API 文档（`include/theora/theoraenc.h:52-65`）只说了参数被拷贝，未强调 `*sizes`/`*base_matrices`
  也必须有效；实际需要它们指向 `nranges`/`nranges+1` 个元素。

### 6.3 兼容性含义

* 库作为**解码器**是规范的超集：接受 NBMS 385..512、接受「短」区间流（自行重解释）。
* 库作为**编码器**是规范的子集前提：调用者必须自己保证 `sizes` 和为 63，否则内存不安全。
  若要对外暴露自定义矩阵，建议在 `oc_enc_set_quant_params` 里加校验（本文不改代码）。

---

## 7. 系数与重建范围

### 7.1 量化系数

* 比特流层面：单个 token 的幅值上限 580（`lib/huffman.h:25`；spec 4694 的
  `±69…580`；`lib/tokenize.c:356-358` 以 580 为偏移构造查表）。
* 编码器侧最坏值由 QMIN 保证。`lib/quant.c:24-31` 声称
  「±255 输入时 DCT 最大 8157，最小量化器使量化后（含 DC 预测）不超过 ±510」。
  **实测**（真实 `oc_enc_fdct8x8_c` + `oc_enc_quantize_c`，对所有 64 个基函数的 ±极限输入）：

  | 情况 | 输入范围 | QMIN(DC/AC) | max\|量化值\| | DC 残差上界 |
  | --- | --- | --- | --- | --- |
  | intra（像素减 128） | [−128,127] | 16/8 | AC 510，DC 254 | 508 |
  | inter（残差） | [−255,255] | 32/16 | AC 510，DC 255 | 510 |

  与注释的 ±510 完全吻合，且比 token 上限 580 低 70，因此**合法编码器无需钳位**。
  这也解释了为什么 `analyze.c` 里没有对 `_qdct` 做范围裁剪。

  一个值得记录的细节：注释说「±255 输入时 DCT 最大 8157」，而**实测**真实整数
  `oc_enc_fdct8x8_c` 对全 255 输入的 DC 输出是 **8162**（比注释大 5）。这不影响结论：
  8162/32 = 255.06，取整后仍是 255；inter 输入的 AC 实测极值是 8161（zig-zag 序号 10），
  8161/16 = 510.06，取整仍是 510；intra 输入的 AC 极值是 4081，4081/8 = 510.125 → 510。
  也就是说 510 这个上界在注释的 8157 与实测的 8162 两种取值下都成立，
  但它不是靠「8157」这一定值推出的，而要靠 QMIN 的整数量化才能得到。

### 7.2 反量化乘积

`C = coeff × QMAT`（`lib/decode.c:1574`；spec 6084-6200）。由于 `Q` 可到 4096、
`coeff` 可到 580，乘积上限约 **2.38×10⁶**，远超 int16。规范明确要求
「截断为 16 bit，丢弃高位」（spec 6163-6168）；C 实现把它塞进
`ogg_int16_t`（`lib/decode.c:1574`），行为等价于截断。
同一逻辑在 IDCT 的中间量上也适用（spec 6240-6246 要求用非饱和运算、
丢弃高位而不是钳位），二者共同构成「溢出即回绕」的语义。
对合法编码器产出的流（§7.1）乘积上界可精确算出
（表项 × 该槽的最坏量化值；**实测**逐张表扫描）：

| 参数 | 最坏乘积 | 出现位置 |
| --- | --- | --- |
| `TH_DEF_QUANT_INFO` | 155,040 | qi=0, Y, inter（该表全为 304）×510 |
| `TH_VP31_QUANT_INFO` | 1,305,600 | qi=0, Y, inter, zzi=61（2560×510） |

两者都远超 32767，所以**截断不是只在恶意流里发生**；
它是格式的一部分，解码器必须按截断实现。

### 7.3 IDCT 与像素

* 1D IDCT 中间量 int32、输出 int16；2D 末次 `+8>>4` 舍入（`lib/idct.c:301-330`、
  `oc_idct8x8_slow` 等）。
* 规范指出输入系数超过 ±6201 时 1D 变换可能溢出（spec 6247-6252），
  上述乘积截断正好给出了可能越界的路径。
* 最终像素夹到 0..255：intra `+128`，inter 加预测
  （`lib/fragment.c:49-75` 的 `OC_CLAMP255`）。
* 仅 DC 的快速路径：`p = (dc*Q + 15) >> 5`（`lib/state.c:972`，
  编码器 `lib/analyze.c:800`），与规范一致（+15 后右移 5）。
* DC 预测累加可能溢出 int16：规范要求按 16 bit 截断（spec 5733-5741），
  C 用 16 bit 位域 `signed int dc:16`（`lib/state.h:321`）与
  `frags[fragi].dc += pred`（`lib/decode.c:1422,1486`）实现。

### 7.4 一眼看懂的范围表

| 量 | 范围 | 依据 |
| --- | --- | --- |
| 基矩阵元素 | 0..255 | 8 bit |
| DC/AC scale | 0..65535（位宽字段 0..15 → 实际 0..16 bit） | 4-bit 字段 +1 |
| 生成的量化器 | DC: intra 16..4096、inter 32..4096；AC: intra 8..4096、inter 16..4096 | `lib/quant.c:32-33,88,93`；上限见 `lib/quant.h:27` |
| 帧内 qi 个数 | 1..3（帧头只编码这个范围） | `lib/state.h:459-461`；解码 `lib/decode.c:450-463`；编码器最多加 2 个（`lib/rate.c:191-201`） |
| token 幅值 | ≤ 580 | `lib/huffman.h:25` |
| 编码器最坏量化值 | ≤ 510（DC 残差 ≤ 510） | §7.1 实测 |
| 反量化乘积 | ≤ 2,375,680（截断到 int16） | 580×4096 |
| 重建像素 | 0..255 | `OC_CLAMP255` |

---

## 8. 参数兼容（bitstream / API 语义）

按「会不会让另一端解出不同的东西」排序。

1. **打包/解析往返一致（已验证）**。对以下 5 组参数做了 `oc_quant_params_pack` →
   `oc_quant_params_unpack` → `oc_dequant_tables_init` 的往返：
   VP3 默认（376 B）、当前默认（959 B）、63 个单元素区间 + 64 张不同矩阵（4315 B）、
   6 组完全共享（277 B）、inter 区间复制 intra（399 B）。
   解析后再生成的 384 张表与原表**逐项 0 差异**，区间/端点也一致。

2. **指针别名与 DC 替换的潜在冲突（潜在缺陷，已复现）**。
   `lib/quant.c:96-114` 会让同一 qi 内的表共享存储；
   `lib/analyze.c:549-560` 又会就地把 `dequant_tables[qi][pli][qti][0]` 改写成帧首 qi 的 DC。
   若两张**不同 (pli,qti)** 的表别名、而它们在 qi=0 的 DC 不同，后写者会覆盖前者。
   **实测**：构造 `Cb/Cr` 在 qi=31 交点合并的合成 `qinfo`，帧 `qis={0,31}` 后
   Cb 的 DC 被改成 Cr 的值（200，而期望 40）。
   对两套内置参数**不触发**：我枚举了全部 64 个 qi 的别名分组，
   每个组内各成员在 qi=0 的 DC 都相同（DEF: Y-intra 216 / C 276 / inter 304；
   VP31: 140/148/140），因此改写结果一致。
   结论：默认流安全，自定义矩阵（尤其色度两平面在 qi=0 不同、却在高 qi 交叉）不安全。

3. **loop filter 限值 ≥128 会破坏整个量化段（已复现，后果比单表失效严重）**。
   写侧 `nbits=OC_ILOG_32(max)`（`lib/enquant.c:100-103`）对 128..255 得 8，
   而 3 bit 字段只能表示 0..7，`oggpackB_write(8,3)` 写入 0。
   **实测**（同一份 VP3 参数，只把 `loop_filter_limits[0]` 从 30 改成 200）：
   * 解析端只读到 `NBITS=0`，于是 64 个限值全为 0（in-loop 去块滤波被关闭）；
   * 后面的 AC/DC scale 与区间描述的**起始位全部前移 64 bit**：
     `ac_scale[0]` 解出 4146（期望 500），区间变成 63 个长度 1 的单元素区间；
   * 384 张反量化表里有 **24530 / 24576** 个表项与原参数不同。
   也就是说这不是「只丢一张表」，而是**整段量化配置错位**，且解析器仍返回成功。
   规范把限值定义为 7 bit（≤127），所以这是「越界参数 + 无校验」；
   但 API 不报错，会静默产生一个完全不同的、可正常解码的流。

4. **`TH_DEF_QUANT_INFO` 违反 `codec.h` 自述的编码器单调性**。
   `include/theora/codec.h:404-409`：「当前编码器要求 scale 与 base 随 qi 不增」。
   **实测**：默认参数在 1105 个 (qi, coeff) 点上量化器随 qi **增大**（最大 +8，
   首次在 qi 2→3、系数 26：196→200），根因是 `{32,16,15}` 的粗插值与
   `15→21→JPEG` 的基矩阵斜坡不匹配。VP3 参数无违例。
   规范只说「decoder 不要求」（同段），所以**解码兼容性无损**，但
   依赖单调性的上层（例如按 qi 排序的搜索）在默认参数下名义上不成立。

5. **NBMS 385..512**：规范 MUST ≤384，库接受。属超集，不会破坏兼容。

6. **「短」区间流**：解析器会把任何流重排成和恰为 63 的区间（§6.1），
   因此不存在「解析后缺表」的合法流；但**编码器**若收到和不为 63 的参数会越界（§6.2）。

7. **VP3 兼容开关**：`TH_ENCCTL_SET_VP3_COMPATIBLE` 会用
   `TH_VP31_QUANT_INFO` 覆盖当前参数（`lib/encode.c:1392-1401`），
   之后若再 `SET_QUANT_PARAMS` 则流不再 VP3 兼容（文档 `theoraenc.h:79-84` 亦有说明）。
   `TH_ENCCTL_SET_COMPAT_CONFIG`（`lib/encode.c:1513-1543`）允许从一段 setup header
   字节流加载参数，路径与解码器共用 `oc_quant_params_unpack`。

---

## 9. 数值稳定

1. **定点精确性**。编码器把除法实现成 `m/l` 倒数乘（`oc_iquant_init`，
   `lib/enquant.c:184-192`；`lib/enquant.h:9-11` 注释「This is not an approximation;
   for 16-bit x and d, it is exact」）。量化时先 `<<1`、加偏置、乘、移位
   （`lib/enquant.c:230-247`），偏置使平局向远离零取整，且 token 优化只会减小幅值
   （`:235-240`），因此 quantize 与 tokenize 的取舍方向一致。
2. **无浮点路径**。`oc_dequant_tables_init` 只有整数运算；`qfac` 用 `ogg_uint32_t`，
   最大 65535×255 ≈ 1.67×10⁷，`<<2` 后仍远小于 2³²，不会溢出。
   `(qfac/100)<<2` 在 `qfac` 很小时可为 0，由 QMIN 兜底。
3. **速率控制的 qavg 单调性**。`oc_enquant_qavg_init`（`lib/enquant.c:328-369`）
   用 `OC_RPSD`（按系数位置的期望平方幅值）与 `OC_PCD`（按平面的能量占比）做加权调和平均，
   得到 `log_qavg[qti][qi]`。`oc_enc_calc_lambda` 与
   `oc_enc_find_qi_for_target`（`lib/rate.c:130-149`）**假定** qi 越大 qavg 越小。
   **实测**：对两套内置参数逐 qi 重算整数 `q2`，均严格非增
   （0 次反序），因此现有搜索逻辑安全；但对任意用户矩阵没有保证。
   源码本身已意识到这一点：`lib/rate.c:689-692` 的 `TODO` 写道
   「With user-specified quant matrices, we need to enlarge these limits if they
   don't actually let us change qi values」，即自定义矩阵下
   `[0.8Q, 1.2Q]` 的逐帧限制可能窄到只有一个 qi。
4. **DC 预测的 int16 语义**。编解码两端都以 16 bit 存储量化 DC 并累加预测
   （`lib/state.h:321`、`lib/tokenize.c:1001,1064`、`lib/decode.c:1422,1486`）；
   规范要求截断（spec 5733-5741），实现依赖位域/赋值截断，是格式兼容的一部分，
   不是 bug。**但**这意味着「DC 预测残差 ≤510」这一上界只在单步成立，
   长链累加后依赖截断语义。
5. **反量化乘积截断**（§7.2）是格式规定，不是实现取舍，移植时不要换成饱和。
6. **`OC_FZIG_ZAG` 的 128 项哨兵**（`lib/internal.c:25-29` 说明、`:33-41` 的后 64 项全为 64，
   把 zzi≥64 映射到已知位置）防止畸形零游程越界；
   解析侧 `lib/decode.c:1580` 另有 `OC_MINI(zzi,64)`。
7. **指针别名不跨 qi**（§3.4），所以逐帧换 qi 不会复用上一帧的 DC 修正值。
   这一点对「编码器在 `analyze.c` 里就地改 DC」尤其重要：同一帧内多个 qi 的 DC 被
   统一成 `qis[0]` 后，剩余 qi 的表项与解码端「读 `qis[qii]` 的 AC、`qis[0]` 的 DC」一致。

---

## 10. 感知质量

Theora 的矩阵设计把「形状」与「强度」分开：

* **形状**：基矩阵。VP3 直接沿用 JPEG 亮度/色度量化表（`doc/vp3-format.txt:1099-1128`；
  spec 附录 `app:vp3-quant-params`）；inter 矩阵是平的阶梯斜坡，
  与帧间残差的平坦频谱匹配（`lib/enquant.c:266-276` 的 `OC_RPSD` 注释指出
  inter 的能量分布「significantly flatter」）。
* **强度**：DC/AC 两条 64 项 scale 曲线。DC 与 AC 分开，是因为两者对感知的作用不同：
  DC 误差表现为整块亮度偏移，AC 误差是纹理/边缘损失，因而可以给不同的压缩曲线
  （spec 830-837）。默认参数里 dc_scale 与 ac_scale 的数值完全相同，
  两条曲线的形状由基矩阵决定：**实测** Y-intra 的 DC 量化器在 qi=16/32/48/63
  分别比同一 qi 的最高频 AC 量化器小 17%/29%/50%/67%（100 vs 120、48 vs 68、
  24 vs 48、16 vs 48）；qi=0 时该表是常数、两者相等（216）。
* **中间 qi**：用端点线性插值近似 HVS 的非线性响应（spec 828-851 明说
  「approximate the complex, non-linear response of the human visual system」）。
  默认参数的 `{32,16,15}` 分段就是「低 qi 平台、中段斜坡、高 qi 贴近 JPEG」的折中。
* **色度**：VP3 的色度基矩阵在低频段就抬到 47、其后全为 99（`lib/encode.c:47-68`），
  即几乎只保留最低频；默认参数改成 19 平台 → 36 档的斜坡（`lib/encode.c:181-222`），
  即在高 qi 端明显更细。这是两套参数在感知取向上最大的差异之一，
  但「更细是否更好」需要 §12 的主观/客观测试。
* **编码器内部**：`chroma_rd_scale` 把色度 R-D 代价夹到 `[0.25, 4]` 倍亮度
  （`lib/enquant.c:358-365`），`lambda` 取 `0.2125 * qavg²`（`lib/rate.c:156-165`），
  低码率端再选 3 个邻近量化器（`lib/rate.c:178-201`）。
  这些常数都来自作者在测试集上的拟合，本仓库未附带该测试集或复现脚本。

---

## 11. 实测方法与材料

环境：本机 `gcc`，从仓库同级目录 `/srv/fb2/feng0732/0036-ogg-1-8199952b` 构建 libogg；
把本仓库 `autogen.sh && ./configure --disable-examples && make` 构建出
`libtheoraenc`/`libtheoradec`，另建一份 `CFLAGS=-fsanitize=address` 的 ASAN 版本。
所有脚本放在 `/tmp/tq67/`（与源码分离），未使用容器。

### 11.1 仓库自带的材料及其覆盖范围

| 材料 | 与量化的关系 |
| --- | --- |
| `doc/spec/spec.tex` | 规范正文：setup header 语法、区间插值公式、QMIN 表、反量化截断语义、VP3 附录常量。本文所有「规范要求」都出自这里 |
| `doc/vp3-format.txt` | VP3 的早期描述与附录 A 的基矩阵 / scale 表值；可与 `lib/encode.c:25-135` 逐项对照（我已比对，见 §4.3） |
| `lib/encode.c` 的 `TH_VP31_QUANT_INFO` / `TH_DEF_QUANT_INFO` | 两套可直接取用的参数字面量，是本文实测的输入 |
| `tests/`（`noop*.c`、`comment*.c`、`granulepos*.c`） | **不涉及量化**：只做初始化/注释/granulepos 的冒烟测试，`grep -i quant tests/*.c` 为空 |
| `examples/`（`png2theora.c` 等） | 只通过 `th_info.quality`（0..63 的 qi）间接使用量化，`png2theora.c:554` 把 0..10 映射为 `round(q*6.3)`；**没有任何例子设置自定义矩阵** |
| `lib/collect.c` + `tools/process_modedec_stats.c` | 唯一和量化相关的「统计材料」：`OC_MODE_LOGQ` 用 `log_plq` 给模式/R-D 统计分箱，`tools/` 校验不同统计文件的量化分箱是否一致（`tools/process_modedec_stats.c:81-142`）。它服务于编码器建模，不是矩阵正确性测试 |

因此：**本仓库没有针对 setup header 量化段的单元测试**，§11.2 的往返与边界实验都是本次新写的。

### 11.2 本次实验

| 实验 | 目的 | 结果 |
| --- | --- | --- |
| 用真实 `lib/quant.c` 编译 harness，输入从 `lib/encode.c` 解析出的 `TH_VP31_QUANT_INFO`/`TH_DEF_QUANT_INFO` | 校验表生成公式 | 与独立 Python 模型 **0/24576 差异**；两套参数在每个 qi 上都是 `{Y-intra}/{Cb,Cr-intra}/{三平面-inter}` 的 3 组别名，192 个指针目标 |
| 直接编译真实 `lib/fdct.c` + `lib/enquant.c` 跑最坏输入 | 验证 ±510 上界 | intra 510 / inter 510；DC 254/255 |
| `oc_quant_params_pack` → `oc_quant_params_unpack` 往返（5 组参数，含极端） | 区间共享/拷贝标志 | 384 表全等；字节数 277/376/399/959/4315 |
| ASAN + 手工构造 setup header | 边界校验 | NBMS=384/385/512 的完整流均接受；bmi=NBMS 拒绝（−20）；「和 62」被重解释为 `sizes={62,1}` 且 qi=63 仍有表 |
| ASAN + `TH_ENCCTL_SET_QUANT_PARAMS` | API 健壮性 | 区间和 64/70 → `lib/quant.c:89` 越界写 SEGV |
| ASAN + 直接调用 `oc_quant_params_pack` | 打包健壮性 | 区间和 62 → `lib/enquant.c:177` 越界读 |
| 合成 `qinfo`（Cb/Cr 在 qi=31 交叉） | 别名 vs DC 替换 | Cb 的 DC 被覆盖成 Cr 的值 |
| loop filter[0]=200 往返（其余保持 VP3 原值） | 位宽截断 | 限值全 0，且 scale/区间整体错位；24530/24576 表项改变 |
| 逐 qi 重算 `q2`/`log_qavg` | 速率控制单调性假设 | 两套内置参数均 0 次反序 |
| `th_encode_*` + `th_decode_*` 端到端（32×32，自定义参数） | 真实管线 | 自定义与默认流不同；正常参数往返可解码 |

端到端实验中遇到过测试程序自身的两处问题（与本报告结论无关，列出以免误读）：
1. 未先调用 `th_encode_flushheader` 就取数据包，得到的是空包；
2. 我的测试驱动把 header 包与数据包混排，需用 `th_decode_headerin` 的返回值
   （3/2/1/0）而不是包序号来驱动。

---

## 12. 只是待实测的判断

以下内容**没有**在本次分析中被实验证实，属于推断或对作者注释的转述，
不应作为结论引用：

1. 默认参数的 `{32,16,15}` 分段与 4 个端点，相比 VP3 的单区间常量矩阵，
   在码率–质量（PSNR/SSIM/VMAF）上的**净收益**。本文只验证了它与规范/库自洽。
2. 默认参数违反单调性（1105 点、最大 +8）是否产生可见的码率或质量影响；
   是否会让 `oc_enc_find_qi_for_target` 在极端码率下选到「名义上更差」的 qi
   （实测 qavg 仍单调，所以只影响直觉而非现行搜索）。
3. 别名 + DC 替换的缺陷在真实编码流程中的可触发概率。我用合成 `qinfo` 复现了机制，
   但没有构造出「由 `TH_ENCCTL_SET_QUANT_PARAMS` 交给编码器且能通过常规质量检查」的实例；
   需要后续用随机化 qinfo 做 fuzz 才能给出可达性判断。
4. loop filter ≥128 的「整段错位」是否会被真实调用者触发（规范上限 127，
   但 API 不校验）。目前只有我构造的 `qinfo`；需要检查常见封装层
   （ffmpeg2theora、transcode 等）是否会把 >127 的值写进 `loop_filter_limits`。
5. `OC_RPSD`/`OC_PCD`/`lambda=0.2125`/K-means 邻域 `±0.6,±0.7` 这些常数：
   源码注释说明它们来自作者在 QCIF–1080p 测试集上的测量
   （`lib/enquant.c:253-311`、`lib/rate.c:151-202`），本仓库未附数据，
   我也没有重新测量。特别是 `OC_PCD` 的 TODO 自述「只测了 inter 帧」。
6. 1.3%（默认）/2.3%（VP3）的表项被夹到 QMIN 之后，这些系数在感知上是否仍等价。
   **实测**：夹位集中在 qi 46..63（DEF）/37..63（VP3）、zig-zag 序号 0..27 的低频区，
   且 inter 占多数（DEF 267/310，VP3 471/561）；也就是说被夹住的是高 qi 下的
   低频/inter 量化器。它会不会变成可见的块效应，需要主观测试。
7. 4:2:2/4:4:4 下同一套矩阵的感知表现。本文只确认选表规则与像素格式无关。
8. 在 ARM NEON / SSE2 路径上，`oc_enc_quantize_*` 的移位/乘子重排与 C 路径的
   逐位等价性。我只读了 `lib/x86/x86enquant.c:30-84` 与
   `lib/arm/armenquant.s:23-84` 的注释与结构，未做差分测试。
9. 反量化乘积截断（§7.2）在极端合法流上的画质；本文只确认语义。

---

## 13. 参考

源码：

* `lib/quant.c`（表生成、插值、QMIN/QMAX、指针去重、`pp_dc_scale`）
* `lib/dequant.c`（setup header 解析、区间拷贝标志、清理）
* `lib/enquant.c`（参数打包、`oc_iquant_init`、量化、RPSD/PCD/qavg）
* `lib/encode.c`（`TH_VP31_QUANT_INFO`、`TH_DEF_QUANT_INFO`、参数设置/更新、ctl）
* `lib/analyze.c`（每帧压缩表、DC 替换、qti/qii 选择、R-D 选 qii）
* `lib/decode.c`（DC/AC 表选择、块 qii 解码、反量化、后处理尺度）
* `lib/state.h`（`dequant_tables`/`dequant_table_data`、帧 qi、片段位域）
* `lib/tokenize.c`、`lib/huffman.h`（±580 值域与查表）
* `lib/idct.c`、`lib/fragment.c`（重建与像素钳位）
* `lib/rate.c`（lambda、qavg 搜索、3 量化器选择）
* `include/theora/codec.h:339-420`（`th_quant_base`/`th_quant_ranges`/`th_quant_info` 与公式）
* `include/theora/theoraenc.h:52-65,411`（ctl 与默认参数声明）

文档：

* `doc/spec/spec.tex`：`sec:setupheader`（2605+）、`sub:loop-filter-limits`（2635+）、
  `sub:quant-params`（2686+）、`sub:quant-mat`（2905+）、`sub:dequant`（6084+）、
  1D IDCT 与溢出说明（6255+）、DC 预测溢出（5733+）、附录 `app:vp3-quant-params`（8071+）
* `doc/vp3-format.txt`：221-281（VP3 量化矩阵与 scale）、1093-1160（附录 A 表值）
* `CHANGES`：区间插值、setup header 中量化表的引入等历史条目
