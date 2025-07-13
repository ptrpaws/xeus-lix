#ifndef XEUS_LIX_LOGGER_HPP
#define XEUS_LIX_LOGGER_HPP

#include "lix_interpreter.hpp"
#include "lix/libutil/error.hh"
#include "lix/libutil/logging.hh"

#include <string_view>

namespace xeus_lix
{
    class JupyterLogger : public nix::Logger
    {
    public:
        explicit JupyterLogger(interpreter* interp);

        // redirects general log messages
        void log(nix::Verbosity lvl, std::string_view s) override;
        // redirects structured error info
        void logEI(const nix::ErrorInfo& ei) override;

        // the following are part of the nix::Logger interface but not implemented yet
        void startActivity(nix::ActivityId, nix::Verbosity, nix::ActivityType, const std::string&, const Fields&, nix::ActivityId)
            override;
        void stopActivity(nix::ActivityId) override;
        void result(nix::ActivityId, nix::ResultType, const Fields&) override;

    private:
        interpreter* p_interpreter;
    };
}

#endif
