module CallbackChild;
 import "DPI-C" context function int host_child(input int tag,input bit fixed_target);
endmodule
module CallbackLeaf(input bit clock,reset,enable,input int tag,output int observed);
 import "DPI-C" context task host_task(input int tag,input int delta,output int value);
 import "DPI-C" context task host_inner(input int tag,output int value);
 CallbackChild own();
 int counter=0,inner_done=0;
 export "DPI-C" function sv_inner_count;
 function int sv_inner_count();return inner_done;endfunction
 export "DPI-C" function sv_counter;
 function int sv_counter();return counter;endfunction
 export "DPI-C" task sv_update;
 task sv_update(input int delta,output int value);
  counter=counter+delta;value=counter;
 endtask
 export "DPI-C" task sv_roundtrip;
 task sv_roundtrip(input int which,output int value);
  if(own.host_child(which,0)!=which+11)
   $fatal(1,"relative child callback scope/value changed");
  if($root.__ChipyardArcPorts_ArcRTL_0.rtl.left.own.host_child(0,1)!=11)
   $fatal(1,"fixed child callback scope/value changed");
  host_inner(which,value);
  inner_done++;
  $display("NATIVE_TASK_CALLBACK tag=%0d value=%0d",which,value);
 endtask
 task automatic local_call(input int which,output int value);
  host_task(which,which+1,value);
 endtask
 always @(posedge clock) begin
  if(reset)begin counter=0;inner_done=0;observed<=0;end
  else if(enable)begin
   int value;
   local_call(tag,value);
   observed<=value;
  end
 end
endmodule
module TaskCallbacks(input bit clock,reset,enable,output int first,second);
 CallbackLeaf left(clock,reset,enable,0,first);
 CallbackLeaf right(clock,reset,enable,1,second);
endmodule
