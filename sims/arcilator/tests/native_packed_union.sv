module ArcRTLPackedSelect(input clock, reset, enable, input [1:0] mode,
 input [64:0] index, offset, input [16:0] data,
 output [83:0] observed, output reg [83:0] before_q=0, after_q=0);
 typedef struct packed {logic [5:0] head; logic [64:0] body; logic [12:0] tail;} fields_t;
 typedef union packed {bit[83:0] raw; fields_t fields;} record_t;
 record_t q = record_t'(84'h8_0123456789abcdef_1357);
 assign observed = q;
 always @(posedge clock) begin
  if (reset) begin
   q <= 84'h8_0123456789abcdef_1357;
   before_q <= 0;
   after_q <= 0;
  end else if(enable) begin
   before_q <= q;
   case(mode)
    0: begin q[index] = data[0]; q.fields.body[offset +: 17] <= data; end
    1: begin q[index] <= data[0]; q.fields.body[offset +: 17] = data; end
    2: begin
     for(int i=0;i<4;i++) q[index+i] = data[i];
     for(int i=0;i<3;i++) q.fields.body[offset+i] <= data[i+4];
     q[83] = data[6];
     q[0] <= data[7];
    end
    3: begin q.fields.tail[offset +: 13] = data[12:0]; q[index +: 17] <= data; end
   endcase
   after_q <= q;
  end
 end
endmodule
