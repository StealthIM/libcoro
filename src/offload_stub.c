/*
 * offload 空实现 (LIBCORO_HAVE_OFFLOAD=OFF 时编译, 替代 offload.c)。
 *
 * 用于没有线程的目标 (裸机 NO_SYS) 或显式关掉线程池的构建。loop 后端无条件
 * 调 offload_* (offload_pool_create/has_pending/drain/destroy) + 暴露
 * loop_get_offload/loop_run_in_thread; 这里给全 no-op, 使后端源码无需 #ifdef:
 *   - pool 恒 NULL, has_pending 恒 0, drain/destroy no-op。
 *   - loop_run_in_thread 返 NULL (调用方如 asyncweb resolve_async 需自备无线程
 *     回落, 如 lwIP native-async DNS dns_gethostbyname)。
 *
 * 关掉 offload 的构建不链 pal_thread, 无任何线程依赖。
 */

#include <stddef.h>
#include <libcoro/offload.h>
#include <internal/offload_internal.h>
#include <internal/loop_internal.h>

offload_pool_t *offload_pool_create(loop_t *loop) { (void)loop; return NULL; }
void            offload_pool_destroy(offload_pool_t *pool) { (void)pool; }
future_t       *loop_run_in_thread(void *(*fn)(void *arg), void *arg) { (void)fn; (void)arg; return NULL; }
void            offload_drain_completions(offload_pool_t *pool) { (void)pool; }
int             offload_has_pending(offload_pool_t *pool) { (void)pool; return 0; }
