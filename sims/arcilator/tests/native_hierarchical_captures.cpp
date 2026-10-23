#include "native/runtime.h"
#include <array>
#include <cstdint>
#include <cstdio>
using namespace chipyard_arc;
static unsigned checks=0;
static void check(bool good,const char *message){++checks;if(!good)throw std::runtime_error(message);}
int main(int argc,char **argv){
 for(unsigned round=0;round<3;++round){
  auto m=create_model(argc,argv);m->initialize();uint64_t time=0;
  uint64_t expected[2][2][4];
  for(unsigned fixture=0;fixture<2;++fixture)for(unsigned bank=0;bank<2;++bank)for(unsigned index=0;index<4;++index)expected[fixture][bank][index]=(bank && fixture==0?233:17)+index;
  auto observe=[&](unsigned index){
   for(unsigned fixture=0;fixture<2;++fixture)for(unsigned bank=0;bank<2;++bank){
    std::string prefix=fixture?"same_":"different_",name=bank?"second_value":"first_value",function=bank?"second_function":"first_function";
    check(m->signal(prefix+name).get()==expected[fixture][bank][index],"hierarchical task wrote the wrong physical array/index");
    check(m->signal(prefix+function).get()==expected[fixture][bank][index],"hierarchical function read the wrong physical array/index");
   }
  };
  for(unsigned index=0;index<4;++index){m->advance(++time,[&]{m->signal("clock").set(0);m->signal("write").set(0);m->signal("index").set(index);});observe(index);}
  for(unsigned i=0;i<1024;++i){
   unsigned fixture=(i/2+i/5)%2,bank=(i*7+i/4)%2,index=(i*3+i/7)%4;uint64_t value=(i*157+13*round+17)&8191;
   m->advance(++time,[&]{m->signal("clock").set(0);m->signal("fixture").set(fixture);m->signal("write").set(1);m->signal("address").set((index<<1)|bank);m->signal("data").set(value);m->signal("index").set(index);});observe(index);
   m->advance(++time,[&]{m->signal("clock").set(1);});expected[fixture][bank][index]=value;observe(index);
   for(unsigned read=0;read<4;++read){m->advance(++time,[&]{m->signal("index").set(read);});observe(read);}
  }
 }
 std::printf("PASS: %u original-style hierarchical task/function memory checks, static formals, two nested scopes, non-byte words, distinct and identical subbanks, exact RTL storage and model recreation\n",checks);
}
