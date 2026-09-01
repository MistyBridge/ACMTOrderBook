// acmt_orderbook.h — A股 L2 订单簿重建引擎 C API (Linux 共享库)
//
// 单位约定 (本接口 = 交易所快照原始精度, A股股票):
//   深交所 (exchange=2): 价格 ×10^4 (91600 = 9.16 元), 数量 ×10^2 (10000 = 100 股),
//                       成交额 ×10^4 元, 时间 YYYYMMDDHHMMSSsss (北京时间)
//   上交所 (exchange=1): 价格 ×10^3, 数量 ×10^3 (股), 成交额 ×10^5 元
//
// 注: 引擎内部价格统一为 ×10^6 (逐笔 ×10^4/×10^3 升档、快照 ×10^6, 全 int64),
//     与本接口的原始精度是两个域;
//     喂入/输出的换算由 .so 内部完成, 调用方只需遵循上表的原始精度。
//
// 支持: 深交所 (exchange=2) / 上交所 (exchange=1)。
#ifndef ACMT_ORDERBOOK_H_INCLUDED
#define ACMT_ORDERBOOK_H_INCLUDED

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 导出宏 (编译 .so 时配合 -fvisibility=hidden 只暴露 C API)
#if defined(_WIN32)
    #define ACMT_API __declspec(dllexport)
#else
    #define ACMT_API __attribute__((visibility("default")))
#endif

#define ACMT_DEPTH (10)

typedef struct {
    uint64_t transact_time;          // YYYYMMDDHHMMSSsss
    int64_t  num_trades;             // 累计成交笔数
    int64_t  total_volume_trade;     // 累计成交量 (×10^2)
    int64_t  total_value_trade;      // 累计成交额 (×10^4 元)
    int32_t  last_px;                // 最新价/虚拟撮合价 (×10^4)
    int32_t  open_px, high_px, low_px;
    int32_t  prev_close_px;
    int32_t  upper_limit_px, lower_limit_px;
    int64_t  total_bid_vol, total_ask_vol;
    int32_t  bid_price[ACMT_DEPTH];  // 买盘 10 档 (×10^4)
    int64_t  bid_volume[ACMT_DEPTH]; // (×10^2)
    int32_t  ask_price[ACMT_DEPTH];
    int64_t  ask_volume[ACMT_DEPTH];
} acmt_snap_t;

typedef struct {
    int64_t  num_trades;
    int64_t  total_volume_trade;
    int64_t  total_value_trade;
    int32_t  last_px, open_px, high_px, low_px;
    int64_t  event_count;            // 已处理消息数
    int64_t  order_count;            // 已处理委托数
    int64_t  trade_count;            // 已处理成交数
} acmt_ob_stat_t;

// 快照校验统计 (与市场快照按 num_trades 对齐的等待式匹配)
// 口径: 连续竞价比 20 档+统计; 开盘集合竞价比 genCallSnap 全量; 收盘竞价仅比 last
typedef struct {
    int64_t  trading_total;          // 连续竞价校验快照数
    int64_t  trading_full_exact;     // 20 档+统计全部一致
    int64_t  trading_stats_only;     // 统计一致但档位有差
    int64_t  trading_mismatch;       // 统计不一致
    double   trading_avg_level_match; // 平均匹配档位 (0~20)
    int64_t  call_total;             // 集合竞价校验快照数
    int64_t  call_full_exact;        // 集合竞价全等
    int64_t  call_mismatch;
    // ---- 1s 聚合校验 ----
    int64_t  bar_seconds;            // 有成交的秒数 (秒边界核对次数)
    int64_t  bar_mismatch;           // 引擎累计口径与输入口径不一致的秒数 (笔数/量/额/价任一)
    int64_t  bar_vol_mismatch;       // 其中成交量不一致的秒数
} acmt_ob_validation_t;

typedef void* acmt_ob_handle;

// 创建/销毁订单簿实例 (instrument: 6位代码; exchange: 2=深交所)
ACMT_API acmt_ob_handle acmt_ob_create(const char* instrument, int exchange);
ACMT_API void           acmt_ob_destroy(acmt_ob_handle h);

// ---- 逐条事件输入 ----
// side: '1'=买 '2'=卖; ord_type: '2'=限价 '1'=市价 'U'=本方最优
ACMT_API void acmt_ob_on_order(acmt_ob_handle h, uint64_t seq,
                      int64_t price, int64_t qty,
                      char side, char ord_type, uint64_t transact_time);

