"""LSWTCS: glthread hung at start-up (black screen, Eden 2026-10-09). _mesa_glthread_init queues
glthread_thread_initialization on the new worker, which calls ctx->Driver.SetBackgroundContext ->
st_set_background_context -> smapi->set_background_context unconditionally (the NULL check is only an
assert, compiled out by -Db_ndebug=true). switch-mesa's EGL driver allocates its st_manager with
CALLOC_STRUCT and sets only get_param, so the worker called address 0 and died; the start-up fence was
never signalled and the render thread waited forever. DRI's hook only tells its window-system loader
about the worker thread; the switch driver keeps no per-thread EGL state, so a no-op hook is enough.
Second fix: eglSwapBuffers / eglMakeCurrent now drain the glthread queue first (st thread_finish), as DRI does.
Applied after mesa_egl_share_edits.py. Usage: python mesa_glthread_edits.py <mesa-src-dir>"""
import sys, os
root = sys.argv[1]

def edit(rel, pairs):
    p = os.path.join(root, rel)
    s = open(p, newline='').read()
    for old, new in pairs:
        if new in s:
            continue   # already applied (new contains old here, so test new first)
        assert s.count(old) == 1, (rel, old[:70])
        s = s.replace(old, new)
    open(p, 'w', newline='').write(s)

