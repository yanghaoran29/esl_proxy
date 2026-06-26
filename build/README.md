# esl_proxy 上板构建

编译入口（原 `cmake/onboard/`、`platform/cmake/` 已合并至此）：

```bash
source $ASCEND_HOME_PATH/bin/setenv.bash
bash build/build_aicpu.sh
bash build/build_aicore.sh
bash build/build_onboard_host.sh
```

CMake 源目录：`build/cmake/{aicpu,aicore,host}/`  
构建产物：`build/onboard/{aicpu,aicore,host}/`

上板运行：`bash tools/run_onboard.sh`（需经 `task-submit --device auto` 包装）
