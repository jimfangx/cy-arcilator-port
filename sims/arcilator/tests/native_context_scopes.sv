package context_pkg;
  import "DPI-C" context function int context_package(input int tag);
endpackage
module ContextLeaf(input logic clock, reset, enable, input int tag, output int observed);
  import "DPI-C" context function int context_tick(input int tag);
  import "DPI-C" context function int context_probe(input int tag);
  import "DPI-C" context context_tick = function int alternate_tick(input int value);
  import "DPI-C" context function int context_startup();
  int initial_observed=0;
  initial begin initial_observed=context_startup(); end
  int counter=0;
  export "DPI-C" function context_get;
  function int context_get(); return counter; endfunction
  export "DPI-C" function context_set;
  function void context_set(input int value); counter=value; endfunction
  export "DPI-C" function context_export_probe;
  function int context_export_probe(input int tag); return context_probe(tag); endfunction
  export "DPI-C" function context_export_package;
  function int context_export_package(input int tag); return context_pkg::context_package(tag); endfunction
  function automatic int private_probe(input int value); return context_probe(value); endfunction
  int result;
  always @(posedge clock) begin
    if(reset)begin counter=0;observed<=0;end
    else if(enable)begin
      result=tag[0] ? alternate_tick(tag) : context_tick(tag);
      observed <= (initial_observed!=1) | (context_probe(tag)!=result) | (context_pkg::context_package(tag)!=(tag+500));
      if ($root.__ChipyardArcPorts_ArcRTL_0.rtl.banks[1].\leaf.dot .private_probe((tag & ~255)|1)<0)
        observed<=1;
      if ($root.__ChipyardArcPorts_ArcRTL_0.rtl.banks[0].\leaf.dot .context_tick((tag & ~255)|77)<=0)
        observed<=1;
    end
  end
endmodule
module ContextTop(input logic clock,reset,enable,input int side,output int observed);
  import "DPI-C" context_plain = function int plain_tick(input int tag);
  int first,second,crossed,plain,function_checks;
  wire [1:0][31:0] values;
  assign first=values[0];
  assign second=values[1];
  for(genvar n=0;n<2;n++) begin:banks
    ContextLeaf \leaf.dot (clock,reset,enable,(side<<8)|n,values[n]);
  end
  always @(posedge clock) begin
    if(reset)begin crossed<=0;plain<=0;function_checks<=0;end
    else if(enable)begin
      crossed <= banks[1].\leaf.dot .context_tick((side<<8)|1)>0 ? 0 : 1;
      plain <= plain_tick(99);
      function_checks <= (banks[1].\leaf.dot .private_probe((side<<8)|1)<0) | (banks[0].\leaf.dot .private_probe(side<<8)<0);
    end
  end
  assign observed=first|second|crossed|plain|function_checks;
endmodule
