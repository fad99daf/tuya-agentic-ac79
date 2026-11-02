/* ============================================================================
 * wekws_ctc.c — wekws CTC 前缀束搜索流式移植 + 关键词命中扫描。
 *
 * 逐分支镜像 ref(机构 v9b scorer 部署口径 = wekws loss ctc_prefix_beam_search):
 *   每帧: topk(3) -> 过滤(p>0.05 且 idx∈IDXSET=0..8, 保序=概率降序) -> 空则跳帧
 *   对每个候选 s、每条当前 hyp (pb,pnb,nodes):
 *     s==0(空白):      next[prefix].pb += (pb+pnb)*ps; nodes=cur 浅拷贝
 *     s==last(重复):   pnb≠0: next[prefix].pnb += pnb*ps; nodes=cur 浅拷贝,
 *                        ps>末节点prob 时 原地突变该节点(共享记录穿透!)
 *                      pb≠0: next[prefix+(s)].pnb += pb*ps;
 *                        nodes=cur 浅拷贝+追加新记录
 *     其余(新字):      next[prefix+(s)].pnb += (pb+pnb)*ps;
 *                        该帧已触达: ps>末节点prob 则 pop+append 新记录
 *                        (fresh, 不传染); 否则 nodes=cur 浅拷贝+追加
 *   排序(pb+pnb 降序, 稳定=触达序)取前 20。
 *
 * ★ 别名语义(2026-11-02 定案, 板级分数与考卷/机构部署 scorer 一致的根基):
 *   ref 的 nodes=cur_nodes.copy() 是浅拷贝 —— node dict 跨 hyp 共享; s==last
 *   首支路的 nodes[-1]['prob']=ps 原地改的是共享 dict, 会反向传染到 cur 及
 *   一切持有该 dict 的表项。v6(3 真词)几乎不触发; v9d(9 类, 土/兔/突/秃/~
 *   重复候选)高频触发, hold_pos_013 ref=0.554 vs 误换新 dict=0.237(漏唤醒)。
 *   C 镜像: node 记录池 + 引用计数, 浅拷贝=拷索引并 inc, pop=dec, 突变=写穿。
 *
 * 命中扫描(对齐 ref/onnx_stream_score.py::keyword_score): 束内按总分降序找
 * 变体 [1,2,1,2]/[1,3,1,3](token: 0=blank 1=小 2=涂 3=图) 连续子序列,
 * p=∏nodes[j].prob, score=√p; 首个产生命中的 hyp 即停(后续不看)。
 *
 * 流式附加(参考实现为整句离线, 设备端长流必需, 均不改变单句语义):
 *   - 前缀长度上限 12(考卷验证 clipped=0; 兼作滑窗宽度)
 *   - 前缀滑窗: 满前缀(12)再扩展时挤掉最老 1 token——无 VAD 连续流里
 *     环境声 ~3.5s 即把全部 hyp 灌满, 追加分支全关=束死锁, 词永远拼不出
 *     (板测13板级全零根因之一, E4 宿主复现); 滑窗保住追加能力, 词长
 *     4 token, 12 宽窗内不受影响
 *   - 命中新鲜度门 WKC_FRESH=66 hop(≈2s): 变体末节点超窗判陈旧不计分——
 *     束跨句持久, 已完成变体会被逐帧重扫到(板测⑫: 四段话 utt max 恒同值
 *     0.271; 若过阈值则每冷却期重复触发=自动唤醒死循环)。词尾 2s 内仍可读
 *     分(覆盖慢说/尾音), 跨句(>2s)陈旧变体自动失效
 *   - 连续 300 帧(3s)无任何非空白候选 -> 全复位: 清掉滞留的陈旧前缀,
 *     防止跨语句"拼装"假唤醒(与 FSMN 引擎 gap_threshold 同一动机)
 * ==========================================================================*/
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "wekws_ctc.h"
#include "wekws_model_data.h"

