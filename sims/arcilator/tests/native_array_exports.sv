module ArrayExportBody #(parameter int ID=1,N=3,W=65)(input logic clock,enable,input int seed,output int observed);
 int saved=ID*7;
 string saved_names[0:1];
 chandle saved_handles[0:1];
 always @(posedge clock) if(enable) saved=seed^ID;
 assign observed=saved;
 export "DPI-C" function array_atoms;
 function automatic int array_atoms(input int values[3:4-N],output byte result[N-2:-1],inout shortint work[-1:N-2]);
   int sum;
   sum=0;
   for(int n=0;n<N;n++)begin
     if(n!=N-1)result[N-2-n]=byte'(values[3-n]^(ID*17+n));
     if(n!=N-1)work[-1+n]=shortint'(work[-1+n]+values[3-n]+ID);
     sum^=values[3-n]+int'(work[-1+n]);
   end
   saved=sum;
   return sum;
 endfunction
 export "DPI-C" function array_vectors;
 function automatic void array_vectors(input logic[W-1:0]a[1:-1][4:5],output logic[W+63:0]b[2:0][0:1],inout bit[12:0]c[-1:0][3:1]);
   for(int n=0;n<6;n++)if(n!=5)begin
     b[2-n/2][n%2]={64'h123456789abcdef0^64'(n+ID),a[1-n/2][4+n%2]};
     c[-1+n/3][3-n%3]^=13'(a[1-n/2][4+n%2])^13'(n+ID);
   end
 endfunction
 export "DPI-C" function array_pointers;
 function automatic int array_pointers(input string names[3:2],inout string changed[-2:-1],input chandle handles[0:1],output chandle copied[9:8]);
   changed[-2]={names[3],":",changed[-2]};
   changed[-1]={changed[-1],":",names[2]};
   saved_names[0]=changed[-2];saved_names[1]=changed[-1];
   copied[9]=handles[1];copied[8]=handles[0];
   saved_handles[0]=copied[9];saved_handles[1]=copied[8];
   return changed[-2].len()+changed[-1].len()+ID;
 endfunction
 export "DPI-C" function array_saved_pointers;
 function automatic void array_saved_pointers(output string names[4:5],output chandle handles[-2:-1]);
   names[4]=saved_names[0];names[5]=saved_names[1];
   handles[-2]=saved_handles[0];handles[-1]=saved_handles[1];
 endfunction
 export "DPI-C" function array_zero;
 function automatic void array_zero(output bit[(ID==1?65:39)-1:0] a[0:(ID==1?2:4)]);
   a='{default:'0};
 endfunction
 export "DPI-C" function array_bits;
 function automatic int array_bits(input logic a[-1:1],output bit b[9:7],inout logic c[0:2]);
   for(int n=0;n<3;n++)begin b[9-n]=a[n-1];c[n]^=a[n-1];end
   return int'(a[0])^ID;
 endfunction
 export "DPI-C" function signed_atoms;
 function automatic longint signed_atoms(input byte b,input shortint s,input int i,input longint l,output byte bo,output longint lo);
   bo=b^byte'(ID);lo=l^longint'(ID);return l+i+s+b;
 endfunction
 export "DPI-C" function unsigned_atoms;
 function automatic longint unsigned unsigned_atoms(input byte unsigned b,input shortint unsigned s,input int unsigned i,input longint unsigned l,output byte unsigned bo,output longint unsigned lo);
   bo=b^byte'(ID);lo=l^longint'(ID);return l+i+s+b;
 endfunction
 export "DPI-C" function array_longs;
 function automatic void array_longs(input longint unsigned a[0:2],output longint b[1:-1],inout longint unsigned c[4:2]);
   for(int n=0;n<3;n++)begin b[1-n]=longint'(a[n])^longint'(n+ID);c[4-n]+=a[n];end
 endfunction
 export "DPI-C" function array_saved;
 function automatic int array_saved();return saved;endfunction
endmodule
module ArrayExportFirst(input logic clock,enable,input int seed,output int observed);
 ArrayExportBody #(.ID(1),.N(3),.W(65)) child(.*);
endmodule
module ArrayExportSecond(input logic clock,enable,input int seed,output int observed);
 ArrayExportBody #(.ID(2),.N(5),.W(129)) child(.*);
endmodule
