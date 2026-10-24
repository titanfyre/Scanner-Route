#pragma once
#include <Windows.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ScannerDevice { unsigned id=0; std::string token; std::wstring name; };
struct ScannerSnapshot {
    std::vector<ScannerDevice> devices;
    std::deque<std::string> scans;
    std::wstring message=L"Ready. Detect devices, then choose your scanner.";
    std::uint64_t accepted=0,rejected=0,queue_drops=0,write_errors=0;
    bool discovering=false,routing=false;
};
class ScannerSession {
public:
    ScannerSession()=default;
    ~ScannerSession();
    void Discover();
    void Route(unsigned device,std::string token,std::string prefix,bool suppress,const std::wstring& csv);
    void Stop();
    ScannerSnapshot Snapshot();
private:
    void Run();
    HANDLE stop_=nullptr;
    std::thread worker_;
    std::mutex mutex_;
    ScannerSnapshot snapshot_;
    unsigned device_=0;
    std::string device_token_;
    bool suppress_=false;
    std::string prefix_;
    std::wstring csv_;
    std::uint64_t revision_=0;
};