#define WKC_BEAM    20
#define WKC_TOPK    3
#define WKC_MAXTOK  12
#define WKC_STAGE   96      /* next_hyps 暂存: ≤20 旧前缀 + 3×20 新扩展, 封顶 */
#define WKC_IDLE_RESET_FRAMES 300
#define WKC_FRESH  66          /* 命中新鲜度窗(66 hop≈2.0s@30ms): 变体末节点帧号
                                * 距当前帧超窗即判陈旧, 不计分。板测⑫(11/02)定案:
                                * 无此门时已完成的变体连同节点分数永久留束, 命中扫描
                                * 每帧重扫到旧分(四段话 utt max 恒 0.271 同值), 且
                                * 一旦过阈值会每冷却期重复触发=自动唤醒死循环 */
#define WKC_POOL    1024    /* node 记录池: 活跃 ≤ beam×MAXTOK=240, 余量 4x */

/* §5b 裁头后 token(原 2600 词表 -> 9 类): blank=0 小=1 涂=2 图=3 土=4 兔=5
 * 突=6 秃=7 <filler>~=8 */
#define TOK_XIAO  1
#define TOK_TU1   2
#define TOK_TU2   3
#define WK_NVAR   2
#define WK_VLEN   4
static const int16_t WK_VARIANTS[WK_NVAR][WK_VLEN] = {
    { TOK_XIAO, TOK_TU1, TOK_XIAO, TOK_TU1 },
    { TOK_XIAO, TOK_TU2, TOK_XIAO, TOK_TU2 },
};

/* ---- node 记录池(引用计数共享, 镜像 ref 的 dict 别名语义) ---- */
typedef struct {
    int16_t token;
    int16_t refc;       /* 0=空闲(兼作空闲链 next) */
    int32_t frame;
    float   prob;
} wkc_rec_t;

typedef struct {
    int        len;
    int16_t    tok[WKC_MAXTOK];
    float      pb, pnb;
    int        nn;
    int16_t    node[WKC_MAXTOK];    /* 池索引(浅拷贝语义) */
} wkc_hyp_t;

static wkc_hyp_t  s_cur[WKC_BEAM];      /* 当前束(降序) */
static int        s_ncur;
static wkc_hyp_t  s_nxt[WKC_STAGE];     /* 本帧暂存(触达序=dict 保序) */
static int        s_nnx;
static wkc_rec_t  s_rec[WKC_POOL];
static int16_t    s_freelist;           /* 空闲记录链头(-1=空) */
static int        s_broken;             /* 池耗尽标志: 帧末全复位兜底 */
static float      s_probs[WK_NCLASS];
static uint32_t   s_frame;
static uint32_t   s_idle;               /* 连续无非空白候选帧数 */

/* ---- 快速 exp: exp(x) for x<=0, LUT(257)+线性插值, 相对误差 ~1e-5 ---- */
static inline float wkc_fast_exp(float x)
{
    float e = x * 1.442695041f;
    int   t = (int)e;
    float f, tab;
    int   i;
    if (e < 0.0f && e != (float)t) t--;         /* floor (e<=0) */
    f = (e - (float)t) * 256.0f;
    i = (int)f;
    f -= (float)i;
    tab = wk_exp2_tab[i] + (wk_exp2_tab[i + 1] - wk_exp2_tab[i]) * f;
    if (t < -126) return 0.0f;
    {
        union { uint32_t u; float f; } s;
        s.u = (uint32_t)(t + 127) << 23;        /* 2^t 精确 */
        return tab * s.f;
    }
}

static inline int wkc_is_kw(int t)
{
    (void)t;
    return 1;   /* v9d 9 类: 全部 token 进束(IDXSET=0..8), 不再过滤 */
}

/* ---- 记录池: 分配/释放(空闲链穿 refc 字段) ---- */
static int16_t wkc_rec_alloc(int16_t tok, int32_t fr, float pb)
{
    int16_t id = s_freelist;
    if (id < 0) {                               /* 池满: 置位, 帧末兜底复位 */
        s_broken = 1;
        return -1;
    }
    s_freelist = s_rec[id].refc;
    s_rec[id].token = tok;
    s_rec[id].frame = fr;
    s_rec[id].prob = pb;
    s_rec[id].refc = 1;
    return id;
}

