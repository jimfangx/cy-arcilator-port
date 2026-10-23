#include "native/runtime.h"
#include "native_realtime_exports_api.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
using namespace chipyard_arc;
static svScope expected_scope;
static unsigned imported_calls=0, checks=0;
static void check(bool good,const char *message){++checks;if(!good)throw std::runtime_error(message);}
template<class T,class U> static T bits(U value){static_assert(sizeof(T)==sizeof(U));T result;std::memcpy(&result,&value,sizeof(result));return result;}
extern "C" double rt_c_identity(double value){check(svGetScope()==expected_scope,"realtime import declaration scope mismatch");++imported_calls;return value;}
int main(int argc,char **argv){
 const std::array<uint64_t,12> patterns={0,0x8000000000000000ull,1,0x8000000000000001ull,0x0010000000000000ull,0x3ff4000000000000ull,0xbff4000000000000ull,0x7fefffffffffffffull,0x7ff0000000000000ull,0xfff0000000000000ull,0x7ff8123456789abcull,0x7ff0123456789abcull};
 for(unsigned round=0;round<3;++round){
  auto m=create_model(argc,argv);m->initialize();std::array<svScope,2> scopes;
  for(unsigned n=0;n<2;++n){
   auto path=std::string("RealtimeHarness/")+(n?"second":"first")+"/rtl/rtl";scopes[n]=svGetScopeFromName(path.c_str());check(scopes[n],"missing realtime scope");svSetScope(scopes[n]);expected_scope=scopes[n];
   check(rt_zero()==0.0,"realtime default initializer mismatch");check(rt_constant()==-3.125,"typed realtime constant mismatch");
   check(rt_saved()==1.25*(n+1),"realtime initializer mismatch");check(rt_literal()==(n?12.5:1.25),"realtime time literal ignored lexical units");
   auto identity=[&](uint64_t value){check(bits<uint64_t>(rt_identity(bits<double>(value)))==value,"realtime IEEE identity mismatch");check(bits<uint64_t>(rt_import_identity(bits<double>(value)))==value,"realtime context import IEEE mismatch");};
   for(auto value:patterns)identity(value);
   uint64_t random=0x283d3fa72f11ab39ull;
   for(unsigned i=0;i<2048;++i){random^=random<<13;random^=random>>7;random^=random<<17;identity(random);}
   double y=1,z=0;bool rejected=false;try{rt_transform(1,nullptr,&z);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"null realtime inout accepted");rejected=false;try{rt_transform(1,&y,nullptr);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"null realtime output accepted");
  }
  const std::array<uint64_t,8> times={1,125,250,1000,1250,9321,1000000,123456789};
  for(auto ps:times){
   m->advance(ps);
   for(unsigned n=0;n<2;++n){svSetScope(scopes[n]);expected_scope=scopes[n];check(rt_now()==double(ps)/(n?100.0:1000.0),"realtime current-time lexical scale mismatch");check(rt_direct_time()==double(ps)/(n?100.0:1000.0),"direct realtime clock differs from SV helper clock");}
  }
  for(unsigned i=0;i<256;++i){
   unsigned n=i%2;svSetScope(scopes[n]);expected_scope=scopes[n];double x=(int(i)-117)/8.0,y=(int(i%19)-9)/16.0,z=0,old_y=y;double returned=rt_transform(x,&y,&z);
   check(y==x*1.25+old_y && z==old_y-x*.5 && returned==z+1.25*(n+1),"realtime output/inout arithmetic mismatch");check(rt_saved()==y,"realtime capture write mismatch");check(m->signal(n?"second_value":"first_value").get()==bits<uint64_t>(y),"realtime export write not visible to RTL");
   uint64_t time=123456790+uint64_t(i)*2;m->advance(time,[&]{m->signal("clock").set(0);m->signal("enable").set(1);m->signal("input_bits").set(bits<uint64_t>(x));});m->advance(time+1,[&]{m->signal("clock").set(1);});
   for(unsigned k=0;k<2;++k){svSetScope(scopes[k]);expected_scope=scopes[k];check(bits<uint64_t>(rt_saved())==bits<uint64_t>(x),"RTL realtime write not visible to export");}
  }
 }
 check(imported_calls==12360,"realtime callback count mismatch");std::printf("PASS: %u realtime IEEE, callback, scope, capture, and lexical-time checks\n",checks);
}
