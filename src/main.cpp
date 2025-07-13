#include "lix_interpreter.hpp"

#include <lix/libexpr/eval.hh>
#include <lix/libmain/shared.hh>
#include <lix/libutil/signals.hh>

#include "xeus-zmq/xserver_zmq.hpp"
#include "xeus-zmq/xzmq_context.hpp"
#include "xeus/xhelper.hpp"
#include "xeus/xkernel.hpp"
#include "xeus/xkernel_configuration.hpp"

#include <csignal>
#include <iostream>

void sigint_handler(int /*signum*/)
{
    nix::triggerInterrupt();
}

int main(int argc, char* argv[])
{
    // handle the --version flag
    if (xeus::should_print_version(argc, argv))
    {
        std::cout << "xeus-lix " << "0.1.0" << std::endl;
        return 0;
    }

    // set up the global state required by lix
    nix::initNix();
    nix::initLibExpr();

    // using sigaction is more robust and portable than `signal`
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    std::string connection_file = xeus::extract_filename(argc, argv);

    if (connection_file.empty())
    {
        std::cerr << "Error: No connection file provided. The Jupyter kernel requires the -f flag." << std::endl;
        return 1;
    }

    auto context = xeus::make_zmq_context();
    auto interpreter = std::make_unique<xeus_lix::interpreter>();

    xeus::xkernel kernel(
        xeus::load_configuration(connection_file),
        xeus::get_user_name(),
        std::move(context),
        std::move(interpreter),
        xeus::make_xserver_zmq
    );

    std::cout << "Starting xeus-lix kernel..." << std::endl;
    kernel.start();

    return 0;
}
