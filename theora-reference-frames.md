# Theora 解码器参考帧状态分析

本文分析 libtheora 解码器如何在关键帧（INTRA）与预测帧（INTER）之间维护
**前帧（previous frame）** 与 **黄金参考帧（golden frame）** 两个参考缓冲，
并说明未编码块、色度运动矢量在参考更新中的角色，以及"跳过画面"与"显示时间
推进"的区别。所有结论均给出源码或测试材料出处（`文件:行号`）。

---

## 1. 参考帧状态模型

### 1.1 缓冲池与角色索引

解码器内部维护一个参考帧缓冲池和一组"角色 → 缓冲"的间接索引：

- `oc_theora_state::ref_frame_bufs[6]` / `ref_frame_idx[6]` / `ref_frame_data[6]`
  （`lib/state.h:431-439`）。`ref_frame_idx[i]` 是缓冲池下标，
  `ref_frame_data[i]` 是缓存的像素指针（`ref_frame_bufs[ref_frame_idx[i]][0].data`）。
- 角色常量（`lib/state.h:169-184`）：
  - `OC_FRAME_GOLD (0)` — 黄金参考帧（上一关键帧的重建图像）
  - `OC_FRAME_PREV (1)` — 前帧（上一帧的重建图像）
  - `OC_FRAME_SELF (2)` — 当前正在重建的帧
  - `OC_FRAME_NONE (3)` — 仅作标记用：未编码块（参与 DC 预测逻辑）
  - `OC_FRAME_GOLD_ORIG (4)` / `OC_FRAME_PREV_ORIG (5)` — 编码器专用的
    未压缩原始输入参考，解码器不使用
- 缓冲池大小：解码器分配 **3** 个缓冲（`lib/decode.c:374`，
  `oc_state_init(&_dec->state,_info,3)`），编码器分配 6 个
  （`lib/encode.c:1158`）。3 个缓冲正好够 GOLD、PREV、SELF 三者轮换。

### 1.2 初始状态

`oc_state_ref_bufs_init()` 把全部角色索引置为 -1、指针置为 NULL
（`lib/state.c:657-669`），即解码开始时**没有任何有效参考帧**。

### 1.3 无参考起解（dummy frame）

若第一帧不是关键帧（码流被截断/从中间起解），解码器在需要参考帧而
`ref_frame_idx[GOLD]` 或 `ref_frame_idx[PREV]` 仍为 -1 时调用
`oc_dec_init_dummy_frame()`（`lib/decode.c:2757-2762`）：

- GOLD、PREV、SELF 三个角色全部指向缓冲 0（`lib/decode.c:2062-2068`）；
- 整个缓冲被填充为 0x80（纯灰色，`lib/decode.c:2079`）。

**状态转移**：`(GOLD,PREV)=(-,-)` → `(0,0)`，内容为灰色。这是一种容错
兜底，而非正常码流路径。

---

## 2. 每帧的参考缓冲选择与更新

`th_decode_packetin()`（`lib/decode.c:2740`）是全部状态转移的入口。

### 2.1 SELF 缓冲的选择

对于有编码块的帧，解码器选取一个**既不被 GOLD 也不被 PREV 占用**的空闲
缓冲作为 SELF（`lib/decode.c:2789-2794`）：

```c
for(refi=0;refi==_dec->state.ref_frame_idx[OC_FRAME_GOLD]||
 refi==_dec->state.ref_frame_idx[OC_FRAME_PREV];refi++);
_dec->state.ref_frame_idx[OC_FRAME_SELF]=refi;
```

这保证重建当前帧时不会覆盖两个参考帧——因为 INTER 帧的未编码块和运动
补偿都要读 PREV/GOLD（见第 3、4 节）。

### 2.2 关键帧（INTRA）之后：GOLD 与 PREV 同时指向新帧

- 帧头解析出帧类型（`lib/decode.c:442-476`）；关键帧由
  `oc_dec_mark_all_intra()` 把**所有**块标记为 coded、`refi=OC_FRAME_SELF`、
  `mb_mode=OC_MODE_INTRA`（`lib/decode.c:483-521`），即关键帧不读取任何
  参考帧。
- 重建完成后（`lib/decode.c:2947-2955`）：

```c
if(_dec->state.frame_type==OC_INTRA_FRAME){
  /*The new frame becomes both the previous and gold reference frames.*/
  _dec->state.ref_frame_idx[OC_FRAME_GOLD]=
   _dec->state.ref_frame_idx[OC_FRAME_PREV]=
   _dec->state.ref_frame_idx[OC_FRAME_SELF];
  ...
}
```

**状态转移**：`GOLD ← SELF`，`PREV ← SELF`。关键帧同时刷新两个参考。
同时 `keyframe_num ← curframe_num`（`lib/decode.c:2798-2799`），供 granule
position 计算（见第 5 节）。

