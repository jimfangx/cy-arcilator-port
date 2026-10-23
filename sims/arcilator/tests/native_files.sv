module ArcRTLFiles(input clock, reset, enable, input [31:0] data,
                   input [128:0] text, output wire [3:0] status);
  integer fd, first, second, missing;
  initial begin
    fd = $fopen("native-file-a.log", "w");
    first = $fopen("native-file-b.log");
    second = $fopen("native-file-c.log");
    missing = $fopen("missing-directory/native-file.log", "w");
    // Opening a file remains observable even when the result is unused.
    $fopen("native-file-empty.log", "w");
    $fwrite(fd, "CONSTANT |%5s|%-5s|\n", "x", "é");
  end
  assign status = {missing == 0, second != 0, first != 0, fd != 0};
  always @(posedge clock) begin
    if (!reset && enable) begin
      $fwrite(fd, "F %08x |%s|%-20s|%20s|\n", data, text, text, text);
      $fwrite(first | second, "M %0d |%s|\n", data, text);
      $fflush();
      if (data[0]) begin
        $fclose(fd);
        fd = $fopen("native-file-a.log", "a");
      end
    end
  end
  final begin
    $fwrite(fd, "FINAL A\n");
    $fwrite(first | second, "FINAL M\n");
    $fclose(fd);
    $fclose(first | second);
  end
endmodule
