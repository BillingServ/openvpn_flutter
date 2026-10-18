from pathlib import Path
import subprocess,tempfile,sys
root=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parents[2]
header=(root/'windows/wintun_manager.h').read_text()
header='\n'.join(line for line in header.splitlines() if not line.startswith('#include') and not line.startswith('#pragma')).replace('private:','public:')
source=(root/'windows/wintun_manager.cpp').read_text()
def function(sig):
 start=source.index(sig);brace=source.index('{',start);depth=1;end=brace+1
 while depth:
  if source[end]=='{':depth+=1
  elif source[end]=='}':depth-=1
  end+=1
 return source[start:end]
cpp=r'''
#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#define WINAPI
using DWORD=unsigned long;using BOOL=int;using BYTE=unsigned char;using HANDLE=void*;using HMODULE=void*;using LPCWSTR=const wchar_t*;
struct GUID {unsigned long value;};
DWORD GetLastError(){return 0;}
'''+header+r'''
static_assert(std::is_same_v<std::invoke_result_t<WINTUN_CLOSE_ADAPTER_FUNC,WINTUN_ADAPTER_HANDLE>,void>);
namespace openvpn_flutter {
WinTunManager::WinTunManager(){}
WinTunManager::~WinTunManager(){}
std::string WinTunManager::generateAdapterName(){return "test";}
GUID WinTunManager::generateGuid(){return GUID{1};}
'''+function('bool WinTunManager::createAdapter(')+function('bool WinTunManager::destroyAdapter(')+r'''
}
int closed=0;
WINTUN_ADAPTER_HANDLE create(LPCWSTR,LPCWSTR,const GUID*){return reinterpret_cast<WINTUN_ADAPTER_HANDLE>(1);}
void closeAdapter(WINTUN_ADAPTER_HANDLE){++closed;}
int main(){openvpn_flutter::WinTunManager manager;manager.wintunDll=reinterpret_cast<HMODULE>(1);manager.WinTunCreateAdapter=&create;manager.WinTunCloseAdapter=&closeAdapter;assert(manager.createAdapter("one"));assert(!manager.createAdapter("two"));assert(manager.destroyAdapter());assert(closed==1);assert(manager.destroyAdapter());assert(closed==1);std::cout<<"PASS: actual Wintun create ownership and void close API\n";}
'''
with tempfile.TemporaryDirectory(prefix='vpn-wintun-api-') as directory:
 path=Path(directory);(path/'test.cpp').write_text(cpp)
 subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
 subprocess.run([str(path/'test')],check=True)
