#include "lix_logger.hpp"

namespace xeus_lix
{
    JupyterLogger::JupyterLogger(interpreter* interp)
        : p_interpreter(interp)
    {
    }

    // called by lix to log general messages.
    void JupyterLogger::log(nix::Verbosity lvl, const std::string_view s)
    {
        // error/warn: redirected to stderr of cell
        if (lvl <= nix::lvlWarn) {
            p_interpreter->publish_stream("stderr", std::string(s) + "\n");
        }
        // notice/info: redirected to stdout of cell
        else if (lvl <= nix::lvlInfo) {
            p_interpreter->publish_stream("stdout", std::string(s) + "\n");
        }
        // talkative+: ignore
    }

    // called by lix to log structured error information.
    void JupyterLogger::logEI(const nix::ErrorInfo& ei)
    {
        std::stringstream oss;
        // use lix's own error formatting utility.
        nix::showErrorInfo(oss, ei, nix::loggerSettings.showTrace.get());
        // ErrorInfo structures are typically more severe and should always be shown.
        p_interpreter->publish_stream("stderr", oss.str());
    }

    // we don't need the following methods for now

    void JupyterLogger::startActivity(
        nix::ActivityId,
        nix::Verbosity,
        nix::ActivityType,
        const std::string&,
        const Fields&,
        nix::ActivityId
    )
    {
    }

    void JupyterLogger::stopActivity(nix::ActivityId)
    {
    }

    void JupyterLogger::result(nix::ActivityId, nix::ResultType, const Fields&)
    {
    }
}
