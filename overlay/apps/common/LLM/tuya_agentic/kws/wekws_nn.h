#ifndef __WEKWS_NN_H__
#define __WEKWS_NN_H__
/* wekws_nn.h — v9d FSMN int16 流式推理:
 * push() 入队一帧堆叠特征(400 = 5x80 上下文), flush() 批算队列内全部帧,
 * 逐帧 T=1 状态推进(4 层 x 11 帧 x 64 int16 缓存, 已证与整句 T=N 逐位
 * 一致)。logits 按入队次序逐行输出(反量化 float, softmax 归 CTC)。
 * 契约: 每次喂音(wekws_fbank_feed)之后必须 flush; fbank 每 640 样本片
 * 最多产 2 个堆叠帧(30ms 步长, 尾帧滞留), 队列容量 WK_NN_QH 为其数倍。
 * wekws_nn_step() = push + flush(1) 薄包装(自测/兼容保留)。 */
#include "wekws_model_data.h"

#define WK_NN_QH 8   /* 堆叠帧队列容量 */

void wekws_nn_reset(void);
void wekws_nn_push(const float *f400);
int  wekws_nn_flush(float (*logits)[WK_NCLASS], int max_out);
void wekws_nn_step(const float *f400, float *logits);
/* 拆段计时(心跳归因; FSMN 版仅保留接口, stage 恒 0, pw=nn 总 tick) */
void wekws_nn_timers(unsigned int *stage, unsigned int *pw);
#endif /* __WEKWS_NN_H__ */
