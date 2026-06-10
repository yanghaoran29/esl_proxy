# Qwen3_14b_tensormap

## Conf
spmd = 4

## Performance
```
make CASE=qwen3_dynamic_manual_scope.h run
make CASE=qwen3_dynamic_tensormap.h run
```
### Orchestrator单核性能
| Item |Base|Assign|Tensor|
|:----:|:----:|:----:|:----:|
|tasks| 864 | 
|subtasks| 3096|
|spmd| 3.58 |
|platform| m5 |
|duration/us| 90| 187 |407 | 
|task throughput MTasks/s| 9.60 | 4.62 |2.12 | 
|subtask throughput MTasks/s| 34.40 | 16.5 |7.6 | 
### Scheduler单Cluster性能


### Cost

## Debug
```
make run WORKER_LOG=1
```

## Apple M5
10 (4 Super and 6 Efficiency)，无SMT，采用多级动态频率调节（DVFS）机制

### 超大核 (Super Core) 
最高主频： 4.61 GHz
闲时频率：1.3 GHz
L1 Cache：192 KB 指令缓存 (L1i) + 128 KB 数据缓存 (L1d)
L2 Cache：4 核共享 12～16 MB，3M/C
L3 / SLC：24 MB 的系统级缓存（System Level Cache）, 所有 CPU 、GPU 以及 Neural Engine 共享，6M/C

### 高能效核 (E-Core) 
最高主频： 3.05 GHz
闲时频率：972 MHz
L1 Cache：128 KB 指令缓存 (L1i) + 64 KB 数据缓存 (L1d)
L2 Cache：6核共享 6～8 MB，1M/C