static void wkc_rec_free(int16_t id)
{
    s_rec[id].refc = s_freelist;
    s_freelist = id;
}

/* hyp 挂/卸 nodes(所有权语义: assign=先卸旧; copy=浅拷贝逐索引 inc) */
static void wkc_nodes_drop(wkc_hyp_t *h)
{
    int i;
    for (i = 0; i < h->nn; i++) {
        if (--s_rec[h->node[i]].refc == 0) wkc_rec_free(h->node[i]);
    }
    h->nn = 0;
}

static void wkc_nodes_copy(wkc_hyp_t *h, const wkc_hyp_t *src)
{
    int i;
    for (i = 0; i < src->nn; i++) {
        h->node[i] = src->node[i];
        s_rec[src->node[i]].refc++;
    }
    h->nn = src->nn;
}

static void wkc_nodes_assign_copy(wkc_hyp_t *h, const wkc_hyp_t *src)
{
    wkc_nodes_drop(h);
    wkc_nodes_copy(h, src);
}

/* cur 浅拷贝 + 追加新记录(ref: cur_nodes.copy()+append(fresh dict))。
 * src 满前缀(len==MAXTOK)时退化为滑窗: 挤掉最老 1 记录再追加——无 VAD
 * 连续流束会永久饱和(板测⑬全零根因之一: 追加分支全关, 词永远拼不出来),
 * 滑窗保住追加能力; 词长 4 token, 12 宽窗内不受影响(板测13全零根因之一)。 */
static void wkc_nodes_assign_ext(wkc_hyp_t *h, const wkc_hyp_t *src,
                                 int16_t tok, uint32_t fr, float pb)
{
    int16_t id;
    int     i, from = 0;
    if (src->len >= WKC_MAXTOK) from = 1;   /* 满前缀滑窗: 丢最老 */
    wkc_nodes_drop(h);
    for (i = from; i < src->nn; i++) {
        h->node[h->nn] = src->node[i];
        s_rec[src->node[i]].refc++;
        h->nn++;
    }
    id = wkc_rec_alloc(tok, (int32_t)fr, pb);
    if (id >= 0 && h->nn < WKC_MAXTOK) h->node[h->nn++] = id;
}

/* 已触达表项的 pop+append(ref: nodes.pop(); nodes.append(fresh dict)) */
static void wkc_nodes_pop_append(wkc_hyp_t *h, int16_t tok, uint32_t fr,
                                 float pb)
{
    int16_t id;
    if (h->nn > 0) {
        if (--s_rec[h->node[h->nn - 1]].refc == 0) {
            wkc_rec_free(h->node[h->nn - 1]);
        }
        h->nn--;
    }
    id = wkc_rec_alloc(tok, (int32_t)fr, pb);
    if (id >= 0 && h->nn < WKC_MAXTOK) h->node[h->nn++] = id;
}

/* ★ 穿透突变(ref: nodes[-1]['prob']=ps 原地改共享 dict -> 传染全持有人) */
static void wkc_nodes_mutate_last(wkc_hyp_t *h, uint32_t fr, float pb)
{
    if (h->nn > 0) {
        int16_t id = h->node[h->nn - 1];
        if (pb > s_rec[id].prob) {
            s_rec[id].prob = pb;
            s_rec[id].frame = (int32_t)fr;
        }
    }
}

void wkc_reset(void)
{
    int i;
    for (i = 0; i < WKC_POOL; i++) s_rec[i].refc = (int16_t)(i + 1);
    s_rec[WKC_POOL - 1].refc = -1;
    s_freelist = 0;
    s_broken = 0;
    memset(s_cur, 0, sizeof(s_cur));
    memset(s_nxt, 0, sizeof(s_nxt));
    s_cur[0].pb = 1.0f;
    s_ncur = 1;
    s_nnx = 0;
    s_frame = 0;
    s_idle = 0;
}