编码器侧是对称的：只有 INTRA 帧才更新 GOLD（`lib/encode.c:1702-1709`），
INTER 帧只更新 PREV（`lib/encode.c:1697-1701`）。因此本编码器产生的码流中，
**GOLD 永远等于最近一个关键帧的重建图像**（Theora 格式允许其他编码器更
频繁地刷新黄金帧，解码器并不依赖"GOLD=最近关键帧"这一假设——它只按索引
轮转缓冲）。

### 2.3 预测帧（INTER）之后：仅 PREV 指向新帧

`lib/decode.c:2956-2962`：

```c
else{
  /*Otherwise, just replace the previous reference frame.*/
  _dec->state.ref_frame_idx[OC_FRAME_PREV]=
   _dec->state.ref_frame_idx[OC_FRAME_SELF];
  ...
}
```

**状态转移**：`PREV ← SELF`，`GOLD 不变`。原来的 PREV 缓冲被释放为下一
帧的空闲缓冲（第 2.1 节的选择循环会再次选中它，三个缓冲由此轮转）。

### 2.4 重复帧 / 丢帧（0 字节包）：参考状态完全不变

- 完全为空的包被视为"丢帧"，等价于一个没有任何编码块的 INTER 帧
  （`lib/decode.c:2744-2749`）：

```c
/*A completely empty packet indicates a dropped frame and is treated exactly
   like an inter frame with no coded blocks.*/
if(_op->bytes==0){
  _dec->state.frame_type=OC_INTER_FRAME;
  _dec->state.ntotal_coded_fragis=0;
}
```

- 无编码块时直接返回 `TH_DUPFRAME`，**不分配 SELF 缓冲、不触碰任何参考
  索引**，只推进 granule position 和帧计数（`lib/decode.c:2763-2772`）。
- `TH_DUPFRAME` 的公开语义（`include/theora/codec.h:90-93`）："播放器可以
  继续显示当前帧，因为解码帧缓冲的内容没有改变。"

**状态转移**：`(GOLD,PREV)` 保持不变。注意这与第 4.1 节的"部分块未编码"
不同——整帧无编码块时连 SELF 缓冲都不存在，连复制都不发生。

编码器侧的对应物：
- `oc_enc_drop_frame()`（`lib/encode.c:1259-1271`）：丢帧时令 SELF=PREV
  （即"本帧重建 = 前帧重建"），发出 0 字节包；VP3 兼容模式则显式打包一个
  无编码块的 INTER 帧（`oc_enc_drop_frame_pack()`，`lib/encode.c:875-906`）。
- 应用层还可通过 `TH_ENCCTL_SET_DUP_COUNT` 主动请求重复帧
  （`lib/encode.c:1443-1448`），编码器用 `dup_count`/`prev_dup_count` 计数
  （`lib/encode.c:1751`、`1776`）。

### 2.5 状态转移汇总

| 事件 | GOLD | PREV | 出处 |
|---|---|---|---|
| 初始 | –（-1） | –（-1） | `lib/state.c:657-669` |
| 无参考起解 | 灰帧（buf0） | 灰帧（buf0） | `lib/decode.c:2053-2080` |
| 关键帧重建后 | ← SELF | ← SELF | `lib/decode.c:2947-2955` |
| 预测帧重建后 | 不变 | ← SELF | `lib/decode.c:2956-2962` |
| 0 字节包 / 无编码块帧 | 不变 | 不变 | `lib/decode.c:2744-2772` |

---

## 3. 各预测模式下两个参考缓冲分别指向的图像

宏块模式到参考帧角色的映射由 `OC_FRAME_FOR_MODE()` 给出
（`lib/state.h:214-217`，配合 nibble 表宏 `lib/ocintrin.h:111-113`）：

| 模式 | 值 | 参考 | 含义（`lib/state.h:186-212`） |
|---|---|---|---|
| `OC_MODE_INTER_NOMV` | 0 | PREV | 前帧同位置，无运动矢量 |
| `OC_MODE_INTRA` | 1 | SELF | 帧内编码，不用参考 |
| `OC_MODE_INTER_MV` | 2 | PREV | 前帧 + 新运动矢量 |
| `OC_MODE_INTER_MV_LAST` | 3 | PREV | 前帧 + 上一个运动矢量 |
| `OC_MODE_INTER_MV_LAST2` | 4 | PREV | 前帧 + 上上个运动矢量 |
| `OC_MODE_GOLDEN_NOMV` | 5 | GOLD | 黄金帧同位置 |
| `OC_MODE_GOLDEN_MV` | 6 | GOLD | 黄金帧 + 新运动矢量 |
| `OC_MODE_INTER_MV_FOUR` | 7 | PREV | 前帧 + 每个亮度块各自的运动矢量 |

