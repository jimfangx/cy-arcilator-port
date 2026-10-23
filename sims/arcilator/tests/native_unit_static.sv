function int count(input bit enable);
 int counter=77;
 if(enable)counter++;
 return counter;
endfunction
module UnitStatic(input clock,enable,output int observed);
 initial observed=0;
 always @(posedge clock) observed <= count(enable);
endmodule