int wkc_hyp_count(void)
{
    return s_ncur;
}

/* 暂存区按 token 序列找 hyp; 找不到返回 -1 */
static int wkc_find(const int16_t *tok, int len)
{
    for (int i = 0; i < s_nnx; i++) {
        if (s_nxt[i].len == len &&
            (len == 0 || memcmp(s_nxt[i].tok, tok, len * sizeof(int16_t)) == 0)) {
            return i;
        }
    }
    return -1;
}

/* 稳定插入排序: (pb+pnb) 降序, 截断到 WKC_BEAM。所有权: 保留项交 s_cur,
 * 落选项与旧 s_cur 全部卸除(引用计数回收)。 */
static void wkc_sort_prune(void)
{
    int n = s_nnx < WKC_BEAM ? s_nnx : WKC_BEAM;
    int i;
    for (i = 1; i < s_nnx; i++) {
        wkc_hyp_t key = s_nxt[i];
        float     ks = key.pb + key.pnb;
        int       j = i - 1;
        while (j >= 0 && (s_nxt[j].pb + s_nxt[j].pnb) < ks) {
            s_nxt[j + 1] = s_nxt[j];
            j--;
        }
        s_nxt[j + 1] = key;
    }
    for (i = 0; i < s_ncur; i++) wkc_nodes_drop(&s_cur[i]);   /* 旧束卸除 */
    for (i = 0; i < n; i++) s_cur[i] = s_nxt[i];              /* 保留项移交 */
    s_ncur = n;
    for (i = n; i < s_nnx; i++) wkc_nodes_drop(&s_nxt[i]);    /* 落选卸除 */
    s_nnx = 0;
}

/* 束扫描: 变体子序列命中分。首个产生命中的 hyp 即停。
 * 新鲜度门: 匹配的末节点(词尾)帧号必须落在当前帧 WKC_FRESH 窗内——
 * 束是跨句持久的, 无此门则历史变体永久可扫到(板测⑫冻结分教训);
 * 词头侧不受影响(VAD 起始前的节点只要在窗内仍有效)。 */
static float wkc_hit_scan(void)
{
    float best = 0.0f;
    for (int h = 0; h < s_ncur; h++) {
        const wkc_hyp_t *hp = &s_cur[h];
        for (int v = 0; v < WK_NVAR && best <= 0.0f; v++) {
            for (int i = 0; i + WK_VLEN <= hp->len; i++) {
                if (memcmp(hp->tok + i, WK_VARIANTS[v],
                           WK_VLEN * sizeof(int16_t)) != 0) {
                    continue;
                }
                if ((int32_t)(s_frame - 1 - s_rec[hp->node[i + WK_VLEN - 1]]
                                     .frame) > WKC_FRESH) {
                    continue;   /* 词尾陈旧: 不是刚说完的词, 不计 */
                }
                float p = 1.0f;
                for (int j = i; j < i + WK_VLEN; j++) {
                    p *= s_rec[hp->node[j]].prob;
                }
                if (p > 0.0f) {
                    float s = sqrtf(p);
                    if (s > best) best = s;
                }
            }
        }
        if (best > 0.0f) break;   /* 对齐 ref: 首个非零 hyp 即停 */
    }
    return best;
}

