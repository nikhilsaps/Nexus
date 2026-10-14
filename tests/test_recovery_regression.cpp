#include <cassert>
#include <cstdio>
#include <cerrno>
#include <map>
#include <set>
#include <string>
static std::map<std::string,std::string> files;
static std::set<int> fails;
static int calls;
static int exists_mock(const char* p) { return files.count(p); }
static int rename_mock(const char* a,const char* b) {
 if(fails.count(++calls) || !files.count(a) || files.count(b)) { errno=EIO; return -1; }
 files[b]=files[a]; files.erase(a); return 0;
}
static int remove_mock(const char* p) { return files.erase(p) ? 0 : -1; }
#define NX_SR_EXISTS exists_mock
#define NX_SR_RENAME rename_mock
#define NX_SR_REMOVE remove_mock
#include "../main/safe_replace.h"
static void setup() { files={{"dst","old"},{"tmp","new"}}; fails.clear();calls=0; }
int main() {
 setup(); assert(nx_sr_replace("tmp","dst","bak")==NX_SR_OK);assert(files["dst"]=="new");assert(!files.count("bak"));
 setup();fails={1};assert(nx_sr_replace("tmp","dst","bak")==NX_SR_E_BACKUP);assert(files["dst"]=="old");
 setup();fails={2};assert(nx_sr_replace("tmp","dst","bak")==NX_SR_E_RENAME);assert(files["dst"]=="old");
 setup();fails={2,3};assert(nx_sr_replace("tmp","dst","bak")==NX_SR_E_RENAME_BAK_KEPT);assert(files["bak"]=="old");assert(!files.count("dst"));
 fails.clear();assert(nx_sr_replace("tmp","dst","bak")==NX_SR_E_STALE_BAK);assert(files["bak"]=="old");
 assert(nx_sr_recover("dst","bak")==NX_SR_OK);assert(files["dst"]=="old");assert(nx_sr_replace("tmp","dst","bak")==NX_SR_OK);
 setup();files["bak"]="older";assert(nx_sr_recover("dst","bak")==NX_SR_E_STALE_BAK);assert(files["bak"]=="older");assert(files["dst"]=="old");
 setup();files.erase("dst");assert(nx_sr_replace("tmp","dst","bak")==NX_SR_OK);assert(files["dst"]=="new");
 setup();files.erase("dst");fails={1};assert(nx_sr_replace("tmp","dst","bak")==NX_SR_E_RENAME);assert(files["tmp"]=="new");
 assert(nx_sr_rel_path_ok("/folder/data.json",100));
 for(const char* p:{"/../x","/x/./y","/x\\y","/file.nexus-bak","/file.NEXUS-TMP","/file.nexus-bak.","/file.nexus-bak ","/.. /x","//x","/x/","/x:y"}) assert(!nx_sr_rel_path_ok(p,100));
 puts("PASS: replacement, backup failure, rollback, double failure, retry preservation, recovery, ambiguous backup, new file, path rejection");
}