即：**8 种模式里 5 种以前帧为参考、2 种以黄金帧为参考、1 种不用参考**。
运动矢量解包与逐块赋值在 `oc_dec_mv_unpack_and_frag_modes_fill()`
（`lib/decode.c:806-900`）：

- 非 FOUR 模式：整个宏块（亮度 4 块 + 各色度块）共享一个 `mbmv`，
  `refi=OC_FRAME_FOR_MODE(mb_mode)`（`lib/decode.c:884-896`）；
- `INTER_MV_FOUR`：4 个亮度块各自解一个 MV，`refi=OC_FRAME_PREV`
  （`lib/decode.c:841-854`），色度块的 MV 由亮度 MV 推导（见 4.2）；
- `MV_LAST`/`MV_LAST2` 通过 `last_mv`/`prior_mv` 两个变量复用最近解出的
  运动矢量（`lib/decode.c:869-878`），参考仍是 PREV。

结合第 2 节可知这些指针在不同时刻指向的图像：

- 在**关键帧之后的第一帧 INTER**：GOLD 与 PREV 指向同一图像（刚解码的
  关键帧），GOLDEN_* 与 INTER_* 模式此时参考内容相同，只是索引不同。
- 在**连续的 INTER 帧序列中**：PREV 逐帧滑动（始终为上一帧重建），GOLD
  一直锚定在最近关键帧上，直到下一个关键帧同时刷新两者。

---

## 4. 未编码块与色度运动矢量如何参与参考更新

### 4.1 未编码块（uncoded fragments）

INTER 帧中未被编码的块经历以下路径：

1. **标记**：`oc_dec_coded_flags_unpack()` 按超块/块游程标志建立 coded 与
   uncoded 两个列表，未编码块的 `refi` 被置为 `OC_FRAME_NONE`
   （`lib/decode.c:655-658`）。`OC_FRAME_NONE` 只是 DC 预测中的标记
   （`lib/state.h:176-177`），不是真实缓冲。
2. **DC 预测**：`oc_dec_dc_unpredict_mcu_plane_c()` 按 `refi` 把邻居分桶
   （每个参考角色一条 `pred_last` 链）；未编码邻居不参与预测（`l_ref=-1`
   分支，`lib/decode.c:1440-1493`）。这保证 GOLD 参考块与 PREV 参考块的
   DC 预测链互不污染。
3. **像素来源**：重建完编码块后，未编码块被**从 PREV 逐块复制到 SELF**
   （`lib/decode.c:1598-1606`）：

```c
/*Copy the uncoded blocks from the previous reference frame.*/
if(_pipe->nuncoded_fragis[_pli]>0){
  ...
  oc_frag_copy_list(&_dec->state,
   _dec->state.ref_frame_data[OC_FRAME_SELF],
   _dec->state.ref_frame_data[OC_FRAME_PREV], ...);
}
```

**对参考更新的意义**：未编码块使 SELF 缓冲在对应区域成为 PREV 的像素级
拷贝；帧末 `PREV ← SELF`（第 2.3 节）后，这些区域在新 PREV 中保持不变，
效果上等价于"前帧内容跨帧延续"。GOLD 完全不受未编码块影响——复制源永远
是 PREV，且 GOLD 索引在 INTER 帧从不改变。当**整帧**都没有编码块时，连
复制都省略，直接走第 2.4 节的 `TH_DUPFRAME` 早退路径
（`lib/decode.c:2763-2772`）。

### 4.2 色度运动矢量

色度运动矢量**不从码流读取**，全部由亮度运动矢量推导：

- 非 FOUR 模式：色度块与亮度块共用宏块级 `mbmv`
  （`lib/decode.c:884-896` 的循环覆盖 `map_nidxs` 个块，含色度）。
- `INTER_MV_FOUR`：调用 `OC_SET_CHROMA_MVS_TABLE[pixel_fmt]`
  （`lib/decode.c:821`、`855`），按像素格式从 4 个亮度 MV 合成色度 MV
  （`lib/state.c:33-97`）：
  - 4:2:0（`oc_set_chroma_mvs00`）：4 个亮度 MV 分量求和后除以 4
    （`OC_DIV_ROUND_POW2(dx,2,2)`），即均值再按 1/2 像素精度取整；
  - 4:2:2 / 4:4:0（`oc_set_chroma_mvs10`/`01`）：沿未抽取方向两两平均；
  - 4:4:4（`oc_set_chroma_mvs11`）：直接照搬 4 个亮度 MV。
- 色度块的**参考帧角色与亮度一致**（同一 `refi` 赋值循环，
  `lib/decode.c:856-865`、`887-896`）：宏块模式选 GOLD，色度也从 GOLD
  做运动补偿；选 PREV 则色度也从 PREV。

