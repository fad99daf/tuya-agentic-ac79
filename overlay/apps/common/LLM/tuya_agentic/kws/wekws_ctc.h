#ifndef __WEKWS_CTC_H__
#define __WEKWS_CTC_H__
/* wekws_ctc.h — wekws CTC 前缀束搜索流式移植(v9d: ref/kws_loss.py 逐分支
 * 对齐, 含 node dict 别名语义——见 wekws_ctc.c 头注) + 小涂小涂 命中扫描
 * (变体几何均分 √p, ref/onnx_stream_score.py 同义)。 */

/* 全复位: beam 回到 {(), (pb=1, pnb=0)}。 */
void wkc_reset(void);

/* 喂一帧 logits(WK_NCLASS=9)。内部: 快速exp softmax -> topk(3) 过滤
 * (p>0.05 且 token∈0..8, 保序) -> beam 推进(束 20, 前缀滑窗宽 12——满前缀
 * 再扩展挤掉最老 1 token, 无 VAD 连续流不饱和) ->
 * 变体 [1,2,1,2]/[1,3,1,3] 扫描(词尾 2s 新鲜度门, 防陈旧变体跨句计分)。
 * 返回当前帧命中分(0=无)。帧无候选时 beam 不动(对齐 ref 的 continue);
 * 连续 300 帧无非空白候选自动全复位(防跨语句拼装)。 */
float wkc_step(const float *logits);

/* 策略复位判据(喂端静音复位用): 束内是否有"进行中的词"——某 hyp 末尾
 * 1..4 个 token 恰为某变体前缀, 且末节点在新鲜窗(≈2s)内。返回 1=有进展,
 * 喂端只复位 nn(保束桥接词内停顿); 0=无进展, 喂端三件套全复位。 */
int  wkc_has_variant_progress(void);

/* 诊断: 当前 beam 内 hyp 数(给自测/日志)。 */
int  wkc_hyp_count(void);

#endif /* __WEKWS_CTC_H__ */
