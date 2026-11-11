#pragma once
#include <QThreadPool>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <future>
#include <limits>
#include <memory>
#include <vector>

// Reuse worker threads across frames. Each job owns its painter/image; shared
// scene snapshots and source images are read-only until all jobs have joined.
inline QThreadPool &renderPool() {
  static QThreadPool pool;
  static const bool configured = [] {
    pool.setMaxThreadCount(std::max(1, QThread::idealThreadCount()));
    pool.setExpiryTimeout(30000);
    return true;
  }();
  Q_UNUSED(configured);
  return pool;
}
inline thread_local bool inRenderWorker = false;
// Preview frames and frames prepared ahead share one pool. Background work is
// deliberately lower priority and may cap how many workers it occupies, so a
// cache fill can run beside playback without queuing the frame on screen
// behind all of its own look-ahead work.
inline thread_local int renderTaskPriority = 0;
inline thread_local int renderWorkerCeiling = std::numeric_limits<int>::max();
class RenderSchedulingScope {
  int previousPriority, previousCeiling;
public:
  RenderSchedulingScope(int priority, int workers)
      : previousPriority(renderTaskPriority), previousCeiling(renderWorkerCeiling) {
    renderTaskPriority = priority;
    renderWorkerCeiling = std::max(1, workers);
  }
  ~RenderSchedulingScope() {
    renderTaskPriority = previousPriority;
    renderWorkerCeiling = previousCeiling;
  }
};
inline int renderWorkerLimit() {
  return std::max(1, std::min(renderPool().maxThreadCount(), renderWorkerCeiling));
}
// What expressions evaluated on this thread look layers, the frame rate and the
// composition up in, and the layer being drawn. A render hands both on to the
// jobs it starts, so a property reads the same on every worker.
struct Scene;
struct Layer;
inline thread_local const Scene *expressionScene = nullptr;
inline thread_local const Layer *expressionLayer = nullptr;
// What one frame being drawn works out once for every worker drawing it -- where
// moving letters land, asked by every band and every sample (engine.cpp). A
// render hands it on to the jobs it starts, like the expression scene.
struct RenderFrameContext;
inline thread_local std::shared_ptr<RenderFrameContext> renderFrame;
template<class F> auto renderJob(F function) {
  using Result = decltype(function());
  auto task = std::make_shared<std::packaged_task<Result()>>(std::move(function));
  auto future = task->get_future();
  const int priority=renderTaskPriority;
  const Scene *scene=expressionScene;
  const Layer *layer=expressionLayer;
  std::shared_ptr<RenderFrameContext> frame=renderFrame;
  renderPool().start([task,scene,layer,frame] {
    bool old=inRenderWorker; inRenderWorker=true;
    const Scene *oldScene=expressionScene; const Layer *oldLayer=expressionLayer;
    expressionScene=scene; expressionLayer=layer;
    std::shared_ptr<RenderFrameContext> oldFrame=std::move(renderFrame);
    renderFrame=frame;
    (*task)();
    renderFrame=std::move(oldFrame);
    expressionScene=oldScene; expressionLayer=oldLayer;
    inRenderWorker=old;
  },priority);
  return future;
}

// Nested jobs execute inline to avoid exhausting the same worker pool.

template<class F> void renderParallel(int count, F function) {
  if(count<=0) return;
  int workers=inRenderWorker?1:std::min(renderWorkerLimit(),std::max(1,count/32));
  if(workers==1) {function(0,count);return;}
  std::vector<std::future<void>> jobs;
  for(int i=0;i<workers;++i) jobs.push_back(renderJob([=] {
    bool old=inRenderWorker;inRenderWorker=true;
    try {function(count*i/workers,count*(i+1)/workers);} catch(...) {inRenderWorker=old;throw;}
    inRenderWorker=old;
  }));
  std::exception_ptr error;
  for(auto &job:jobs) {try{job.get();}catch(...){error=std::current_exception();}}
  if(error) std::rethrow_exception(error);
}

// For items that are each a lot of work (a whole layer's effects), where
// renderParallel's thirty-two items per worker would leave most cores idle: one
// item is handed out at a time to whichever worker is free, so uneven layers
// still finish together. Nested calls run inline, like renderParallel.
template<class F> void renderParallelEach(int count, F function, int maxWorkers = std::numeric_limits<int>::max()) {
  if(count<=0) return;
  const int workers=inRenderWorker?1:std::max(1,std::min({renderWorkerLimit(),count,maxWorkers}));
  if(workers==1) {for(int i=0;i<count;++i) function(i);return;}
  auto next=std::make_shared<std::atomic<int>>(0);
  std::vector<std::future<void>> jobs;
  for(int i=0;i<workers;++i) jobs.push_back(renderJob([=] {
    bool old=inRenderWorker;inRenderWorker=true;
    try {for(int n;(n=next->fetch_add(1))<count;) function(n);} catch(...) {inRenderWorker=old;throw;}
    inRenderWorker=old;
  }));
  std::exception_ptr error;
  for(auto &job:jobs) {try{job.get();}catch(...){error=std::current_exception();}}
  if(error) std::rethrow_exception(error);
}
