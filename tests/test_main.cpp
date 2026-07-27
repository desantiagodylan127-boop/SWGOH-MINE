#include <iostream>

int run_content_pack_tests();
int run_core_tests();
int run_rpc_tests();
int run_state_tests();

int main() {
  const struct {
    const char* name;
    int (*run)();
  } suites[] = {
      {"content_pack", run_content_pack_tests},
      {"core", run_core_tests},
      {"rpc_registry", run_rpc_tests},
      {"state", run_state_tests},
  };

  for (const auto& suite : suites) {
    const int result = suite.run();
    if (result != 0) {
      std::cerr << suite.name << " failed at line " << result << '\n';
      return 1;
    }
    std::cout << suite.name << " passed\n";
  }
  return 0;
}
