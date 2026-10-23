module Reference;
 timeunit 1ps; timeprecision 1ps;
 reg clock=0,peer=0,reset=1,en=0,test_en=0,second_en=0,pad=0,ie=0,oe=0,signal_in=0;
 wire gated,chained,inverted,joined,nor_clock,read_value,written;
 reg [7:0] count0=0,count1=0,count2=0,count3=0;
 GenericDigitalInIOCell ic(.pad(pad),.ie(ie),.i(read_value));
 GenericDigitalOutIOCell oc(.o(read_value),.oe(oe),.pad(written));
 ClockInverter inv(.clockIn(clock),.clockOut(inverted));
 ClockOr2 or_clock(.clocksIn_0(clock),.clocksIn_1(peer),.clockOut(joined));
 ClockSignalNor2 nor_gate(.clockIn(clock),.signalIn(signal_in),.clockOut(nor_clock));
 EICG_wrapper gate(.in(clock),.en(en),.test_en(test_en),.out(gated));
 EICG_wrapper chain(.in(gated),.en(second_en),.test_en(1'b0),.out(chained));
 always @(posedge gated or posedge reset) if(reset) count0<=0;else count0<=count0+1;
 always @(posedge chained or posedge reset) if(reset) count1<=0;else count1<=count1+1;
 always @(posedge inverted or posedge reset) if(reset) count2<=0;else count2<=count2+1;
 always @(posedge nor_clock or posedge reset) if(reset) count3<=0;else count3<=count3+1;
 reg [63:0] rng=64'hb869fa6502327bd7;
 integer i;
 initial begin
  for(i=0;i<16384;i=i+1)begin
   #1;
   rng=rng^(rng<<13);rng=rng^(rng>>7);rng=rng^(rng<<17);
   if(i%2)begin clock=rng[0];peer=rng[1];end
   else begin
    en=rng[2];test_en=rng[3];second_en=rng[4];pad=rng[5];ie=rng[6];oe=rng[7];signal_in=rng[8];reset=(i==0 || i%97<3);
   end
   #1;
   $display("GLUE %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d",i,gated,chained,inverted,joined,nor_clock,read_value,written,count0,count1,count2,count3);
  end
  $finish;
 end
endmodule
