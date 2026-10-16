#include "runner.h"

#include <cstring>

#include "stack_kill_child.h"

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "__test-stack-kill-child") == 0) {
    return stack_kill_child_main(argc - 2, argv + 2);
  }
  return ::test::run_all();
}
