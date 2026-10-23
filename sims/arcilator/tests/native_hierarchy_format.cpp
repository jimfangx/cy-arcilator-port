#include "native/runtime.h"
#include <cstdio>
using namespace chipyard_arc;
int main(int argc,char **argv){
 const char *roots[]={"first","second","third"};
 unsigned biases[]={3,3,7};
 for(unsigned repeat=0;repeat<2;++repeat){
  auto model=create_model(argc,argv);model->signal("arc_printf_enable").set(1);model->initialize();
  for(auto root:roots)for(unsigned n=0;n<2;++n)
   std::fprintf(stderr,"EXPECTED_HIER NATIVE_HIER_INIT m=[TestHarness.%s.banks[%u].leaf.dot] M=[TestHarness.%s.banks[%u].\\leaf.dot ]\n",root,n,root,n);
  model->advance(0,[&]{model->signal("clock").set(0);model->signal("reset").set(1);model->signal("enable").set(0);model->signal("data").set(0);});
  unsigned expected[3]={};
  for(unsigned cycle=0;cycle<32;++cycle){
   bool reset=cycle%11==0,enable=cycle%3!=1;
   unsigned data=(cycle*29+5)&255;
   model->advance(cycle*2,[&]{model->signal("clock").set(0);model->signal("reset").set(reset);model->signal("enable").set(enable);model->signal("data").set(data);});
   for(unsigned root=0;root<3;++root){
    if(reset)expected[root]=(biases[root]*2+1)&255;
    else if(enable){
     expected[root]=(data*2+biases[root]*2+1)&255;
     for(unsigned n=0;n<2;++n)std::fprintf(stderr,"EXPECTED_HIER NATIVE_HIER n=%u v=%u m=[TestHarness.%s.banks[%u].leaf.dot] M=[TestHarness.%s.banks[%u].\\leaf.dot ]\n",n,data+biases[root]+n,roots[root],n,roots[root],n);
    }
   }
   model->advance(cycle*2+1,[&]{model->signal("clock").set(1);});
   for(unsigned root=0;root<3;++root)if(model->signal(roots[root]).get()!=expected[root])throw std::runtime_error("hierarchy print fixture register/reset/enable mismatch");
  }
 }
 std::fprintf(stderr,"PASS: source hierarchy prints with physical roots, shared RTL specializations, generated/escaped names, startup and clock/reset/enable gating, and192 register checks across model recreation\n");
}
