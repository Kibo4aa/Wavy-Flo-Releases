#include "performance_monitor.h"
#include <QTimer>
#include <future>
#include <chrono>
#include <algorithm>
#include <vector>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#endif

namespace {
struct Sampler {
#ifdef Q_OS_WIN
  PDH_HQUERY query = nullptr;
  PDH_HCOUNTER gpu = nullptr;
  quint64 previousCpu = 0, previousWall = 0;
  bool attempted = false, gpuPrimed = false;
  ~Sampler() { if (query) PdhCloseQuery(query); }
  static quint64 ticks(FILETIME value) {
    return (quint64(value.dwHighDateTime)<<32) | value.dwLowDateTime;
  }
#endif
  QString sample() {
#ifdef Q_OS_WIN
    QString cpuText="N/A", ramText="N/A", gpuText="N/A";
    FILETIME created, exited, kernel, user, now;
    GetSystemTimeAsFileTime(&now);
    if (GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) {
      const auto cpu = ticks(kernel)+ticks(user), wall=ticks(now);
      const auto cores=std::max<DWORD>(1,GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
      if(previousWall && wall>previousWall)
        cpuText=QString::number(std::clamp(100.*double(cpu-previousCpu)/double(wall-previousWall)/cores,0.,100.),'f',1)+"%";
      previousCpu=cpu; previousWall=wall;
    }
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb=sizeof(memory);
    MEMORYSTATUSEX system{}; system.dwLength=sizeof(system);
    if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)) &&
        GlobalMemoryStatusEx(&system) && system.ullTotalPhys) {
      ramText=QString("%1 GB (%2%)").arg(double(memory.WorkingSetSize)/(1024.*1024*1024),0,'f',2)
          .arg(100.*double(memory.WorkingSetSize)/system.ullTotalPhys,0,'f',1);
    }
    if(!attempted) {
      attempted=true;
      if(PdhOpenQueryW(nullptr,0,&query)==ERROR_SUCCESS)
        PdhAddEnglishCounterW(query,L"\\GPU Engine(*)\\Utilization Percentage",0,&gpu);
    }
    if(gpu && PdhCollectQueryData(query)==ERROR_SUCCESS) {
      if(gpuPrimed) {
        DWORD bytes=0,count=0;
        if(PdhGetFormattedCounterArrayW(gpu,PDH_FMT_DOUBLE,&bytes,&count,nullptr)==PDH_MORE_DATA && bytes < 32*1024*1024) {
          std::vector<unsigned char> storage(bytes);
          auto *items=reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
          if(PdhGetFormattedCounterArrayW(gpu,PDH_FMT_DOUBLE,&bytes,&count,items)==ERROR_SUCCESS) {
            const QString pid="pid_"+QString::number(GetCurrentProcessId())+"_";
            double busy=0; bool found=false;
            for(DWORD i=0;i<count;++i) {
              if(!QString::fromWCharArray(items[i].szName).startsWith(pid)) continue;
              if(items[i].FmtValue.CStatus!=PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus!=PDH_CSTATUS_NEW_DATA) continue;
              found=true; busy=std::max(busy,items[i].FmtValue.doubleValue);
            }
            if(found) gpuText=QString::number(std::clamp(busy,0.,100.),'f',1)+"%";
          }
        }
      }
      gpuPrimed=true;
    }
    return "CPU "+cpuText+"   RAM "+ramText+"   GPU "+gpuText;
#else
    return "CPU N/A   RAM N/A   GPU N/A";
#endif
  }
};
class Monitor : public QLabel {
  Sampler sampler;
  // Declared last: outstanding work completes before sampler destruction.
  std::future<QString> pending;
public:
  explicit Monitor(QWidget *parent) : QLabel(parent) {
    setObjectName("performanceMonitor");
    setText("CPU …   RAM …   GPU …");
    setToolTip("Wavy-Flo process usage. CPU: share of total logical CPU capacity. RAM: resident working set and percentage of physical RAM. GPU: busiest available process engine counter (including render/copy/decode); not total GPU or VRAM. N/A: counter unavailable. Sampled approximately once per second in a background worker.");
    auto *timer=new QTimer(this);
    connect(timer,&QTimer::timeout,this,[this] {
      if(pending.valid()) {
        if(pending.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return;
        try { setText(pending.get()); } catch(...) { setText("CPU N/A   RAM N/A   GPU N/A"); }
      }
      pending=std::async(std::launch::async,[this] { return sampler.sample(); });
    });
    timer->start(1000);
  }
};
}
QLabel *createPerformanceMonitor(QWidget *parent) { return new Monitor(parent); }
