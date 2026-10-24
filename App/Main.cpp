#include "Session.h"
#include <CommCtrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
constexpr COLORREF Background=RGB(17,21,30),Panel=RGB(28,34,47),Text=RGB(242,246,255),Blue=RGB(48,112,255);
enum {Discover=10,Start,Stop,Browse,Discord,Setup,Devices=30,Prefix,Suppress,CsvPath,Status,Stats,Log};
HBRUSH background=nullptr,panel=nullptr;
HFONT font=nullptr,titleFont=nullptr;
bool preview=false;
bool autoDiscover=false;
struct App { HWND window=nullptr; ScannerSession session; std::vector<ScannerDevice> devices; std::wstring csv; unsigned dpi=96; };
int Scale(App& a,int value) { return MulDiv(value,static_cast<int>(a.dpi),96); }
HWND Control(App& a,int id,const wchar_t* type,const wchar_t* text,DWORD style=0) {
    const auto h=CreateWindowExW(type==std::wstring(L"EDIT")?WS_EX_CLIENTEDGE:0,type,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,a.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE); return h;
}
void SetTextIfChanged(HWND control,const std::wstring& text) {
    const int length=GetWindowTextLengthW(control);
    std::wstring current(static_cast<std::size_t>(length)+1,0);
    GetWindowTextW(control,current.data(),length+1); current.resize(static_cast<std::size_t>(length));
    if(current!=text) SetWindowTextW(control,text.c_str());
}
void Message(App& a,const std::wstring& text) { SetTextIfChanged(GetDlgItem(a.window,Status),text); }
void Place(App& a,int id,int x,int y,int w,int h) { MoveWindow(GetDlgItem(a.window,id),Scale(a,x),Scale(a,y),Scale(a,w),Scale(a,h),TRUE); }
void Layout(App& a) {
    RECT r{};GetClientRect(a.window,&r);const int width=MulDiv(r.right,96,static_cast<int>(a.dpi));
    const int height=MulDiv(r.bottom,96,static_cast<int>(a.dpi));
    const int logHeight=std::max(60,height-560);
    Place(a,Discover,28,112,156,38);Place(a,Start,194,112,148,38);Place(a,Stop,352,112,100,38);
    Place(a,Discord,width-342,112,174,38);Place(a,Setup,width-158,112,130,38);
    Place(a,101,28,178,250,24);Place(a,Devices,28,208,width-56,260);
    Place(a,102,28,254,200,24);Place(a,Prefix,28,284,260,32);
    Place(a,Suppress,310,284,width-338,32);
    Place(a,103,28,336,width-56,24);Place(a,CsvPath,28,366,width-190,32);Place(a,Browse,width-150,364,122,36);
    Place(a,Stats,28,424,width-56,30);Place(a,Log,28,466,width-56,logHeight);
    Place(a,Status,28,480+logHeight,width-56,58);
}
std::wstring Read(HWND h) { const int n=GetWindowTextLengthW(h);std::wstring value(static_cast<std::size_t>(n)+1,0);GetWindowTextW(h,value.data(),n+1);value.resize(static_cast<std::size_t>(n));return value; }
std::string Ascii(const std::wstring& text) { std::string result; for(auto c:text) { if(c<32||c>126)throw std::runtime_error("Prefix must contain printable ASCII characters."); result+=static_cast<char>(c); } return result; }
void CopyDiscord(App& a) {
    if(!OpenClipboard(a.window)) return;
    EmptyClipboard();const wchar_t handle[]=L"titancsa";
    const auto memory=GlobalAlloc(GMEM_MOVEABLE,sizeof(handle));
    if(memory) {if(auto p=GlobalLock(memory)) {memcpy(p,handle,sizeof(handle));GlobalUnlock(memory);if(!SetClipboardData(CF_UNICODETEXT,memory))GlobalFree(memory);}else GlobalFree(memory);}
    CloseClipboard();Message(a,L"Discord handle copied: titancsa. Discord > Friends > Add Friend; mention Scanner-Route.");
}
void Refresh(App& a) {
    if(preview) return;
    const auto snapshot=a.session.Snapshot();
    bool changed=snapshot.devices.size()!=a.devices.size();
    if(!changed) for(std::size_t i=0;i<a.devices.size();++i) if(a.devices[i].token!=snapshot.devices[i].token){changed=true;break;}
    if(changed) {
        const auto combo=GetDlgItem(a.window,Devices);const auto selected=SendMessageW(combo,CB_GETCURSEL,0,0);
        const auto token=(selected>=0 && static_cast<std::size_t>(selected)<a.devices.size())?a.devices[static_cast<std::size_t>(selected)].token:std::string{};
        SendMessageW(combo,CB_RESETCONTENT,0,0);a.devices=snapshot.devices;
        for(const auto& d:a.devices)SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(d.name.c_str()));
        for(std::size_t i=0;i<a.devices.size();++i)if(!token.empty()&&a.devices[i].token==token)SendMessageW(combo,CB_SETCURSEL,i,0);
    }
    SetTextIfChanged(GetDlgItem(a.window,Stats),std::to_wstring(snapshot.accepted)+L" scans accepted     "+std::to_wstring(snapshot.rejected)+L" rejected     "+std::to_wstring(snapshot.queue_drops)+L" queue drops     "+std::to_wstring(snapshot.write_errors)+L" write errors");
    std::wstring text;
    for(const auto& scan:snapshot.scans)text+=std::wstring(scan.begin(),scan.end())+L"\r\n";
    SetTextIfChanged(GetDlgItem(a.window,Log),text);Message(a,snapshot.message);
    EnableWindow(GetDlgItem(a.window,Devices),!snapshot.routing);
    EnableWindow(GetDlgItem(a.window,Start),snapshot.discovering&&!snapshot.routing);
    EnableWindow(GetDlgItem(a.window,Stop),snapshot.discovering);
    for(const auto id:{Prefix,Suppress,Browse})EnableWindow(GetDlgItem(a.window,id),!snapshot.routing);
}
bool Elevate(App& a) {
    SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;PSID administrators=nullptr;BOOL elevated=FALSE;
    if(AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&administrators)) { CheckTokenMembership(nullptr,administrators,&elevated);FreeSid(administrators); }
    if(elevated)return false;
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
    SHELLEXECUTEINFOW request{sizeof(request)};request.hwnd=a.window;request.lpVerb=L"runas";request.lpFile=path;request.lpParameters=L"--discover";request.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&request))throw std::runtime_error("Administrator approval is required for live capture. No input was captured.");
    DestroyWindow(a.window);return true;
}
LRESULT CALLBACK Proc(HWND w,UINT m,WPARAM p,LPARAM l) {
    auto a=reinterpret_cast<App*>(GetWindowLongPtrW(w,GWLP_USERDATA));
    if(m==WM_NCCREATE){a=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);a->window=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
    if(!a)return DefWindowProcW(w,m,p,l);
    switch(m){
    case WM_CREATE:{
        a->dpi=GetDpiForWindow(w);BOOL dark=TRUE;DwmSetWindowAttribute(w,20,&dark,sizeof(dark));
        font=CreateFontW(-Scale(*a,15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        titleFont=CreateFontW(-Scale(*a,31),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        for(const auto& pair: {std::pair<int,const wchar_t*>{Discover,L"Detect devices"},{Start,L"Start routing"},{Stop,L"Stop"},{Browse,L"Choose CSV"},{Discord,L"Copy Discord handle"},{Setup,L"Driver setup"}})Control(*a,pair.first,L"BUTTON",pair.second,BS_OWNERDRAW|WS_TABSTOP);
        Control(*a,101,L"STATIC",L"SOURCE DEVICE");Control(*a,Devices,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP);
        Control(*a,102,L"STATIC",L"REQUIRED PREFIX (OPTIONAL)");Control(*a,Prefix,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP);SendDlgItemMessageW(w,Prefix,EM_SETLIMITTEXT,128,0);
        Control(*a,Suppress,L"BUTTON",L"Suppress typing from the selected scanner",BS_AUTOCHECKBOX|WS_TABSTOP);
        SetWindowTheme(GetDlgItem(w,Suppress),L"",L"");
        Control(*a,103,L"STATIC",L"CSV DESTINATION  /  leave empty to display scans only");Control(*a,CsvPath,L"EDIT",L"",ES_AUTOHSCROLL|ES_READONLY);
        Control(*a,Stats,L"STATIC",L"0 scans accepted     0 rejected     0 CSV queue drops");
        Control(*a,Log,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL|ES_AUTOVSCROLL);
        Control(*a,Status,L"STATIC",preview?L"OFFLINE PREVIEW. Sample data only; no driver is opened.":L"Ready. Detect devices, select only your scanner, then start routing.");
        if(preview){SendDlgItemMessageW(w,Devices,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Warehouse scanner  [preview]"));SendDlgItemMessageW(w,Devices,CB_SETCURSEL,0,0);SetWindowTextW(GetDlgItem(w,Log),L"SKU-2026-001\r\nSKU-2026-002\r\n");}
        else SetTimer(w,1,100,nullptr);
        RegisterHotKey(w,1,MOD_NOREPEAT,VK_F12);Layout(*a);if(autoDiscover&&!preview)PostMessageW(w,WM_COMMAND,Discover,0);return 0;}
    case WM_GETMINMAXINFO:{auto info=reinterpret_cast<MINMAXINFO*>(l);info->ptMinTrackSize={Scale(*a,880),Scale(*a,680)};return 0;}
    case WM_DPICHANGED:{a->dpi=HIWORD(p);const auto oldFont=font,oldTitle=titleFont;
        font=CreateFontW(-Scale(*a,15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        titleFont=CreateFontW(-Scale(*a,31),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        for(auto child=GetWindow(w,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        DeleteObject(oldFont);DeleteObject(oldTitle);const auto r=reinterpret_cast<RECT*>(l);SetWindowPos(w,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);Layout(*a);InvalidateRect(w,nullptr,FALSE);return 0;}
    case WM_SIZE:Layout(*a);return 0;
    case WM_HOTKEY:a->session.Stop();Message(*a,L"Stopped by F12. Physical passthrough restored.");return 0;
    case WM_TIMER:Refresh(*a);return 0;
    case WM_COMMAND:
        try {
            if(HIWORD(p)!=BN_CLICKED)break;
            switch(LOWORD(p)){
            case Discover:if(!preview&&!Elevate(*a))a->session.Discover();break;
            case Start:{if(preview)break;const auto selected=SendDlgItemMessageW(w,Devices,CB_GETCURSEL,0,0);if(selected<0||static_cast<std::size_t>(selected)>=a->devices.size())throw std::runtime_error("Select a USB HID scanner first.");const auto& source=a->devices[static_cast<std::size_t>(selected)];a->session.Route(source.id,source.token,Ascii(Read(GetDlgItem(w,Prefix))),SendDlgItemMessageW(w,Suppress,BM_GETCHECK,0,0)==BST_CHECKED,a->csv);break;}
            case Stop:a->session.Stop();Message(*a,L"Stopped. Passthrough restored; CSV output is flushed.");break;
            case Discord:CopyDiscord(*a);break;
            case Browse:{wchar_t path[32768]=L"scans.csv";OPENFILENAMEW file{sizeof(file)};file.hwndOwner=w;file.lpstrFilter=L"CSV files\0*.csv\0\0";file.lpstrFile=path;file.nMaxFile=32768;file.lpstrDefExt=L"csv";file.Flags=OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;if(GetSaveFileNameW(&file)){a->csv=path;SetWindowTextW(GetDlgItem(w,CsvPath),path);}break;}
            case Setup:{wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);const auto setup=std::filesystem::path(path).parent_path()/L"Dependencies"/L"InterceptorSetup.exe";if(!std::filesystem::exists(setup))throw std::runtime_error("Extract the complete release ZIP; Dependencies/InterceptorSetup.exe is missing.");ShellExecuteW(w,L"open",setup.c_str(),nullptr,setup.parent_path().c_str(),SW_SHOWNORMAL);break;}
            default:break;}
        }catch(const std::exception& e){const std::string s=e.what();Message(*a,std::wstring(s.begin(),s.end()));}
        return 0;
    case WM_SETCURSOR:if(GetClassLongPtrW(reinterpret_cast<HWND>(p),GCW_ATOM)==GetClassLongPtrW(GetDlgItem(w,Discover),GCW_ATOM)){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}break;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:SetTextColor(reinterpret_cast<HDC>(p),Text);SetBkColor(reinterpret_cast<HDC>(p),Background);return reinterpret_cast<LRESULT>(background);
    case WM_CTLCOLOREDIT:SetTextColor(reinterpret_cast<HDC>(p),Text);SetBkColor(reinterpret_cast<HDC>(p),Panel);return reinterpret_cast<LRESULT>(panel);
    case WM_DRAWITEM:{auto d=reinterpret_cast<DRAWITEMSTRUCT*>(l);const bool primary=d->CtlID==Start||d->CtlID==Discover;const auto brush=CreateSolidBrush(primary?Blue:Panel);FillRect(d->hDC,&d->rcItem,brush);DeleteObject(brush);SetBkMode(d->hDC,TRANSPARENT);SetTextColor(d->hDC,Text);SelectObject(d->hDC,font);wchar_t label[128]{};GetWindowTextW(d->hwndItem,label,128);DrawTextW(d->hDC,label,-1,&d->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);if(d->itemState&ODS_FOCUS)DrawFocusRect(d->hDC,&d->rcItem);return TRUE;}
    case WM_PAINT:{PAINTSTRUCT ps{};const auto dc=BeginPaint(w,&ps);RECT r{};GetClientRect(w,&r);FillRect(dc,&r,background);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,Text);SelectObject(dc,titleFont);RECT title{Scale(*a,28),Scale(*a,24),r.right,Scale(*a,68)};DrawTextW(dc,L"Scanner Route",-1,&title,DT_LEFT|DT_SINGLELINE);SelectObject(dc,font);RECT subtitle{Scale(*a,28),Scale(*a,72),r.right,Scale(*a,96)};DrawTextW(dc,L"Device-specific scan routing  /  USB HID keyboard scanners  /  Windows x64",-1,&subtitle,DT_LEFT|DT_SINGLELINE);EndPaint(w,&ps);return 0;}
    case WM_ERASEBKGND:return 1;
    case WM_DESTROY:KillTimer(w,1);UnregisterHotKey(w,1);a->session.Stop();PostQuitMessage(0);return 0;
    default:break;}
    return DefWindowProcW(w,m,p,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    bool selfTest=false;std::wstring screenshot;
    for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--preview")preview=true;else if(std::wstring(argv[i])==L"--discover")autoDiscover=true;else if(std::wstring(argv[i])==L"--ui-self-test"){preview=true;selfTest=true;}else if(std::wstring(argv[i])==L"--screenshot" && i+1<argc){preview=true;selfTest=true;screenshot=argv[++i];}else{LocalFree(argv);return 2;}}
    LocalFree(argv);background=CreateSolidBrush(Background);panel=CreateSolidBrush(Panel);
    WNDCLASSW type{};type.lpfnWndProc=Proc;type.hInstance=instance;type.lpszClassName=L"TitanScannerRoute";type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.hbrBackground=background;
    if(!RegisterClassW(&type))return 1;
    App app;app.dpi=GetDpiForSystem();RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    const auto window=CreateWindowW(type.lpszClassName,L"Scanner Route | Titanfyre",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,std::min(Scale(app,1020),static_cast<int>(work.right-work.left)),std::min(Scale(app,820),static_cast<int>(work.bottom-work.top)),nullptr,nullptr,instance,&app);
    if(!window)return 1;ShowWindow(window,show);UpdateWindow(window);
    if(selfTest){bool okay=GetDlgItem(window,Devices)&&GetDlgItem(window,Start)&&!app.session.Snapshot().discovering;
        if(!screenshot.empty()) {
            ShowWindow(window,SW_SHOWNOACTIVATE);RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);
            Gdiplus::GdiplusStartupInput init;ULONG_PTR token=0;
            if(Gdiplus::GdiplusStartup(&token,&init,nullptr)!=Gdiplus::Ok)okay=false;
            else {
                RECT r{};GetWindowRect(window,&r);const auto source=GetDC(window);const auto dc=CreateCompatibleDC(source);const auto bitmap=CreateCompatibleBitmap(source,r.right-r.left,r.bottom-r.top);const auto old=SelectObject(dc,bitmap);
                okay=okay && PrintWindow(window,dc,PW_RENDERFULLCONTENT);
                SelectObject(dc,old);
                {Gdiplus::Bitmap image(bitmap,nullptr);const CLSID png={0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};okay=okay && image.Save(screenshot.c_str(),&png,nullptr)==Gdiplus::Ok;}
                DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(window,source);Gdiplus::GdiplusShutdown(token);
            }
        }
        DestroyWindow(window);DeleteObject(font);DeleteObject(titleFont);DeleteObject(panel);DeleteObject(background);return okay?0:1;}
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    DeleteObject(font);DeleteObject(titleFont);DeleteObject(panel);DeleteObject(background);return static_cast<int>(message.wParam);
}
