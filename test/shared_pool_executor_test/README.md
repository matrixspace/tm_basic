# SharedPool 与执行器契约回归

SharedPool现位于 `tm_kit/basic/RealTimeSharedPoolExecutor.hpp`，类型为 `basic::RealTimeSharedPoolExecutor`、`basic::SharedThreadPoolConfig`、`basic::SharedThreadPool` 和 `basic::RealTimeSharedPoolExecutorComponent`。Environment继承该component以选择共享池。先configure再构建节点。infra仅提供InlineExecutor和DedicatedThreadExecutor。

从infra迁来的handler和机制测试覆盖默认执行器、共享池的多生产者FIFO、串行回调、idle唤醒、stop、图节点、异常、析构生命周期及Inline提交。专用原始MPSC队列仅用于basic共享池，位于basic::shared_pool_queue_detail。

```sh
meson test -C builddir_release --no-rebuild threaded_handler_test executor_mechanism_test
```

迁移说明见 [执行器实现分层](../../../tm_infra/doc/20261105-realtime-threaded-executor/executor-layering.md)。