// exec_type: 'F'=成交 '4'=撤单; price 撤单时为 0; bid_seq/ask_seq 为委托序号 (无对应侧为 0)
ACMT_API void acmt_ob_on_exec(acmt_ob_handle h, uint64_t seq,
                     uint64_t bid_seq, uint64_t ask_seq,
                     int64_t price, int64_t qty, char exec_type,
                     uint64_t transact_time);

// 市场快照 (用于常量初始化与校验; 可只喂 8:00~9:15 的首批快照)
ACMT_API void acmt_ob_on_snap(acmt_ob_handle h, const acmt_snap_t* snap);

// ---- 批量回放: 直连 ClickHouse 拉取一个交易日并回放 (无本地文件) ----
// date: yyyymmdd; instrument: 6位代码; exchange: 2=深交所 1=上交所
// 返回处理的消息数; 失败返回 -1
ACMT_API int64_t acmt_ob_replay_ch(acmt_ob_handle h,
                                   const char* host, int port,
                                   const char* user, const char* password,
                                   const char* date, const char* instrument,
                                   int exchange);

// ---- 纯回放基准: 同上, 但快照仅喂入引擎(初始化常量)不做校验比对 ----
// 用于测量引擎本身吞吐 (不含快照校验开销)
ACMT_API int64_t acmt_ob_replay_ch_bench(acmt_ob_handle h,
                                         const char* host, int port,
                                         const char* user, const char* password,
                                         const char* date, const char* instrument,
                                         int exchange);

// ---- 快照兜底开关 (生产安全网, 默认关闭; 校验回放勿开) ----
// 启用后: 竞价时段强制/逐笔流滞后/连续竞价交叉簿时, 输出路由切至市场快照镜像。
ACMT_API void acmt_ob_set_fallback(acmt_ob_handle h, int enable);

// ---- 生产健壮性开关 (长跑稳定性, 校验回放默认关闭) ----
// orderMap 懒清理: 全成交订单延迟删除 (60 秒安全窗口), 控制长跑内存增长
ACMT_API void acmt_ob_set_order_cleanup(acmt_ob_handle h, int enable);
// 跨日脏数据过滤: 过滤前一交易日残留消息
ACMT_API void acmt_ob_set_stale_filter(acmt_ob_handle h, int enable);

// ---- 全档位簿输出 ----
typedef struct {
    int32_t price;   // 快照原始精度 (深 ×10^4 / 沪 ×10^3), 与 acmt_snap_t 一致
    int64_t qty;     // 快照原始精度 (深 ×10^2 / 沪 ×10^3)
} acmt_level_t;
// 返回实际档数; asks 为升序 (卖一在最前), bids 为降序 (买一在最前)
ACMT_API int64_t acmt_ob_get_levels(acmt_ob_handle h,
                                    acmt_level_t* asks, acmt_level_t* bids,
                                    int max_levels);

// ---- 健康计数 (可恢复错误路径的监控口径) ----
typedef struct {
    uint64_t order_not_found;   // 成交/撤单回链失败的订单号
    uint64_t neg_level_clear;   // 负量档清除 (削减越界保护)
    uint64_t snap_route_adopt;  // 快照兜底切换次数
    uint64_t cleanup_erased;    // orderMap 清理删除的订单数
    uint64_t stale_filtered;    // 跨日脏数据过滤条数
} acmt_ob_health_t;
ACMT_API void acmt_ob_get_health(acmt_ob_handle h, acmt_ob_health_t* out);

// ---- 丢单模拟回放 (兜底验证用) ----
// fallback: 启用快照兜底; skipSod: 丢弃逐笔的起始秒 (HHMMSS, 0=不丢);
// skipSec: 丢弃时长(秒)。快照照喂, 用于模拟生产断流。
ACMT_API int64_t acmt_ob_replay_ch_sim(acmt_ob_handle h,
                                       const char* host, int port,
                                       const char* user, const char* password,
                                       const char* date, const char* instrument,
                                       int exchange,
                                       int fallback, int skipSod, int skipSec);

// ---- 输出 ----
// 重建订单簿快照 (连续竞价 10 档; 集合竞价阶段为虚拟撮合视图)
ACMT_API void acmt_ob_get_book(acmt_ob_handle h, acmt_snap_t* out);
// 累计统计
ACMT_API void acmt_ob_get_stat(acmt_ob_handle h, acmt_ob_stat_t* out);
// 快照校验统计 (喂入市场快照后实时累积)
ACMT_API void acmt_ob_get_validation(acmt_ob_handle h, acmt_ob_validation_t* out);

#ifdef __cplusplus
}
#endif

#endif // ACMT_ORDERBOOK_H_INCLUDED
