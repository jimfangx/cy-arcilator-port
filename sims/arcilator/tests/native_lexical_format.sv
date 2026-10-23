module LexicalFormat #(parameter integer BIAS=3)
 (input bit clock,reset,enable,input bit[7:0] data,output bit[7:0] observed,output integer startup);
 function automatic integer factorial(input integer n);
  if(n==0)return 1;
  return n*factorial(n-1);
 endfunction
 localparam integer FACT=factorial(6);
 function automatic integer copies();
  $display("NATIVE_HIER_ELAB_SHOULD_NOT_RUN");
  return 2;
 endfunction
 function automatic integer value(input integer n);
  begin:inside_function
   $display("NATIVE_HIER_FUNC m=[%m] n=%0d",n);
  end
  return n+BIAS;
 endfunction
 function automatic integer twice(input integer n);
  return 2*value(n);
 endfunction
 task automatic show(input integer n);
  begin:inside_task
   $display("NATIVE_HIER_TASK m=[%m] n=%0d",n);
  end
 endtask
 initial begin:boot
  integer width,checks;
  $display("NATIVE_HIER_BLOCK m=[%m]");
  startup=1+twice(5);
  width=$bits(value(99));
  checks=(value(7)===(7+BIAS));
  case(BIAS+9)
   value(9):checks+=2;
   default:checks=0;
  endcase
  if({copies(){1'b1}}!=3)$fatal(1,"replication elaboration result changed");
  show(startup);
  $display("NATIVE_HIER_RESULT m=[%m] v=%0d width=%0d fact=%0d checks=%0d",startup,width,FACT,checks);
 end
 always @(posedge clock) begin:tick
  if(reset)observed<=BIAS;
  else if(enable)begin:enabled
   observed<=value(data);
   $display("NATIVE_HIER_TICK m=[%m] n=%0d",data);
  end
 end
endmodule