**对参考更新的意义**：色度 MV 只影响从 GOLD/PREV **读取**哪些像素做运动
补偿，不改变参考缓冲的**写入**路径——参考更新只发生在帧末的索引轮换
（第 2 节），与 MV 无关。未编码的色度块同样走 4.1 的 PREV→SELF 复制。

---

## 5. 跳过画面 vs 显示时间变化

这两个概念在解码器中是**正交**的：

### 5.1 跳过画面（dropped / duplicated frame）

指码流中的 0 字节包（或无编码块的 INTER 帧）。其特征是：

- **参考状态不变**（第 2.4 节）：GOLD、PREV 索引与缓冲内容都不动；
- **显示时间照常推进**：`curframe_num++`，granule position 更新
  （`lib/decode.c:2766-2770`），返回 `TH_DUPFRAME` 提示应用继续显示上一帧
  图像（`include/theora/codec.h:90-93`）。

即"画面内容停了一帧，但播放时钟走了一帧"。

### 5.2 显示时间（granule position）

Theora 的 granpos 把"最近关键帧号"与"距关键帧的帧偏移"打包在一个 64 位
整数里（`lib/decode.c:2827-2829`）：

```c
_dec->state.granpos=(_dec->state.keyframe_num+_dec->state.granpos_bias<<
 _dec->state.info.keyframe_granule_shift)
 +(_dec->state.curframe_num-_dec->state.keyframe_num);
```

- `keyframe_num` 在关键帧时更新（`lib/decode.c:2798-2799`）；
- `granpos_bias` 区分码流版本：3.2.0 存帧索引、3.2.1 起存帧计数
  （`lib/state.c:746-752`）；
- `th_granule_frame()` / `th_granule_time()` 把 granpos 换算为帧号与秒
  （`lib/state.c:1242-1267`）；
- seek 之后应用可用 `TH_DECCTL_SET_GRANPOS` 重设时间计数
  （`lib/decode.c:1998-2010`）——这只改 `keyframe_num`/`curframe_num`，
  **不改任何参考缓冲**；参考状态要靠后续关键帧（或 dummy 灰帧兜底，
  第 1.3 节）重建。

编码器侧，重复帧也计入 granpos：`dup_offs=prev_dup_count-nqueued_dups`
（`lib/encode.c:1322-1336`），保证丢帧不造成时间戳空洞。

### 5.3 测试材料佐证

`tests/granulepos_theora.c:93-118` 对编码器输出逐帧验证：

- granulepos 单调不减（`granulepos_theora.c:100-102`）；
- `(granpos>>shift) + (granpos&mask) == frame+1`，即关键帧号加偏移恰好
  还原帧计数（`granulepos_theora.c:112-113`）；
- `theora_granule_frame()` 返回精确帧号、`theora_granule_time()` 返回
  `(frame+1)/fps` 秒（`granulepos_theora.c:114-117`），与
  `lib/state.c:1242-1267` 的实现一一对应。

### 5.4 小结

| 操作 | 参考缓冲状态 | 显示时间 |
|---|---|---|
| 正常关键帧 | GOLD、PREV 都刷新 | 推进，`keyframe_num` 重置 |
| 正常预测帧 | 仅 PREV 刷新 | 推进 |
| 跳过画面（0 字节包） | **完全不变** | **照样推进**（TH_DUPFRAME） |
| seek 后 SET_GRANPOS | 不变 | 计数被重设 |

---

## 附：关键源码位置索引

| 主题 | 位置 |
|---|---|
| 角色常量与模式→参考映射 | `lib/state.h:156-217` |
| 缓冲池初始化 / 索引清零 | `lib/state.c:545-671` |
| 色度 MV 合成表 | `lib/state.c:33-97` |
| 帧头解析（帧类型） | `lib/decode.c:442-476` |
| 关键帧全 INTRA 标记 | `lib/decode.c:483-521` |
| 未编码块标记（OC_FRAME_NONE） | `lib/decode.c:599-699` |
| MV 解包与逐块赋值 | `lib/decode.c:806-900` |
| DC 反预测（按 refi 分桶） | `lib/decode.c:1392-1500` |
| 未编码块 PREV→SELF 复制 | `lib/decode.c:1598-1606` |
| dummy 灰帧兜底 | `lib/decode.c:2053-2080` |
| 主入口与全部状态转移 | `lib/decode.c:2740-2986` |
| 编码器侧对称更新 / 丢帧 | `lib/encode.c:1259-1271`、`1696-1776` |
| granpos 换算 | `lib/state.c:1242-1267` |
| granulepos 测试 | `tests/granulepos_theora.c:93-118` |
