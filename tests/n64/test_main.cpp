#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstring>
#include <vector>
#include "n64/n64.hpp"

int main(int argc, char** argv) {
    std::vector<char*> args;
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--rdp-threaded") == 0)
            ezy::n64::N64::set_default_rdp_mode(ezy::n64::RdpMode::Threaded);
        else if (std::strcmp(argv[i], "--rdp-sync") == 0)
            ezy::n64::N64::set_default_rdp_mode(ezy::n64::RdpMode::Synchronous);
        else if (std::strcmp(argv[i], "--rsp-jit") == 0)
            ezy::n64::set_default_rsp_mode(ezy::n64::RspMode::Jit);
        else if (std::strcmp(argv[i], "--rsp-interpreter") == 0)
            ezy::n64::set_default_rsp_mode(ezy::n64::RspMode::Interpreter);
        else args.push_back(argv[i]);
    }
    doctest::Context context;
    context.applyCommandLine(static_cast<int>(args.size()), args.data());
    return context.run();
}