float wkc_step(const float *logits)
{
    float  maxl = logits[0];
    float  den = 0.0f;
    float  top[WKC_TOPK];
    int    tidx[WKC_TOPK];
    int    cand[WKC_TOPK];
    int    ncand = 0;
    uint32_t t = s_frame++;

    for (int c = 1; c < WK_NCLASS; c++) {
        if (logits[c] > maxl) maxl = logits[c];
    }
    for (int c = 0; c < WK_NCLASS; c++) {
        s_probs[c] = wkc_fast_exp(logits[c] - maxl);
        den += s_probs[c];
    }
    {
        float inv = 1.0f / den;
        for (int c = 0; c < WK_NCLASS; c++) s_probs[c] *= inv;
    }

    /* topk(3): 概率降序, 同值取低索引 */
    for (int j = 0; j < WKC_TOPK; j++) { top[j] = -1.0f; tidx[j] = -1; }
    for (int c = 0; c < WK_NCLASS; c++) {
        float p = s_probs[c];
        int   pos = WKC_TOPK;
        while (pos > 0 && p > top[pos - 1]) pos--;
        if (pos < WKC_TOPK) {
            for (int j = WKC_TOPK - 1; j > pos; j--) {
                top[j] = top[j - 1];
                tidx[j] = tidx[j - 1];
            }
            top[pos] = p;
            tidx[pos] = c;
        }
    }
    for (int j = 0; j < WKC_TOPK; j++) {
        if (tidx[j] >= 0 && top[j] > 0.05f && wkc_is_kw(tidx[j])) {
            cand[ncand++] = tidx[j];
        }
    }

    int any_nonblank = 0;
    for (int j = 0; j < ncand; j++) {
        if (cand[j] != 0) any_nonblank = 1;
    }
    if (any_nonblank) {
        s_idle = 0;
    } else if (++s_idle >= WKC_IDLE_RESET_FRAMES) {
        wkc_reset();               /* 长静默: 清陈旧前缀防跨句拼装 */
        s_frame = t + 1;
        return 0.0f;
    }

    if (ncand == 0) {
        return 0.0f;               /* 对齐 ref: 无候选跳帧, 束不动 */
    }

    s_nnx = 0;
    for (int ci = 0; ci < ncand; ci++) {
        int   s = cand[ci];
        float ps = s_probs[s];

        for (int h = 0; h < s_ncur; h++) {
            const wkc_hyp_t *ch = &s_cur[h];
            int   last = ch->len > 0 ? ch->tok[ch->len - 1] : -1;

            /* 找/建 next 表项(前缀=ch 前缀, nn=0 表示 nodes 未挂) */
            #define WKC_TOUCH_SAME()                                     \
                do {                                                      \
                    j = wkc_find(ch->tok, ch->len);                       \
                    if (j < 0 && s_nnx < WKC_STAGE) {                     \
                        j = s_nnx++;                                      \
                        memset(&s_nxt[j], 0, sizeof(wkc_hyp_t));          \
                        memcpy(s_nxt[j].tok, ch->tok,                     \
                               ch->len * sizeof(int16_t));                \
                        s_nxt[j].len = ch->len;                           \
                    }                                                    \
                } while (0)
            /* 找/建 next 表项(前缀=ch+(s); ch 满前缀时滑窗: 丢最老 1 token) */
            #define WKC_TOUCH_EXT()                                      \
                do {                                                     \
                    int16_t nt_[WKC_MAXTOK];                             \
                    int     nl_;                                         \
                    if (ch->len < WKC_MAXTOK) {                          \
                        memcpy(nt_, ch->tok, ch->len * sizeof(int16_t)); \
                        nt_[ch->len] = (int16_t)s;                       \
                        nl_ = ch->len + 1;                               \
                    } else {                                             \
                        memcpy(nt_, ch->tok + 1,                         \
                               (size_t)(ch->len - 1) * sizeof(int16_t)); \
                        nt_[WKC_MAXTOK - 1] = (int16_t)s;                \
                        nl_ = WKC_MAXTOK;                                \
                    }                                                    \
                    j = wkc_find(nt_, nl_);                              \
                    if (j < 0 && s_nnx < WKC_STAGE) {                    \
                        j = s_nnx++;                                     \
                        memset(&s_nxt[j], 0, sizeof(wkc_hyp_t));         \
                        memcpy(s_nxt[j].tok, nt_,                        \
                               (size_t)nl_ * sizeof(int16_t));           \
                        s_nxt[j].len = nl_;                              \
                    }                                                    \
                } while (0)

            if (s == 0) {
                /* 空白: pb += (pb+pnb)*ps; nodes 无条件 = cur 浅拷贝 */
                int j;
                WKC_TOUCH_SAME();
                if (j >= 0) {
                    s_nxt[j].pb += (ch->pb + ch->pnb) * ps;
                    wkc_nodes_assign_copy(&s_nxt[j], ch);
                }
            } else if (s == last) {
                if (fabsf(ch->pnb) > 1e-6f) {
                    /* *ss -> *s: pnb += pnb*ps; nodes = cur 浅拷贝,
                     * ps 超过末节点则 穿透突变该共享记录(ref 别名语义) */
                    int j;
                    WKC_TOUCH_SAME();
                    if (j >= 0) {
                        s_nxt[j].pnb += ch->pnb * ps;
                        wkc_nodes_assign_copy(&s_nxt[j], ch);
                        wkc_nodes_mutate_last(&s_nxt[j], t, ps);
                    }
                }
                if (fabsf(ch->pb) > 1e-6f) {
                    /* *s-s -> *ss: pnb += pb*ps; nodes = cur 浅拷贝+追加
                     * (满前缀时 TOUCH_EXT/assign_ext 自动滑窗) */
                    int j;
                    WKC_TOUCH_EXT();
                    if (j >= 0) {
                        s_nxt[j].pnb += ch->pb * ps;
                        wkc_nodes_assign_ext(&s_nxt[j], ch, (int16_t)s,
                                             t, ps);
                    }
                }
            } else {
                {
                    /* 新字: pnb += (pb+pnb)*ps;
                     * 本帧未触达(nn==0): nodes = cur 浅拷贝+追加(满前缀滑窗);
                     * 已触达: 仅当 ps>末记录 prob 时 pop+append 新记录
                     * (fresh, 不传染) —— 与 ref 逐字一致 */
                    int j;
                    WKC_TOUCH_EXT();
                    if (j >= 0) {
                        s_nxt[j].pnb += (ch->pb + ch->pnb) * ps;
                        if (s_nxt[j].nn > 0) {
                            if (ps > s_rec[s_nxt[j].node[s_nxt[j].nn - 1]]
                                     .prob) {
                                wkc_nodes_pop_append(&s_nxt[j], (int16_t)s,
                                                     t, ps);
                            }
                        } else {
                            wkc_nodes_assign_ext(&s_nxt[j], ch, (int16_t)s,
                                                 t, ps);
                        }
                    }
                }
            }
            #undef WKC_TOUCH_SAME
            #undef WKC_TOUCH_EXT
        }
    }

    if (s_broken) {
        /* 池耗尽(理论不可达: 活跃 ≤240, 池 1024): 丢弃本帧, 全复位自愈,
         * 避免 nn<len 的半截前缀污染命中扫描 */
        wkc_reset();
        s_frame = t + 1;
        return 0.0f;
    }

    wkc_sort_prune();
    return wkc_hit_scan();
}

/* 策略复位判据(喂端静音复位用, 板测13): 束内是否有"进行中的词"——某 hyp
 * 末尾 1..4 个 token 恰为某变体的前缀, 且末节点在新鲜窗(WKC_FRESH)内
 * (>2s 的陈旧残段不算, 避免历史垃圾永远挡住全复位)。有进展=喂端只复位
 * nn(保束桥接词内停顿); 无进展=三件套全复位。 */
int wkc_has_variant_progress(void)
{
    for (int h = 0; h < s_ncur; h++) {
        const wkc_hyp_t *hp = &s_cur[h];
        if (hp->len <= 0 || hp->nn != hp->len) continue;
        if ((int32_t)(s_frame - 1 - s_rec[hp->node[hp->len - 1]].frame) >
            WKC_FRESH) {
            continue;
        }
        for (int v = 0; v < WK_NVAR; v++) {
            for (int l = 1; l <= WK_VLEN && l <= hp->len; l++) {
                if (memcmp(hp->tok + hp->len - l, WK_VARIANTS[v],
                           (size_t)l * sizeof(int16_t)) == 0) {
                    return 1;
                }
            }
        }
    }
    return 0;
}
