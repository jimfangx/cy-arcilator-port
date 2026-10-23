#include "native/runtime.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
using namespace chipyard_arc;
struct Input{uint64_t time,clock;std::array<std::string,3> values;};
struct Expected{uint64_t time;std::array<std::string,16> values;uint64_t count;};
static std::vector<uint8_t> bits(std::string text,unsigned width){
 std::vector<uint8_t> result((width+7)/8,0);unsigned nibble=0;
 for(auto it=text.rbegin();it!=text.rend();++it,++nibble){unsigned c=*it,value=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:0;if(nibble/2<result.size())result[nibble/2]|=value<<((nibble%2)*4);}
 if(width%8)result.back()&=(1u<<(width%8))-1;return result;
}
static unsigned long long checks=0;
int main(int argc,char**argv){
 auto folder=std::filesystem::path(__FILE__).parent_path()/"delay_precision";std::vector<Input> inputs;std::ifstream input_file(folder/"stimulus.txt");Input input;
 while(input_file>>input.time>>input.clock>>input.values[0]>>input.values[1]>>input.values[2])inputs.push_back(input);
 std::vector<Expected> expected;std::ifstream reference(folder/"reference.txt");std::string tag;Expected row;
 while(reference>>tag>>row.time>>row.values[0]>>row.values[1]>>row.values[2]>>row.values[3]>>row.values[4]>>row.values[5]>>row.values[6]>>row.values[7]>>row.values[8]>>row.values[9]>>row.values[10]>>row.values[11]>>row.values[12]>>row.values[13]>>row.values[14]>>row.values[15]>>row.count){if(tag!="S")throw std::runtime_error("invalid stock timing record");expected.push_back(row);}
 if(inputs.empty()||expected.size()!=12001||expected.back().time!=12000)throw std::runtime_error("incomplete independent SV timing corpus");
 const std::array<const char*,16> names={"q13","qfast","qchain","q65","q193","constant_value","delayed_clock","qround","qtime","qexpr","qnvdla","qwait","cast_dynamic","cast_negative","cast_tie_pos","cast_tie_neg"};const std::array<unsigned,16> widths={13,13,13,65,193,13,1,193,65,13,13,13,32,32,32,32};
 for(unsigned recreation=0;recreation<3;++recreation)for(unsigned coarse=0;coarse<2;++coarse){
  auto m=create_model(argc,argv);m->initialize();std::set<uint64_t> samples;
  if(!coarse)for(uint64_t t=0;t<=12000;++t)samples.insert(t);
  else{for(auto &i:inputs)samples.insert(i.time);for(uint64_t t=0;t<=12000;t+=137)samples.insert(t);samples.insert(12000);}
  unsigned next=0;
  for(auto time:samples){
   auto drive=[&]{m->signal("clock").set(inputs[next].clock);m->signal("value").assign(bits(inputs[next].values[0],13));m->signal("wide65").assign(bits(inputs[next].values[1],65));m->signal("wide193").assign(bits(inputs[next].values[2],193));};
   if(next<inputs.size()&&inputs[next].time==time){m->advance_fs(time,drive);++next;}else m->advance_fs(time);
   if(m->time_fs()!=time||m->time()!=time/1000)throw std::runtime_error("femtosecond advance lost physical time");++checks;
   const auto &e=expected.at(time);if(e.time!=time)throw std::runtime_error("noncontiguous stock timing oracle");
   for(unsigned n=0;n<names.size();++n){++checks;auto actual=m->signal(names[n]).copy(),wanted=bits(e.values[n],widths[n]);if(widths[n]%8)actual.back()&=(1u<<(widths[n]%8))-1;if(actual!=wanted)throw std::runtime_error(std::string(names[n])+" differs from stockSV at "+std::to_string(time)+"fs");}
   ++checks;if(m->signal("count").get()!=e.count)throw std::runtime_error("delayed-clock edge count differs from stockSV at "+std::to_string(time)+"fs; actual="+std::to_string(m->signal("count").get())+" expected="+std::to_string(e.count));
  }
 }
 std::printf("PASS: %llu independently checked SV delay precision/cast values, exact fs times, lexical rounding, fractional literals/expressions, real casts, pulse rejection, vector/chain/constant/procedural delays, delayed-clock edge counts, coarse advances and model recreation\n",checks);
}
