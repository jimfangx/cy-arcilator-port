#include "slang/ast/ScriptSession.h"
#include <iomanip>
#include <iostream>
#include <string>
int main() {
  const char *expressions[] = {
    "$sformatf(\"%s\",32'h41004243)",
    "string'(32'h41004243)",
    "string'(\"A\\000BC\")",
    "$sformatf(\"%s\",32'h00414243)",
    "$sformatf(\"%s\",32'h41004200)",
    "$sformatf(\"%s\",32'h0)",
    "$sformatf(\"%8s\",32'h41004243)",
    "$sformatf(\"%0s\",32'h41004243)"
  };
  for (auto expression : expressions) {
    slang::ast::ScriptSession session;
    auto value = session.eval(expression);
    if (!value.isString() || !session.getDiagnostics().empty()) return 1;
    const auto &text = value.str();
    std::cout << expression << '\t' << std::dec << text.size() << '\t';
    for (unsigned char c : text)
      std::cout << std::hex << std::setfill('0') << std::setw(2) << unsigned(c);
    std::cout << '\n';
  }
}
