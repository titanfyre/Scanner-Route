#include "Session.h"
#include "Scanner.h"
#include "Interceptor.h"
#include <algorithm>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace {
std::wstring Wide(const std::string& s) { return std::wstring(s.begin(),s.end()); }
std::wstring DeviceName(const Interceptor::Device& d) {
    std::wstring text;
    for(auto c:d.name) { if(!c) break; text += static_cast<wchar_t>(c); }
    if(text.empty()) text=L"HID keyboard";
    return text+L"  ["+std::to_wstring(d.id)+L"]";
}
}
ScannerSession::~ScannerSession() { Stop(); }
void ScannerSession::Discover() {
    Stop();
    Interceptor::Get_Status();
    stop_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!stop_) throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"CreateEvent");
    { std::lock_guard<std::mutex> lock(mutex_); snapshot_={}; snapshot_.discovering=true; device_=0; ++revision_; }
    try { worker_=std::thread([this] { try { Run(); } catch(const std::exception& e) { std::lock_guard<std::mutex> lock(mutex_); snapshot_.message=Wide(e.what()); snapshot_.discovering=snapshot_.routing=false; } }); }
    catch(...) { CloseHandle(stop_); stop_=nullptr; throw; }
}
void ScannerSession::Route(unsigned device,std::string token,std::string prefix,bool suppress,const std::wstring& csv) {
    if(!worker_.joinable() || !stop_ || WaitForSingleObject(stop_,0)!=WAIT_TIMEOUT || !device || token.empty()) throw std::runtime_error("Detect devices and select a scanner first.");
    Scanner::Assembler validation({prefix,1,1024,300000});
    if(!csv.empty()) {
        const auto root=std::filesystem::path(csv).root_path().wstring();
        const auto type=GetDriveTypeW(root.c_str());
        if(root.empty() || (type!=DRIVE_FIXED && type!=DRIVE_REMOVABLE)) throw std::runtime_error("Choose a local disk CSV file; network shares are not supported in this version.");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if(!snapshot_.discovering || std::none_of(snapshot_.devices.begin(),snapshot_.devices.end(),[&](const auto& d){return d.id==device && d.token==token;})) throw std::runtime_error("The selected scanner is no longer in this capture. Detect devices again.");
    device_=device; device_token_=std::move(token); prefix_=std::move(prefix); suppress_=suppress; csv_=csv; ++revision_;
    snapshot_.routing=true; snapshot_.message=L"Routing the selected scanner. F12 or Stop ends the session.";
}
void ScannerSession::Stop() {
    if(stop_) SetEvent(stop_);
    if(worker_.joinable()) worker_.join();
    if(stop_) { CloseHandle(stop_); stop_=nullptr; }
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.discovering=snapshot_.routing=false;
    snapshot_.message=L"Stopped. Passthrough restored; check the CSV write-error counter before using exported scans.";
}
ScannerSnapshot ScannerSession::Snapshot() { std::lock_guard<std::mutex> lock(mutex_); return snapshot_; }
void ScannerSession::Run() {
    struct CsvWork { std::wstring path; std::string scan; };
    std::mutex output_mutex;
    std::condition_variable output_ready;
    std::deque<CsvWork> output;
    bool output_stopping=false;
    std::thread writer([&] {
      try {
        for(;;) {
            CsvWork work;
            { std::unique_lock<std::mutex> lock(output_mutex); output_ready.wait(lock,[&]{return output_stopping||!output.empty();});
              if(output.empty()) return; work=std::move(output.front()); output.pop_front(); }
            try {
                std::ofstream file(std::filesystem::path(work.path),std::ios::app|std::ios::binary);
                if(!file) throw std::runtime_error("CSV output could not be opened.");
                SYSTEMTIME utc{}; GetSystemTime(&utc);
                char timestamp[40]{};
                sprintf_s(timestamp,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,utc.wMilliseconds);
                file<<Scanner::CsvCell(timestamp)<<','<<Scanner::CsvCell(work.scan)<<"\r\n";
                file.flush(); if(!file) throw std::runtime_error("CSV output could not be saved.");
            } catch(const std::exception& e) { std::lock_guard<std::mutex> lock(mutex_); ++snapshot_.write_errors; snapshot_.message=Wide(e.what())+L" Recent scans remain visible in this window."; }
        }
      } catch(...) {
        SetEvent(stop_);
        try { std::lock_guard<std::mutex> lock(mutex_); snapshot_.message=L"CSV writer stopped after an allocation or queue error. Capture is stopping safely."; } catch(...) {}
      }
    });
    struct WriterShutdown {
        std::thread& thread; std::mutex& mutex; std::condition_variable& ready; bool& stopping;
        ~WriterShutdown() noexcept { { std::lock_guard<std::mutex> lock(mutex); stopping=true; } ready.notify_one(); if(thread.joinable())thread.join(); }
    } writerShutdown{writer,output_mutex,output_ready,output_stopping};
    try {
        Scanner::Assembler assembler;
        Scanner::UsKeyboard decoder;
        Scanner::KeyGate gate;
        std::uint64_t seen_revision=~std::uint64_t{0};
        unsigned selected=0;
        std::string selected_token;
        bool suppress=false;
        std::wstring csv;
        auto last_inventory=std::chrono::steady_clock::time_point::min();
        while(WaitForSingleObject(stop_,0)!=WAIT_OBJECT_0) {
            auto inputs=Interceptor::Get_Input_With_Timeout(50,stop_);
            const auto now=std::chrono::steady_clock::now();
            if(last_inventory==std::chrono::steady_clock::time_point::min() || now-last_inventory>std::chrono::seconds(1)) {
                const auto capture=Interceptor::Get_Context();
                std::vector<ScannerDevice> devices;
                for(const auto& d:capture.devices) if(d.type==IC_TYPE_KEYBOARD) devices.push_back({d.id,d.token,DeviceName(d)});
                { std::lock_guard<std::mutex> lock(mutex_); snapshot_.devices=std::move(devices);
                  if(!snapshot_.routing) snapshot_.message=L"Choose a USB HID scanner configured for US keyboard layout and Enter suffix.";
                  if(snapshot_.routing && std::none_of(snapshot_.devices.begin(),snapshot_.devices.end(),[&](const auto& d){return d.token==device_token_;})) {
                      snapshot_.message=L"The selected scanner disconnected or changed identity. Routing stopped; detect and select it again."; SetEvent(stop_);
                  } }
                last_inventory=now;
            }
            { std::lock_guard<std::mutex> lock(mutex_);
              if(seen_revision!=revision_) { assembler=Scanner::Assembler({prefix_,1,1024,300000}); decoder.Reset(); selected=device_; selected_token=device_token_; suppress=suppress_; csv=csv_; seen_revision=revision_; } }
            for(auto& event:inputs) if(auto* key=event.Get<Interceptor::Keyboard_Input>()) {
                if(key->stroke.make_code==0x58 && !(key->stroke.flags&Interceptor::KeyUp)) {
                    { std::lock_guard<std::mutex> lock(mutex_); snapshot_.message=L"Stopped by F12. Physical passthrough restored."; }
                    SetEvent(stop_);
                }
                const bool active=WaitForSingleObject(stop_,0)!=WAIT_OBJECT_0;
                const bool match=selected && event.device==selected && event.device_info && event.device_info->token==selected_token;
                if(event.device_info) event.blocked=gate.Block(event.device,event.device_info->token,key->stroke.make_code,key->stroke.flags,active && match && suppress);
                if(!active) continue;
                if(!selected || event.device!=selected) continue;
                if(!event.device_info || event.device_info->token!=selected_token) {
                    { std::lock_guard<std::mutex> lock(mutex_); snapshot_.message=L"Scanner identity changed. Routing stopped without capturing another keyboard."; }
                    SetEvent(stop_); continue;
                }
                if(const auto c=decoder.Feed(key->stroke.make_code,key->stroke.flags)) {
                    const auto us=std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
                    if(auto scan=assembler.Feed(*c,static_cast<std::uint64_t>(us))) {
                        { std::lock_guard<std::mutex> lock(mutex_); ++snapshot_.accepted; snapshot_.scans.push_front(*scan); if(snapshot_.scans.size()>32) snapshot_.scans.pop_back(); }
                        if(!csv.empty()) {
                            std::lock_guard<std::mutex> lock(output_mutex);
                            if(output.size()<256) { output.push_back({csv,*scan}); output_ready.notify_one(); }
                            else { std::lock_guard<std::mutex> stats(mutex_); ++snapshot_.queue_drops; snapshot_.message=L"CSV queue full: a scan was not written. Stop and check the output disk."; }
                        }
                    }
                }
            }
            std::size_t expected=0;
            for(const auto& event:inputs) if(!event.blocked) ++expected;
            const auto sent=Interceptor::Send_Input(inputs);
            if(!sent || sent.accepted!=expected) throw std::system_error(static_cast<int>(sent.error?sent.error:ERROR_WRITE_FAULT),std::system_category(),"Forward input");
            { std::lock_guard<std::mutex> lock(mutex_); snapshot_.rejected=assembler.Rejected(); }
        }
    } catch(const std::exception& e) { std::lock_guard<std::mutex> lock(mutex_); snapshot_.message=Wide(e.what())+L" Install the matching driver and run as administrator."; }
    try { Interceptor::Destroy_Context(); }
    catch(const std::exception& e) { std::lock_guard<std::mutex> lock(mutex_); snapshot_.message=L"Capture cleanup failed: "+Wide(e.what()); }
    catch(...) { std::lock_guard<std::mutex> lock(mutex_); snapshot_.message=L"Capture cleanup reported an unexpected error. Close the application to release its handles."; }
    { std::lock_guard<std::mutex> lock(mutex_); snapshot_.discovering=snapshot_.routing=false; }
}
