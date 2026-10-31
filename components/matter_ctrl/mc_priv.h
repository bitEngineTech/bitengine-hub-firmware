/* matter_ctrl 内部：matter_ctrl.cpp（启动、状态）、mc_comm.cpp（节点表、配网、移除）、mc_iv.cpp（访谈）、mc_sub.cpp（订阅）之间的交接。不对外。 */
#pragma once
#include "matter_ctrl.h"

/* matter_ctrl.cpp 提供 */
bool mc_running(void);
const matter_ctrl_hooks_t *mc_hooks(void);

/* mc_comm.cpp 提供 */
esp_err_t mc_nodes_boot_load(void);       /* 启动任务里、esp_matter::start 之前：读节点表、丢掉上次配到一半的 */
void mc_comm_on_setup(void);              /* CHIP 任务里、setup_commissioner 之后：登记配网 / 移除回调 */
void mc_comm_status(uint8_t *nodes, bool *busy);

/* mc_iv.cpp 提供（访谈，M7 第四步） */
esp_err_t mc_iv_boot_load(const uint64_t *nodes, int n);   /* 启动任务里：读回这些节点的访谈结果，没访谈完的排上 */
void mc_iv_on_setup(void);                                 /* CHIP 任务里：起访谈的轮询计时器 */
void mc_iv_request(uint64_t node, bool added);             /* CHIP 任务里：排上一次访谈；added = 配网刚成功（做完推 EV_ADDED） */
void mc_iv_forget(uint64_t node);                          /* CHIP 任务里：移除之后清掉 */

/* mc_sub.cpp 提供（订阅，M7 第五步；全在 CHIP 任务里） */
struct mc_iv_s;
void mc_sub_on_setup(void);                                /* 起订阅的轮询计时器（照表立即开订，同一时刻最多 2 条在建） */
void mc_sub_want(uint64_t node, const struct mc_iv_s *iv); /* 访谈完了（含开机读回的）：按它的簇订；路径变了就拆了重建 */
void mc_sub_forget(uint64_t node);                         /* 移除之后：拆订阅、清读数 */
void mc_sub_status(uint8_t *subs, uint8_t *up);            /* 任何任务：该订的几台、订着的几台 */

/* mc_ctl.cpp 提供（控制，M7 第六步） */
void mc_ctl_init(void);                                    /* CHIP 任务里、setup 时：建锁与完成信号（RUNNING 之前） */
