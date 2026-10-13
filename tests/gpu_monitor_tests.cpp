#include "monitor/gpu_monitor.h"
#include <iostream>
#include <stdexcept>
#include <limits>

static void Check(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
int main() {
    try {
        wchar_t a[] = L"pid_1_luid_0x0_0xA_phys_0_eng_0_engtype_3D";
        wchar_t b[] = L"pid_2_luid_0x0_0xA_phys_0_eng_0_engtype_3D";
        wchar_t c[] = L"pid_1_luid_0x0_0xA_phys_0_eng_1_engtype_Copy";
        wchar_t d[] = L"pid_1_luid_0x0_0xB_phys_0_eng_0_engtype_3D";
        PDH_FMT_COUNTERVALUE_ITEM_W values[4]{};
        const wchar_t* names[] = {a,b,c,d};
        const double usage[] = {25,35,40,55};
        for (int i=0;i<4;++i) { values[i].szName=const_cast<wchar_t*>(names[i]); values[i].FmtValue.CStatus=PDH_CSTATUS_VALID_DATA; values[i].FmtValue.doubleValue=usage[i]; }
        Check(gpu_monitor::Usage(values,4)==60,"Process totals or independent engine/adapter grouping incorrect");
        values[1].FmtValue.doubleValue=95;
        Check(gpu_monitor::Usage(values,4)==100,"Usage not capped at 100 percent");
        values[0].FmtValue.CStatus=PDH_CSTATUS_NO_INSTANCE;
        values[1].FmtValue.doubleValue=std::numeric_limits<double>::quiet_NaN();
        values[2].FmtValue.doubleValue=-1;
        values[3].FmtValue.doubleValue=0;
        Check(gpu_monitor::Usage(values,4)==0,"Valid idle data confused with missing data");
        values[3].FmtValue.CStatus=PDH_CSTATUS_NO_INSTANCE;
        Check(gpu_monitor::Usage(values,4)==-1,"Invalid data incorrectly reported as idle");
        Check(gpu_monitor::Usage(nullptr,0)==-1,"Missing counters incorrectly reported as idle");
        gpu_monitor::Sampler sampler;
        sampler.Enable(true);
        for(int i=0;i<6 && sampler.Value()<0;++i) std::this_thread::sleep_for(std::chrono::seconds(1));
        const double actual=sampler.Value();
        Check(actual==-1 || (std::isfinite(actual)&&actual>=0&&actual<=100),"Live usage outside valid range");
        std::cout << "Live GPU percentage: " << actual << (actual<0?" (counter unavailable)":"") << "\n";
        sampler.Enable(false);
        Check(sampler.Value()==-1,"Disabled sample retained stale value");
        for(int i=0;i<100;++i) { sampler.Enable(true); sampler.Enable(false); }
        std::cout << "PASS GPU aggregation, invalid/idle data, live sample, enable/disable and cleanup\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<"\n";return 1; }
}
