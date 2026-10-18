"""Compile actual VPNManager methods with substituted Win32 APIs; no host networking.
Run: python3 windows/tests/vpn_manager_ownership_regression.py [repository root]
Requires a C++17 clang++ compiler. This does not replace Windows integration tests.
"""
from pathlib import Path
import os,subprocess,tempfile,sys
root=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parents[2]
source=(root/'windows/vpn_manager.cpp').read_text()
header=(root/'windows/vpn_manager.h').read_text()
def function(signature):
 start=source.index(signature);brace=source.index('{',start);depth=1;end=brace+1
 while depth:
  if source[end]=='{':depth+=1
  elif source[end]=='}':depth-=1
  end+=1
 return source[start:end]
preamble=r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
using DWORD=unsigned long;
using BOOL=int;
using LPSTR=char*;
constexpr DWORD NO_ERROR=0,STILL_ACTIVE=259,WAIT_OBJECT_0=0,WAIT_TIMEOUT=258;
constexpr int FALSE=0,CREATE_NO_WINDOW=1,STARTF_USESHOWWINDOW=1,SW_HIDE=0,MAX_PATH=260,IfOperStatusUp=1;
struct GUID {unsigned long long value=0;};
struct NET_LUID {unsigned long long Value=0;};
struct _MIB_IF_ROW2 {NET_LUID InterfaceLuid; GUID InterfaceGuid;int OperStatus=0;uint64_t InOctets=0,OutOctets=0,InUcastPkts=0,InNUcastPkts=0,OutUcastPkts=0,OutNUcastPkts=0;};
using MIB_IF_ROW2=_MIB_IF_ROW2;
struct FakeProcess {std::atomic<bool> active{true};bool tool=false;DWORD exitCode=0;};
using HANDLE=FakeProcess*;
struct PROCESS_INFORMATION {HANDLE hProcess=nullptr,hThread=nullptr;};
struct STARTUPINFOA {unsigned cb=0,dwFlags=0,wShowWindow=0;};
#define ZeroMemory(p,n) std::memset(p,0,n)
#define FAILED(n) ((n)<0)
std::mutex fakeMutex;
std::map<std::wstring,MIB_IF_ROW2> adapters;
std::vector<std::string> childCommands,toolCommands;
std::vector<std::unique_ptr<FakeProcess>> processes;
unsigned long long nextGuid=1,nextInterface=1000;
bool tapctlAvailable=true,toolSucceeds=true,allowTerminate=true,processQuerySucceeds=true;
const char* fakeTemp=nullptr;
int CoCreateGuid(GUID* guid){guid->value=nextGuid++;return 0;}
int StringFromGUID2(const GUID& guid,wchar_t* text,int size){return std::swprintf(text,size,L"{%016llx}",guid.value)>0?19:0;}
bool IsEqualGUID(const GUID& a,const GUID& b){return a.value==b.value;}
DWORD GetLastError(){return 5;}
void Sleep(int){}
DWORD GetTempPathA(DWORD size,char* dest){const auto len=std::strlen(fakeTemp);assert(len<size);std::memcpy(dest,fakeTemp,len+1);return len;}
BOOL DeleteFileA(const char* path){return std::remove(path)==0;}
DWORD ConvertInterfaceAliasToLuid(const wchar_t* alias,NET_LUID* luid){std::lock_guard<std::mutex> l(fakeMutex);auto it=adapters.find(alias);if(it==adapters.end())return 2;*luid=it->second.InterfaceLuid;return 0;}
DWORD ConvertInterfaceLuidToGuid(const NET_LUID* luid,GUID* guid){std::lock_guard<std::mutex> l(fakeMutex);for(auto& item:adapters)if(item.second.InterfaceLuid.Value==luid->Value){*guid=item.second.InterfaceGuid;return 0;}return 2;}
DWORD GetIfEntry2(MIB_IF_ROW2* row){std::lock_guard<std::mutex> l(fakeMutex);for(auto& item:adapters)if(item.second.InterfaceLuid.Value==row->InterfaceLuid.Value){*row=item.second;return 0;}return 2;}
MIB_IF_ROW2 addAdapter(const std::string& name){MIB_IF_ROW2 row{};row.InterfaceLuid.Value=nextInterface++;row.InterfaceGuid.value=nextInterface++;row.OperStatus=IfOperStatusUp;row.InOctets=7;row.OutOctets=11;row.InUcastPkts=2;row.InNUcastPkts=3;row.OutUcastPkts=3;row.OutNUcastPkts=4;adapters[std::wstring(name.begin(),name.end())]=row;return row;}
std::string quotedAfter(const std::string& cmd,const std::string& marker){auto start=cmd.find(marker);assert(start!=std::string::npos);start+=marker.size();const auto end=cmd.find('"',start);return cmd.substr(start,end-start);}
BOOL CreateProcessA(const char* app,char* command,void*,void*,BOOL,DWORD,void*,const char*,STARTUPINFOA*,PROCESS_INFORMATION* info){
 auto process=std::make_unique<FakeProcess>();process->tool=std::string(app)=="tapctl.exe";
 if(process->tool){
  toolCommands.push_back(command);process->active=false;process->exitCode=toolSucceeds?0:1;
  if(toolSucceeds){std::string cmd(command);
   if(cmd.find(" create ")!=std::string::npos)addAdapter(quotedAfter(cmd,"--name \""));
   else if(cmd.find(" delete ")!=std::string::npos){const auto node=quotedAfter(cmd,"delete \"");for(auto it=adapters.begin();it!=adapters.end();){wchar_t text[40]{};StringFromGUID2(it->second.InterfaceGuid,text,40);std::wstring wide(text);if(node==std::string(wide.begin(),wide.end()))it=adapters.erase(it);else ++it;}}
  }
 }else childCommands.push_back(command);
 info->hProcess=process.get();info->hThread=process.get();processes.push_back(std::move(process));return 1;
}
BOOL GetExitCodeProcess(HANDLE process,DWORD* code){if(!process || !processQuerySucceeds)return 0;*code=process->active?STILL_ACTIVE:process->exitCode;return 1;}
DWORD WaitForSingleObject(HANDLE process,DWORD){return process->active?WAIT_TIMEOUT:WAIT_OBJECT_0;}
BOOL TerminateProcess(HANDLE process,DWORD){if(!allowTerminate)return 0;process->active=false;return 1;}
BOOL CloseHandle(HANDLE){return 1;}
namespace flutter {
struct EncodableValue {std::string value;explicit EncodableValue(const std::string& v):value(v){}};
template<class T> struct EventSink {std::vector<std::string> events;void Success(const T& value){events.push_back(value.value);}};
}
namespace openvpn_flutter {
class WinTunManager {std::string alias;public:bool initialize(){return true;}bool isWinTunAvailable(){return true;}bool createAdapter(const std::string& name){alias=name;addAdapter(alias);return true;}bool destroyAdapter(){if(!alias.empty()){adapters.erase(std::wstring(alias.begin(),alias.end()));alias.clear();}return true;}};
}
'''
header='\n'.join(line for line in header.splitlines() if not line.startswith('#include') and not line.startswith('#pragma')).replace('private:','public:')
signatures=[
'std::string guidString(', 'std::string newAdapterAlias(',
'VPNManager::VPNManager(', 'VPNManager::~VPNManager(', 'void VPNManager::setEventSink(',
'bool VPNManager::startVPN(', 'void VPNManager::stopVPN(', 'std::string VPNManager::getStatus(',
'std::string VPNManager::getConnectionStats(', 'bool VPNManager::initializeDriver(',
'bool VPNManager::initializeWinTun(', 'bool VPNManager::initializeTapDriver(', 'bool VPNManager::installTapDriver(',
'void VPNManager::monitorConnection(', 'bool VPNManager::checkConnectionStatus(',
'bool VPNManager::readConnectionLog(', 'bool VPNManager::readOwnedAdapterRow(',
'bool VPNManager::captureOwnedAdapter(', 'bool VPNManager::runAdapterTool(',
'bool VPNManager::createOwnedAdapter(', 'void VPNManager::releaseOwnedAdapter(',
'void VPNManager::updateStatus(', 'void VPNManager::updateStatusThreadSafe(',
'void VPNManager::processPendingStatusUpdates(', 'bool VPNManager::createConfigFile(',
'void VPNManager::cleanupTempFiles(', 'bool VPNManager::createTapAdapter(',
'std::pair<uint64_t, uint64_t> VPNManager::getRealNetworkStats(',
'void VPNManager::updateSpeedCalculations('
]
stubs=r'''
std::string VPNManager::getBundledOpenVPNPath(){return "openvpn.exe";}
std::string VPNManager::findBundledExecutable(const std::string& name){return name=="tapctl.exe"&&tapctlAvailable?name:"";}
bool VPNManager::isRunningAsAdmin(){return true;}
bool VPNManager::isTapDriverInstalled(){return true;}
bool VPNManager::runAsAdmin(const std::string&,const std::string&){return false;}
std::string VPNManager::getAppDirectory(){return ".";}
'''
tests=r'''
using namespace openvpn_flutter;
void append(VPNManager& manager,const std::string& text){std::ofstream out(manager.currentLogPath,std::ios::app|std::ios::binary);out<<text;}
void freezeMonitor(VPNManager& manager){manager.shouldMonitor=false;if(manager.statusMonitorThread.joinable())manager.statusMonitorThread.join();}
void resetLog(VPNManager& manager){std::ofstream out(manager.currentLogPath,std::ios::trunc);manager.logReadOffset=0;manager.pendingLogLine.clear();manager.initializationComplete=false;manager.initializationFailed=false;}
int main(int,char** argv){
 fakeTemp=argv[1];
 auto foreign=addAdapter("Other VPN");foreign.InOctets=999999;adapters[L"Other VPN"]=foreign;
 std::string oldAlias,oldLog;
 for(const auto driver:{DriverType::WINTUN,DriverType::TAP_WINDOWS}){
  VPNManager manager;manager.preferredDriver=driver;
  assert(manager.startVPN("client\ndev tun\ndev-node {foreign}\nwindows-driver wrong\nlog old.log\nmachine-readable-output\n<key>\ndev secret-inline-content\n</key>\n"));
  freezeMonitor(manager);
  assert(toolCommands.back().find(driver==DriverType::WINTUN?"create --hwid wintun":"create --hwid root\\tap0901")!=std::string::npos);
  assert(manager.hasOwnedAdapter && manager.ownedAdapterLuid.Value!=foreign.InterfaceLuid.Value);
  assert(childCommands.back().find("--dev-node \""+manager.ownedAdapterNode+"\"")!=std::string::npos);
  assert(childCommands.back().find(driver==DriverType::WINTUN?"--windows-driver wintun":"--windows-driver tap-windows6")!=std::string::npos);
  assert(childCommands.back().find("--dev tun --dev-type tun")!=std::string::npos);
  assert(childCommands.back().find("--log-append \""+manager.currentLogPath+"\"")!=std::string::npos);
  assert(!manager.checkConnectionStatus()); // foreign AND own precreated UP adapter + child alive do not prove establishment
  assert(manager.getRealNetworkStats()==std::make_pair(uint64_t{0},uint64_t{0}));
  assert(manager.getStatus()=="connecting");
  std::ifstream config(manager.currentConfigPath);std::string configText((std::istreambuf_iterator<char>(config)),{});
  assert(configText.find("{foreign}")==std::string::npos && configText.find("machine-readable-output")==std::string::npos);
  assert(configText.find("dev secret-inline-content")!=std::string::npos);
  append(manager,"Peer Connection Initiated\n");assert(!manager.checkConnectionStatus());
  append(manager,"Initialization Sequence Completed");assert(!manager.checkConnectionStatus()); // incomplete line is not evidence
  append(manager,"\r\n");assert(manager.checkConnectionStatus());
  assert(manager.getRealNetworkStats()==std::make_pair(uint64_t{7},uint64_t{11}));
  const auto stats=manager.getConnectionStats();assert(stats.find("\"packets_in\":\"5\"")!=std::string::npos && stats.find("\"packets_out\":\"7\"")!=std::string::npos);
  manager.updateStatusThreadSafe("connected");assert(manager.getStatus()=="connected");
  processQuerySucceeds=false;assert(manager.getStatus()=="connecting");processQuerySucceeds=true;
  manager.updateStatusThreadSafe("error");assert(manager.getStatus()=="connecting"); // monitor timeout cannot prove down while child lives
  manager.updateStatusThreadSafe("connected");
  auto& row=adapters[std::wstring(manager.ownedAdapterAlias.begin(),manager.ownedAdapterAlias.end())];
  const auto guid=row.InterfaceGuid;row.InterfaceGuid=foreign.InterfaceGuid;assert(!manager.checkConnectionStatus());row.InterfaceGuid=guid;
  row.OperStatus=0;assert(!manager.checkConnectionStatus());row.OperStatus=IfOperStatusUp;
  append(manager,"SIGUSR1[soft,ping-restart] received, process restarting\n");assert(!manager.checkConnectionStatus());
  assert(manager.getStatus()=="connecting");
  flutter::EventSink<flutter::EncodableValue> events;manager.setEventSink(&events);manager.processPendingStatusUpdates();assert(std::find(events.events.begin(),events.events.end(),"connected")==events.events.end());
  append(manager,"Initialization Sequence Completed\n");assert(manager.checkConnectionStatus());
  append(manager,"ERROR: Windows route add command failed\nInitialization Sequence Completed\n");assert(!manager.checkConnectionStatus());
  append(manager,"SIGUSR1[soft,retry] received, process restarting\nInitialization Sequence Completed\n");assert(manager.checkConnectionStatus());
  append(manager,"Initialization Sequence Completed With Errors\n");assert(!manager.checkConnectionStatus());
  resetLog(manager);append(manager,"Initialization Sequence Completed\n");assert(manager.checkConnectionStatus());
  append(manager,std::string(65536,'x')+"\nSIGUSR1[soft,retry] received, process restarting\n");assert(!manager.checkConnectionStatus());assert(!manager.checkConnectionStatus());
  resetLog(manager);append(manager,"Initialization Sequence Completed\n");assert(manager.checkConnectionStatus());
  const auto alias=std::wstring(manager.ownedAdapterAlias.begin(),manager.ownedAdapterAlias.end());
  const auto saved=adapters.at(alias);adapters.erase(alias);assert(!manager.checkConnectionStatus());assert(manager.getRealNetworkStats()==std::make_pair(uint64_t{0},uint64_t{0}));adapters[alias]=saved;
  manager.hProcess->active=false;assert(!manager.checkConnectionStatus());assert(manager.getStatus()=="disconnected");
  oldAlias=manager.ownedAdapterAlias;oldLog=manager.currentLogPath;
  manager.setEventSink(nullptr);manager.stopVPN();assert(!manager.hasOwnedAdapter && !adapters.count(alias));assert(adapters.count(L"Other VPN"));
  assert(manager.startVPN("client\ndev tun\n"));freezeMonitor(manager);assert(manager.ownedAdapterAlias!=oldAlias && manager.currentLogPath!=oldLog);assert(!manager.checkConnectionStatus());
  allowTerminate=false;const auto retained=manager.ownedAdapterNode;manager.stopVPN();assert(manager.hProcess && manager.ownedAdapterNode==retained && manager.hasOwnedAdapter);assert(manager.currentStatus=="error" && manager.getStatus()=="connecting");allowTerminate=true;manager.stopVPN();assert(!manager.hProcess && !manager.hasOwnedAdapter);
 }
 {VPNManager manager;manager.preferredDriver=DriverType::TAP_WINDOWS;assert(manager.startVPN("dev \"tap\"\n"));freezeMonitor(manager);assert(childCommands.back().find("--dev tap --dev-type tap")!=std::string::npos);}
 {
  VPNManager manager;assert(manager.startVPN("dev tun\n"));
  append(manager,"Initialization Sequence Completed\n");
  for(int i=0;i<100 && !manager.isConnected;i++)std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(manager.isConnected && manager.getStatus()=="connected");
  append(manager,"SIGUSR1[soft,ping-restart] received, process restarting\n");
  for(int i=0;i<100 && manager.isConnected;i++)std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(!manager.isConnected && manager.isConnecting && manager.getStatus()=="connecting");
  append(manager,"Initialization Sequence Completed\n");
  for(int i=0;i<100 && !manager.isConnected;i++)std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(manager.isConnected);
 }
 tapctlAvailable=false;
 {VPNManager manager;manager.preferredDriver=DriverType::TAP_WINDOWS;assert(!manager.initializeDriver());assert(!manager.hasOwnedAdapter);}
 {VPNManager manager;assert(manager.startVPN("dev tun\n"));freezeMonitor(manager);assert(manager.hasOwnedAdapter);append(manager,"Initialization Sequence Completed\n");assert(manager.checkConnectionStatus());}
 tapctlAvailable=true;toolSucceeds=false;
 {VPNManager manager;manager.preferredDriver=DriverType::TAP_WINDOWS;assert(!manager.initializeDriver());assert(!manager.hasOwnedAdapter);}
 assert(adapters.size()==1 && adapters.count(L"Other VPN"));
 std::cout<<"PASS: owned WinTun/TAP GUID launch, exact LUID/GUID counters, precreated/foreign rejection, complete log readiness, restart/error/exit revocation, stale event/log rejection, cleanup and unavailable-tool failure\n";
}
'''
with tempfile.TemporaryDirectory(prefix='vpn-openvpn-ownership-') as directory:
 path=Path(directory);cpp=preamble+header+'\nnamespace openvpn_flutter {\n'+'\n'.join(function(s) for s in signatures)+stubs+'\n}\n'+tests
 (path/'main.cpp').write_text(cpp)
 subprocess.run(['clang++','-std=c++17','-pthread','-Wall','-Wextra','-Werror','-Wno-unused-parameter',str(path/'main.cpp'),'-o',str(path/'regression')],check=True)
 subprocess.run([str(path/'regression'),str(path)+'/'],check=True,timeout=20)
