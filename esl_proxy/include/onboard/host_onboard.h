/*
 * host_onboard.h — Host-side onboard bring-up (loader + launcher + AICore register mapping).
 *
 * 调用链路（主要步骤）：
 *   esl_onboard_run — 加载 SO/ELF、H2D GM、依次 launch AICPU init / AICore / AICPU exec。
 */
#ifndef ESL_PROXY_HOST_ONBOARD_H
#define ESL_PROXY_HOST_ONBOARD_H

int esl_onboard_run(int argc, char **argv);

#endif /* ESL_PROXY_HOST_ONBOARD_H */