edit('src/egl/drivers/switch/egl_switch.c', [
    ("static EGLBoolean\nswitch_initialize(_EGLDriver *drv, _EGLDisplay *dpy)\n",
     "// LSWTCS: called on the glthread worker when glthread starts (switch/mesa/mesa_glthread_edits.py).\n"
     "// Nothing to do: this driver keeps no per-thread EGL state.\n"
     "static void\n"
     "switch_st_set_background_context(struct st_context_iface *stctxi,\n"
     "                                 struct util_queue_monitoring *queue_info)\n"
     "{\n"
     "}\n"
     "\n"
     "static EGLBoolean\nswitch_initialize(_EGLDriver *drv, _EGLDisplay *dpy)\n"),
    ("    stmgr->get_param = switch_st_get_param;\n",
     "    stmgr->get_param = switch_st_get_param;\n"
     "    stmgr->set_background_context = switch_st_set_background_context;\n"),
    # Like DRI (dri_drawable.c dri_flush, dri_context.c dri_unbind_context): drain the glthread queue before
    # the window-system code touches the context. Otherwise eglSwapBuffers flushes the pipe context while the
    # worker is still drawing on it, and reads cur_slot before the worker's framebuffer validate has set it.
    ("    if (!_eglBindContext(ctx, dsurf, rsurf, &old_ctx, &old_dsurf, &old_rsurf))\n",
     "    _EGLContext *cur_ctx = _eglGetCurrentContext();\n"
     "    if (cur_ctx && switch_egl_context(cur_ctx)->stctx->thread_finish)\n"
     "        switch_egl_context(cur_ctx)->stctx->thread_finish(switch_egl_context(cur_ctx)->stctx);\n"
     "\n"
     "    if (!_eglBindContext(ctx, dsurf, rsurf, &old_ctx, &old_dsurf, &old_rsurf))\n"),
    ("    struct switch_egl_context* context = switch_egl_context(surface->base.CurrentContext);\n"
     "\n"
     "    if (surface->cur_slot < 0) {\n",
     "    struct switch_egl_context* context = switch_egl_context(surface->base.CurrentContext);\n"
     "\n"
     "    if (context && context->stctx->thread_finish)\n"
     "        context->stctx->thread_finish(context->stctx);\n"
     "\n"
     "    if (surface->cur_slot < 0) {\n"),
])
# Sync-point profiling: every GL call glthread cannot queue drains the worker via _mesa_glthread_finish_before.
# The game may set lsw_glthread_sync_hook (LSWTCS_GLSYNCLOG) to learn which calls make the render thread wait.
edit('src/mesa/main/glthread.c', [
    ("void\n_mesa_glthread_finish_before(struct gl_context *ctx, const char *func)\n{\n"
     "   _mesa_glthread_finish(ctx);\n",
     "/* LSWTCS (switch/mesa/mesa_glthread_edits.py): optional sync-point profiler, set by the game. */\n"
     "void (*lsw_glthread_sync_hook)(const char *func, uint64_t wait_ns) = NULL;\n"
     "\n"
     "void\n_mesa_glthread_finish_before(struct gl_context *ctx, const char *func)\n{\n"
     "   if (lsw_glthread_sync_hook) {\n"
     "      int64_t t0 = os_time_get_nano();\n"
     "      _mesa_glthread_finish(ctx);\n"
     "      lsw_glthread_sync_hook(func, (uint64_t)(os_time_get_nano() - t0));\n"
     "      return;\n"
     "   }\n"
     "   _mesa_glthread_finish(ctx);\n"),
    ('#include "util/u_thread.h"\n',
     '#include "util/u_thread.h"\n#include "util/os_time.h"\n'),
])
# Frame-end pipelining. glFenceSync / glClientWaitSync / glMapBufferRange / eglSwapBuffers all drain the worker, so
# with them on the render thread the render thread waits each frame until the worker has executed the whole frame.
# lsw_glthread_call(fn, arg) queues fn(arg) to run on the worker, in order after the GL commands recorded so far;
# the game puts its fences, arena upload and swap there. _mesa_glthread_finish also waits for the latest such job
# (it only knew about GL batches), so a later sync never runs commands on the render thread while a job is running.
edit('src/mesa/main/glthread.c', [
    ("void\n_mesa_glthread_finish(struct gl_context *ctx)\n{\n",
     "/* LSWTCS (switch/mesa/mesa_glthread_edits.py): run game callbacks on the glthread worker. */\n"
     "int lsw_glthread_call(void (*fn)(void *), void *arg);\n"
     "struct lsw_glthread_job {\n"
     "   struct gl_context *ctx;\n"
     "   void (*fn)(void *);\n"
     "   void *arg;\n"
     "   struct util_queue_fence fence;\n"
     "};\n"
     "#define LSW_GLTHREAD_JOBS 16\n"
     "static struct lsw_glthread_job lsw_jobs[LSW_GLTHREAD_JOBS];\n"
     "static unsigned lsw_job_next;\n"
     "static bool lsw_jobs_ready;\n"
     "static struct util_queue_fence *lsw_last_job_fence;\n"
     "\n"
     "static void\n"
     "lsw_glthread_job_exec(void *data, int thread_index)\n"
     "{\n"
     "   struct lsw_glthread_job *job = (struct lsw_glthread_job *)data;\n"
     "   /* Direct (non-marshalling) dispatch, as glthread_unmarshal_batch sets it. */\n"
     "   _glapi_set_dispatch(job->ctx->CurrentServerDispatch);\n"
     "   job->fn(job->arg);\n"
     "}\n"
     "\n"
     "/* Returns 1 if fn was queued on the worker, 0 if glthread is off and fn already ran on this thread. */\n"
     "int\n"
     "lsw_glthread_call(void (*fn)(void *), void *arg)\n"
     "{\n"
     "   GET_CURRENT_CONTEXT(ctx);\n"
     "   if (!ctx || !ctx->GLThread.enabled) {\n"
     "      fn(arg);\n"
     "      return 0;\n"
     "   }\n"
     "   if (!lsw_jobs_ready) {\n"
     "      for (unsigned i = 0; i < LSW_GLTHREAD_JOBS; i++)\n"
     "         util_queue_fence_init(&lsw_jobs[i].fence);\n"
     "      lsw_jobs_ready = true;\n"
     "   }\n"
     "   _mesa_glthread_flush_batch(ctx);\n"
     "   struct lsw_glthread_job *job = &lsw_jobs[lsw_job_next];\n"
     "   lsw_job_next = (lsw_job_next + 1) % LSW_GLTHREAD_JOBS;\n"
     "   util_queue_fence_wait(&job->fence);   /* queued 16 calls ago: long done */\n"
     "   job->ctx = ctx;\n"
     "   job->fn = fn;\n"
     "   job->arg = arg;\n"
     "   util_queue_add_job(&ctx->GLThread.queue, job, &job->fence, lsw_glthread_job_exec, NULL, 0);\n"
     "   lsw_last_job_fence = &job->fence;\n"
     "   return 1;\n"
     "}\n"
     "\n"
     "void\n_mesa_glthread_finish(struct gl_context *ctx)\n{\n"),
    ("   if (!util_queue_fence_is_signalled(&last->fence)) {\n"
     "      util_queue_fence_wait(&last->fence);\n"
     "      synced = true;\n"
     "   }\n",
     "   if (!util_queue_fence_is_signalled(&last->fence)) {\n"
     "      util_queue_fence_wait(&last->fence);\n"
     "      synced = true;\n"
     "   }\n"
     "   if (lsw_last_job_fence && !util_queue_fence_is_signalled(lsw_last_job_fence)) {\n"
     "      util_queue_fence_wait(lsw_last_job_fence);   /* LSWTCS: game callbacks queued after the last batch */\n"
     "      synced = true;\n"
     "   }\n"),
    ('#include "util/os_time.h"\n',
     '#include "util/os_time.h"\n#include "main/context.h"\n'),
])
# eglSwapBuffers requires the surface's context to be current on the calling thread, which the worker never is
# (the render thread keeps it). The worker calls the driver's swap directly through this export; the driver's
# thread_finish is a no-op there (_mesa_glthread_finish returns early on the worker itself).
edit('src/egl/drivers/switch/egl_switch.c', [
    ("\n/*\n * Called from eglGetProcAddress() via drv->API.GetProcAddress().\n */\n",
     "\n// LSWTCS: swap from the glthread worker (switch/mesa/mesa_glthread_edits.py), skipping eglSwapBuffers'\n"
     "// current-context check. Same display lock as eglSwapBuffers.\n"
     "EGLBoolean lsw_egl_swap_buffers_worker(EGLDisplay dpy, EGLSurface surface);\n"
     "EGLBoolean\n"
     "lsw_egl_swap_buffers_worker(EGLDisplay dpy, EGLSurface surface)\n"
     "{\n"
     "    _EGLDisplay *disp = _eglLookupDisplay(dpy);\n"
     "    if (!disp)\n"
     "        return EGL_FALSE;\n"
     "    mtx_lock(&disp->Mutex);\n"
     "    _EGLSurface *surf = _eglLookupSurface(surface, disp);\n"
     "    EGLBoolean ret = surf && surf->Type == EGL_WINDOW_BIT ? switch_swap_buffers(NULL, disp, surf) : EGL_FALSE;\n"
     "    mtx_unlock(&disp->Mutex);\n"
     "    return ret;\n"
     "}\n"
     "\n/*\n * Called from eglGetProcAddress() via drv->API.GetProcAddress().\n */\n"),
])
# nouveau_fence_wait polls the fence in a loop and only yields on PIPE_OS_UNIX, which the Switch is not: a pure spin
# for as long as the GPU is behind (~25 ms per heavy frame). With LSWTCS_GLPIPE that wait runs on the glthread worker
# on core 2, next to the audio output thread, and audio became extremely choppy on hardware (2026-10-09). After a few
# quick polls, sleep 50 us per poll instead: the core is free for other threads, and the wake-up delay is negligible
# against a multi-millisecond wait. (NOUVEAU_FENCE_MAX_SPINS = 2^31 polls, so the timeout only gets longer.)
edit('src/gallium/drivers/nouveau/nouveau_fence.c', [
    ("#ifdef PIPE_OS_UNIX\n      if (!(spins % 8)) /* donate a few cycles */\n         sched_yield();\n#endif\n",
     "#if defined(__SWITCH__)\n"
     "      /* LSWTCS (switch/mesa/mesa_glthread_edits.py): sleep, don't spin. */\n"
     "      if (spins > 16) {\n"
     "         struct timespec ts = {0, 50000};\n"
     "         nanosleep(&ts, NULL);\n"
     "      }\n"
     "#elif defined(PIPE_OS_UNIX)\n      if (!(spins % 8)) /* donate a few cycles */\n         sched_yield();\n#endif\n"),
    ('#include "util/os_time.h"\n\n#ifdef PIPE_OS_UNIX\n#include <sched.h>\n#endif\n',
     '#include "util/os_time.h"\n#include <time.h>\n\n#ifdef PIPE_OS_UNIX\n#include <sched.h>\n#endif\n'),
])
print('glthread edits applied')
