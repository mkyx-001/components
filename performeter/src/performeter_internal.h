/*
 * Performeter 组件内部接口（不对外暴露，仅供 src/ 内多个 .c 共享）。
 */
#ifndef PERFORMETER_INTERNAL_H
#define PERFORMETER_INTERNAL_H

#include "performeter.h"

/**
 * 将一次采样快照格式化输出到日志。
 * 由后台任务每个采样周期调用一次。
 */
void performeter_print(const performeter_snapshot_t *snap);

#endif /* PERFORMETER_INTERNAL_H */
